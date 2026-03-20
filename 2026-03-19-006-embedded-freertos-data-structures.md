# 原始问题

嵌入式/FreeRTOS 的各种数据结构应用结合代码（定义、使用、分析），要求高知识密度。

---

# 嵌入式 + FreeRTOS 数据结构高密度实战手册

> 目标：不是“背定义”，而是让你在项目里能立刻选对结构、写对代码、讲清取舍。  
> 核心原则：实时系统优先“确定性”而不是理论最优复杂度。

---

## 0. 选型总原则（先看这个）

嵌入式/RTOS 中选数据结构优先级通常是：

1. 时间确定性（最坏时延是否可控）
2. 内存确定性（是否碎片化、是否可静态分配）
3. 并发边界清晰（ISR/任务共享是否可证明安全）
4. 代码复杂度可维护（排障成本）

一句话：**宁可 O(n) 但确定，也不要“平均快但最坏不可控”。**

---

## 1. 定长数组 + 结构体数组（设备对象管理基础）

## 1.1 定义

```c
typedef struct {
    uint8_t  in_use;
    uint8_t  id;
    uint32_t last_seen_ms;
    int16_t  value;
} sensor_node_t;

#define SENSOR_MAX 32
static sensor_node_t g_sensors[SENSOR_MAX];
```

## 1.2 使用

```c
sensor_node_t *sensor_get_or_alloc(uint8_t id, uint32_t now_ms)
{
    int free_idx = -1;
    for (int i = 0; i < SENSOR_MAX; ++i) {
        if (g_sensors[i].in_use && g_sensors[i].id == id) {
            g_sensors[i].last_seen_ms = now_ms;
            return &g_sensors[i];
        }
        if (!g_sensors[i].in_use && free_idx < 0) free_idx = i;
    }
    if (free_idx < 0) return NULL;
    g_sensors[free_idx].in_use = 1;
    g_sensors[free_idx].id = id;
    g_sensors[free_idx].last_seen_ms = now_ms;
    g_sensors[free_idx].value = 0;
    return &g_sensors[free_idx];
}
```

## 1.3 分析

1. 时间复杂度：查找 O(n)。
2. 空间：固定，零碎片，最适合长期运行系统。
3. 典型场景：设备表、连接表、任务上下文表。

---

## 2. 环形缓冲区 Ring Buffer（ISR->Task 流数据首选）

## 2.1 定义（SPSC：单生产者 ISR，单消费者任务）

```c
#include <stdint.h>
#include <stdbool.h>

#define RB_CAP 256u // 2^n

typedef struct {
    uint8_t data[RB_CAP];
    volatile uint16_t w;
    volatile uint16_t r;
    volatile uint32_t overflow;
} rb_t;

static inline uint16_t rb_next(uint16_t x) {
    return (uint16_t)((x + 1u) & (RB_CAP - 1u));
}
```

## 2.2 使用

```c
bool rb_push_isr(rb_t *rb, uint8_t ch)
{
    uint16_t n = rb_next(rb->w);
    if (n == rb->r) { rb->overflow++; return false; }
    rb->data[rb->w] = ch;
    rb->w = n;
    return true;
}

bool rb_pop_task(rb_t *rb, uint8_t *ch)
{
    if (rb->r == rb->w) return false;
    *ch = rb->data[rb->r];
    rb->r = rb_next(rb->r);
    return true;
}
```

## 2.3 分析

1. 时间复杂度：push/pop O(1)。
2. 内存：固定，零拷贝（相对队列拷贝模型开销更低）。
3. 注意：仅在 SPSC 前提下无锁安全，多生产者要加锁或改结构。

---

## 3. FreeRTOS Queue（任务间“消息对象”通道）

## 3.1 定义

```c
#include "FreeRTOS.h"
#include "queue.h"

typedef struct {
    uint16_t cmd;
    uint16_t len;
    uint8_t payload[32];
} msg_t;

static QueueHandle_t g_msg_q;
```

## 3.2 使用

```c
void app_queue_init(void)
{
    g_msg_q = xQueueCreate(16, sizeof(msg_t)); // 16 个消息槽
}

void producer_task(void *arg)
{
    msg_t m = {.cmd = 0x1001, .len = 1, .payload = {0x55}};
    for (;;) {
        (void)xQueueSend(g_msg_q, &m, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void consumer_task(void *arg)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(g_msg_q, &m, portMAX_DELAY) == pdPASS) {
            handle_msg(&m);
        }
    }
}
```

