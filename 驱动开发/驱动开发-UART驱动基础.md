# 驱动开发-UART驱动基础

## 原始问题

Linux 里串口驱动怎么写？为什么有 `uart_driver`、`uart_port`、`uart_ops` 这么多层？自己写一个 UART 驱动需要实现哪些回调？用户态怎么通过 `/dev/ttySx` 访问串口？从裸机 UART 中断收发到 Linux 串口驱动，完整链路是什么？

## 先给结论

Linux UART 驱动的核心是"把硬件收发操作封装成 TTY 子系统的标准接口"：

1. UART 驱动分三层：`uart_driver`（注册驱动）、`uart_port`（硬件端口）、`uart_ops`（硬件操作回调）。
2. 驱动只需要实现 `uart_ops` 里的回调，TTY 子系统处理缓冲、流控、线路规程等。
3. 数据接收通过中断完成，驱动把收到的数据 push 到 TTY 的 flip buffer。
4. 数据发送由 TTY 子系统触发，驱动在 `start_tx` 回调里启动硬件发送。
5. 大多数 SoC 厂商已提供 UART 控制器驱动，实际工作中更多是配置设备树而非从头写驱动。

如果只能背"UART 就是串口收发"，但讲不清 TTY 子系统分层、`uart_ops` 各回调的作用和数据流转路径，就还没有真正掌握 Linux UART 驱动。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 知道裸机 UART 怎么收发，但不知道 Linux 下怎么写 UART 驱动。
2. 不理解 `uart_driver`、`uart_port`、`uart_ops` 三者的关系和各自职责。
3. 不知道数据从硬件中断到用户态 `read()` 的完整流转路径。
4. 不清楚 DMA 模式下 UART 驱动怎么写。
5. 面试时知道"串口驱动属于 TTY 子系统"，但讲不清分层和数据流。

它在AI时代依然重要，因为AI生成 UART 驱动代码时经常忽略 TTY 子系统的缓冲机制、流控处理和错误状态上报。理解 UART 驱动分层后，你才能判断AI给的代码是否正确对接了 TTY 子系统。

## 核心概念 / 本质机制

### 1. Linux 串口子系统的分层

```text
用户态:  open("/dev/ttyS0") / read() / write()
  |
  V
TTY 核心层:  缓冲管理、线路规程、流控
  |
  V
UART 核心层: uart_driver / uart_port / uart_ops
  |
  V
UART 控制器驱动: 操作具体硬件寄存器
  |
  V
硬件: UART 控制器
```

各层职责：

1. **TTY 核心层**：提供 `/dev/ttySx` 设备节点，管理缓冲、线路规程（line discipline）、流控。
2. **UART 核心层**：提供 `uart_driver` 注册框架和 `uart_ops` 接口定义。
3. **UART 控制器驱动**：实现 `uart_ops`，操作具体硬件。

### 2. `uart_driver`：注册驱动

```c
static struct uart_driver my_uart_driver = {
    .owner       = THIS_MODULE,
    .driver_name = "my_uart",
    .dev_name    = "ttyMY",       /* /dev/ttyMY0, ttyMY1, ... */
    .major       = 0,             /* 0 = 自动分配 */
    .minor       = 0,
    .nr          = 2,             /* 支持的端口数量 */
};

/* 注册 */
uart_register_driver(&my_uart_driver);

/* 注销 */
uart_unregister_driver(&my_uart_driver);
```

`uart_driver` 的作用：

1. 向 TTY 子系统注册一个 UART 驱动。
2. 定义设备节点名称（`/dev/ttyMY0`）。
3. 定义支持的端口数量。

### 3. `uart_port`：描述硬件端口

```c
struct uart_port port = {
    .iobase   = 0x10000000,       /* IO 基地址 */
    .irq      = 25,               /* 中断号 */
    .uartclk  = 48000000,         /* 输入时钟频率 */
    .fifosize = 16,               /* FIFO 大小 */
    .ops      = &my_uart_ops,     /* 硬件操作回调 */
    .type     = PORT_16550A,      /* UART 类型 */
    .flags    = UPF_BOOT_AUTOCONF,
    .line     = 0,                /* 端口编号 */
};
```

`uart_port` 的作用：

1. 描述一个具体 UART 端口的硬件参数。
2. 绑定 `uart_ops` 回调。
3. 一个 `uart_driver` 可以管理多个 `uart_port`。

### 4. `uart_ops`：硬件操作回调

