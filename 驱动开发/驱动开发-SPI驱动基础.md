# 驱动开发-SPI驱动基础

## 原始问题

<<<<<<< HEAD
Linux 里的 SPI 驱动到底怎么写？它和 platform 驱动、I2C 驱动分别是什么关系？为什么很多 SPI 设备明明挂在控制器下面了，驱动还是会出现 `probe` 不进、读回数据全错、时好时坏这些问题？真正写一个 SPI 驱动时，主线应该怎么抓？

## 先给结论

SPI 驱动的本质，是让一个挂在 SPI 控制器下面的从设备，通过 SPI 总线模型接入内核。

先记住下面几个结论：

1. SPI 从设备不是直接做 MMIO，而是依赖 `spi_controller`、`spi_device`、`spi_driver` 这一整套总线模型。
2. SPI 驱动能否稳定工作，不只是“能发出时钟”这么简单，还取决于模式、片选、电平、时序、位宽、速率、dummy byte 和寄存器协议是否全都对齐。
3. `probe` 的核心不是“把功能都写完”，而是先完成最小闭环：匹配成功 -> 配置总线参数 -> `spi_setup()` -> 读出 chip ID 或状态寄存器 -> 再扩展功能。
4. SPI 排障主线通常是：控制器是否工作 -> 设备树是否创建设备 -> 驱动是否匹配 -> SPI mode / 频率 / 片选是否正确 -> 协议格式是否正确。
5. AI 可以很快生成一个 `spi_driver` 模板，但它经常不知道你的芯片到底要求 `SPI_MODE_0` 还是 `SPI_MODE_3`、读命令是否需要 dummy、片选释放时机是否正确。真正决定能不能落地的，还是你对总线和芯片协议的理解。

## 这个知识解决什么问题

1. 知道 SPI 外设怎么接线和通信，但不会把它写成 Linux 可维护驱动。
2. `probe` 进了却读不到正确寄存器，或者不同板子上表现不一致时，没有稳定排查主线。
3. 面试和工作里经常被问 SPI 驱动怎么写，但回答容易只停留在“会调 `spi_write_then_read()`”。

## 核心概念 / 本质机制

### 1. SPI 驱动和 platform 驱动、I2C 驱动是什么关系

它们的共同点是都要接入 Linux 设备模型，最终都绕不开匹配、`probe`、资源申请和错误处理。

区别在于：

1. platform 驱动更常面对固定 MMIO 设备，主线是寄存器资源、中断、时钟、复位。
2. I2C 驱动面对的是 I2C 总线上的从设备，主线是地址、ACK、寄存器读写事务。
3. SPI 驱动面对的是 SPI 总线上的从设备，主线是 mode、片选、速率、位宽和完整事务格式。

SPI 驱动比 I2C 更容易出现“总线能动但数据不对”的问题，因为 SPI 没有像 ACK 那样直接告诉你“这次地址阶段就失败了”，更多时候要靠波形和协议细节自己判断。

### 2. 内核里一个 SPI 设备是如何表示的

典型对象是 `struct spi_device`。

它至少表达：

1. 设备挂在哪个 SPI 控制器上。
2. 用哪个片选。
3. 当前配置的 `mode`、`bits_per_word`、`max_speed_hz`。
4. 它关联的 `struct device` 和驱动匹配信息。

驱动进入 `probe` 时，最核心的入口对象就是 `spi_device`。

### 3. SPI 驱动里的 `probe` 在做什么

一个典型 SPI 驱动的 `probe` 常做这些事：

1. 分配私有数据。
2. 设置 `mode`、`bits_per_word`、`max_speed_hz`。
3. 调用 `spi_setup()` 让控制器按目标设备要求配置总线参数。
4. 读取 chip ID、状态寄存器或版本寄存器，确认设备真实存在且协议打通。
5. 申请 GPIO、中断、regulator、reset 等额外依赖。
6. 注册 input、iio、hwmon、字符设备等对外接口。

### 4. 为什么 `spi_setup()` 很关键

很多人以为只要 `probe` 进了，后面直接收发就行，这很容易出错。