## 3.3 分析

1. 语义：拷贝消息，边界清晰，易维护。
2. 代价：每次 send/recv 都有拷贝，payload 大时成本上升。
3. 场景：控制命令、告警事件、小报文。

---

## 4. Queue + 内存池（大对象传指针，减少拷贝）

## 4.1 定义

```c
#define PKT_POOL_N 8
#define PKT_MAX    256

typedef struct {
    uint16_t len;
    uint8_t  buf[PKT_MAX];
} pkt_t;

static pkt_t g_pool[PKT_POOL_N];
static uint8_t g_used[PKT_POOL_N];
static QueueHandle_t g_pkt_q; // 队列里放 pkt_t*
```

## 4.2 使用

```c
pkt_t *pool_alloc(void)
{
    taskENTER_CRITICAL();
    for (int i = 0; i < PKT_POOL_N; ++i) {
        if (!g_used[i]) { g_used[i] = 1; taskEXIT_CRITICAL(); return &g_pool[i]; }
    }
    taskEXIT_CRITICAL();
    return NULL;
}

void pool_free(pkt_t *p)
{
    taskENTER_CRITICAL();
    int idx = (int)(p - g_pool);
    if (idx >= 0 && idx < PKT_POOL_N) g_used[idx] = 0;
    taskEXIT_CRITICAL();
}
```

## 4.3 分析

1. 相比 Queue 拷贝大对象，指针队列吞吐更高。
2. 关键风险是“所有权管理”混乱，必须定义谁分配谁释放。
3. 推荐给每个对象加 `owner` 注释和生命周期规则。

---

## 5. FreeRTOS StreamBuffer / MessageBuffer（字节流/变长消息）

## 5.1 StreamBuffer（适合 UART 字节流）

```c
#include "stream_buffer.h"

static StreamBufferHandle_t g_sb;

void sb_init(void)
{
    g_sb = xStreamBufferCreate(512, 1); // 触发阈值 1 字节
}

void uart_isr_feed(uint8_t ch)
{
    BaseType_t hpw = pdFALSE;
    xStreamBufferSendFromISR(g_sb, &ch, 1, &hpw);
    portYIELD_FROM_ISR(hpw);
}
```

## 5.2 MessageBuffer（带消息边界）

```c
#include "message_buffer.h"

static MessageBufferHandle_t g_mb;

void mb_init(void)
{
    g_mb = xMessageBufferCreate(1024);
}
```

## 5.3 分析

1. StreamBuffer：流式，适合协议层自己分帧。
2. MessageBuffer：内置长度前缀，适合变长消息。
3. 两者都常用于“单写单读”模型，超出模型要谨慎。

---

## 6. FreeRTOS EventGroup + 位图 Bitmap（状态聚合）

## 6.1 定义

```c
#include "event_groups.h"

#define EVT_NET_OK   (1u << 0)
#define EVT_SENSOR_OK (1u << 1)
#define EVT_CFG_OK   (1u << 2)

static EventGroupHandle_t g_evt;
```

## 6.2 使用

```c
void wait_all_ready(void)
{
    EventBits_t bits = xEventGroupWaitBits(
        g_evt,
        EVT_NET_OK | EVT_SENSOR_OK | EVT_CFG_OK,
        pdFALSE,  // 不自动清位
        pdTRUE,   // 等待全部位满足
        portMAX_DELAY
    );
    (void)bits;
}
```

## 6.3 分析

1. 复杂度 O(1) 位运算，表达力强。
2. 适合“系统就绪门控”“多子系统同步”。
3. 不适合承载大数据。

---

## 7. 链表（intrusive list）与定时任务管理

## 7.1 定义（侵入式节点，减少额外分配）

```c
typedef struct node {
    struct node *next;
    uint32_t due_ms;
    void (*cb)(void *arg);
    void *arg;
} node_t;

static node_t *g_timer_list = NULL;
```

## 7.2 使用（按到期时间有序插入）

```c
void timer_insert(node_t *n)
{
    node_t **pp = &g_timer_list;
    while (*pp && (*pp)->due_ms <= n->due_ms) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}
```

## 7.3 分析

1. 插入 O(n)，取最早到期 O(1)。
2. 节点可由静态池提供，避免 `malloc`。
3. 适合小规模定时任务列表（几十级别）。

---

