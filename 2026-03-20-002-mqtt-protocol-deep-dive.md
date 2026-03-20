# 原始问题

详细讲解一下 MQTT 协议，最好是附带代码，高可读高知识密度。

---

# MQTT 协议高密度实战讲解

> 这篇以 **MQTT 3.1.1** 为主线，因为它在嵌入式设备、云平台 SDK、工业网关里最常见。
> MQTT 5.0 在此基础上增加了属性、原因码、会话过期、用户属性等增强能力，但骨架仍然是同一套。

---

## 0. 一句话先抓住本质

MQTT 不是“一个传字符串的接口”，而是一个建立在 **TCP 长连接** 之上的、面向 **发布/订阅** 模型的轻量级消息协议。

它的核心目标有 4 个：

1. **低带宽开销**
2. **弱网络下尽量稳定**
3. **异步解耦**
4. **适合设备长期在线**

所以它特别适合这些场景：

- 设备周期上报数据
- 云端下发控制命令
- 设备状态上线/离线通知
- 网关聚合多节点数据后上云

一句话理解通信路径：

```text
设备A  --publish-->  Broker  --forward-->  设备B/云服务/APP
设备B  --subscribe-> Broker  --match----->  Topic Filter
```

MQTT 里最重要的不是“谁连谁”，而是：

- 谁往哪个 `topic` 发布
- 谁订阅哪个 `topic filter`
- 消息以什么 `QoS` 交付
- 断线后会话是否保留

---

## 1. MQTT 通信模型

### 1.1 三个角色

1. **Client**
   任何接入 MQTT 的设备、APP、网关、服务端进程都可以是 client。

2. **Broker**
   中间服务器，负责接收消息、匹配主题、转发消息、维护会话、处理保活。

3. **Topic**
   主题是路由键，不是队列名。Broker 根据主题把消息路由给订阅者。

典型主题设计：

```text
factory/line1/plc01/temp
factory/line1/plc01/status
factory/line1/plc01/cmd
factory/line1/plc01/event
```

### 1.2 为什么它不是点对点协议

如果你用裸 TCP/UDP，发送方必须知道接收方是谁。
MQTT 不一样，发送方只管发到主题，接收方只管订阅主题，中间由 Broker 完成解耦。

这带来两个直接好处：

- 设备端逻辑简单，不需要维护大量对端连接关系
- 一条消息可以被多个订阅者同时接收

### 1.3 Topic 和 Topic Filter

- `topic`：实际发布时使用，必须是确定字符串
- `topic filter`：订阅时使用，可以带通配符

通配符规则：

- `+`：匹配单层
- `#`：匹配多层，且必须出现在最后一层

例如：

```text
订阅: factory/+/plc01/temp
匹配: factory/line1/plc01/temp
不匹配: factory/line1/plc01/status

订阅: factory/line1/#
匹配: factory/line1/plc01/temp
匹配: factory/line1/plc02/status
```

注意：

- 通配符只能用于订阅，不能用于发布主题
- 主题大小写敏感
- 主题层级是按 `/` 分割的

---

## 2. MQTT 为什么适合嵌入式

和 HTTP 相比，MQTT 更像“常驻连接上的消息总线”：

- 报文头短，控制报文非常轻
- 建好连接后，后续无需反复建链
- 支持心跳保活
- 支持离线会话
- 支持遗嘱消息
- 支持 QoS 语义

但它不是“天然省心”：

- 它依赖 TCP，所以你仍然要处理重连、超时、DNS、TLS
- 它的 QoS 不是魔法，设备端仍要维护消息状态
- 它很适合遥测和控制，但不适合大文件传输

嵌入式里常见选择：

- 高频遥测：`QoS 0`
- 关键控制命令：`QoS 1`
- 设备在线状态：`retain + will`
- 超低带宽链路：较大上报周期 + 较长 keep alive

---

## 3. MQTT 报文总体结构

每个 MQTT 控制报文都由最多 3 部分组成：

```text
+----------------+
| Fixed Header   |  必有
+----------------+
| Variable Header|  按报文类型决定
+----------------+
| Payload        |  按报文类型决定
+----------------+
```

### 3.1 固定头 Fixed Header

固定头至少 2 字节：

- 第 1 字节：报文类型 + 标志位
- 后续 1~4 字节：`Remaining Length`

第 1 字节格式：

```text
bit7 bit6 bit5 bit4 | bit3 bit2 bit1 bit0
报文类型             | 标志位
```

常见报文类型：

```text
1  CONNECT
2  CONNACK
3  PUBLISH
4  PUBACK
5  PUBREC
6  PUBREL
7  PUBCOMP
8  SUBSCRIBE
9  SUBACK
10 UNSUBSCRIBE
11 UNSUBACK
12 PINGREQ
13 PINGRESP
14 DISCONNECT
```

