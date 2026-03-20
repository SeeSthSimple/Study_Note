# FreeRTOS 高密度上手手册（可直接落项目）

## 0. 一句话先讲透 FreeRTOS

FreeRTOS 不是“多线程库”，而是一个可裁剪的实时内核。  
它的核心价值是：**把异步事件拆成可调度任务，并给你确定性的时序控制手段**。

你上手时要盯住三件事：

1. 调度是否可预测。
2. 任务通信是否无竞态。
3. 内存和栈是否可控。

---

## 1. 内核机制（必须先吃透）

## 1.1 任务状态机

任务常见状态：

1. Running：当前正在执行。
2. Ready：可执行，等调度。
3. Blocked：等待事件/超时。
4. Suspended：被显式挂起。

工程关键点：

- 不要让任务“忙等”，该 Block 就 Block。
- 真正决定实时性的不是任务数量，而是“最高优先级 Ready 任务的可控行为”。

## 1.2 调度本质

常见配置：

1. 抢占式调度（`configUSE_PREEMPTION = 1`）。
2. 时间片轮转（同优先级任务分时，`configUSE_TIME_SLICING = 1`）。

调度规则核心：

- 永远运行 **最高优先级的 Ready 任务**。
- 高优先级任务如果不阻塞，会长期压制低优先级任务。

## 1.3 Tick 与实时性边界

`configTICK_RATE_HZ` 决定系统节拍分辨率。  
例如 `1000Hz` 代表 1ms 一拍。

注意：

- Tick 越高，调度/中断开销越高。
- Tick 不是高精度定时器，微秒级要用硬件定时器。

---

## 2. 对象选型（项目里最实用）

## 2.1 队列 Queue：传“数据对象”

适用：

1. 任务间传结构体消息。
2. ISR 往任务送小数据。

特点：

- 按拷贝语义传输，安全但有开销。

## 2.2 二值信号量 Binary Semaphore：传“事件”

适用：

1. ISR 通知任务“有事发生”。
2. 任务间简单同步。

特点：

- 不携带数据，只表示事件。

## 2.3 互斥锁 Mutex：保护共享资源

适用：

1. 多任务共享串口、I2C、日志接口。

特点：

- 支持优先级继承，能缓解优先级反转。
- 不能在 ISR 里用。

## 2.4 事件组 EventGroup：多事件位图同步

适用：

1. 等待多个子系统就绪（网络、存储、传感器）。

特点：

- 位图语义清晰，适合系统状态聚合。

## 2.5 StreamBuffer / MessageBuffer

适用：

1. StreamBuffer：连续字节流（UART 数据流）。
2. MessageBuffer：变长消息边界明确。

---

## 3. ISR 与 FreeRTOS 的边界（高频踩坑区）

规则只有两条：

1. ISR 里必须用 `xxxFromISR` API。
2. ISR 结束前根据 `xHigherPriorityTaskWoken` 决定是否触发切换。

示例：

```c
void USART1_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t ch;

    if (uart_read_byte_isr(&ch)) {
        xQueueSendFromISR(g_uart_rx_q, &ch, &xHigherPriorityTaskWoken);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

不要做：

1. ISR 调普通 `xQueueSend`。
2. ISR 里拿 `Mutex`。
3. ISR 里做阻塞等待。

---

## 4. 直接可用的工程骨架（任务分层）

推荐最小分层：

1. `drv_isr_layer`：中断只搬运数据、发事件。
2. `service_layer`：协议解析、状态机、设备管理。
3. `app_layer`：业务逻辑和控制策略。

示例任务划分：

1. `task_comm_rx`：处理通信接收数据。
2. `task_comm_tx`：统一发送出口。
3. `task_ctrl`：控制逻辑与状态机。
4. `task_monitor`：喂狗、统计、健康监控。

---

## 5. 高频代码模板（复制即可改）

## 5.1 队列通信模板

```c
typedef struct {
    uint16_t id;
    uint16_t len;
    uint8_t  payload[32];
} app_msg_t;

static QueueHandle_t g_app_q;