`spi_setup()` 的作用是把 `spi_device` 上配置好的：

1. `mode`
2. `bits_per_word`
3. `max_speed_hz`

真正同步到控制器侧。

如果这一步没做，或者做之前参数就错了，后面很可能不是“完全不通”，而是出现更难查的错位数据、全 0、全 0xff、偶发成功。

### 5. 为什么很多 SPI 驱动先读 chip ID

因为 SPI 匹配成功，只代表驱动接管了这个节点，不代表：

1. 片选一定对。
2. 时钟相位极性一定对。
3. 读命令格式一定对。
4. 设备电源、复位、启动延时一定满足。

先读固定寄存器有几个价值：

1. 验证总线参数是否大体正确。
2. 验证协议封装是否正确。
3. 验证这个设备真的是目标芯片。

### 6. 为什么 SPI 调试经常要看波形

因为 SPI 很多错误不是软件日志直接能看出来的，而是时序细节错了：

1. CPOL / CPHA 不对。
2. CS 拉低拉高时机不对。
3. 主机发命令后设备需要 dummy byte 才开始回数据。
4. 读写位定义和寄存器地址编码方式理解错了。

这类问题只看代码很难定位，往往必须结合逻辑分析仪或示波器。

## 数据流 / 控制流 / 时序关系

```text
SPI 控制器驱动工作正常
-> 设备树在 SPI 控制器节点下描述从设备
-> 内核创建 spi_device
-> SPI 驱动注册
-> 设备与驱动匹配成功进入 probe
-> 驱动设置 mode / bits_per_word / max_speed_hz
-> 调用 spi_setup()
-> 发送读寄存器命令，验证 chip ID 或状态寄存器
-> 申请 GPIO / IRQ / regulator / reset 等资源
-> 注册对外接口
-> 设备进入可用状态
```

如果任何一环不对，最后现象都可能只是“驱动加载了，但数据不对”。

## 最小可运行示例

### 1. 设备树节点示例

```dts
&spi0 {
    status = "okay";

    demo@0 {
        compatible = "demo,my-spi-dev";
        reg = <0>;
        spi-max-frequency = <1000000>;
    };
};
```

### 2. 最小 SPI 驱动示例
=======
Linux 里的 SPI 驱动到底和 platform 驱动、I2C 驱动有什么不同？为什么 SPI 设备已经挂在总线上了，驱动还是会遇到模式不对、`probe` 成功但读 ID 失败、片选时序不对、数据全是 `0xFF` 这种问题？真正写一个 SPI 驱动时，主线应该怎么抓？

## 先给结论

SPI 驱动的本质，是让一个挂在 SPI 控制器下的从设备通过 Linux SPI 总线模型接入内核，并按目标器件要求组织事务完成寄存器或数据访问。

先记住下面几个结论：

1. SPI 驱动的关键不只是“能发字节”，而是 `mode`、位宽、位序、片选时序、最大时钟、dummy 周期这些约束必须和器件手册匹配。
2. SPI 总线层通了，不代表驱动就对了；很多问题发生在“事务格式对不对”，而不是“有没有波形”。
3. `probe` 的核心不是写完整功能，而是先建立一个最小闭环，例如成功读出 chip ID。
4. SPI 驱动排障主线通常是：控制器工作正常 -> 设备树 / board info 创建设备 -> 驱动匹配 -> SPI 参数配置正确 -> 最小读写事务正确 -> 再扩展功能。
5. AI 可以快速生成 `spi_driver` 模板，但它不知道你的器件是 `Mode 0` 还是 `Mode 3`，不知道读寄存器前是否要 dummy byte，也不知道 CS 拉低后要不要延时。真正决定驱动能不能跑的是你对设备事务格式的理解。

