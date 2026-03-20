# 原始问题

在我的项目学习路径里，想系统学习嵌入式中断知识，并结合代码达到“能直接上手做项目”的程度。

---

# 组织化讲解（高密度 + 可落地）

## 0. 先建立一个正确认知

中断不是“某个函数被调用”，而是 **硬件异步抢占当前执行流**。  
你要掌握的不是 API，而是这 4 层：

1. 触发层：外设/引脚/定时器如何产生中断请求。
2. 仲裁层：NVIC 如何按优先级决定谁先执行。
3. 执行层：ISR（中断服务程序）如何快速、正确地处理。
4. 协作层：ISR 和主循环/任务如何安全共享数据。

一句话：**中断=异步事件驱动 + 实时约束 + 并发一致性**。

---

## 1. 底层机制（必须吃透）

## 1.1 中断响应路径（以 Cortex-M 为例）

1. 外设置位 `pending`（比如 UART RXNE、EXTI line、TIM update）。
2. NVIC 检查该中断是否 `enable`，并比较优先级。
3. CPU 保存现场（硬件自动压栈核心寄存器）。
4. 跳到向量表对应 ISR。
5. ISR 执行完成后 `exception return` 恢复现场。

关键点：

- 不是所有 pending 都会立刻执行，优先级更高的会先跑。
- ISR 时间越长，系统抖动越大，低优先级中断越容易饿死。

## 1.2 中断优先级你必须讲清楚的 3 件事

1. **数值越小优先级越高**（Cortex-M 常见规则）。
2. 有“抢占优先级”和“响应顺序（子优先级）”的概念（具体看分组配置）。
3. 优先级位数由 `__NVIC_PRIO_BITS` 决定，不同芯片不是固定 4 位。

常见坑：

- 以为 `0` 是最低优先级，结果系统行为反了。
- 不看优先级分组，导致“能否抢占”的理解错误。

## 1.3 中断延迟来源

中断响应延迟通常来自：

1. 当前正在关中断（临界区太长）。
2. 更高优先级 ISR 正在运行。
3. Flash wait states、总线占用、缓存行为。
4. ISR 里做了重活（解析协议/打印日志/阻塞等待）。

工程目标不是“0 延迟”，而是 **可预测延迟上界**。

---

## 2. ISR 设计铁律（项目稳定分水岭）

## 2.1 ISR 里应该做什么

1. 快速读取硬件状态。
2. 清中断标志（按芯片手册要求顺序）。
3. 把数据搬到缓冲区或置事件标志。
4. 立即退出。

## 2.2 ISR 里不要做什么

1. 不做复杂协议解析。
2. 不做动态内存分配 `malloc/free`。
3. 不做阻塞等待（轮询某个硬件 ready）。
4. 不做大段 `printf`（很容易导致时序灾难）。

## 2.3 可复用模板：ISR 最小化

```c
volatile uint8_t g_uart_rx_event = 0;

void USARTx_IRQHandler(void)
{
    uint32_t sr = USARTx->SR;

    if (sr & USART_SR_RXNE) {
        uint8_t ch = (uint8_t)USARTx->DR;   // 读数据寄存器
        rb_push_isr(&g_uart_rb, ch);        // 只搬数据
        g_uart_rx_event = 1;                // 置标志
    }

    if (sr & USART_SR_ORE) {
        (void)USARTx->DR;                   // 按手册清错误
        g_uart_overrun_cnt++;
    }
}
```

---

## 3. 并发与共享数据（最容易出隐蔽 bug）

## 3.1 `volatile` 的边界

`volatile` 只保证“每次都真实读写内存/寄存器”，**不保证原子性和互斥**。

错误认知：加了 `volatile` 就线程安全。  
正确做法：根据访问宽度和并发场景决定是否加临界区或原子操作。

## 3.2 临界区最小封装（裸机通用思路）

```c
uint32_t irq_save(void);      // 保存中断状态并关中断（平台实现）
void irq_restore(uint32_t s); // 恢复中断状态（平台实现）

void shared_counter_inc(volatile uint32_t *cnt)
{
    uint32_t s = irq_save();
    (*cnt)++;
    irq_restore(s);
}
```

原则：**临界区越短越好，只保护必要读改写**。

## 3.3 ISR->主循环的首选数据通道：SPSC 环形缓冲区

```c
#define RB_CAP 128u  // 2 的幂

typedef struct {
    uint8_t data[RB_CAP];
    volatile uint16_t w; // ISR 写
    volatile uint16_t r; // 主循环读
    volatile uint32_t overflow;
} rb_t;

static inline uint16_t rb_next(uint16_t x) {
    return (uint16_t)((x + 1u) & (RB_CAP - 1u));
}

bool rb_push_isr(rb_t *rb, uint8_t v)
{
    uint16_t n = rb_next(rb->w);
    if (n == rb->r) {
        rb->overflow++;
        return false;
    }
    rb->data[rb->w] = v;
    rb->w = n;
    return true;
}

bool rb_pop(rb_t *rb, uint8_t *v)
{
    if (rb->r == rb->w) return false;
    *v = rb->data[rb->r];
    rb->r = rb_next(rb->r);
    return true;
}
```

这套结构是中断项目的“地基代码”，UART/SPI/CAN 都可套用。

---

## 4. 三个可直接上手的实战模块

## 4.1 模块 A：SysTick 毫秒时基

用途：超时控制、软件定时、去抖。

```c
volatile uint32_t g_ms = 0;

void SysTick_Handler(void)
{
    g_ms++;
}

uint32_t millis(void)
{
    return g_ms; // 32 位 MCU 读 32 位通常是原子的
}
```

