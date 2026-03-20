# 原始问题

在 UART 文件中相关部分记录笔记，如果 UART 想对串口收到的帧进行自定义协议格式，在 C/C++ 格式下是如何实现的（最好包括收发全流程包括处理和应用都呈现出来），高知识密度、高可读性、附带代码解释，详细准确。

---

# UART 自定义协议帧全流程实战

> 目标不是“会写一个 if/else 解析串口数组”，而是把 **驱动层、协议层、应用层、发送回包、错误恢复** 这一整条链真正打通。
> 这篇默认你已经理解 UART 基础原理，这里重点讲“收到一串字节后，怎样把它稳定地变成业务命令”。

---

## 0. 一句话先抓住本质

UART 只负责 **按顺序收发字节流**，不负责：

- 一帧从哪里开始
- 一帧到哪里结束
- 一帧多长
- 这一帧是不是完整
- 这一帧是不是有效
- 这一帧该交给哪个业务处理

所以，所谓“UART 自定义协议”，本质上就是你在软件里补上这 6 件事：

1. **定义帧边界**
2. **定义长度字段**
3. **定义校验规则**
4. **定义命令字和应答格式**
5. **定义错误恢复策略**
6. **定义驱动层到应用层的分层关系**

如果这 6 件事没设计清楚，串口通信就会很容易出现：

- 半包
- 粘包
- 帧头错位
- CRC 失败
- 回包混乱
- 后期加命令非常痛苦

---

## 1. 最推荐的工程分层

不要把串口协议写成“中断里收一个字节，然后直接 switch(cmd) 干业务”。

更稳的方式是固定成 3 层：

### 1.1 Driver 层

职责：

- 初始化 UART 硬件
- 收字节、发字节
- 中断或 DMA 搬运数据
- 提供 RX/TX 缓冲区接口

不负责：

- 找帧头
- 判长度
- CRC 校验
- 命令处理

### 1.2 Protocol 层

职责：

- 逐字节解析
- 找帧头
- 长度判断
- CRC 校验
- 组帧回包
- 错位重同步

不负责：

- 点灯
- 配参数
- 控 PWM
- 存 Flash

### 1.3 App 层

职责：

- 按 `CMD` 分发业务
- 调用 `board_led_set()`、`motor_set_speed()` 之类的业务接口
- 决定回应什么 payload

不负责：

- 操作 UART 寄存器
- 管理环形缓冲区
- 手工拼字节

一句话：

```text
driver 收发字节
protocol 解析和组帧
app 执行业务
```

这条边界越清楚，项目越稳。

---

## 2. 一个适合 MCU 的自定义帧格式

推荐格式：

```text
[HEAD1][HEAD2][LEN][CMD][SEQ][PAYLOAD...][CRC8]
```

字段定义：

- `HEAD1`：固定帧头 1，例如 `0xAA`
- `HEAD2`：固定帧头 2，例如 `0x55`
- `LEN`：`CMD + SEQ + PAYLOAD` 的总长度
- `CMD`：命令字
- `SEQ`：序号，用于请求/应答配对
- `PAYLOAD`：业务数据
- `CRC8`：对 `[LEN][CMD][SEQ][PAYLOAD...]` 做校验

### 2.1 为什么这套格式比“帧头 + 数据 + 帧尾”更稳

原因有 4 个：

1. **双帧头便于重同步**
   如果前面丢字节或进了垃圾数据，更容易重新找到起点。

2. **长度字段便于处理不定长**
   不需要靠等待固定帧尾，也不怕 payload 里恰好出现帧尾值。

3. **序号字段便于请求/响应对应**
   PC 发第 16 个命令，MCU 回包也带 `SEQ=16`，上位机很好匹配。

4. **CRC 是帧级校验，不是单字节校验**
   能覆盖整帧业务数据，而不是只靠 UART 的奇偶校验。

### 2.2 一个具体例子

命令：设置 LED 状态

```text
AA 55 03 02 10 01 CRC
```

解释：