如果只能写出一个 `spi_write_then_read()` 示例，却讲不清 `spi_device` 上的 `mode`、`bits_per_word`、`max_speed_hz` 为什么重要，也讲不清为什么 SPI 比 I2C 更依赖器件手册，就说明这部分还没有真正掌握。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 知道 SPI 外设原理，但不会把它写成 Linux 驱动。
2. 驱动已经 `probe` 了，但读不到寄存器、chip ID 全错、数据全是 `0x00/0xFF`，不知道先查哪一层。
3. 面试和工作中经常会被问到“SPI 驱动怎么写”“和 I2C 驱动有什么区别”，缺少稳定主线。
4. 后续做 SPI Flash、屏幕、ADC、传感器或带中断的 SPI 外设时，需要一篇连接外设知识和 Linux 驱动模型的桥梁笔记。

它在 AI 时代仍然重要，因为 AI 很容易生成一个能注册到总线上的驱动模板，但它不会自动知道你的器件命令字里读写位在哪一位，也不会替你判断一次事务里 CS 应该保持多久。SPI 理解不够时，AI 只会让你更快地得到一堆“看起来合理”的错误驱动。

## 核心概念 / 本质机制

### 1. SPI 驱动和 platform 驱动的本质区别

platform 驱动面对的通常是：

1. 固定 MMIO 设备。
2. 通过寄存器窗口直接访问。

SPI 驱动面对的则是：

1. 挂在某个 SPI 控制器下面的从设备。
2. 通过 SPI 事务和从设备交换数据。

所以 SPI 驱动真正拿到的核心对象是：

1. `struct spi_device`

它不仅表达“这是哪个设备”，还表达：

1. 用哪个控制器。
2. 片选是谁。
3. 当前模式、位宽、最大频率是多少。

### 2. SPI 和 I2C 驱动的关键差异

I2C 更像：

1. 总线层约束较强。
2. 地址和 ACK 机制比较统一。

SPI 更像：

1. 总线电气形式统一，但设备事务格式差异极大。
2. 没有统一地址和 ACK 语义。
3. 真正能否通信高度依赖器件手册给出的命令和时序。

所以 SPI 驱动更容易出现：

1. 波形有了，但事务格式不对。
2. `probe` 了，但读出来全错。
3. 某芯片能跑，换个兼容型号就不对。

### 3. `spi_device` 上最关键的几个参数

#### 3.1 `mode`

它通常决定：

1. `CPOL`
2. `CPHA`
3. 某些特殊模式位

如果模式错了，常见现象就是：

1. 数据位错位。
2. 读回值稳定但完全不对。
3. 低速偶尔能用，高速就全错。

#### 3.2 `bits_per_word`

默认很多设备按 8 位传输，但也有：

1. 16 位
2. 32 位
3. 特殊打包格式

位宽错了，事务边界和数据解释都会错。

#### 3.3 `max_speed_hz`

太高时常见问题：

1. 器件采样不到。
2. 板级走线边沿太差。
3. 某些 dummy 周期或时序约束不满足。

所以最小闭环阶段常常先低速跑通，再逐步提升。

### 4. 为什么 `spi_setup()` 很关键

在驱动里设置：

1. `spi->mode`
2. `spi->bits_per_word`
3. `spi->max_speed_hz`

之后，通常要调用 `spi_setup()`，让控制器层真正应用这些配置。

如果你只改了字段不 `setup`，表面代码像配了，底层控制器未必真的按新配置跑。

### 5. 什么叫 SPI 驱动的“最小闭环”

通常不是一上来就做完整业务，而是：

1. 匹配成功。
2. 配置正确的 SPI 参数。
3. 发出一个最小读事务。
4. 成功读到固定 chip ID 或版本寄存器。

这一步极其重要，因为它把问题范围大幅缩小到：

1. 总线和模式是否基本正确。
2. 事务格式是否基本正确。
3. 目标芯片是否真的在线。

### 6. 为什么事务格式决定成败

典型 SPI 器件手册会约定：

1. 读命令码是多少。
2. 地址长度几字节。
3. 数据在第几个字节开始。
4. 是否需要 dummy byte 或 dummy cycle。
5. CS 拉低后和拉高前是否要满足保持时间。

所以常见读寄存器链路可能是：

```text
CS 拉低
-> 发命令字
-> 发寄存器地址
-> 发一个 dummy byte
-> 再收数据
-> CS 拉高
```

只要其中任何一步和手册不匹配，波形看起来再“正常”，读回也可能全错。