```c
static const struct uart_ops my_uart_ops = {
    .tx_empty     = my_tx_empty,
    .set_mctrl    = my_set_mctrl,
    .get_mctrl    = my_get_mctrl,
    .stop_tx      = my_stop_tx,
    .start_tx     = my_start_tx,
    .stop_rx      = my_stop_rx,
    .enable_ms    = my_enable_ms,
    .break_ctl    = my_break_ctl,
    .startup      = my_startup,
    .shutdown     = my_shutdown,
    .set_termios  = my_set_termios,
    .type         = my_type,
    .release_port = my_release_port,
    .request_port = my_request_port,
    .config_port  = my_config_port,
};
```

核心回调说明：

| 回调 | 调用时机 | 作用 |
| --- | --- | --- |
| `startup` | 用户态 `open()` | 初始化硬件，申请中断 |
| `shutdown` | 用户态 `close()` | 关闭硬件，释放中断 |
| `set_termios` | 用户态 `tcsetattr()` | 配置波特率、数据位、停止位、校验 |
| `start_tx` | TTY 有数据要发送 | 启动硬件发送 |
| `stop_tx` | TTY 要求停止发送 | 停止硬件发送 |
| `stop_rx` | TTY 要求停止接收 | 停止硬件接收 |
| `tx_empty` | TTY 检查发送是否完成 | 返回发送 FIFO 是否为空 |
| `set_mctrl` | 设置 modem 控制线 | 控制 RTS、DTR |
| `get_mctrl` | 读取 modem 状态线 | 读取 CTS、DSR、DCD、RI |

### 5. 数据接收路径

```text
硬件收到数据
-> UART 中断触发
-> 驱动在中断 handler 里读取 RX FIFO
-> 调用 uart_insert_char() 或 tty_insert_flip_string() 写入 flip buffer
-> 调用 tty_flip_buffer_push() 通知 TTY 核心
-> TTY 核心把数据从 flip buffer 搬到 line discipline 缓冲
-> 用户态 read() 从 line discipline 缓冲读取
```

中断接收的关键代码：

```c
static void my_rx_chars(struct uart_port *port)
{
    while (!(readl(port->membase + REG_LSR) & LSR_RX_EMPTY)) {
        u8 ch = readl(port->membase + REG_RBR);
        u8 flag = TTY_NORMAL;

        if (readl(port->membase + REG_LSR) & LSR_OE)
            flag = TTY_OVERRUN;
        if (readl(port->membase + REG_LSR) & LSR_PE)
            flag = TTY_PARITY;
        if (readl(port->membase + REG_LSR) & LSR_FE)
            flag = TTY_FRAME;

        uart_insert_char(port, readl(port->membase + REG_LSR),
                         LSR_OE, ch, flag);
    }

    tty_flip_buffer_push(&port->state->port);
}
```

### 6. 数据发送路径

```text
用户态 write()
-> TTY 核心把数据写入环形缓冲区
-> 调用驱动的 start_tx 回调
-> 驱动从环形缓冲区取数据写入 TX FIFO
-> TX FIFO 发送完成触发中断
-> 驱动继续从环形缓冲区取数据
-> 环形缓冲区空了，调用 uart_write_wakeup() 通知 TTY
-> TTY 核心可以继续接受 write()
```

发送的关键代码：

```c
static void my_start_tx(struct uart_port *port)
{
    struct circ_buf *xmit = &port->state->xmit;

    if (uart_circ_empty(xmit))
        return;

    /* 使能 TX 中断 */
    writel(readl(port->membase + REG_IER) | IER_TX_ENABLE,
           port->membase + REG_IER);
}

static void my_tx_chars(struct uart_port *port)
{
    struct circ_buf *xmit = &port->state->xmit;
    int count;

    if (port->x_char) {
        writel(port->x_char, port->membase + REG_TBR);
        port->icount.tx++;
        port->x_char = 0;
        return;
    }

    if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
        my_stop_tx(port);
        return;
    }

    count = port->fifosize;
    do {
        writel(xmit->buf[xmit->tail], port->membase + REG_TBR);
        xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
        port->icount.tx++;
        if (uart_circ_empty(xmit))
            break;
    } while (--count > 0);

    if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
        uart_write_wakeup(port);

    if (uart_circ_empty(xmit))
        my_stop_tx(port);
}
```

### 7. `set_termios` 回调

