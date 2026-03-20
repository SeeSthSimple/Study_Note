# 原始问题

帮我生成一个有关嵌入式从硬件到软件（代码实现）的全流程开发过程，加入 FreeRTOS。

---

# 嵌入式项目全流程（硬件 -> 软件 -> FreeRTOS -> 量产）实战手册

> 目标：给你一条可直接执行的项目路径，学完就能从 0 推到“可交付版本”。  
> 默认假设：Cortex-M（如 STM32/GD32/NXP）+ C + FreeRTOS。  
> 方法论：每个阶段都给“输入、动作、输出、验收标准”。

---

## 0. 项目总览（先看全局）

一个完整嵌入式项目通常分 10 个阶段：

1. 需求与系统约束定义
2. 硬件选型与系统架构
3. 原理图/PCB 与硬件风险收敛
4. 板级 bring-up（点亮板子）
5. BSP/驱动层搭建
6. FreeRTOS 架构落地（任务/同步/中断边界）
7. 业务功能实现（协议、控制、状态机）
8. 调试与可靠性验证（长稳、异常注入）
9. 生产与运维能力（版本、日志、升级）
10. 复盘与模板沉淀（可复用资产）

---

## 1. 需求与系统约束定义

## 1.1 输入

1. 产品需求（功能列表）
2. 非功能需求（实时性、功耗、成本、可靠性）
3. 外部接口需求（UART/CAN/I2C/SPI/BLE/Wi-Fi）

## 1.2 关键动作

1. 把需求量化为技术指标：
- 控制环周期（如 1ms/10ms）
- 启动时间（如 < 2s）
- 通信吞吐（如 115200bps 连续无丢包）
- 连续运行稳定性（如 72h 无异常）
2. 定义“必须满足”和“可降级”项。
3. 提前列出高风险（资源不足、EMI、温升、升级失败）。

## 1.3 输出

1. 《系统技术规格书》
2. 《资源预算表》（Flash/RAM/CPU/IO/功耗）
3. 《风险清单 v1》

## 1.4 验收标准

所有团队成员对“成功定义”一致，无口头模糊项。

---

## 2. 硬件选型与系统架构

## 2.1 MCU 选型

评估维度：

1. Flash/RAM 是否满足未来 1~2 年增长
2. 外设是否覆盖需求（定时器、DMA、通信接口）
3. 性能余量（主频、FPU、DSP）
4. 供货稳定性和成本

经验：

1. 资源预算至少留 30% 余量。
2. 如果要 OTA + TLS + FreeRTOS，RAM 预算要保守估计。

## 2.2 系统架构图（建议先画）

```text
Sensors/Buttons ---> ISR Layer ---> Driver/BSP ---> FreeRTOS Service Tasks ---> App State Machine
        |                 |                |                    |                       |
      ADC/EXTI         DMA/UART         HAL/LL             Queue/Notify             Business Logic
```

## 2.3 输出

1. 《硬件选型报告》
2. 《系统模块图 + 数据流图》
3. 《接口定义文档》（电气 + 协议）

---

## 3. 原理图/PCB 与硬件风险收敛

## 3.1 必做清单

1. 电源完整性：LDO/DC-DC 余量、去耦布局、地回流
2. 复位与时钟：复位电路可靠、晶振参数校核
3. 调试接口：SWD/JTAG/UART 留测试点
4. 关键外设引脚复用冲突检查
5. ESD/浪涌/EMI 基本防护

## 3.2 输出

1. 原理图评审记录
2. PCB 评审记录
3. Bring-up 测试点清单

---

## 4. 板级 Bring-up（硬件上电后第一阶段）

## 4.1 先做最小闭环

1. 时钟初始化
2. GPIO 输出（LED）
3. UART 打印（日志口）
4. 看门狗基础验证

## 4.2 Bring-up 代码骨架

```c
int main(void)
{
    board_clock_init();
    board_gpio_init();
    board_uart_init(115200);

    log_info("boot ok");

    while (1) {
        led_toggle();
        board_delay_ms(500);
    }
}
```

## 4.3 验收标准

1. 连续上电 100 次可稳定启动
2. 串口日志稳定输出
3. 无异常复位

---

## 5. BSP/驱动层搭建（软件地基）

## 5.1 目录建议

```text
project/
  bsp/
    bsp_clock.c
    bsp_gpio.c
    bsp_uart.c
    bsp_i2c.c
  drivers/
    drv_sensor.c
    drv_comm.c
  middleware/
    freertos/
  app/
```

## 5.2 驱动接口原则

1. 统一错误码
2. 同步/异步语义明确
3. 所有阻塞接口要有超时

示例：

```c
typedef enum {
    DRV_OK = 0,
    DRV_E_TIMEOUT = -1,
    DRV_E_IO = -2
} drv_err_t;

drv_err_t uart_write(const uint8_t *buf, uint16_t len, uint32_t timeout_ms);
drv_err_t uart_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
```

---

## 6. FreeRTOS 架构落地（核心阶段）

## 6.1 任务分层模板

1. `task_isr_bridge`：中断事件桥接（轻量）
2. `task_comm`：通信收发与协议
3. `task_ctrl`：控制算法/状态机
4. `task_monitor`：看门狗、统计、告警