void task_producer(void *arg)
{
    app_msg_t m = {0};
    for (;;) {
        m.id = 100;
        m.len = 1;
        m.payload[0] = 0x55;
        xQueueSend(g_app_q, &m, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void task_consumer(void *arg)
{
    app_msg_t m;
    for (;;) {
        if (xQueueReceive(g_app_q, &m, portMAX_DELAY) == pdPASS) {
            app_handle_msg(&m);
        }
    }
}
```

## 5.2 任务通知模板（轻量高效）

```c
static TaskHandle_t g_worker_task = NULL;

void sensor_isr(void)
{
    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(g_worker_task, &hpw);
    portYIELD_FROM_ISR(hpw);
}

void task_worker(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sensor_process_once();
    }
}
```

任务通知比队列更轻，适合“只通知不传数据”。

## 5.3 软件定时器模板

```c
static TimerHandle_t g_tmr;

static void tmr_cb(TimerHandle_t xTimer)
{
    monitor_kick();
}

void timers_init(void)
{
    g_tmr = xTimerCreate("mon", pdMS_TO_TICKS(1000), pdTRUE, NULL, tmr_cb);
    xTimerStart(g_tmr, 0);
}
```

---

## 6. 内存与栈（量产稳定性的关键）

## 6.1 动态 vs 静态创建

建议：

1. 量产项目优先静态创建（`xTaskCreateStatic` 等）。
2. 若用动态内存，尽量集中在初始化阶段完成。

## 6.2 heap_x 选型速记

1. `heap_1`：只分配不释放，最简单。
2. `heap_2`：可分配/释放，碎片控制弱。
3. `heap_4`：常用，支持合并空闲块。
4. `heap_5`：多内存区域管理。

多数项目优先 `heap_4` 或全静态。

## 6.3 栈水位检查

每个任务都要观测栈余量：

```c
UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
```

规则：

1. 栈不足是高危问题，宁可多给一点。
2. 任务栈按“最坏路径”估算，不按平均路径估算。

---

## 7. 优先级设计（可解释、可维护）

可用一个简单分层：

1. P4：硬实时控制任务（短、快、可预测）。
2. P3：通信处理任务。
3. P2：业务逻辑任务。
4. P1：日志/维护任务。

设计原则：

1. 高优先级任务必须可快速阻塞，不能常驻占用 CPU。
2. 持锁代码段必须短，避免放大优先级反转。
3. 所有优先级分配都要写进表格和注释，避免后续失控。

---

## 8. 调试与可观测性（上手后立即要做）

必须落地的观测项：

1. 任务 CPU 占用趋势（可通过运行时间统计）。
2. 各任务栈高水位。
3. 队列峰值深度和丢消息计数。
4. 关键任务循环周期抖动。
5. 看门狗复位原因统计。

典型故障定位路径：

1. 系统卡顿：看是否有高优先级任务未阻塞。
2. 偶发死锁：查 Mutex 持有路径和超时策略。
3. 数据丢失：看队列是否满、ISR 通知是否丢。
4. 随机崩溃：先查栈溢出和越界写。

---

## 9. 7 天上手计划（做完就能独立写模块）

## Day 1：最小系统跑通

1. 建立 `main + FreeRTOSConfig + 2 tasks`。
2. 任务里分别 100ms/500ms 打点。

验收：任务可稳定调度，无异常复位。

## Day 2：队列通信

1. Producer/Consumer 通过 Queue 传结构体。
2. 增加队列满计数。

验收：高负载下不丢关键消息。

## Day 3：中断到任务

1. ISR 用 `xQueueSendFromISR` 或任务通知。
2. 主任务处理事件并统计吞吐。

验收：连续 30 分钟运行无堆积。

## Day 4：同步原语

1. 用 Mutex 保护共享串口输出。
2. 演示错误用法和正确用法差异。

验收：并发日志无串扰、无死锁。

## Day 5：软件定时器与超时机制

1. 加周期监控定时器。
2. 任务等待加超时回退路径。

验收：任何等待点都不会无限阻塞。

## Day 6：栈与内存治理

1. 统计所有任务高水位。
2. 固化栈配置并记录依据。

验收：压测后栈余量可解释、可复现。

## Day 7：系统复盘与压测报告

1. 输出任务拓扑图、优先级表、通信图。
2. 输出瓶颈与改进清单。

验收：你能完整讲清系统调度与通信闭环。

---

## 10. 面试/项目汇报可直接复述（90 秒）

我在 FreeRTOS 项目里会先做任务分层和优先级预算，确保最高优先级任务是短路径且可阻塞。任务通信按场景选型，传数据用 Queue，传事件用 Notify 或二值信号量，共享资源用 Mutex 并控制持锁时间。  

中断侧只做搬运和通知，统一使用 `FromISR` API，并在 ISR 末尾根据 `xHigherPriorityTaskWoken` 决定是否切换。系统稳定性方面，我会持续跟踪任务栈高水位、队列峰值、任务周期抖动和复位原因，优先处理栈溢出、优先级反转和阻塞超时缺失这三类高风险问题。  

这样系统在功能增长后仍可预测、可调试、可维护。

---

# 关键点速记

1. FreeRTOS 的核心不是“会创建任务”，而是“会设计可预测调度系统”。
2. 队列传数据，通知传事件，Mutex 保护共享资源。
3. ISR 只用 `FromISR` API，不能阻塞。
4. 栈水位、队列深度、任务抖动必须持续观测。
5. 先稳定再扩展，先可测再优化。

---

# 实操备注（马上开干）

1. 先做一个三任务 Demo：`comm_rx + ctrl + monitor`。
2. 再接一个 ISR 事件入口（比如 UART RX）到任务通知。
3. 最后输出一页架构图和优先级表，形成你自己的 FreeRTOS 模板工程。

做到这三步，你就能在真实项目里独立接管 FreeRTOS 模块。