```c
static void my_set_termios(struct uart_port *port,
                           struct ktermios *new,
                           struct ktermios *old)
{
    unsigned int baud, quot;
    u32 lcr = 0;

    /* 获取波特率 */
    baud = uart_get_baud_rate(port, new, old, 9600, 115200);
    quot = port->uartclk / (16 * baud);

    /* 数据位 */
    switch (new->c_cflag & CSIZE) {
    case CS5: lcr |= LCR_DATABITS_5; break;
    case CS6: lcr |= LCR_DATABITS_6; break;
    case CS7: lcr |= LCR_DATABITS_7; break;
    default:  lcr |= LCR_DATABITS_8; break;
    }

    /* 校验位 */
    if (new->c_cflag & PARENB) {
        lcr |= LCR_PARITY_ENABLE;
        if (!(new->c_cflag & PARODD))
            lcr |= LCR_EVEN_PARITY;
    }

    /* 停止位 */
    if (new->c_cflag & CSTOPB)
        lcr |= LCR_STOPBITS_2;

    /* 配置硬件 */
    writel(lcr, port->membase + REG_LCR);
    writel(quot & 0xFF, port->membase + REG_DLL);
    writel((quot >> 8) & 0xFF, port->membase + REG_DLH);

    /* 更新超时 */
    uart_update_timeout(port, new->c_cflag, baud);
}
```

### 8. DMA 模式

当 UART 支持 DMA 时，收发路径改为：

1. **接收**：DMA 把数据从 RX FIFO 搬到内存缓冲区，DMA 完成中断里 push 到 flip buffer。
2. **发送**：DMA 把数据从环形缓冲区搬到 TX FIFO，DMA 完成中断里通知 TTY。

DMA 模式的优势：

1. CPU 不参与逐字节搬运，降低 CPU 占用。
2. 适合高波特率场景。
3. 中断次数大幅减少。

DMA 模式的代价：

1. 代码更复杂，需要管理 DMA 缓冲区和描述符。
2. 需要处理 DMA 对齐和缓存一致性。

### 9. 设备树描述

```dts
uart0: serial@10000000 {
    compatible = "vendor,my-uart";
    reg = <0x10000000 0x100>;
    interrupts = <0 25 4>;
    clocks = <&clk_uart0>;
    clock-frequency = <48000000>;
    dmas = <&dma0 2>, <&dma0 3>;
    dma-names = "rx", "tx";
    status = "okay";
};
```

## 数据流 / 控制流 / 时序关系

UART 驱动的完整生命周期：

```text
模块加载:
  uart_register_driver() -> 创建 /dev/ttyMYx 设备节点

probe:
  获取设备树资源 -> 初始化 uart_port -> uart_add_one_port()

用户态 open():
  TTY 核心 -> uart_ops.startup() -> 申请中断、使能硬件

用户态 tcsetattr():
  TTY 核心 -> uart_ops.set_termios() -> 配置波特率等

数据接收:
  硬件中断 -> 读取 RX FIFO -> uart_insert_char() -> tty_flip_buffer_push()
  -> TTY 缓冲 -> 用户态 read()

数据发送:
  用户态 write() -> TTY 环形缓冲 -> uart_ops.start_tx()
  -> 写入 TX FIFO -> 发送完成中断 -> 继续发送或停止

用户态 close():
  TTY 核心 -> uart_ops.shutdown() -> 禁用硬件、释放中断

remove:
  uart_remove_one_port()

模块卸载:
  uart_unregister_driver()
```

## 最小可运行示例

以下是一个简化版 UART 驱动框架（不含完整寄存器操作）：

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/io.h>
#include <linux/of.h>

#define MY_UART_NR      2
#define MY_UART_FIFO    16
#define MY_UART_XMIT    4096

#define REG_RBR  0x00
#define REG_TBR  0x00
#define REG_IER  0x04
#define REG_LSR  0x14

#define LSR_RX_EMPTY  BIT(0)
#define LSR_TX_EMPTY  BIT(5)
#define LSR_OE        BIT(1)
#define IER_RX_ENABLE BIT(0)
#define IER_TX_ENABLE BIT(1)

struct my_uart_port {
    struct uart_port port;
};

static unsigned int my_tx_empty(struct uart_port *port)
{
    return (readl(port->membase + REG_LSR) & LSR_TX_EMPTY) ?
           TIOCSER_TEMT : 0;
}

