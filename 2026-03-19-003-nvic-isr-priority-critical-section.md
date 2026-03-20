# 原始问题

NVIC 是什么？中断的流程详细讲解？  
假如优先级分组 A > 优先级分组 B，虽然优先级分组 B 有更高优先级的，但是依旧优先高优先级分组执行？  
这个优先级机制会作用在程序还是硬件还是哪里？  
ISR 是什么详细讲解？  
临界区在嵌入式是指什么？

---

# 组织化讲解（高密度 + 易混点纠正）

## 1. NVIC 是什么

NVIC = Nested Vectored Interrupt Controller（嵌套向量中断控制器）。  
它是 Cortex-M 内核里的硬件中断控制单元，负责：

1. 每个中断的使能/屏蔽（Enable/Disable）。
2. 挂起状态管理（Pending）。
3. 活动状态管理（Active）。
4. 优先级比较与抢占决策。
5. 中断向量分发（跳到对应 ISR）。

一句话：**NVIC 是“中断调度器硬件”**，不是普通软件库。

---

## 2. 中断完整流程（从触发到返回）

以 Cortex-M 为例，典型流程：

1. 外设事件发生，比如 UART 收到字节，外设置位中断标志。
2. 外设把中断请求送到 NVIC（对应 IRQn）。
3. NVIC 判断：
- 该 IRQ 是否使能。
- 是否被全局屏蔽（PRIMASK/BASEPRI/FAULTMASK）。
- 与当前活动中断相比，是否具有更高抢占优先级。
4. 若可响应，CPU 完成异常入口：
- 自动压栈（R0-R3, R12, LR, PC, xPSR）。
- 从向量表取 ISR 地址并跳转执行。
5. ISR 内处理事件：
- 读状态。
- 清标志。
- 搬数据/发事件。
- 快速退出。
6. 异常返回，硬件自动出栈，恢复到被打断位置继续执行。

工程上要点：

1. ISR 清标志顺序要按芯片手册，否则会反复进中断。
2. ISR 过长会提高系统抖动，导致低优先级中断饥饿。
3. 不要把中断当任务线程，中断是“抢占式短处理路径”。

## 2.1 用伪代码看“硬件+软件”完整路径

### A. 初始化阶段（软件配置 NVIC）

```c
void irq_init(void)
{
    // 1) 全局设置优先级分组（全局唯一，不是每个中断各自分组）
    NVIC_SetPriorityGrouping(PRIO_GROUP_2_2); // 示例：2 位抢占 + 2 位子优先级

    // 2) 配置具体中断优先级
    NVIC_SetPriority(USART1_IRQn, NVIC_EncodePriority(PRIO_GROUP_2_2, 1, 1));
    NVIC_SetPriority(TIM2_IRQn,   NVIC_EncodePriority(PRIO_GROUP_2_2, 0, 2));

    // 3) 清 pending + 使能中断
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);

    // 4) 使能外设侧中断源
    USART1->CR1 |= USART_CR1_RXNEIE;
    TIM2->DIER  |= TIM_DIER_UIE;
}
```

### B. 运行阶段（NVIC 硬件裁决，可理解为如下状态机）

```text
loop forever:
  if peripheral_event_happened:
      peripheral_set_irq_flag()
      nvic_set_pending(IRQn)

  if nvic_pending(IRQn) and nvic_enabled(IRQn) and not masked_by_global_irq():
      if preempt_prio(IRQn) higher than current_active_exception:
          cpu_auto_stack_context()
          jump_to_vector(ISR_of_IRQn)
          // ISR 返回后
          cpu_auto_unstack_context()
```

### C. ISR 执行阶段（工程可落地模板）

```c
volatile uint8_t g_uart_evt = 0;

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;

    if (sr & USART_SR_RXNE) {
        uint8_t ch = (uint8_t)USART1->DR; // 读数据
        rb_push_isr(&g_uart_rb, ch);      // 快速搬运
        g_uart_evt = 1;                   // 发轻量事件
    }

    if (sr & USART_SR_ORE) {
        (void)USART1->DR;                 // 依手册清错误
        g_uart_ore_cnt++;
    }
}
```

### D. ISR 外的“慢处理”阶段（主循环/任务）

```c
void app_loop(void)
{
    uint8_t ch = 0;

    for (;;) {
        if (g_uart_evt) {
            g_uart_evt = 0;
            while (rb_pop(&g_uart_rb, &ch)) {
                proto_feed(ch); // 复杂解析放这里
            }
        }
    }
}
```