- `AA 55`：帧头
- `03`：后面有效体长度为 3 字节，即 `CMD + SEQ + PAYLOAD(1)`
- `02`：`CMD_SET_LED`
- `10`：序号 `SEQ = 0x10`
- `01`：payload，表示开灯
- `CRC`：校验字节

对应应答可以设计成：

```text
AA 55 04 82 10 00 01 CRC
```

解释：

- `82`：`SET_LED_ACK`
- `10`：沿用请求序号
- `00`：结果码，0 表示成功
- `01`：当前 LED 实际状态

这比“只回一个 OK 字符串”工程价值高得多，因为：

- 命令可追踪
- 错误可编码
- 自动化脚本容易测

---

## 3. 收发全流程到底长什么样

### 3.1 接收链路

```text
上位机发送字节流
-> UART 外设收到字节
-> RXNE 中断 / DMA 把字节放到 RX 缓冲区
-> 协议任务从 RX 缓冲区取字节
-> proto_parse_byte() 逐字节推进状态机
-> 成功得到完整 frame
-> app_handle_frame() 根据 CMD 执行业务
-> app 构造应答 payload
-> proto_build_frame() 统一组帧
-> uart_drv_write_async() 送入 TX 缓冲区
-> TXE 中断 / DMA 发出
```

### 3.2 发送链路

发送也不要直接到处 `uart_send_byte()`，而要走统一路径：

```text
app 想发一条业务消息
-> 填 payload
-> 调 proto_build_frame()
-> 得到完整协议帧
-> 放入 TX ring buffer
-> TXE 中断持续搬运到 UART DR
```

这有两个很重要的好处：

1. 所有发出去的数据格式统一，不会有人漏加帧头或 CRC。
2. 发送变成非阻塞，业务线程不会傻等串口一个字节一个字节发完。

---

## 4. 先给出完整 C 版骨架

下面这套代码是面向嵌入式的最小可落地版本，逻辑分成 4 块：

1. 协议定义
2. 协议编码
3. 协议逐字节解析
4. 应用层命令分发

---

## 5. 协议定义与 CRC8

```c
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define PROTO_HEAD1         0xAAu
#define PROTO_HEAD2         0x55u
#define PROTO_PAYLOAD_MAX   64u
#define PROTO_BODY_MIN      2u    /* CMD + SEQ */
#define PROTO_BODY_MAX      (PROTO_BODY_MIN + PROTO_PAYLOAD_MAX)
#define PROTO_FRAME_MAX     (2u + 1u + PROTO_BODY_MAX + 1u)

typedef enum {
    CMD_PING        = 0x01,
    CMD_SET_LED     = 0x02,
    CMD_GET_VERSION = 0x03,

    CMD_PING_ACK        = 0x81,
    CMD_SET_LED_ACK     = 0x82,
    CMD_GET_VERSION_ACK = 0x83,
    CMD_ERROR_ACK       = 0xF0
} proto_cmd_t;

typedef enum {
    PROTO_ERR_OK        = 0x00,
    PROTO_ERR_LEN       = 0x01,
    PROTO_ERR_CMD       = 0x02,
    PROTO_ERR_PARAM     = 0x03,
    PROTO_ERR_INTERNAL  = 0x04
} proto_err_t;

typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t payload_len;
    uint8_t payload[PROTO_PAYLOAD_MAX];
} proto_frame_t;

static uint8_t proto_crc8_maxim_update(uint8_t crc, uint8_t data)
{
    uint8_t i;
    crc ^= data;
    for (i = 0; i < 8u; ++i) {
        if (crc & 0x01u) {
            crc = (uint8_t)((crc >> 1) ^ 0x8Cu);
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

static uint8_t proto_crc8_calc(const uint8_t *buf, size_t len)
{
    uint8_t crc = 0x00u;
    size_t i;
    for (i = 0; i < len; ++i) {
        crc = proto_crc8_maxim_update(crc, buf[i]);
    }
    return crc;
}
```

### 5.1 这里几个设计点必须吃透