### 7. 为什么 `spi_sync_transfer()` 很常用

因为一个 SPI 事务往往不是单一方向：

1. 先发命令。
2. 再发地址。
3. 再读数据。

用 `spi_sync_transfer()` 或多个 `spi_transfer` 组合，可以更清晰表达：

1. 每一段传输长度。
2. 每一段的 tx/rx buffer。
3. 是否保持片选不释放。

这比把所有逻辑硬塞进一个模糊的读写函数更清楚。

## 数据流 / 控制流 / 时序关系

下面用“读 chip ID”的典型 SPI 驱动链路来理解：

```text
设备树创建 spi_device
-> SPI 驱动注册并匹配
-> probe 获取 spi_device
-> 设置 mode / bits_per_word / max_speed_hz
-> 调用 spi_setup()
-> 构造最小读 ID 事务
-> 控制器按事务驱动 SCLK / MOSI / MISO / CS
-> 设备返回 chip ID
-> 驱动确认型号正确
-> 再继续初始化中断、缓存和上层接口
```

这条链路里最常见的问题包括：

1. `probe` 成功，但 SPI 参数没真正应用。
2. 模式错误，导致读回值错位。
3. 地址长度或 dummy byte 数量不对。
4. 频率太高，低速能跑高速不行。
5. 片选控制或保持时间不满足器件要求。

## 最小可运行示例

下面给一个最小 SPI 驱动闭环，目标是读出一个 8 位 chip ID：
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

```c
#include <linux/module.h>
#include <linux/of.h>
<<<<<<< HEAD
#include <linux/slab.h>
#include <linux/spi/spi.h>

struct demo_priv {
    struct spi_device *spi;
};

static int demo_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
    u8 tx = reg | 0x80;

    return spi_write_then_read(spi, &tx, 1, val, 1);
}

static int demo_probe(struct spi_device *spi)
{
    struct demo_priv *priv;
    u8 chip_id;
    int ret;

    priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    if (!spi->max_speed_hz)
        spi->max_speed_hz = 1000000;
=======
#include <linux/spi/spi.h>

struct demo_spi_priv {
    struct spi_device *spi;
};

static int demo_spi_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
    u8 tx = reg | 0x80; /* 假设最高位表示读 */
    int ret;

    ret = spi_write_then_read(spi, &tx, 1, val, 1);
    if (ret < 0)
        return ret;

    return 0;
}

static int demo_spi_probe(struct spi_device *spi)
{
    struct demo_spi_priv *priv;
    u8 chip_id = 0;
    int ret;

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = 1000000;
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

    ret = spi_setup(spi);
    if (ret)
        return ret;

<<<<<<< HEAD
    ret = demo_read_reg(spi, 0x00, &chip_id);
    if (ret)
        return ret;
=======
    priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

    priv->spi = spi;
    spi_set_drvdata(spi, priv);

<<<<<<< HEAD
=======
    ret = demo_spi_read_reg(spi, 0x00, &chip_id);
    if (ret)
        return dev_err_probe(&spi->dev, ret, "read chip id failed\n");

>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2
    dev_info(&spi->dev, "chip id = 0x%02x\n", chip_id);
    return 0;
}

<<<<<<< HEAD
static const struct of_device_id demo_of_match[] = {
    { .compatible = "demo,my-spi-dev" },
    { }
};
MODULE_DEVICE_TABLE(of, demo_of_match);

static const struct spi_device_id demo_id_table[] = {
    { "my_spi_dev", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, demo_id_table);

static struct spi_driver demo_driver = {
    .driver = {
        .name = "demo_spi_dev",
        .of_match_table = demo_of_match,
    },
    .probe = demo_probe,
    .id_table = demo_id_table,
};
module_spi_driver(demo_driver);
=======
static const struct of_device_id demo_spi_of_match[] = {
    { .compatible = "demo,my-spi-dev" },
    { }
};
MODULE_DEVICE_TABLE(of, demo_spi_of_match);

static struct spi_driver demo_spi_driver = {
    .probe = demo_spi_probe,
    .driver = {
        .name = "demo_spi_dev",
        .of_match_table = demo_spi_of_match,
    },
};
module_spi_driver(demo_spi_driver);
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

MODULE_LICENSE("GPL");
```