### 3.2 Remaining Length 编码

`Remaining Length` 表示“固定头后面还有多少字节”，采用变长编码，每字节低 7 位有效，高 1 位表示是否还有后续字节。

这点很重要，因为 MQTT 解析器第一步通常就是：

1. 读首字节判断报文类型
2. 解析 Remaining Length
3. 等待整包到齐后再进入具体分支处理

例如：

- 长度 `64` -> 一个字节就够
- 长度 `321` -> 需要多个字节编码

---

## 4. CONNECT / CONNACK：连接阶段必须吃透

### 4.1 连接建立流程

```text
Client -> Broker : TCP Connect
Client -> Broker : CONNECT
Broker -> Client : CONNACK
之后进入正常消息阶段
```

### 4.2 CONNECT 报文包含什么

CONNECT 的关键字段：

- 协议名：`MQTT`
- 协议级别：3.1.1 对应 `4`
- 连接标志 `Connect Flags`
- `Keep Alive`
- `Client ID`
- 可选 `Will Topic / Will Payload`
- 可选 `Username / Password`

`Connect Flags` 里最重要的是：

- `Clean Session`
- `Will Flag`
- `Will QoS`
- `Will Retain`
- `Username Flag`
- `Password Flag`

### 4.3 Clean Session 是什么

这是面试和项目里都非常高频的概念。

- `Clean Session = 1`
  表示这次连接是“临时会话”。断开后，Broker 不保留订阅和未完成状态。

- `Clean Session = 0`
  表示持久会话。Broker 可以保留订阅关系，以及离线期间积压的 QoS1/QoS2 消息。

工程理解：

- 传感器周期上报，且断线重连后重新订阅成本低，可以用 `Clean Session = 1`
- 需要稳定接收下行命令、不希望离线期间消息丢掉，可以考虑持久会话

### 4.4 CONNACK 需要看什么

CONNACK 主要看两件事：

- `Session Present`
- `Return Code`

如果返回码不是成功，就要根据原因决定是否重试、换账号、换 Client ID，还是放弃。

---

## 5. PUBLISH：真正干活的核心报文

### 5.1 PUBLISH 固定头标志位

PUBLISH 的首字节很关键：

```text
bit3 DUP
bit2~1 QoS
bit0 RETAIN
```

也就是说，一个 `PUBLISH` 报文里最容易混淆的 3 个概念其实都在这里：

- `DUP`
- `QoS`
- `RETAIN`

### 5.2 PUBLISH 的结构

```text
Fixed Header
Topic Name
[Packet Identifier]   // QoS1/QoS2 时才有
Payload
```

注意：

- `Packet Identifier` 只有在 QoS1/QoS2 时才存在
- QoS0 的 PUBLISH 没有包 ID
- 主题名是 UTF-8 字符串，前面带 2 字节长度

### 5.3 QoS 0 / 1 / 2 到底是什么意思

很多人把 QoS 理解成“消息一定到达多少次”，这个理解不完整。
更准确地说，它描述的是 **Client 和 Broker 之间这条链路的交付语义**。

#### QoS 0：At most once

```text
Client -> Broker : PUBLISH
```

- 不确认
- 最省资源
- 可能丢
- 适合高频遥测、可容忍偶发丢包的上报

#### QoS 1：At least once

```text
Client -> Broker : PUBLISH(packet_id=10)
Broker -> Client : PUBACK(packet_id=10)
```

- 至少一次
- 可能重复
- 发送方需要缓存未确认报文，超时重发时要置 `DUP=1`
- 接收方业务层要考虑去重或幂等

#### QoS 2：Exactly once

```text
Client -> Broker : PUBLISH(packet_id=10)
Broker -> Client : PUBREC(10)
Client -> Broker : PUBREL(10)
Broker -> Client : PUBCOMP(10)
```

- 语义最强
- 状态最多
- 代码最复杂
- 在资源受限设备里并不总是值得

工程结论：

- 大多数嵌入式场景，`QoS 1` 已经是主力
- `QoS 2` 只在“重复也不行，丢失也不行”的少数场景用
- `QoS 0` 非常常见，不要因为它“最低”就嫌弃它

### 5.4 QoS 不是端到端保证

这是非常容易答错的一点。

MQTT 的 QoS 是 **链路级语义**，至少先是“客户端到 Broker”这一段，Broker 再按订阅者的 QoS 继续交付。

也就是说：

- 发布者 `QoS 1`
- 订阅者订阅时请求 `QoS 0`