static void my_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int my_get_mctrl(struct uart_port *port)
{
    return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void my_stop_tx(struct uart_port *port)
{
    writel(readl(port->membase + REG_IER) & ~IER_TX_ENABLE,
           port->membase + REG_IER);
}

static void my_start_tx(struct uart_port *port)
{
    writel(readl(port->membase + REG_IER) | IER_TX_ENABLE,
           port->membase + REG_IER);
}

static void my_stop_rx(struct uart_port *port)
{
    writel(readl(port->membase + REG_IER) & ~IER_RX_ENABLE,
           port->membase + REG_IER);
}

static void my_rx_chars(struct uart_port *port)
{
    while (readl(port->membase + REG_LSR) & LSR_RX_EMPTY) {
        u8 ch = readl(port->membase + REG_RBR);
        uart_insert_char(port, 0, 0, ch, TTY_NORMAL);
    }
    tty_flip_buffer_push(&port->state->port);
}

static void my_tx_chars(struct uart_port *port)
{
    struct circ_buf *xmit = &port->state->xmit;
    int count;

    if (port->x_char) {
        writel(port->x_char, port->membase + REG_TBR);
        port->icount.tx++;
        port->x_char = 0;
        return;
    }

    if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
        my_stop_tx(port);
        return;
    }

    count = port->fifosize;
    do {
        writel(xmit->buf[xmit->tail], port->membase + REG_TBR);
        xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
        port->icount.tx++;
        if (uart_circ_empty(xmit)) break;
    } while (--count > 0);

    if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
        uart_write_wakeup(port);

    if (uart_circ_empty(xmit))
        my_stop_tx(port);
}

static irqreturn_t my_uart_irq(int irq, void *dev_id)
{
    struct uart_port *port = dev_id;
    u32 status = readl(port->membase + REG_LSR);

    if (status & LSR_RX_EMPTY)
        my_rx_chars(port);

    if (status & LSR_TX_EMPTY)
        my_tx_chars(port);

    return IRQ_HANDLED;
}

static int my_startup(struct uart_port *port)
{
    int ret = request_irq(port->irq, my_uart_irq, 0, "my_uart", port);
    if (ret) return ret;

    writel(IER_RX_ENABLE, port->membase + REG_IER);
    return 0;
}

static void my_shutdown(struct uart_port *port)
{
    writel(0, port->membase + REG_IER);
    free_irq(port->irq, port);
}

static void my_set_termios(struct uart_port *port,
                           struct ktermios *new,
                           struct ktermios *old)
{
    unsigned int baud = uart_get_baud_rate(port, new, old, 9600, 115200);
    uart_update_timeout(port, new->c_cflag, baud);
}

static const char *my_type(struct uart_port *port)
{
    return "MY_UART";
}

static const struct uart_ops my_uart_ops = {
    .tx_empty     = my_tx_empty,
    .set_mctrl    = my_set_mctrl,
    .get_mctrl    = my_get_mctrl,
    .stop_tx      = my_stop_tx,
    .start_tx     = my_start_tx,
    .stop_rx      = my_stop_rx,
    .startup      = my_startup,
    .shutdown     = my_shutdown,
    .set_termios  = my_set_termios,
    .type         = my_type,
};

static struct uart_driver my_uart_driver = {
    .owner       = THIS_MODULE,
    .driver_name = "my_uart",
    .dev_name    = "ttyMY",
    .major       = 0,
    .minor       = 0,
    .nr          = MY_UART_NR,
};

static int my_uart_probe(struct platform_device *pdev)
{
    struct my_uart_port *up;
    struct resource *res;
    int ret, irq;

    up = devm_kzalloc(&pdev->dev, sizeof(*up), GFP_KERNEL);
    if (!up) return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    up->port.membase = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(up->port.membase)) return PTR_ERR(up->port.membase);

    up->port.mapbase = res->start;
    irq = platform_get_irq(pdev, 0);
    if (irq < 0) return irq;
    up->port.irq = irq;

    up->port.uartclk = 48000000;
    up->port.fifosize = MY_UART_FIFO;
    up->port.ops = &my_uart_ops;
    up->port.dev = &pdev->dev;
    up->port.line = 0;

    ret = uart_add_one_port(&my_uart_driver, &up->port);
    if (ret) return ret;

    platform_set_drvdata(pdev, up);
    return 0;
}

static int my_uart_remove(struct platform_device *pdev)
{
    struct my_uart_port *up = platform_get_drvdata(pdev);
    uart_remove_one_port(&my_uart_driver, &up->port);
    return 0;
}

static const struct of_device_id my_uart_match[] = {
    { .compatible = "vendor,my-uart" },
    {}
};
MODULE_DEVICE_TABLE(of, my_uart_match);