<<<<<<< HEAD
## 代码解读

### 1. 为什么示例先写 `mode`、`bits_per_word`、`max_speed_hz`

因为 SPI 设备是否能正常说话，前提不是“驱动结构体填完整”，而是总线参数对齐。

其中最关键的是：

1. 时钟模式。
2. 位宽。
3. 最大频率。

很多读错数据的问题，本质不是寄存器逻辑错，而是这三项有一项没对齐芯片手册。

### 2. 为什么示例要显式调用 `spi_setup()`

因为修改了 `spi_device` 参数后，要让控制器侧真正按这些参数工作，通常就要调用 `spi_setup()`。

这一步非常适合放在最小闭环里先做对，否则后面所有读写结果都不可信。

### 3. 为什么示例用 `spi_write_then_read()`

因为很多 SPI 寄存器型设备的最小读操作就是：

1. 先发一个命令或寄存器地址。
2. 再收回一个或多个字节。

这很适合作为“先打通链路”的第一步。

### 4. 为什么 `reg = <0>` 很重要

在 SPI 从设备节点里，`reg` 常表示片选号。

所以 `reg = <0>` 不是 I2C 那种从地址，而是在告诉内核这个设备挂在哪个 CS 上。

如果这里写错，即使驱动匹配成功，真正通信也可能一直在跟错误的设备或错误的片选打交道。
=======
这个示例虽然很小，但已经体现了 SPI 驱动的主线：

1. 驱动通过 `spi_device` 接入总线模型。
2. 先设置并应用 SPI 参数。
3. 用最小事务验证设备是否真的在线。
4. 读 chip ID 成功后，才有资格继续做复杂初始化。

## 代码解读

### 1. 为什么 `probe` 里先做 `spi_setup()`

因为：

1. `mode`
2. `bits_per_word`
3. `max_speed_hz`

这些参数不只是字段赋值，而是要真正下发给控制器层。

`spi_setup()` 成功后，后续事务才更有可能按预期时序执行。

### 2. 为什么最小读寄存器函数只有一字节命令

因为很多寄存器型 SPI 设备最小闭环就是：

1. 发一个带读位的寄存器地址。
2. 读回一个固定值。

这特别适合拿来：

1. 确认器件在线。
2. 确认 `mode` 基本正确。
3. 确认事务格式至少第一步没错。

### 3. 为什么 `dev_err_probe()` 依然值得用

虽然 SPI 驱动不像 I2C 那样常有 ACK 语义，但初始化失败一样需要：

1. 带设备上下文打印错误。
2. 在资源依赖失败时统一处理。

所以它仍然是很好的错误边界工具。

### 4. 为什么这还只是最小驱动

真实 SPI 驱动通常还要补：

1. 中断。
2. 缓冲区。
3. DMA。
4. 多字节寄存器访问。
5. dummy 周期处理。
6. 接入对应子系统，例如 IIO、input、mtd、net 等。
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
<<<<<<< HEAD
| 总线参数 | 先对齐手册里的 `mode`、位宽、频率，再 `spi_setup()` | 默认参数直接用 | 读回数据可能错位或全错 |
| 最小验证 | 先读 chip ID 或固定状态寄存器 | 一上来就写一大段初始化序列 | 出问题时定位面太大 |
| 片选理解 | 把 SPI 节点里的 `reg` 当片选号理解 | 把 `reg` 当设备寄存器地址理解 | 模型理解错误 |
| 协议封装 | 单独封装 `read_reg` / `write_reg` | 到处手写事务细节 | 易出重复 bug，维护差 |
| 排障路径 | 先查控制器、匹配、参数，再查业务逻辑 | 只盯着 `probe` 代码改 | 容易在错误层级上反复打转 |

## 边界条件与适用范围