那么 Broker 发给该订阅者时仍可能按较低语义交付。

### 5.5 发送端视角：QoS 到底多了哪些状态

从设备代码角度看，QoS 的差异本质上是“你要额外维护多少状态”。

#### QoS 0 发送端

```text
应用生成消息 -> 编码 PUBLISH -> 发送 -> 结束
```

特点：

- 不分配 packet id
- 不进入 inflight 表
- 不等 ACK
- 最省 RAM、CPU、链路往返时间

代价：

- TCP 断开、缓存丢失、瞬时网络抖动时，消息可能直接丢掉

#### QoS 1 发送端

```text
应用生成消息
-> 分配 packet id
-> 编码 PUBLISH(QoS1)
-> 放入 inflight
-> 发送
-> 等待 PUBACK
-> 收到 PUBACK 后删除 inflight
```

如果超时未收到 `PUBACK`：

```text
重发同一 packet id 的 PUBLISH
并把 DUP 位置 1
```

这说明 QoS1 至少要解决 4 个工程问题：

1. `packet id` 分配与回收
2. inflight 表容量
3. 超时重发策略
4. 重复消息的业务幂等

#### QoS 2 发送端

```text
PUBLISH -> PUBREC -> PUBREL -> PUBCOMP
```

这意味着发送端至少要经历两个确认阶段，所以状态管理明显比 QoS1 更重。

如果你的设备 RAM 本来就紧、代码复杂度预算也有限，QoS2 往往不是第一选择。

### 5.6 接收端视角：为什么 QoS1 仍可能看到重复

很多人以为“我收到了 QoS1 消息，就只会处理一次”，这是错误的。

正确理解是：

- 发送端没收到 `PUBACK`，可能会重发
- 重发时 `DUP=1`
- 接收端即使已经处理过，也可能再次收到同一个 `packet id`

所以接收端有两条常见工程路线：

1. **业务幂等**
   比如“设置 LED 亮度为 80%”这种命令，重复执行不会出大问题。

2. **协议去重**
   记录最近处理过的 `packet id` 或业务序号，重复包直接丢弃。

嵌入式项目里，第一种通常更实用，因为它更简单、更稳。

### 5.7 `DUP`、`packet id`、重传三者的关系

这三个概念经常被混着记，建议直接分开：

- `packet id`
  是 QoS1/QoS2 报文的事务编号，用于把请求和确认对应起来。

- `DUP`
  表示这条 PUBLISH 可能是重发的，不代表它一定是重复业务消息。

- `重传`
  是发送方在超时未确认时重新发送同一事务。

要点：

- QoS0 没有 `packet id`
- QoS1/QoS2 才有 `packet id`
- 重发 QoS1 PUBLISH 时，通常要保留原 `packet id`
- 同一个连接中，未完成事务不能随意复用 `packet id`

这也是为什么 MQTT 客户端库通常都要维护一个“下一个 packet id”计数器。

### 5.8 QoS1 的典型超时重发框架

下面这段代码补上了 QoS1 里最关键的一步：定期扫描 inflight 表，超时就重发。

```c
typedef struct {
    uint16_t packet_id;
    uint32_t tick_sent;
    uint16_t pkt_len;
    uint8_t used;
    uint8_t dup;
    uint8_t pkt[128];
} mqtt_inflight_pkt_t;

static mqtt_inflight_pkt_t g_qos1[MQTT_INFLIGHT_MAX];

void mqtt_qos1_retry_scan(int sock, uint32_t now, uint32_t retry_ms)
{
    size_t i;

    for (i = 0; i < MQTT_INFLIGHT_MAX; ++i) {
        mqtt_inflight_pkt_t *m = &g_qos1[i];

        if (!m->used) {
            continue;
        }

        if ((now - m->tick_sent) < retry_ms) {
            continue;
        }

        m->pkt[0] |= 0x08u;      // PUBLISH 固定头的 DUP bit
        net_send_all(sock, m->pkt, m->pkt_len);
        m->tick_sent = now;
        m->dup = 1u;
    }
}
```

这里你要注意两个点：

- 重发的是“同一条事务”，不是重新分配一个新的 `packet id`
- DUP 只是告诉对端“这可能是重发”，不是帮你自动去重

### 5.9 QoS2 为什么复杂

QoS2 的复杂度，不只是多了两个报文，而是多了“两阶段完成”的状态持有成本。

你至少要记录：

- 这条事务现在是在 `等待 PUBREC`，还是 `等待 PUBCOMP`
- 是否已经把业务 payload 交付给上层
- 断线重连后未完成事务如何恢复

因此 QoS2 的问题不是“会不会写”，而是：

- 值不值得为它付出 RAM 和复杂度
- 你的业务是否真的需要严格一次