static struct platform_driver my_uart_platform = {
    .probe  = my_uart_probe,
    .remove = my_uart_remove,
    .driver = {
        .name = "my_uart",
        .of_match_table = my_uart_match,
    },
};

static int __init my_uart_init(void)
{
    int ret = uart_register_driver(&my_uart_driver);
    if (ret) return ret;
    return platform_driver_register(&my_uart_platform);
}

static void __exit my_uart_exit(void)
{
    platform_driver_unregister(&my_uart_platform);
    uart_unregister_driver(&my_uart_driver);
}

module_init(my_uart_init);
module_exit(my_uart_exit);

MODULE_LICENSE("GPL");
```

## 代码解读

### 1. 为什么 `startup` 里用 `request_irq` 而不是 `devm_request_irq`

因为 `startup`/`shutdown` 是用户态 `open`/`close` 时调用的，不是 `probe`/`remove` 时调用的。`devm` 资源在设备移除时释放，而中断需要在最后一个用户关闭设备时释放。

### 2. 为什么发送用环形缓冲区（`circ_buf`）

TTY 核心维护一个环形缓冲区，用户态 `write()` 的数据先进入这里，驱动从里面取数据发送。这样用户态不需要等硬件发送完才返回。

### 3. 为什么 `my_tx_chars` 里要检查 `WAKEUP_CHARS`

当环形缓冲区里的数据量低于 `WAKEUP_CHARS`（默认 256）时，调用 `uart_write_wakeup()` 通知 TTY 核心"缓冲区快空了，可以继续写"。这是流控的一部分。

### 4. 为什么 `my_get_mctrl` 返回硬编码值

简化示例里假设 CTS/DSR/DCD 始终有效。实际驱动需要读取硬件状态。

### 5. 为什么 `uart_register_driver` 在 `module_init` 里调用

`uart_register_driver` 只需要调用一次，注册整个驱动。`uart_add_one_port` 在 `probe` 里调用，每个端口一次。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 接收数据 | `uart_insert_char` + `tty_flip_buffer_push` | 直接写用户态缓冲 | 绕过 TTY 子系统，破坏缓冲管理 |
| 发送数据 | 从 `circ_buf` 取数据 | 自己维护发送缓冲 | 跟 TTY 缓冲管理冲突 |
| 中断注册 | `startup` 里注册，`shutdown` 里释放 | `probe` 里注册 | 用户没 open 时不需要中断 |
| 波特率配置 | `uart_get_baud_rate` + `uart_update_timeout` | 自己算波特率 | 没处理 TTY 标志和超时 |
| 流控 | 检查 `WAKEUP_CHARS` 并调用 `uart_write_wakeup` | 不通知 TTY | TTY 层可能阻塞 |
| 错误标志 | `uart_insert_char` 带 flag | 丢弃错误数据 | 用户态需要知道错误 |

## 边界条件与适用范围

1. UART 驱动不适合高吞吐场景（如高速数据采集），应该用 SPI 或 DMA 方案。
2. `uart_insert_char` 不能在中断上下文之外调用。
3. `tty_flip_buffer_push` 会触发软中断处理，不能在持有自旋锁时调用。
4. 波特率配置要考虑时钟精度，实际波特率可能跟请求值有偏差。
5. 多端口驱动需要正确管理 `port->line` 编号。
6. 实际工作中，大多数 SoC 已有厂商提供的 UART 驱动，通常只需配置设备树。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| `/dev/ttyMYx` 不出现 | `uart_register_driver` 或 `uart_add_one_port` 失败 | `dmesg` 看返回值 |
| `open` 失败 | `startup` 回调返回错误 | 检查中断注册是否成功 |
| 收不到数据 | RX 中断未使能或中断号错误 | `cat /proc/interrupts` 看计数 |
| 发不出去 | `start_tx` 没使能 TX 中断 | 检查 IER 寄存器 |
| 波特率不对 | `uartclk` 配置错误 | 示波器量波形 |
| 数据丢失 | FIFO 溢出或 flip buffer 处理太慢 | 增大 FIFO 或用 DMA |

### 推荐排查顺序

1. 先确认设备节点是否存在：`ls /dev/ttyMY*`。
2. 再确认 `open` 是否成功：用户态程序打印 `fd`。
3. 再确认中断是否触发：`cat /proc/interrupts`。
4. 再确认波特率是否正确：示波器量波形。
5. 最后看数据收发的具体内容。

## 工程落地建议

### 1. 大多数情况不需要从头写 UART 驱动

主流 SoC（如 i.MX、STM32、Rockchip）都有厂商提供的 UART 驱动。实际工作通常是：

1. 配置设备树（波特率、引脚、DMA）。
2. 调整参数（时钟、FIFO 大小）。
3. 修复特定场景的 bug。

### 2. 如果需要写自定义 UART 驱动

1. 先参考内核里同系列的驱动（如 `8250.c`、`imx.c`）。
2. 从最小功能开始：先实现收发，再加 DMA 和流控。
3. 用 `minicom` 或 `picocom` 做基本验证。

### 3. 用户态串口编程

```c
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>