1. 这篇笔记聚焦寄存器型 SPI 从设备驱动，不覆盖 SPI 控制器驱动实现细节。
2. 不同芯片的协议差异很大，有些是“地址 + 数据”，有些需要 dummy，有些是流式传输，不能照搬同一套寄存器读写函数。
3. 某些 SPI 设备更适合接入已有子系统，比如 `mtd`、`spi-mem`、`iio`、`input`、`hwmon`，不应只写裸收发。
4. 如果主线是 SPI 控制器本身、DMA、FIFO、时钟分频和中断处理，那更偏向平台驱动或控制器驱动，而不是从设备驱动。
=======
| 驱动入口 | 通过 `spi_device` 和总线模型工作 | 把 SPI 器件当 platform MMIO 设备写 | 模型不对 |
| 参数配置 | 设置 `mode/bits_per_word/max_speed_hz` 后 `spi_setup()` | 改了字段却不 setup | 控制器未必真的应用 |
| 最小验证 | 先读 chip ID 建立闭环 | 一上来就写整套业务逻辑 | 出问题时范围太大 |
| 排障路径 | 先查模式和事务格式，再查业务逻辑 | 只要有波形就盲信总线正常 | 很容易误判 |
| 事务实现 | 按器件手册组织命令、地址、dummy 和数据 | 把所有 SPI 设备都按同一种读法处理 | 器件差异被忽略 |
| 频率策略 | 先低速跑通再提速 | 一上来拉最高频率 | 很容易把时序问题误判成软件 bug |

## 边界条件与适用范围

1. 这篇笔记聚焦典型寄存器型 SPI 从设备驱动，不覆盖 SPI Flash、显示控制器等更复杂设备的全部细节。
2. 有些设备读写事务并不适合 `spi_write_then_read()`，更适合 `spi_sync_transfer()` 或 `spi_mem` 框架。
3. 某些器件对 CS 拉低保持、片选极性、dummy 周期要求非常严格，必须回到 datasheet 逐项确认。
4. 带中断或 DMA 的 SPI 设备，后续还要把 IRQ、缓存和并发一起纳入设计。
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
<<<<<<< HEAD
| `probe` 不进 | 控制器节点没开；从设备节点没挂对；`compatible` 不匹配 | 先看设备树层级、`status` 和匹配表 |
| 能发波形但读回全 `0xff` | 片选错；设备没上电；MISO 没接通；读命令格式错 | 看波形、片选、供电和读协议 |
| 读回数据错位 | `SPI_MODE` 不对；dummy byte 不够；位宽不对 | 对照手册和逻辑分析仪波形 |
| 低速正常高速异常 | `spi-max-frequency` 过高；板级信号质量差；控制器时序边界问题 | 降频验证并看波形边沿质量 |
| 某些板子能跑某些不行 | reset、电源、启动延时、GPIO 依赖未补齐 | 查设备上电时序和板级资源 |
| `spi_setup()` 失败 | 控制器不支持请求的 mode 或位宽 | 看返回值和控制器能力日志 |

### 推荐排查顺序

1. 先确认运行中的设备树确实在正确的 SPI 控制器下面创建了这个从设备。
2. 再确认 `compatible`、`reg`、`spi-max-frequency` 是否与预期一致。
3. 再确认 `mode`、位宽、频率和片选极性是否对齐芯片手册。
4. 再抓一次最小读寄存器波形，看命令、dummy、回读数据是否符合协议。
5. 最后再看更上层的初始化顺序、中断、缓存和并发问题。

## 调试与验证方法

### 1. 先验证软件侧最小闭环

至少确认下面三件事：

1. `probe` 确实进入。
2. `spi_setup()` 返回成功。
3. `dmesg` 里能看到稳定一致的 chip ID 日志。

如果连这三步都没稳定，就不要急着继续补复杂功能。

### 2. 再验证波形是否符合协议

至少看下面几项：

1. CS 是否在一次完整事务期间保持有效。
2. SCLK 的空闲电平和采样边沿是否符合目标 mode。
3. MOSI 上发出的命令字节是否和手册一致。
4. MISO 上的数据是立即返回，还是延后一字节 / 几位后返回。

### 3. 用降频法缩小问题范围

如果高频不稳定，可以先把频率降到一个很保守的值，比如 100kHz 或 1MHz。