在很多设备侧项目里，更常见的做法是：

- 协议层用 QoS1
- 业务层加命令序号、事务号或幂等保护

这样通常比直接全量上 QoS2 更划算。

### 5.10 嵌入式里怎么选 QoS 才合理

可以按“消息丢了会怎样、重复了会怎样”来选：

#### 适合 QoS0 的消息

- 温度、电压、电流等高频遥测
- 周期状态刷新
- 可被下一帧覆盖的数据

核心判断：

- 丢一两帧通常无伤大雅
- 追求低延迟、低资源占用

#### 适合 QoS1 的消息

- 开关机命令
- 参数下发
- 固件升级控制指令
- 报警事件

核心判断：

- 不能轻易丢
- 即使偶尔重复，也可以通过幂等设计兜住

#### 什么时候才考虑 QoS2

- 重复执行代价极高
- 丢失也不可接受
- 设备和 Broker 两侧都能承受更复杂的状态机

如果你的项目还在早期，优先把 `QoS0 + QoS1` 用稳，通常收益最高。

### 5.11 一张表把 QoS 记牢

| QoS | 语义 | 额外报文 | 发送端状态成本 | 是否可能重复 | 典型场景 |
| --- | --- | --- | --- | --- | --- |
| 0 | At most once | 无 | 最低 | 否，但可能丢 | 高频遥测 |
| 1 | At least once | PUBACK | 中等 | 是 | 控制命令、报警 |
| 2 | Exactly once | PUBREC/PUBREL/PUBCOMP | 最高 | 协议层避免重复 | 极少数关键事务 |

---

## 6. SUBSCRIBE / SUBACK：下行控制的入口

### 6.1 为什么 SUBSCRIBE 的固定头不是随便写的

SUBSCRIBE 的报文类型是 `8`，但固定头低 4 位必须是 `0010`，所以首字节固定是 `0x82`。

这说明一点：

- 不是所有报文的 flags 都能自由组合
- 解析器通常会校验某些报文的固定 flags 是否符合协议

### 6.2 SUBSCRIBE Payload 结构

一个订阅报文可以一次带多个主题过滤器：

```text
Packet Identifier
Topic Filter 1 + Requested QoS
Topic Filter 2 + Requested QoS
...
```

SUBACK 会返回每个 topic filter 对应的授予 QoS 或失败码。

### 6.3 订阅 QoS 和发布 QoS 的关系

最终送到订阅者的 QoS，受两侧共同影响。常见工程理解可以记成：

- 发布者提出一个 QoS
- 订阅者请求一个 QoS
- Broker 按不高于两者交集的方式向订阅者投递

你不用背成标准条文，但一定要知道：

- 发布端 QoS 和订阅端 QoS 不是一回事
- 同一条消息发给不同订阅者，交付行为可能不同

---

## 7. Keep Alive、PINGREQ、Will、Retain、Session

这一组概念是 MQTT 最容易混成一团的地方。

### 7.1 Keep Alive

`Keep Alive` 是客户端承诺的“最长静默时间”。

意思是：

- 在这个时间内，客户端至少要发一个控制报文给 Broker
- 如果没有业务报文，也要发 `PINGREQ`
- Broker 回 `PINGRESP`

如果 Broker 在大约 `1.5 * Keep Alive` 时间内都没等到客户端任何控制报文，就可以判定连接失效并断开。

工程含义：

- Keep Alive 太短：流量增加，弱网下误判更多
- Keep Alive 太长：掉线发现更慢

常见取值：`30s`、`60s`、`120s`

### 7.2 Will Message 遗嘱消息

如果客户端异常断线，没有正常发 `DISCONNECT`，Broker 可以代替它发布一条预先声明好的消息，这就是遗嘱消息。

典型用途：

```text
topic: device/123/status
payload: offline
retain: true
```

这样设备异常掉线时，其他订阅者就能及时知道它离线了。

### 7.3 Retained Message 保留消息

Retain 的意义不是“消息长期缓存很多条”，而是：

- Broker 为某个 topic **只保留最后一条 retain 消息**
- 新订阅者一订阅，就能立刻收到这条“最近状态”

很适合做：

- 设备在线状态
- 最近一次配置状态
- 当前开关量状态

一个常见细节：

- 向某个 retain topic 发送 **零长度 payload**，通常用于清除该主题的保留消息

### 7.4 Persistent Session 持久会话

持久会话解决的是：

- 设备离线时，Broker 是否还要记住它的订阅和待确认消息

它和 retain 不是一回事：

- `retain` 针对“主题上的最后状态”
- `session` 针对“这个客户端的连接上下文”