如果 MCU 位宽较小，读 `g_ms` 需要短临界区保护。

## 4.2 模块 B：按键 EXTI + 软件去抖

```c
volatile uint32_t g_btn_irq_ts = 0;
volatile uint8_t  g_btn_event = 0;

void EXTI15_10_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PR13) {
        EXTI->PR = EXTI_PR_PR13;  // 写 1 清 pending（示例）

        uint32_t now = millis();
        if ((now - g_btn_irq_ts) > 20u) { // 20ms 去抖窗口
            g_btn_irq_ts = now;
            g_btn_event = 1;
        }
    }
}

void app_poll(void)
{
    if (g_btn_event) {
        g_btn_event = 0;
        led_toggle();
    }
}
```

重点：去抖逻辑尽量轻量，不要在中断里做业务状态机。

## 4.3 模块 C：UART RX 中断 + 主循环解析

```c
rb_t g_uart_rb;

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;
    if (sr & USART_SR_RXNE) {
        uint8_t ch = (uint8_t)USART1->DR;
        (void)rb_push_isr(&g_uart_rb, ch);
    }
}

void uart_process_task(void)
{
    uint8_t ch;
    while (rb_pop(&g_uart_rb, &ch)) {
        proto_feed(ch); // 协议解析放主循环/任务
    }
}
```

这就是你在项目里最常见的“快中断 + 慢处理”架构。

---

## 5. RTOS 场景（必须知道 ISR API 边界）

如果用 FreeRTOS：

1. ISR 里只能调用 `FromISR` 后缀 API。
2. 通过 `xQueueSendFromISR` / `vTaskNotifyGiveFromISR` 把事件扔给任务。
3. 用 `portYIELD_FROM_ISR()` 在必要时触发任务切换。

示例：

```c
void USART2_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t ch;

    if (uart_read_byte_isr(&ch)) {
        xQueueSendFromISR(g_uart_queue, &ch, &xHigherPriorityTaskWoken);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

不要在 ISR 里用普通 `xQueueSend`，这会直接踩系统边界。

---

## 6. 中断调试与定位（工程能力核心）

## 6.1 必备观测量

1. 每路中断计数器（`irq_cnt_xxx`）。
2. ISR 最大执行时间（GPIO 拉高拉低 + 逻辑分析仪）。
3. 缓冲区溢出计数（`overflow_cnt`）。
4. 错误中断计数（ORE/FE/PE 等）。

## 6.2 现场定位套路

1. 怀疑丢数：先看 ISR 计数与上层处理计数是否匹配。
2. 怀疑优先级问题：拉出 NVIC 优先级表做全局审查。
3. 怀疑时序问题：测 ISR 执行时间分布而不是只看平均值。
4. 怀疑死锁：检查是否在 ISR 里调用了阻塞 API。

---

## 7. 7 天上手计划（按这个做，能独立干活）

## Day 1：中断基础和向量表

1. 画出“触发 -> NVIC -> ISR -> 返回”的时序图。
2. 跑通 `SysTick_Handler`，实现 `millis()`。

验收：`millis()` 每秒增长约 1000。

## Day 2：EXTI 外部中断

1. 配好一个按键上升/下降沿中断。
2. 实现 20ms 去抖。

验收：连续按压不误触发、不连跳。

## Day 3：UART RX 中断 + 环形缓冲区

1. ISR 收字节进 rb。
2. 主循环做 echo 和基础协议解析。

验收：高于 115200bps 连续输入不丢包。

## Day 4：中断优先级管理

1. 加一个高频定时器中断。
2. 调整 UART/Timer 优先级，观察抖动与丢包变化。

验收：能解释“为什么这样配优先级”。

## Day 5：并发保护

1. 给共享计数和状态加最小临界区。
2. 增加 overflow/error 统计。

验收：压测 30 分钟无异常增长。

## Day 6：RTOS 迁移（可选）

1. 把 ISR 数据出口改成 Queue/Notify。
2. 比较裸机和 RTOS 实现差异。

验收：任务抢占后系统行为稳定一致。

## Day 7：故障注入与复盘

1. 人为增加 ISR 延时，观察系统退化路径。
2. 输出中断设计复盘文档（优先级表 + 风险点 + 改进项）。

验收：能复述 5 个实际踩坑点及解决策略。

---

## 8. 面试/汇报 90 秒表达模板

我在中断设计上遵循“快进快出 + 分层处理”。ISR 只做读寄存器、清标志、搬运数据和发事件，重逻辑放主循环或任务。并发上我区分了 `volatile` 和同步语义，涉及读改写的共享变量用最小临界区保护。  

通信场景采用 ISR 生产者、主循环消费者的环形缓冲区，配套溢出计数和错误计数做可观测性。优先级配置不是拍脑袋，而是基于实时性需求和最坏时延预算做表格化管理。  

这样系统在高负载下仍能保持可预测性，不容易出现隐蔽丢数和时序抖动问题。

---

# 关键点速记

1. 中断核心不是“会写 ISR”，而是“会做异步并发系统设计”。
2. `volatile` 不能替代临界区/原子操作。
3. ISR 只搬运，不做重活。
4. 优先级配置要和实时性目标绑定，不能随意设。
5. 每路中断必须有计数和错误观测量，方便现场定位。

---

# 实操备注（你现在就可以开始）

1. 先做最小闭环：`SysTick + EXTI 按键 + UART RX rb`。
2. 再加压测：持续串口灌包 30 分钟，看 overflow 和 error 计数。
3. 最后做文档化：一张优先级表 + 一张数据流图 + 一页故障处理策略。

你把这三步做完，就已经具备“能直接上手项目中断模块”的能力。