- `PROTO_BODY_MIN = 2`，因为最少也要有 `CMD + SEQ`
- `LEN` 不把帧头和 CRC 算进去，这样协议更清晰
- `CRC8` 放帧尾，便于流式解析完后统一校验
- `ACK` 命令使用高位区分，请求/应答关系一眼能看出来

---

## 6. 组帧：任何发送都统一走这里

```c
size_t proto_build_frame(uint8_t cmd,
                         uint8_t seq,
                         const uint8_t *payload,
                         uint8_t payload_len,
                         uint8_t *out,
                         size_t out_cap)
{
    uint8_t body_len;
    uint8_t crc;
    uint8_t *p = out;

    if (out == NULL) {
        return 0u;
    }
    if ((payload == NULL) && (payload_len != 0u)) {
        return 0u;
    }
    if (payload_len > PROTO_PAYLOAD_MAX) {
        return 0u;
    }

    body_len = (uint8_t)(PROTO_BODY_MIN + payload_len);
    if (out_cap < (size_t)(2u + 1u + body_len + 1u)) {
        return 0u;
    }

    *p++ = PROTO_HEAD1;
    *p++ = PROTO_HEAD2;
    *p++ = body_len;
    *p++ = cmd;
    *p++ = seq;

    if (payload_len > 0u) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }

    crc = proto_crc8_calc(&out[2], (size_t)(1u + body_len));
    *p++ = crc;

    return (size_t)(p - out);
}
```

### 6.1 这个函数的意义

你要把它理解成“协议层的唯一出口”。

也就是说：

- `PING ACK` 用它发
- `SET_LED ACK` 用它发
- `ERROR ACK` 也用它发

这样整个工程里不会出现：

- 有的地方忘记加 `SEQ`
- 有的地方 CRC 算法不一致
- 有的地方帧格式偷偷变形

---

## 7. 逐字节解析状态机：这是最核心的一段

### 7.1 为什么必须是状态机

因为 UART 收到的是一个一个字节，完整帧不一定一次到齐。

所以解析过程一定是：

- 先等帧头
- 再等长度
- 再收命令和序号
- 再收 payload
- 最后等 CRC

这天然就是一个“逐步推进”的状态机，而不是“收到一个数组再暴力解析”。

### 7.2 状态定义

```c
typedef enum {
    ST_WAIT_HEAD1 = 0,
    ST_WAIT_HEAD2,
    ST_WAIT_LEN,
    ST_WAIT_CMD,
    ST_WAIT_SEQ,
    ST_WAIT_PAYLOAD,
    ST_WAIT_CRC
} proto_state_t;

typedef struct {
    proto_state_t state;
    uint8_t len;
    uint8_t cmd;
    uint8_t seq;
    uint8_t payload_len;
    uint8_t payload_idx;
    uint8_t payload[PROTO_PAYLOAD_MAX];
    uint8_t crc_calc;

    uint32_t rx_ok_cnt;
    uint32_t crc_fail_cnt;
    uint32_t len_err_cnt;
    uint32_t sync_loss_cnt;
} proto_parser_t;

static void proto_parser_reset(proto_parser_t *ps)
{
    ps->state = ST_WAIT_HEAD1;
    ps->len = 0u;
    ps->cmd = 0u;
    ps->seq = 0u;
    ps->payload_len = 0u;
    ps->payload_idx = 0u;
    ps->crc_calc = 0u;
}
```

### 7.3 逐字节解析实现