很多人把这两个概念混掉，后面项目就会出现“为什么重连后没收到命令”的问题。

---

## 8. 你在嵌入式里真正要写的，不是协议，而是状态机

站在 MCU/RTOS 视角，MQTT 客户端本质上是这几个模块：

1. **网络层**：TCP/TLS 连接、发送、接收、超时
2. **编解码层**：MQTT 报文打包/解包
3. **会话层**：连接状态、重连、订阅恢复、keep alive
4. **消息层**：发布队列、订阅回调、inflight 管理
5. **业务层**：把传感器数据映射到 topic，把控制命令映射到执行动作

建议架构：

```text
sensor task ----> publish queue ---->
                                  mqtt task ----> socket/tls
control callback <---- rx parser <---
```

为什么推荐把 MQTT 收发集中在一个任务里：

- 简化 socket 并发问题
- 简化 inflight 状态维护
- 统一处理重连和重发
- 方便把 ISR 和 MQTT 解耦

原则非常重要：

- ISR 不直接操作 MQTT socket
- ISR 只采集事件或数据
- 由任务把数据整理后再 publish

---

## 9. 代码一：最小 MQTT 编码工具

下面这段代码不是完整 SDK，而是让你吃透 MQTT 报文编码核心。

```c
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t keep_alive_s;
    bool clean_session;
    bool will_enable;
    uint8_t will_qos;
    bool will_retain;
    const char *will_topic;
    const void *will_payload;
    uint16_t will_len;
} mqtt_connect_cfg_t;

static uint8_t *mqtt_write_u16(uint8_t *p, uint16_t v)
{
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)(v & 0xFFu);
    return p;
}

static uint8_t *mqtt_write_bytes(uint8_t *p, const void *buf, size_t len)
{
    memcpy(p, buf, len);
    return p + len;
}

static uint8_t *mqtt_write_utf8(uint8_t *p, const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    p = mqtt_write_u16(p, len);
    return mqtt_write_bytes(p, s, len);
}

static size_t mqtt_encode_remaining_length(uint32_t len, uint8_t out[4])
{
    size_t i = 0;

    do {
        uint8_t byte = (uint8_t)(len % 128u);
        len /= 128u;
        if (len > 0u) {
            byte |= 0x80u;
        }
        out[i++] = byte;
    } while ((len > 0u) && (i < 4u));

    return i;
}

size_t mqtt_build_connect(uint8_t *out, size_t cap, const mqtt_connect_cfg_t *cfg)
{
    uint8_t flags = 0;
    uint32_t payload_len;
    uint32_t remaining_len;
    uint8_t rl[4];
    size_t rl_len;
    uint8_t *p;

    if (cfg == NULL || cfg->client_id == NULL) {
        return 0;
    }

    if (cfg->clean_session) {
        flags |= 0x02u;
    }

    if (cfg->will_enable) {
        flags |= 0x04u;
        flags |= (uint8_t)((cfg->will_qos & 0x03u) << 3);
        if (cfg->will_retain) {
            flags |= 0x20u;
        }
    }

    if (cfg->password != NULL) {
        flags |= 0x40u;
    }

    if (cfg->username != NULL) {
        flags |= 0x80u;
    }

    payload_len = 2u + (uint32_t)strlen(cfg->client_id);

    if (cfg->will_enable) {
        payload_len += 2u + (uint32_t)strlen(cfg->will_topic);
        payload_len += 2u + cfg->will_len;
    }
    if (cfg->username != NULL) {
        payload_len += 2u + (uint32_t)strlen(cfg->username);
    }
    if (cfg->password != NULL) {
        payload_len += 2u + (uint32_t)strlen(cfg->password);
    }

    remaining_len = 10u + payload_len;
    rl_len = mqtt_encode_remaining_length(remaining_len, rl);

    if (cap < (1u + rl_len + remaining_len)) {
        return 0;
    }

    *out++ = 0x10u;                       // CONNECT
    memcpy(out, rl, rl_len);
    out += rl_len;

    p = out;
    p = mqtt_write_utf8(p, "MQTT");       // Protocol Name
    *p++ = 0x04u;                         // Protocol Level 4 => MQTT 3.1.1
    *p++ = flags;                         // Connect Flags
    p = mqtt_write_u16(p, cfg->keep_alive_s);

    p = mqtt_write_utf8(p, cfg->client_id);

    if (cfg->will_enable) {
        p = mqtt_write_utf8(p, cfg->will_topic);
        p = mqtt_write_u16(p, cfg->will_len);
        p = mqtt_write_bytes(p, cfg->will_payload, cfg->will_len);
    }
    if (cfg->username != NULL) {
        p = mqtt_write_utf8(p, cfg->username);
    }
    if (cfg->password != NULL) {
        p = mqtt_write_utf8(p, cfg->password);
    }

    return (size_t)(p - (out - rl_len - 1u));
}
```