int fd = open("/dev/ttyMY0", O_RDWR);

struct termios tio;
tcgetattr(fd, &tio);
cfsetispeed(&tio, B115200);
cfsetospeed(&tio, B115200);
tio.c_cflag |= CS8 | CLOCAL | CREAD;
tio.c_cflag &= ~(PARENB | CSTOPB);
tcsetattr(fd, TCSANOW, &tio);

write(fd, "hello", 5);

char buf[64];
int n = read(fd, buf, sizeof(buf));

close(fd);
```

## 性能、稳定性、可维护性影响

1. 中断模式 UART 的 CPU 占用在高波特率下可能很高，DMA 模式可以显著降低。
2. flip buffer 的大小和处理速度直接影响接收性能，数据量大时需要调优。
3. 环形缓冲区大小（`UART_XMIT_SIZE`，默认 4096）影响发送吞吐量。
4. 流控（硬件流控 RTS/CTS 或软件流控 XON/XOFF）在高波特率下很重要。
5. UART 驱动的 bug 最常见的表现是"偶发丢数据"，通常跟 FIFO 溢出或中断延迟有关。

## 面试 / 问答怎么讲

### 30 秒版本

Linux UART 驱动基于 TTY 子系统，分三层：`uart_driver` 注册驱动、`uart_port` 描述端口、`uart_ops` 实现硬件操作。接收通过中断读取 RX FIFO 并 push 到 flip buffer，发送从 TTY 环形缓冲取数据写入 TX FIFO。大多数 SoC 已有厂商驱动，实际工作主要是配置设备树。

### 3 分钟版本

可以从分层讲起：TTY 核心层管理缓冲和线路规程，UART 核心层提供 `uart_ops` 接口，驱动实现具体硬件操作。然后说明数据收发路径：接收是中断 → `uart_insert_char` → flip buffer → 用户态 `read`；发送是用户态 `write` → 环形缓冲 → `start_tx` → TX FIFO。再补充 `set_termios` 配置波特率的流程。最后说明 DMA 模式的优势。

### 10 分钟版本

可以结合一个完整驱动展开：从 `module_init` 注册 `uart_driver`，到 `probe` 里初始化 `uart_port`，到 `startup`/`shutdown` 管理中断，到收发中断处理函数的具体实现。然后讨论 DMA 模式如何改造、流控如何处理、高波特率下 FIFO 溢出怎么解决。最后说明实际工作中通常是配置设备树而非从头写驱动。

## 实战练习

1. 在已有 SoC 平台上配置设备树，启用一个 UART 端口，用 `minicom` 验证收发。
2. 写一个用户态程序，用 `termios` API 配置串口并收发数据。
3. 参考内核 `8250.c` 驱动，理解标准 UART 驱动的完整实现。
4. 在驱动里加 `dev_info` 日志，观察 `open`/`close`/`read`/`write` 时各回调的调用顺序。
5. 用示波器量 UART TX 波形，验证波特率和数据格式是否正确。

## 关键要点

1. UART 驱动分三层：`uart_driver`（注册）、`uart_port`（端口）、`uart_ops`（操作）。
2. 接收路径：中断 → `uart_insert_char` → `tty_flip_buffer_push` → 用户态 `read`。
3. 发送路径：用户态 `write` → 环形缓冲 → `start_tx` → TX FIFO。
4. `startup`/`shutdown` 跟 `open`/`close` 对应，不是 `probe`/`remove`。
5. `set_termios` 配置波特率、数据位、停止位、校验。
6. DMA 模式降低 CPU 占用，适合高波特率场景。
7. 大多数 SoC 已有厂商 UART 驱动，实际工作主要是配置设备树。

## 关联笔记

1. `外设-UART基础`
2. `驱动开发-platform驱动基础`
3. `驱动开发-IRQ处理基础`
4. `驱动开发-字符设备基础`
5. `通信协议-串口协议设计`
6. `Linux-设备树`