```c
bool proto_parse_byte(proto_parser_t *ps, uint8_t ch, proto_frame_t *out)
{
    if (ps == NULL || out == NULL) {
        return false;
    }

    switch (ps->state) {
    case ST_WAIT_HEAD1:
        if (ch == PROTO_HEAD1) {
            ps->state = ST_WAIT_HEAD2;
        }
        break;

    case ST_WAIT_HEAD2:
        if (ch == PROTO_HEAD2) {
            ps->state = ST_WAIT_LEN;
        } else if (ch == PROTO_HEAD1) {
            /* 允许 AA AA 55 这种情况下快速重同步 */
            ps->state = ST_WAIT_HEAD2;
        } else {
            ps->state = ST_WAIT_HEAD1;
            ps->sync_loss_cnt++;
        }
        break;

    case ST_WAIT_LEN:
        if (ch < PROTO_BODY_MIN || ch > PROTO_BODY_MAX) {
            ps->len_err_cnt++;
            proto_parser_reset(ps);
            break;
        }

        ps->len = ch;
        ps->payload_len = (uint8_t)(ch - PROTO_BODY_MIN);
        ps->payload_idx = 0u;
        ps->crc_calc = proto_crc8_maxim_update(0x00u, ch);
        ps->state = ST_WAIT_CMD;
        break;

    case ST_WAIT_CMD:
        ps->cmd = ch;
        ps->crc_calc = proto_crc8_maxim_update(ps->crc_calc, ch);
        ps->state = ST_WAIT_SEQ;
        break;

    case ST_WAIT_SEQ:
        ps->seq = ch;
        ps->crc_calc = proto_crc8_maxim_update(ps->crc_calc, ch);
        if (ps->payload_len == 0u) {
            ps->state = ST_WAIT_CRC;
        } else {
            ps->state = ST_WAIT_PAYLOAD;
        }
        break;

    case ST_WAIT_PAYLOAD:
        ps->payload[ps->payload_idx++] = ch;
        ps->crc_calc = proto_crc8_maxim_update(ps->crc_calc, ch);
        if (ps->payload_idx >= ps->payload_len) {
            ps->state = ST_WAIT_CRC;
        }
        break;

    case ST_WAIT_CRC:
        if (ch == ps->crc_calc) {
            out->cmd = ps->cmd;
            out->seq = ps->seq;
            out->payload_len = ps->payload_len;
            if (ps->payload_len > 0u) {
                memcpy(out->payload, ps->payload, ps->payload_len);
            }
            ps->rx_ok_cnt++;
            proto_parser_reset(ps);
            return true;
        }

        ps->crc_fail_cnt++;
        proto_parser_reset(ps);

        /* 收到 CRC 错误后，如果这个字节碰巧又是 HEAD1，允许直接转入下一轮同步 */
        if (ch == PROTO_HEAD1) {
            ps->state = ST_WAIT_HEAD2;
        }
        break;

    default:
        proto_parser_reset(ps);
        break;
    }

    return false;
}
```

### 7.4 这段状态机解决了什么工程问题

#### 问题 1：半包

比如你本来一帧 8 字节，这次只收到了前 5 字节。

状态机会停在某个中间状态，继续等待后续字节，不会误判成错误帧。

#### 问题 2：粘包

如果连续来了两帧，前一帧解析完成后，后面字节还会继续喂进状态机，继续解析下一帧。

#### 问题 3：错位和垃圾字节

如果缓冲区前面混进垃圾数据，状态机会持续等到 `AA 55`，重新同步。

#### 问题 4：CRC 错误

整帧校验失败就丢弃，并重新进入找帧头状态。

这就是自定义协议“稳定”的关键，不在于 if/else 多不多，而在于异常路径有没有闭环。

---

## 8. 应用层分发：协议层不要直接点灯

拿到一帧后，协议层的任务已经完成了。接下来进入 App 层。