## 8. 二叉堆（最小堆）做超时队列（规模大时更优）

## 8.1 定义

```c
typedef struct {
    uint32_t due_ms;
    uint16_t id;
} timer_item_t;

#define HEAP_CAP 128
static timer_item_t g_heap[HEAP_CAP];
static uint16_t g_hsz = 0;
```

## 8.2 核心操作（示意）

```c
void heap_push(timer_item_t x); // O(log n)
timer_item_t heap_pop_min(void); // O(log n)
timer_item_t heap_peek_min(void); // O(1)
```

## 8.3 分析

1. 比有序链表更适合大规模定时器。
2. 实现复杂度高于链表，调试成本更高。
3. 小项目通常直接用 FreeRTOS 软件定时器即可。

---

## 9. 哈希表（参数表/缓存索引）在嵌入式的克制使用

## 9.1 建议

1. 尺寸固定，装载因子可控。
2. 冲突处理选开地址法或拉链法。
3. 禁止运行中无限扩容。

## 9.2 分析

1. 平均 O(1)，最坏 O(n)。
2. 若最坏时延敏感，宁可用有界数组 + 二分/线性扫描。

---

## 10. 协议解析状态机表（“逻辑数据结构”）

把状态转移表当结构来设计，常比硬编码 if-else 更稳。

```c
typedef enum { S_HEAD, S_LEN, S_PAYLOAD, S_CRC } st_t;
typedef st_t (*step_fn)(uint8_t ch);

static st_t on_head(uint8_t ch);
static st_t on_len(uint8_t ch);
static st_t on_payload(uint8_t ch);
static st_t on_crc(uint8_t ch);

static step_fn g_tbl[] = { on_head, on_len, on_payload, on_crc };
```

分析：

1. 扩展性好，易做单元测试。
2. 比分散 `switch` 更容易做覆盖率和异常注入。

---

## 11. 复杂度 + 实时性速查表

1. 数组/结构体数组：查找 O(n)，内存固定，确定性强。
2. RingBuffer：push/pop O(1)，ISR 友好，流数据首选。
3. FreeRTOS Queue：O(1) 近似，拷贝语义，易维护。
4. Queue+Pool：O(1) 近似，高吞吐，需严格生命周期管理。
5. EventGroup/Bitmap：O(1) 位运算，适合状态同步。
6. 链表：插入/查找多为 O(n)，适合小规模动态集合。
7. 最小堆：push/pop O(log n)，适合大量定时项。

---

## 12. 选型决策树（项目里直接用）

1. 连续字节流 ISR->Task？
- 选 RingBuffer 或 StreamBuffer。

2. 任务间传小命令对象？
- 选 Queue（拷贝模型）。

3. 任务间传大块数据？
- 选 Queue 传指针 + 静态内存池。

4. 等待多个模块就绪？
- 选 EventGroup/Bitmap。

5. 管理大量到期事件？
- 选最小堆或 RTOS timer 机制。

---

## 13. 高频坑点与规避

1. 在 ISR 里用非 `FromISR` API。  
规避：所有 ISR 侧 API 单独包一层。

2. 队列深度拍脑袋。  
规避：按峰值速率和处理周期算容量：`depth >= burst_rate * worst_block_time`。

3. 动态内存碎片导致随机失败。  
规避：对象池 + 静态创建优先。

4. 多生产者无保护写 RingBuffer。  
规避：只保持 SPSC 或增加锁。

---

## 14. 一个可直接复用的“通信子系统”组合

推荐组合：

1. UART ISR -> RingBuffer（字节搬运）
2. `task_proto` -> 解析状态机（分帧/校验）
3. 业务事件 -> Queue（结构体命令）
4. 系统状态 -> EventGroup（就绪位/故障位）
5. 大包缓存 -> Static Pool（指针传递）

这是很多量产项目的稳定解法。

---

# 关键点速记

1. 数据结构选型首先服务于最坏时延和可预测性。
2. RTOS 对象是“并发语义结构”，不仅是容器。
3. 流数据与消息对象不要混用同一种结构硬扛。
4. 静态内存策略是长期稳定系统的朋友。
5. 任何结构都要配“观测指标”（溢出计数、峰值深度、延时）。

---

# 关联笔记

1. `2026-03-19-001-interrupt-hands-on.md`
2. `2026-03-19-002-freertos-hands-on.md`
3. `2026-03-19-005-embedded-full-process-with-freertos.md`