## 6.2 任务通信选型

1. 传数据：Queue
2. 传事件：Task Notify / Binary Semaphore
3. 共享资源保护：Mutex

## 6.3 中断到任务示例

```c
static QueueHandle_t g_uart_rx_q;

void USART1_IRQHandler(void)
{
    BaseType_t hpw = pdFALSE;
    uint8_t ch;

    if (uart_read_byte_isr(&ch)) {
        xQueueSendFromISR(g_uart_rx_q, &ch, &hpw);
    }
    portYIELD_FROM_ISR(hpw);
}

void task_comm(void *arg)
{
    uint8_t ch;
    for (;;) {
        if (xQueueReceive(g_uart_rx_q, &ch, portMAX_DELAY) == pdPASS) {
            proto_feed(ch);
        }
    }
}
```

## 6.4 FreeRTOS 配置重点

1. `configTICK_RATE_HZ`：通常 1000 或 100
2. `configMAX_PRIORITIES`：够用即可，避免失控
3. `configCHECK_FOR_STACK_OVERFLOW`：必须开
4. `configUSE_TRACE_FACILITY`/运行时统计：建议开

---

## 7. 业务功能实现（可维护写法）

## 7.1 状态机优先

把业务拆成明确状态，避免 if-else 泥球：

```c
typedef enum {
    APP_INIT = 0,
    APP_IDLE,
    APP_RUN,
    APP_ERROR
} app_state_t;
```

## 7.2 协议层建议

1. 帧格式：头 + 长度 + 负载 + CRC
2. 解析方式：字节流状态机
3. 错误恢复：可重同步

---

## 8. 调试与可靠性验证（决定是否能交付）

## 8.1 必测项

1. 长时间运行（24h/72h）
2. 断电重启恢复
3. 高负载通信压测
4. 内存/栈边界压测
5. 看门狗复位路径验证

## 8.2 可观测性必须落地

1. 任务栈高水位
2. 队列峰值深度
3. 中断计数和错误计数
4. 关键周期任务抖动

---

## 9. 生产与运维能力（从“能跑”到“可维护”）

## 9.1 版本体系

1. 固件版本号（语义化）
2. 构建号（commit + 时间）
3. 配置版本（参数集）

## 9.2 日志体系

1. 分级日志（E/W/I/D）
2. 关键事件持久化（复位原因、错误码）
3. 远程诊断字段统一

## 9.3 OTA（建议尽早预留）

1. Bootloader 分区规划
2. 升级回滚策略
3. 升级结果回传

---

## 10. 代码实现主线（你可以照着建）

## 10.1 启动主函数

```c
int main(void)
{
    board_init();
    drivers_init();
    app_init();

    freertos_objects_init();   // queue/mutex/timer
    tasks_create();            // comm/ctrl/monitor

    vTaskStartScheduler();
    for (;;) {}                // 不应到达
}
```

## 10.2 任务创建模板

```c
void tasks_create(void)
{
    xTaskCreate(task_comm,    "comm",    512, NULL, 3, NULL);
    xTaskCreate(task_ctrl,    "ctrl",    512, NULL, 4, NULL);
    xTaskCreate(task_monitor, "monitor", 384, NULL, 2, NULL);
}
```

## 10.3 监控任务模板

```c
void task_monitor(void *arg)
{
    for (;;) {
        feed_watchdog();
        collect_runtime_stats();
        log_health_snapshot();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

---

## 11. 里程碑与交付物（项目管理视角）

## M1：硬件可启动

1. LED + UART + 基础外设通过
2. 交付：bring-up 报告

## M2：RTOS 基础稳定

1. 关键任务可调度
2. 队列通信稳定
3. 交付：任务拓扑图 + 优先级表

## M3：业务功能闭环

1. 协议收发 + 状态机稳定
2. 异常路径可恢复
3. 交付：功能测试报告

## M4：可靠性通过

1. 长稳 + 压测 + 异常注入通过
2. 交付：可靠性报告 + 发布包

---

## 12. 最小 14 天实战计划（快速上手版）

1. Day 1-2：需求量化 + 资源预算
2. Day 3-4：硬件 bring-up（时钟/GPIO/UART）
3. Day 5-6：驱动层接口统一
4. Day 7-8：FreeRTOS 任务与通信落地
5. Day 9-10：协议与业务状态机
6. Day 11-12：压测与异常恢复
7. Day 13：日志与版本体系
8. Day 14：复盘和模板沉淀

---

# 关键点速记

1. 先做“可启动”，再做“可调度”，最后做“可交付”。
2. 中断只搬运，任务做重处理。
3. FreeRTOS 不解决架构问题，架构要你先设计好。
4. 每个阶段都要有量化验收标准。
5. 可观测性（日志/计数/栈水位）是稳定性的前提。

---

# 关联笔记

1. `2026-03-19-001-interrupt-hands-on.md`
2. `2026-03-19-002-freertos-hands-on.md`
3. `2026-03-19-004-critical-section-and-priority-config.md`
4. `../ota-remote-upgrade-project-handbook.md`