这样做的价值是：

1. 先判断是不是纯时序边界问题。
2. 先把“协议错”与“信号质量差”分开。
3. 先建立一个可靠参考点，再慢慢往上提速。

## 工程落地建议

### 1. 优先做最小闭环

先只做下面几步：

1. 匹配成功。
2. `spi_setup()` 成功。
3. 读出 chip ID。

只有这三步稳定了，再扩展后续寄存器初始化和上层接口。

### 2. 把协议层和驱动层分开

建议尽量把：

1. `read_reg`
2. `write_reg`
3. 批量读写
4. 状态轮询

这些协议细节封装成独立函数。

这样后面排错时，能先确定“总线和协议层对不对”，再看业务逻辑。

### 3. 对日志和错误码保持克制但清晰

例如：

1. `failed to setup spi device`
2. `failed to read chip id`
3. `unsupported spi mode`

这类日志对联调非常有价值，远比泛泛打印一个 `ret = -EINVAL` 有用。

## 性能、稳定性、可维护性影响

1. SPI 驱动是否稳定，往往首先取决于时序参数和协议封装，而不是业务代码多少。
2. 先建立最小读寄存器闭环，能显著降低联调成本和误判概率。
3. 把寄存器协议封装清楚、把依赖资源建模完整，后续移植到新板或新芯片族时会轻松很多。
=======
| `probe` 进了但 chip ID 读失败 | `mode` 错；事务格式错；频率过高 | 先降频并抓波形 |
| 读回全 `0xFF` | MISO 悬空；CS 无效；器件没响应 | 查片选和硬件连接 |
| 读回全 `0x00` | 模式错；读命令错；dummy 周期不对 | 查 datasheet 和波形 |
| 某些板子能跑某些不行 | 板级时序、频率、片选极性或电源差异 | 比较设备树和波形 |
| 低速正常高速异常 | 走线边沿差；频率超规格；采样边沿不匹配 | 降频、切换 mode、抓波形 |
| 能写不能读 | 读事务格式和写事务不同；漏了 dummy | 查器件命令格式 |

排查 SPI 驱动时，推荐按下面顺序走：

1. 设备树是否创建设备并匹配到驱动。
2. `spi_setup()` 是否成功。
3. `mode`、位宽、频率是否符合器件手册。
4. 最小读 ID 事务是否正确。
5. 波形里的 CS、SCLK、MOSI、MISO 是否和预期一致。
6. 再去看中断、DMA 和上层业务逻辑。

## 工程落地建议

1. 任何新 SPI 驱动都先做“低速读 chip ID”最小闭环。
2. 把器件事务格式整理成笔记或注释，例如“读命令、地址长度、dummy、最大频率、CS 约束”。
3. 优先保留一套能和逻辑分析仪波形对上的测试事务，后续排障特别值钱。
4. 如果设备有现成内核子系统可挂，尽量接入标准框架，不要长期维持裸驱动接口。
5. 一旦读写开始变复杂，优先把“寄存器访问 helper”和“业务流程”拆开，方便定位问题。

## 性能、稳定性、可维护性影响

1. SPI 参数和事务格式理解准确，驱动联调成本会明显下降。
2. 先做最小闭环，再扩功能，能大幅降低“哪里都像有问题”的调试混乱感。
3. 清晰的事务封装和手册约束记录，会直接降低后续芯片替换和维护成本。
4. 由于 SPI 缺少统一 ACK 语义，越是缺乏结构化设计，后期越难排障。
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

## 面试 / 问答怎么讲

### 30 秒版本

<<<<<<< HEAD
Linux SPI 驱动的核心，是接管一个 `spi_device`，在 `probe` 里先设置 mode、位宽、频率并调用 `spi_setup()`，然后通过最小 SPI 事务读出 chip ID，确认总线参数和芯片协议都打通，再去做后续初始化。

### 3 分钟版本

可以先讲 SPI 驱动和 platform、I2C 驱动的区别：SPI 从设备建立在总线模型上，但比 I2C 更依赖 mode、片选和时序。然后讲设备树如何在控制器下面创建 `spi_device`，驱动如何匹配进入 `probe`，为什么要先 `spi_setup()`，为什么要先读 chip ID，最后再补充常见问题集中在 mode、dummy、频率和片选。