```c
extern void board_led_set(uint8_t on);
extern uint8_t board_led_get(void);
extern uint16_t board_version_get(void);
extern size_t uart_drv_write_async(const uint8_t *buf, size_t len);

static bool app_send_frame(uint8_t cmd,
                           uint8_t seq,
                           const uint8_t *payload,
                           uint8_t payload_len)
{
    uint8_t frame[PROTO_FRAME_MAX];
    size_t n = proto_build_frame(cmd, seq, payload, payload_len, frame, sizeof(frame));
    if (n == 0u) {
        return false;
    }
    return (uart_drv_write_async(frame, n) == n);
}

static void app_send_error(uint8_t seq, uint8_t err)
{
    app_send_frame(CMD_ERROR_ACK, seq, &err, 1u);
}

void app_handle_frame(const proto_frame_t *f)
{
    uint8_t rsp[PROTO_PAYLOAD_MAX];

    if (f == NULL) {
        return;
    }

    switch (f->cmd) {
    case CMD_PING: {
        static const uint8_t pong[] = { 'P', 'O', 'N', 'G' };
        app_send_frame(CMD_PING_ACK, f->seq, pong, (uint8_t)sizeof(pong));
    } break;

    case CMD_SET_LED:
        if (f->payload_len != 1u) {
            app_send_error(f->seq, PROTO_ERR_LEN);
            break;
        }

        board_led_set(f->payload[0] ? 1u : 0u);
        rsp[0] = PROTO_ERR_OK;
        rsp[1] = board_led_get();
        app_send_frame(CMD_SET_LED_ACK, f->seq, rsp, 2u);
        break;

    case CMD_GET_VERSION:
        if (f->payload_len != 0u) {
            app_send_error(f->seq, PROTO_ERR_LEN);
            break;
        }

        rsp[0] = PROTO_ERR_OK;
        rsp[1] = (uint8_t)(board_version_get() >> 8);
        rsp[2] = (uint8_t)(board_version_get() & 0xFFu);
        app_send_frame(CMD_GET_VERSION_ACK, f->seq, rsp, 3u);
        break;

    default:
        app_send_error(f->seq, PROTO_ERR_CMD);
        break;
    }
}
```

### 8.1 为什么这里的结构很重要

- `proto_parse_byte()` 只负责“这是不是一帧”
- `app_handle_frame()` 才负责“这帧是什么意思”
- `app_send_frame()` 是业务层统一回包出口

这意味着后面你想扩命令时，只需要加：

- 新的 `CMD_xxx`
- `switch (cmd)` 分支
- 对应 payload 定义

不需要改 UART 中断、不需要改 CRC、不需要改组帧逻辑。

这就是良好分层的收益。

---

## 9. 把所有层串起来：协议任务轮询

下面这段代码是整个接收闭环真正跑起来的“桥”。

```c
extern bool uart_drv_read_byte(uint8_t *ch);

static proto_parser_t g_parser;

void uart_proto_service_init(void)
{
    memset(&g_parser, 0, sizeof(g_parser));
    proto_parser_reset(&g_parser);
}

void uart_proto_service_poll(void)
{
    uint8_t ch;
    proto_frame_t frame;

    while (uart_drv_read_byte(&ch)) {
        if (proto_parse_byte(&g_parser, ch, &frame)) {
            app_handle_frame(&frame);
        }
    }
}
```

### 9.1 这段代码应该在什么地方调用

两种常见方式：

#### 方式 A：裸机主循环

```c
int main(void)
{
    board_init();
    uart_drv_init();
    uart_proto_service_init();

    for (;;) {
        uart_proto_service_poll();
        app_background_task();
    }
}
```

#### 方式 B：FreeRTOS 任务

```c
void uart_proto_task(void *arg)
{
    (void)arg;
    uart_proto_service_init();

    for (;;) {
        uart_proto_service_poll();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
```

原则：

- 串口字节先进入缓冲区
- 解析在主循环或任务里完成
- 中断只搬运，不做协议和业务

---

## 10. Driver 层应该长什么样

Driver 层的关键，不是把串口寄存器写出来，而是形成两个清晰接口：

```c
void   uart_drv_init(void);
bool   uart_drv_read_byte(uint8_t *ch);
size_t uart_drv_write_async(const uint8_t *buf, size_t len);
```

它的内部通常是这样的：

### 10.1 RX 路径

```text
UART RXNE 中断
-> 读 DR
-> 写入 rx ring buffer
-> 退出中断
```

### 10.2 TX 路径