这段代码你要学到的不是“抄过去能跑”，而是这几个关键点：

- MQTT 字符串前面都有 2 字节长度
- `Remaining Length` 是变长编码
- CONNECT 的 variable header 固定部分长度是固定的
- Will/用户名/密码是否存在，是由 flags 和 payload 一起决定的

---

## 10. 代码二：构造一个最小 PUBLISH 报文

下面是 QoS0 的最小发布报文构造函数。

```c
size_t mqtt_build_publish_qos0(uint8_t *out,
                               size_t cap,
                               const char *topic,
                               const void *payload,
                               uint16_t payload_len,
                               bool retain)
{
    uint16_t topic_len;
    uint32_t remaining_len;
    uint8_t rl[4];
    size_t rl_len;
    uint8_t *p;

    if (out == NULL || topic == NULL || (payload == NULL && payload_len != 0u)) {
        return 0;
    }

    topic_len = (uint16_t)strlen(topic);
    remaining_len = 2u + topic_len + payload_len;
    rl_len = mqtt_encode_remaining_length(remaining_len, rl);

    if (cap < (1u + rl_len + remaining_len)) {
        return 0;
    }

    *out++ = (uint8_t)(0x30u | (retain ? 0x01u : 0x00u));
    memcpy(out, rl, rl_len);
    out += rl_len;

    p = out;
    p = mqtt_write_utf8(p, topic);
    p = mqtt_write_bytes(p, payload, payload_len);

    return (size_t)(p - (out - rl_len - 1u));
}
```

如果你要扩展到 QoS1，需要新增：

- 固定头里设置 QoS bits
- variable header 中插入 `packet identifier`
- 发送后把该消息放入 inflight 表等待 `PUBACK`

---

## 11. 代码三：解析固定头，拿到报文类型和包长

嵌入式里做 MQTT 解析，通常不会一上来就解析所有字段，而是先把固定头读出来。

```c
typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t remaining_len;
    uint8_t header_len;
} mqtt_fixed_header_t;

bool mqtt_parse_fixed_header(const uint8_t *buf,
                             size_t len,
                             mqtt_fixed_header_t *hdr)
{
    uint32_t multiplier = 1u;
    uint32_t value = 0u;
    size_t i;

    if (buf == NULL || hdr == NULL || len < 2u) {
        return false;
    }

    hdr->type = (uint8_t)(buf[0] >> 4);
    hdr->flags = (uint8_t)(buf[0] & 0x0Fu);

    for (i = 1u; i < len && i <= 4u; ++i) {
        value += (uint32_t)(buf[i] & 0x7Fu) * multiplier;
        if ((buf[i] & 0x80u) == 0u) {
            hdr->remaining_len = value;
            hdr->header_len = (uint8_t)(i + 1u);
            return true;
        }
        multiplier *= 128u;
    }

    return false;
}
```

典型接收流程：

1. 从 socket 收到若干字节
2. 先解析固定头
3. 计算整包长度 `header_len + remaining_len`
4. 如果没收齐，就继续等
5. 收齐后再按 `type` 分支处理

这就是为什么 MQTT 解析器通常需要一个接收缓冲区，而不是“读一字节处理一字节”。

---

## 12. 代码四：QoS1 的最小 inflight 思路

QoS1 的关键不是发包，而是“等待确认前不能忘记它”。

```c
#define MQTT_INFLIGHT_MAX 8

typedef struct {
    uint16_t packet_id;
    uint32_t tick_sent;
    uint8_t used;
    uint8_t dup;
} mqtt_inflight_t;

static mqtt_inflight_t g_inflight[MQTT_INFLIGHT_MAX];

static mqtt_inflight_t *mqtt_inflight_alloc(uint16_t packet_id, uint32_t now)
{
    size_t i;
    for (i = 0; i < MQTT_INFLIGHT_MAX; ++i) {
        if (!g_inflight[i].used) {
            g_inflight[i].used = 1u;
            g_inflight[i].packet_id = packet_id;
            g_inflight[i].tick_sent = now;
            g_inflight[i].dup = 0u;
            return &g_inflight[i];
        }
    }
    return NULL;
}

static void mqtt_inflight_ack(uint16_t packet_id)
{
    size_t i;
    for (i = 0; i < MQTT_INFLIGHT_MAX; ++i) {
        if (g_inflight[i].used && g_inflight[i].packet_id == packet_id) {
            g_inflight[i].used = 0u;
            return;
        }
    }
}
```