这 4 段代码连起来，就是你项目里真实可执行的中断生命周期。

---

## 3. 你问的“优先级分组 A/B”纠正（这是高频误区）

你这个说法里最关键的误区是：  
**“优先级分组”不是给每个中断单独选 A 组/B 组。**

在 STM32/Cortex-M 常见语境里：

1. `Priority Grouping`（PRIGROUP）是一个全局配置。
2. 它只定义“优先级位如何拆分”为：
- 抢占优先级（preempt priority）
- 子优先级（subpriority / response priority）
3. 一旦分组配置好，所有中断都按同一拆分规则解释优先级值。

所以不存在“中断 X 在分组 A、中断 Y 在分组 B，然后 A 一定压 B”这种比较方式。

---

## 4. 真正决定谁先执行的规则

规则按顺序看：

1. 先比抢占优先级（preempt priority）。
2. 抢占优先级相同，再比子优先级（决定挂起时谁先被服务）。
3. 数值越小优先级越高（Cortex-M 常见规则）。

你可以这样记：

1. 抢占优先级决定“能不能打断别人”。
2. 子优先级决定“同级排队谁先上”。

---

## 5. 具体例子（把概念钉牢）

假设某芯片可用 4 位优先级位，分组设成“2 位抢占 + 2 位子优先级”：

1. 中断 A：抢占=1，子=3
2. 中断 B：抢占=0，子=2

结果：

1. B 可抢占 A（因为 0 比 1 高）。
2. 若 A 与 C 抢占同为 1，子优先级小的先被服务。

注意：这里没有“分组 A/B 比大小”，只有在同一个全局分组规则下比较各中断优先级字段。

---

## 6. 这个优先级机制作用在哪里

简明回答：**硬件执行，软件配置。**

1. 作用主体是 NVIC 硬件逻辑（内核里做实时比较和抢占）。
2. 程序代码通过寄存器/CMSIS API 写入配置（例如设置优先级、使能 IRQ）。
3. 运行时谁先执行由硬件状态机决定，不是 C 代码 `if/else` 每次手动判断。

---

## 7. ISR 是什么（详细但项目导向）

ISR = Interrupt Service Routine，中断服务程序。  
它是向量表里绑定到某个 IRQ 的处理函数，比如 `USART1_IRQHandler`。

ISR 的职责边界：

1. 快速确认中断源（哪个标志触发）。
2. 清除中断条件（避免重复触发）。
3. 搬运关键数据到缓冲区/队列。
4. 通知主循环或 RTOS 任务继续处理。

ISR 的工程禁忌：

1. 不做阻塞等待。
2. 不做重计算。
3. 不做大段日志打印。
4. 不在 ISR 使用非 FromISR 的 RTOS API。

---

## 8. 临界区在嵌入式里是什么

临界区 = 访问共享资源时，为防并发打断而进行的最小保护区间。

目的：

1. 防 ISR 与主循环同时改同一变量。
2. 防多任务同时改共享对象。

常见实现方式：

1. 裸机：短时间关中断（如 PRIMASK/BASEPRI）。
2. RTOS：`taskENTER_CRITICAL` / `taskEXIT_CRITICAL`。
3. 多核系统：还需自旋锁/核间同步，单核临界区不够。

关键原则：

1. 只包住“必须原子化”的几行代码。
2. 临界区越短越好。
3. 绝不在临界区做耗时操作。

---

# 项目可直接复用的判断口诀

1. 能放任务里就不要放 ISR 里。
2. `volatile` 解决可见性，不解决互斥。
3. 抢占优先级管“打断”，子优先级管“排队”。
4. 优先级分组是全局位宽拆分，不是中断分门派。
5. 优先级决策是 NVIC 硬件做的，代码负责配置。

---

# 实操建议（你现在就可以做）

1. 画你板子的中断图：外设源 -> IRQn -> ISR -> 数据通道 -> 任务。
2. 列一张 NVIC 优先级表：IRQ 名称、抢占优先级、子优先级、设计理由。
3. 对每个 ISR 标注“最长执行时间预算”和“是否可能丢中断”。
4. 给共享变量加并发说明：`volatile`、临界区、为何这样选。

---

# 关联笔记

1. `2026-03-19-001-interrupt-hands-on.md`（中断实战框架）
2. `2026-03-19-002-freertos-hands-on.md`（RTOS 下 ISR 边界）