```text
业务调用 uart_drv_write_async()
-> 数据写入 tx ring buffer
-> 使能 TXE 中断
-> TXE 中断逐字节把 tx ring buffer 的数据写入 DR
-> 发送空了就关闭 TXE 中断
```

这套模式的本质是：

- 接收非阻塞
- 发送非阻塞
- 协议层和应用层都不碰硬件寄存器

这就是你后面要往 `DMA + IDLE` 升级时，依然能保持上层不动的基础。

---

## 11. 一条命令完整走一遍

以 `SET_LED` 为例，完整链路应当在脑子里形成这样的时序：

```text
PC -> MCU : AA 55 03 02 10 01 CRC
MCU UART 收到每个字节
-> ISR 把字节写入 rx_rb
-> uart_proto_service_poll() 从 rx_rb 取字节
-> proto_parse_byte() 完成解帧
-> 得到: cmd=0x02 seq=0x10 payload=[0x01]
-> app_handle_frame() 识别为 SET_LED
-> board_led_set(1)
-> 组织应答 payload=[0x00, 0x01]
-> proto_build_frame(0x82, 0x10, payload, 2)
-> uart_drv_write_async()
-> TXE 中断把应答逐字节发出去
PC <- MCU : AA 55 04 82 10 00 01 CRC
```

你只要能把这条链讲顺，说明你真的理解“UART 收到一帧后如何落到业务”。

---

## 12. C++ 怎么写更合适

如果工程允许用 C++，最推荐的是：

- Driver 和 Protocol 仍然保留 C 风格
- 用 C++ 做业务封装和状态组织

原因：

- Driver/Protocol 更强调可移植、可复用、无异常、低开销
- App 层更适合用类封装设备状态和业务接口

### 12.1 一个简洁的 C++ 包装方式

```cpp
class UartProtoService {
public:
    void poll()
    {
        uint8_t ch;
        proto_frame_t frame;

        while (uart_drv_read_byte(&ch)) {
            if (proto_parse_byte(&parser_, ch, &frame)) {
                handleFrame(frame);
            }
        }
    }

private:
    proto_parser_t parser_{};
    uint8_t led_on_ = 0;

    void handleFrame(const proto_frame_t &f)
    {
        switch (f.cmd) {
        case CMD_PING: {
            static const uint8_t pong[] = {'P', 'O', 'N', 'G'};
            sendFrame(CMD_PING_ACK, f.seq, pong, sizeof(pong));
        } break;

        case CMD_SET_LED:
            if (f.payload_len == 1u) {
                led_on_ = f.payload[0] ? 1u : 0u;
                board_led_set(led_on_);
                uint8_t rsp[2] = {PROTO_ERR_OK, led_on_};
                sendFrame(CMD_SET_LED_ACK, f.seq, rsp, 2);
            } else {
                uint8_t err = PROTO_ERR_LEN;
                sendFrame(CMD_ERROR_ACK, f.seq, &err, 1);
            }
            break;

        default: {
            uint8_t err = PROTO_ERR_CMD;
            sendFrame(CMD_ERROR_ACK, f.seq, &err, 1);
        } break;
        }
    }

    bool sendFrame(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint8_t len)
    {
        uint8_t frame[PROTO_FRAME_MAX];
        size_t n = proto_build_frame(cmd, seq, payload, len, frame, sizeof(frame));
        return (n > 0u) && (uart_drv_write_async(frame, n) == n);
    }
};
```

### 12.2 C++ 的收益在哪里

- 设备状态可封装到类里
- 不同协议服务更容易拆成对象
- 业务逻辑比全局变量版更清晰

但要注意：

- 不建议在底层驱动里大量用 STL 容器
- 不建议依赖异常处理
- 不建议让 ISR 直接进入复杂对象调用链

最稳的策略依然是：

```text
底层 C，应用层 C++
```

---

## 13. 如果升级到 DMA + IDLE，哪些代码不用动

这是很关键的工程认知。

如果你现在是“RXNE 中断逐字节 -> ring buffer -> 协议状态机”，后续想升级成“DMA + IDLE”，**协议层和应用层通常都不用动**。