### 10 分钟版本

可以进一步结合一个传感器或 ADC 驱动展开：设备树在 `spi0` 下描述从设备，内核创建设备实例，驱动匹配进入 `probe`，先设置 `SPI_MODE_0`、位宽和频率并调用 `spi_setup()`，再通过 `spi_write_then_read()` 读取 ID，确认协议通了后再做寄存器初始化、中断申请和子系统注册。排障时要同时看设备树、控制器能力、波形和芯片手册，而不是只盯代码。

## 实战练习

1. 写一个最小 SPI 驱动，只读取一次 chip ID 并打印日志。
2. 故意把 `SPI_MODE_0` 改成错误模式，抓波形并观察为什么数据会错位。
3. 把 `spi-max-frequency` 从 1MHz 提到 20MHz，比较低速和高速下波形与读数差异。
4. 给驱动补一个 reset GPIO 和上电延时，再总结为什么有些芯片不是“能通信就等于能工作”。

## 关键要点

1. SPI 驱动的核心不是会调 API，而是把 mode、片选、频率和协议格式真正对齐。
2. `spi_setup()` 和“先读 chip ID”是最关键的最小闭环。
3. SPI 很多问题必须结合波形看，单靠日志不够。
4. 排障时先分控制器层、总线参数层，再分设备协议层。
=======
Linux SPI 驱动的核心，是通过 `spi_device` 接入 SPI 总线模型，在 `probe` 里先设置 `mode`、位宽和频率并调用 `spi_setup()`，然后用最小事务读出 chip ID 建立闭环。排障时重点看事务格式、模式和片选时序，而不是只看有没有波形。

### 3 分钟版本

可以先讲 SPI 驱动和 platform、I2C 驱动的区别：SPI 更依赖器件手册定义的事务格式。然后讲 `spi_device`、`spi_setup()` 和最小读 ID 闭环，再补 `mode`、`bits_per_word`、`max_speed_hz` 的意义，以及为什么 SPI 常用逻辑分析仪辅助排障。

### 10 分钟版本

可以进一步结合一个传感器或 ADC 设备展开：设备树创建 `spi_device`，驱动匹配后设置 `Mode 0`、频率和位宽，用 `spi_write_then_read()` 或 `spi_sync_transfer()` 读版本寄存器，确认事务正确后再初始化中断和上层接口。然后往下讲为什么有些设备需要 dummy byte、为什么低速能跑高速不行、为什么 `0xFF` 常常意味着片选或 MISO 问题。这会非常贴近真实联调场景。

## 实战练习

1. 写一个最小 SPI 驱动，只完成 `spi_setup()` 和读 chip ID。
2. 用逻辑分析仪抓一次“读寄存器”事务，对照 datasheet 标出命令、地址、dummy 和数据阶段。
3. 故意把 `mode` 改错，观察读回值和波形的变化，并写出定位过程。
4. 给 SPI 驱动增加一个 IRQ 资源，再设计“SPI 事务 + 中断上报”的排障主线。

## 关键要点

1. SPI 驱动的难点常常在事务格式，不只是总线模型。
2. `spi_setup()` 和最小读 ID 闭环是关键起点。
3. 波形正常不代表事务正确，必须回到器件手册核对。
4. 先低速跑通，再扩功能和提频率，是很稳的工程路径。
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2

## 关联笔记

1. `驱动开发-总览`
<<<<<<< HEAD
2. `驱动开发-platform驱动基础`
3. `驱动开发-I2C驱动基础`
4. `Linux-设备树`
5. `外设-SPI通信基础`
=======
2. `驱动开发-I2C驱动基础`
3. `驱动开发-platform驱动基础`
4. `外设-SPI通信基础`
5. `Linux-platform总线与设备模型`
>>>>>>> 0e44dbb1d2c2be2b86aa1e8cb21055963b47cba2
6. `工具链与调试-示波器与逻辑分析仪排障`