你在项目里通常还要继续补上：

- 超时扫描
- 重发时置 `DUP=1`
- 达到最大重试次数后断线重连
- 持久会话下的消息恢复策略

这就是为什么 `QoS 1` 虽然常用，但并不是“只多一个 PUBACK 那么简单”。

---

## 13. 代码五：FreeRTOS 风格的 MQTT 任务骨架

下面这段不是完整实现，但它反映了最接近项目真实形态的组织方式。

```c
typedef enum {
    MQTT_ST_DOWN = 0,
    MQTT_ST_TCP_CONNECTED,
    MQTT_ST_WAIT_CONNACK,
    MQTT_ST_ONLINE
} mqtt_state_t;

typedef struct {
    int sock;
    mqtt_state_t state;
    uint32_t last_tx_tick;
    uint32_t last_rx_tick;
    uint32_t keep_alive_ms;
} mqtt_client_t;

void mqtt_task(void *arg)
{
    mqtt_client_t *c = (mqtt_client_t *)arg;
    uint8_t rx_buf[512];
    uint8_t tx_buf[256];

    for (;;) {
        switch (c->state) {
        case MQTT_ST_DOWN:
            c->sock = net_connect_broker();
            if (c->sock >= 0) {
                size_t n = mqtt_build_connect(tx_buf, sizeof(tx_buf), &g_mqtt_cfg);
                if (n > 0 && net_send_all(c->sock, tx_buf, n)) {
                    c->state = MQTT_ST_WAIT_CONNACK;
                    c->last_tx_tick = os_tick_ms();
                } else {
                    net_close(c->sock);
                }
            }
            os_delay_ms(1000);
            break;

        case MQTT_ST_WAIT_CONNACK:
            if (net_recv_packet(c->sock, rx_buf, sizeof(rx_buf)) > 0) {
                if (mqtt_is_connack_ok(rx_buf)) {
                    mqtt_send_initial_subscriptions(c->sock);
                    c->state = MQTT_ST_ONLINE;
                    c->last_rx_tick = os_tick_ms();
                } else {
                    net_close(c->sock);
                    c->state = MQTT_ST_DOWN;
                }
            }
            break;

        case MQTT_ST_ONLINE:
            if (net_poll_readable(c->sock, 10)) {
                int n = net_recv_packet(c->sock, rx_buf, sizeof(rx_buf));
                if (n <= 0) {
                    net_close(c->sock);
                    c->state = MQTT_ST_DOWN;
                    break;
                }

                c->last_rx_tick = os_tick_ms();
                mqtt_process_one_packet(c, rx_buf, (size_t)n);
            }

            while (publish_queue_try_pop(&g_pub_queue, &g_pub_msg)) {
                size_t n = mqtt_build_publish_qos0(tx_buf,
                                                   sizeof(tx_buf),
                                                   g_pub_msg.topic,
                                                   g_pub_msg.payload,
                                                   g_pub_msg.len,
                                                   false);
                if (n > 0 && net_send_all(c->sock, tx_buf, n)) {
                    c->last_tx_tick = os_tick_ms();
                }
            }

            if ((os_tick_ms() - c->last_tx_tick) >= c->keep_alive_ms) {
                mqtt_send_pingreq(c->sock);
                c->last_tx_tick = os_tick_ms();
            }
            break;

        default:
            c->state = MQTT_ST_DOWN;
            break;
        }
    }
}
```

这段骨架背后的设计思想是：

- 一个任务统一管理 MQTT 状态
- 网络接收、协议处理、保活发送都在同一个状态机里完成
- 业务任务只往 `publish queue` 塞消息，不直接碰 socket
- 一旦断线，统一回到 `DOWN` 状态重连

这就是“项目可维护”的写法。

---

## 14. 嵌入式项目中最常见的 MQTT 设计范式

### 14.1 状态主题

```text
device/001/status -> online/offline
```

推荐：

- 上线后主动 publish `online`
- retain = true
- will = `offline`
- will retain = true

效果：

- 新订阅者一上来就知道设备当前在线状态
- 异常断线时 Broker 会自动把状态切成 `offline`

### 14.2 遥测主题

```text
device/001/telemetry
```

通常：

- `QoS 0`
- 周期上报
- payload 用 JSON、二进制 TLV、或私有紧凑格式

### 14.3 控制主题

```text
device/001/cmd
```

通常：

- 订阅 `cmd`
- 云端发布命令
- 设备执行后再往 `event` 或 `ack` 主题回应

### 14.4 事件主题

```text
device/001/event
```

用于：

- 故障上报
- 参数变更结果
- 命令执行结果

---

## 15. MQTT 和串口协议的思维差别

