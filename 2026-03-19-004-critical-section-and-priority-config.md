# 原始问题

临界区代码实现？优先级代码上怎么配置的？

---

# 直接可用答案（先上代码）

## 1. 裸机临界区实现（Cortex-M 通用）

## 1.1 方案 A：PRIMASK 全局关中断（最常用）

```c
#include <stdint.h>
#include "cmsis_gcc.h"   // 或对应编译器的 CMSIS 头

static inline uint32_t irq_save(void)
{
    uint32_t primask = __get_PRIMASK(); // 保存当前全局中断状态
    __disable_irq();                    // 全局关中断
    __DSB();
    __ISB();
    return primask;
}

static inline void irq_restore(uint32_t primask)
{
    __DSB();
    __ISB();
    if ((primask & 1u) == 0u) {         // 之前是开中断，才恢复开中断
        __enable_irq();
    }
}

volatile uint32_t g_shared_cnt = 0;

void shared_inc(void)
{
    uint32_t s = irq_save();
    g_shared_cnt++;                      // 临界区仅包住必要读改写
    irq_restore(s);
}
```

适用：

1. 简单可靠。
2. 临界区必须很短。

## 1.2 方案 B：BASEPRI 局部屏蔽（高级用法）

```c
static inline uint32_t basepri_save_and_set(uint32_t new_basepri)
{
    uint32_t old = __get_BASEPRI();
    __set_BASEPRI(new_basepri); // 仅屏蔽“优先级不高于阈值”的中断
    __DSB();
    __ISB();
    return old;
}

static inline void basepri_restore(uint32_t old_basepri)
{
    __set_BASEPRI(old_basepri);
    __DSB();
    __ISB();
}
```

适用：

1. 不想完全关掉所有中断。
2. 需要保留高优先级中断的实时响应。

---

## 2. FreeRTOS 临界区（必须区分任务和 ISR）

```c
#include "FreeRTOS.h"
#include "task.h"

void task_side_update(void)
{
    taskENTER_CRITICAL();
    // 共享资源访问
    taskEXIT_CRITICAL();
}

void USART1_IRQHandler(void)
{
    UBaseType_t s = taskENTER_CRITICAL_FROM_ISR();
    // ISR 内必要保护代码
    taskEXIT_CRITICAL_FROM_ISR(s);
}
```

规则：

1. 任务上下文用 `taskENTER_CRITICAL/taskEXIT_CRITICAL`。
2. ISR 上下文用 `...FROM_ISR` 版本。
3. ISR 中调用 RTOS API 必须是 `FromISR` 后缀。

---

## 3. NVIC 优先级配置代码（CMSIS 写法）

```c
#include "core_cm4.h"  // 根据内核替换 cm3/cm7

void nvic_init(void)
{
    // 1) 设置全局优先级分组（示例：2 位抢占 + 2 位子优先级）
    NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_2);

    uint32_t group = NVIC_GetPriorityGrouping();

    // 2) USART1: 抢占优先级 1，子优先级 1
    uint32_t prio_usart = NVIC_EncodePriority(group, 1, 1);
    NVIC_SetPriority(USART1_IRQn, prio_usart);
    NVIC_EnableIRQ(USART1_IRQn);

    // 3) EXTI15_10: 抢占优先级 2，子优先级 0
    uint32_t prio_exti = NVIC_EncodePriority(group, 2, 0);
    NVIC_SetPriority(EXTI15_10_IRQn, prio_exti);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
}
```

记忆点：

1. 数值越小，优先级越高。
2. 先比抢占优先级，再比子优先级。
3. 优先级分组是全局配置，不是每个中断单独分组。

---

## 4. STM32 HAL 写法（如果你在用 HAL）

```c
void MX_NVIC_Init(void)
{
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_2);

    HAL_NVIC_SetPriority(USART1_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}
```

---

## 5. FreeRTOS 下优先级配置的关键补充

如果某 ISR 要调用 `xQueueSendFromISR` 等 API，  
它的中断优先级必须满足 FreeRTOS 约束（不高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 对应阈值）。

实操建议：

1. 把“调用 RTOS API 的中断”统一归到一个安全优先级层级。
2. “极高实时中断”不调用 RTOS API，只做寄存器处理和轻量标记。

---

# 常见错误速查

1. 错误：临界区包太大。  
后果：系统实时性抖动、丢中断风险上升。

2. 错误：ISR 里用普通 `xQueueSend`。  
后果：RTOS 行为异常甚至 HardFault。

3. 错误：把优先级“数值大”当作“优先级高”。  
后果：抢占关系和预期相反。

4. 错误：把“分组”理解成每个中断可选不同组。  
后果：优先级设计逻辑混乱。

---

# 一句话结论

临界区代码本质是“短时间受控屏蔽并发打断”；  
优先级配置本质是“软件写 NVIC 配置，硬件按规则实时裁决”。

---

# 关联笔记

1. `2026-03-19-003-nvic-isr-priority-critical-section.md`
2. `2026-03-19-002-freertos-hands-on.md`