变化只在 Driver 层：

### 13.1 原来是

```text
RXNE ISR 收 1 字节 -> 放 rx_rb
```

### 13.2 升级后变成

```text
DMA 循环接收一批字节
-> IDLE 中断判定当前批次结束
-> 把新增字节批量喂给 uart_proto_service_poll() 或直接喂 parser
```

也就是说，上层依然只是消费“字节流”，而不是关心它到底来自 RXNE 还是 DMA。

这就是好架构的标志：

- 驱动层变化
- 协议层基本不变
- 业务层完全不变

---

## 14. 真正容易踩坑的地方

### 14.1 在中断里直接做协议解析

后果：

- ISR 太长
- 高波特率时更容易丢字节
- 业务代码和中断耦合，后续很难维护

### 14.2 只靠帧尾分帧，不带长度

后果：

- payload 里一旦出现帧尾值，处理会复杂很多
- 不定长帧更难稳健解析

### 14.3 没有序号字段

后果：

- 请求和应答不好配对
- 重发或并发命令时很难排查

### 14.4 没有统一组帧接口

后果：

- 有的 ACK 漏加 CRC
- 有的命令格式偷偷不一致

### 14.5 没有错误码设计

后果：

- 上位机只知道“失败了”，不知道是长度错、参数错还是命令不支持

### 14.6 没有错位重同步能力

后果：

- 丢一个字节后，后面全流错位
- 只能依赖复位才能恢复

---

## 15. 设计 UART 自定义协议时，最推荐的 8 条工程规则

1. **帧边界一定明确**：帧头 + 长度 是最常用组合。
2. **必须有帧级校验**：至少 CRC8，复杂场景可 CRC16。
3. **请求和应答最好共用一套协议骨架**：这样最容易维护。
4. **建议带 `SEQ`**：排障、脚本测试、重发控制都更方便。
5. **中断只搬运**：协议解析和业务处理放到任务层。
6. **发送也要统一组帧**：不要到处散落手拼字节。
7. **错误码要标准化**：长度错、命令错、参数错分开。
8. **解析器要支持错位恢复**：否则线上极容易变脆弱。

---

## 16. 面试或项目里怎么把这件事讲清楚

你可以这样回答：

“如果 UART 想承载自定义协议，我会把它拆成驱动层、协议层、应用层三层。驱动层只负责中断或 DMA 收发和缓冲，不做协议解析；协议层负责根据帧头、长度和 CRC 从字节流中逐字节拼出完整帧，并支持半包、粘包和错位重同步；应用层根据命令字执行业务，并统一通过协议层组帧回包。这样收、发、解析、处理、应答就是一条完整闭环，后面从中断版升级到 DMA + IDLE 时，通常只需要改驱动层，上层基本不动。”

这就是“会做项目”的表达，不只是“会调串口 API”。

---

## 17. 关联资料

- 根文档：`D:\CodeRep\uart-detailed-prep.md`
- 相关笔记：`2026-03-19-001-interrupt-hands-on.md`
- 相关笔记：`2026-03-19-006-embedded-freertos-data-structures.md`
- 总路线图：`2026-03-20-001-embedded-learning-roadmap.md`

---

# 关键要点

1. UART 自定义协议的核心不是“收到了什么字节”，而是“怎样从字节流稳定恢复出业务帧”。
2. 最稳的工程分层是：`driver 收发`、`protocol 解包/组帧`、`app 处理业务`。
3. 帧头 + 长度 + CRC8 + 命令字 + 序号，是非常适合 MCU 的一套骨架。
4. 中断里只做搬运，主循环或任务里做解析，是稳定性的关键。
5. 真正的全流程必须包含：收字节、缓存、拼帧、校验、分发、回包、错误恢复。

---

# 实战建议

如果你要继续往下学，最适合接着补的两篇是：

1. `UART + DMA + IDLE` 不定长帧实战
2. `上位机 Python/Qt 与 MCU 自定义协议联调`