如果你之前更熟悉 UART / Modbus 一类协议，MQTT 的思维方式要切换一下。

串口协议更像：

- 一问一答
- 帧格式固定
- 对端明确
- 链路局部封闭

MQTT 更像：

- 主题路由
- 多方异步
- 会话和状态长期存在
- Broker 是中心节点

所以你不能把 MQTT 仅仅当成“把串口帧搬到云上”。
你要学会设计：

- 主题树
- 消息语义
- 幂等逻辑
- 离线恢复策略

---

## 16. 高频易错点

### 16.1 误把 retain 当离线消息队列

错。
retain 只保留某个主题的“最后一条状态”。它不是帮你存一堆历史消息。

### 16.2 误把 QoS1 当“绝对不重复”

错。
QoS1 是至少一次，重复是允许的，所以业务层最好设计成幂等。

### 16.3 断线重连后忘记重新订阅

如果 `Clean Session = 1`，重连后以前订阅通常就没了，必须重新订阅。

### 16.4 忘记处理半包/粘包

TCP 是字节流，不保证一次 `recv()` 就是一整包 MQTT 报文。
必须按 Remaining Length 组包。

### 16.5 在 ISR 里直接 publish

非常不建议。
ISR 应只记事件或搬数据，再由任务 publish。

### 16.6 一个 Client ID 被多个设备复用

Broker 会认为它们是同一个客户端，后连上的可能把前一个顶掉。

### 16.7 QoS2 设计过重

很多 MCU 项目根本不需要 QoS2，硬上只会放大 RAM、状态和调试成本。

---

## 17. 选型建议：到底怎么配最实用

如果你做的是普通 IoT / 工业采集设备，通常可以这么配：

- 周期遥测：`QoS 0`
- 命令下发：`QoS 1`
- 在线状态：`retain + will`
- keep alive：`60s`
- clean session：
  取决于你是否需要离线期间保留订阅和未达消息

如果设备资源比较紧：

- 控制 inflight 数量
- 限制订阅数
- 限制 topic 长度
- 少用大 JSON，必要时改紧凑二进制格式
- TLS 要评估 RAM、证书、握手耗时和 RTC 校时问题

---

## 18. 你需要牢牢记住的 10 句话

1. MQTT 建立在 TCP 长连接之上。
2. Broker 是中心，主题是路由键。
3. 发布主题和订阅过滤器不是一回事。
4. QoS 描述的是交付语义，不是“永不丢永不重”。
5. QoS1 常用，QoS2 昂贵。
6. retain 保存的是某主题最后状态，不是消息队列。
7. will 解决的是异常断线通知。
8. clean session 决定断线后会话是否保留。
9. MQTT 解析必须考虑 TCP 半包与粘包。
10. 嵌入式里真正难的不是发报文，而是状态机、重连和可靠性处理。

---

## 19. 关联你当前学习路线的下一步

这篇学完后，你最适合继续深化的相关主题是：

1. `socket/tcp` 基础
2. `lwIP` 或网络栈收发模型
3. `FreeRTOS + MQTT` 任务划分
4. `ring buffer` 与网络接收缓存
5. `ota` 与 MQTT 命令通道设计

如果你后面要继续学，我建议下一篇可以直接写：

- `mqtt + freertos 实战架构`
- `mqtt 报文逐字节拆解`
- `lwip socket + mqtt 客户端最小实现`
- `mqtt 在嵌入式中的断线重连与保活策略`

---

# 关键要点

1. MQTT 的核心不是接口调用，而是 Broker + Topic + Session + QoS 这套消息语义。
2. 对嵌入式最重要的是：PUBLISH、SUBSCRIBE、Keep Alive、Will、Retain、Clean Session。
3. 项目里最难的是半包处理、重连状态机、QoS1 inflight 管理，而不是单次收发。
4. 大多数设备场景下，`QoS 0 遥测 + QoS 1 控制 + retain/will 状态主题` 是很实用的组合。
5. 真正要把 MQTT 用稳，必须把协议和 RTOS 任务架构一起设计。

---

# 实战建议

如果你希望我继续帮你把这一块做成完整学习链，我下一步最建议补这两篇：

1. `MQTT + FreeRTOS + lwIP` 最小项目骨架
2. `MQTT 报文逐字节分析`，把 CONNECT / CONNACK / PUBLISH / SUBSCRIBE 每个字段全拆开

---

# 关联笔记

- `2026-03-19-001-interrupt-hands-on.md`
- `2026-03-19-002-freertos-hands-on.md`
- `2026-03-19-006-embedded-freertos-data-structures.md`
- `2026-03-20-001-embedded-learning-roadmap.md`

