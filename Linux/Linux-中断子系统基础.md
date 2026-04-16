# Linux-中断子系统基础

## 原始问题

为什么很多嵌入式 Linux 设备明明能 `probe` 成功，但中断就是不触发、触发了不处理、处理了却把系统拖慢？Linux 中断子系统到底在做什么，为什么它不像裸机那样“配个寄存器和 ISR 就完了”？

## 先给结论

Linux 中断子系统的本质，是把“硬件发出的中断信号”统一抽象成内核可管理的 IRQ 资源，再通过通用框架把它路由到具体驱动处理函数。

先记住下面几个结论：

1. 驱动看到的 IRQ 号，通常已经不是原始硬件线号，而是内核映射后的逻辑 IRQ。
2. Linux 处理中断的主线，不只是“注册一个 handler”，还包括设备树描述、IRQ controller、irq domain、触发类型、线程化处理和运行期统计。
3. 很多中断问题不是 handler 写错，而是设备树 `interrupts` 写错、触发方式不对、pending 没清、共享中断没分辨来源。
4. 真实项目里，中断处理通常要分成上半部和下半部，或者直接用线程化中断，避免把耗时逻辑塞进硬中断上下文。
5. AI 可以快速给你拼出 `request_irq()` 或 `devm_request_threaded_irq()` 模板，但它不知道你的中断控制器怎么映射、IRQ 是否共享、设备是不是电平触发、清 pending 的顺序是否正确，这些必须靠你自己判断。

如果只能背 `request_irq()` 的函数原型，却讲不清设备树到逻辑 IRQ 的映射、为什么要 `IRQF_ONESHOT`、为什么 handler 返回 `IRQ_NONE` 或 `IRQ_HANDLED`、为什么 `/proc/interrupts` 很重要，就说明这部分还没有真正掌握。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 驱动已经 `probe` 成功，但设备事件来了却不进中断处理函数。
2. 中断能进，但系统抖动大、占用高、共享中断误判来源，排查没有主线。
3. 面试和工作中经常被问到“Linux 中断怎么注册”“上半部和下半部怎么分工”“中断如何和设备树关联”，缺少稳定表达。
4. 后续做 GPIO 中断、SPI/I2C 设备 IRQ、platform 驱动、线程化中断和排障时，需要一篇系统主线笔记。

它在 AI 时代仍然重要，因为 AI 很容易生成一个能编译的中断注册模板，但它无法替你判断电平触发设备为什么会中断风暴，也不会自动知道共享 IRQ 上要先确认是不是自己的设备，更不能替你决定是用硬中断、tasklet、workqueue 还是 threaded IRQ。真正决定稳定性的，是你对 Linux 中断链路的理解。

## 核心概念 / 本质机制

### 1. Linux 中断链路到底包含哪些层

可以先把它拆成下面几层：

1. 设备本身产生事件。
2. 事件送到某个 IRQ controller。
3. 设备树或平台信息描述中断来源和触发方式。
4. 内核把硬件中断映射成逻辑 IRQ。
5. 驱动申请 IRQ 并注册处理函数。
6. 中断发生时，通用 IRQ 框架调用驱动处理逻辑。

所以驱动里看到的一个 `irq` 整数，只是整条链的中间结果。

### 2. 为什么逻辑 IRQ 不等于硬件线号

因为 Linux 需要统一管理不同中断控制器、不同 SoC 和级联中断结构。

所以常见做法是：

1. 硬件世界有物理线号或 hwirq。
2. 内核通过 irq domain 映射成逻辑 IRQ。
3. 驱动拿到的是逻辑 IRQ。

这就是为什么：

1. 设备树里写的 `interrupts = <...>` 不一定等于驱动里最后打印的 IRQ 号。
2. 同一个外设在不同平台上 IRQ 号可能不同，但驱动写法仍能统一。

### 3. 上半部和下半部为什么要分开

硬中断上下文的特点是：

1. 不能睡眠。
2. 需要尽快返回。
3. 对系统延迟影响大。

所以 Linux 常见分工是：

1. 上半部：快速确认中断来源、清必要状态、做最小化处理。
2. 下半部：做耗时处理，例如读取更多数据、协议解析、唤醒用户态、复杂状态机。

过去常见下半部机制包括：

1. softirq
2. tasklet
3. workqueue

而在驱动开发里，现在很常见也很推荐的是：

1. `request_threaded_irq()` 或 `devm_request_threaded_irq()`

### 4. 为什么线程化中断很常见

线程化中断的好处是：

1. 复杂逻辑可以放到可调度上下文里。
2. 更容易和 mutex、阻塞式接口、较慢 I/O 配合。
3. 代码组织通常更清晰。

典型模式是：

1. top half 很短，只判断来源并返回 `IRQ_WAKE_THREAD`。
2. threaded handler 做主要处理。

这很像 RTOS 里“ISR 最小化 + 任务完整处理”的思路，只是 Linux 用的是内核线程和通用 IRQ 框架。

### 5. 为什么共享中断最容易出坑

共享中断意味着多设备共用一条 IRQ 线。

这时每个 handler 都必须先判断：

1. 当前中断是不是自己的设备触发的。

如果不判断就直接处理，会出现：

1. 平白做很多无效工作。
2. 错误清别人设备的状态。
3. 返回值不对，导致中断链行为异常。

所以共享中断场景里，一个标准动作是：

1. 先读状态寄存器。
2. 确认是自己的事件后再处理。
3. 否则返回 `IRQ_NONE`。

### 6. 为什么电平触发和边沿触发会影响驱动写法

#### 边沿触发

特点：

1. 在边沿瞬间触发一次。
2. 事件脉冲过去就没有了。

风险：

1. 如果中断被屏蔽或丢了边沿，可能直接错过事件。

#### 电平触发

特点：

1. 只要电平保持有效，就可能持续触发。

风险：

1. 如果不先清设备侧 pending，可能形成中断风暴。

所以中断清除顺序、设备状态读取方式，都必须结合触发类型理解。

### 7. 为什么 `/proc/interrupts` 很重要

它直接告诉你：

1. 哪些 IRQ 在工作。
2. 各 CPU 上中断分布如何。
3. 计数是否在增长。
4. 中断控制器和设备名字是什么。

很多“中断不工作”的问题，先看 `/proc/interrupts` 就能快速区分：

1. 完全没触发。
2. 已经在触发但 handler 没处理对。
3. 触发过快导致风暴。

### 8. 设备树在中断链里扮演什么角色

设备树通常要描述：

1. 中断控制器是谁。
2. 中断号或硬件中断线是什么。
3. 触发类型是什么。

如果这里错了，常见现象是：

1. `platform_get_irq()` 失败。
2. 驱动拿到错误 IRQ。
3. 能注册但永远收不到真正事件。

所以很多 Linux 中断问题，入口不是驱动代码，而是设备树节点。

## 数据流 / 控制流 / 时序关系

下面用一个典型 platform 设备的 IRQ 链路来理解：

```text
外设事件发生
-> 设备内部状态寄存器置位
-> IRQ 线拉高或拉低
-> IRQ controller 感知到该线变化
-> 内核通用 IRQ 框架收到逻辑 IRQ
-> 调用注册的 top half
-> top half 确认来源并返回 IRQ_WAKE_THREAD
-> 内核唤醒该 IRQ 的线程化 handler
-> threaded handler 读取数据、清设备状态、完成后返回
```

这条链中常见断点包括：

1. 设备根本没产生 IRQ。
2. 设备树中断描述错。
3. 中断控制器配置或映射错。
4. handler 注册了，但不是自己的事件。
5. 设备状态没清，导致重复触发。

## 最小可运行示例

下面给一个典型的线程化中断注册示例：

```c
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>

struct demo_priv {
    void __iomem *base;
    int irq;
};

static irqreturn_t demo_irq_handler(int irq, void *dev_id)
{
    struct demo_priv *priv = dev_id;
    u32 status = readl(priv->base + 0x10);

    if (!(status & BIT(0)))
        return IRQ_NONE;

    writel(BIT(0), priv->base + 0x10); /* 假设写 1 清中断 */
    return IRQ_WAKE_THREAD;
}

static irqreturn_t demo_irq_thread(int irq, void *dev_id)
{
    struct demo_priv *priv = dev_id;

    demo_read_data_and_report(priv);
    return IRQ_HANDLED;
}

static int demo_probe(struct platform_device *pdev)
{
    struct demo_priv *priv;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->base = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(priv->base))
        return PTR_ERR(priv->base);

    priv->irq = platform_get_irq(pdev, 0);
    if (priv->irq < 0)
        return priv->irq;

    return devm_request_threaded_irq(&pdev->dev, priv->irq,
                                     demo_irq_handler,
                                     demo_irq_thread,
                                     IRQF_ONESHOT,
                                     dev_name(&pdev->dev),
                                     priv);
}
```

这个示例体现了 Linux 中断主线里最关键的几个点：

1. IRQ 通常从平台资源里获取，而不是硬编码。
2. top half 要先确认中断来源，否则共享中断时会误处理。
3. top half 尽量短，只做最小确认和必要清除。
4. 主要工作放到 threaded handler。
5. `IRQF_ONESHOT` 能避免线程化处理中同一 IRQ 重入。

## 代码解读

### 1. 为什么 `platform_get_irq()` 很关键

因为驱动不应该假定 IRQ 号是固定常量。

它真正表达的是：

1. 从平台描述里取出当前设备实际关联的 IRQ 资源。

这也是驱动能跨板级复用的重要前提。

### 2. 为什么 top half 先读状态再决定返回值

尤其在共享中断场景里，必须先确认：

1. 这是不是自己的设备触发的。

如果不是，却还返回 `IRQ_HANDLED`，会误导内核以为该中断已经被正确处理。

### 3. 为什么示例里在 top half 清 pending

对很多电平触发设备来说，如果不尽早清设备侧中断状态：

1. IRQ 线可能一直保持有效。
2. 线程处理还没跑完，就又被持续触发。

但这一步一定要看手册：

1. 有些设备适合先读数据再清。
2. 有些设备必须先清再读。

顺序错了，一样会出问题。

### 4. 为什么 threaded handler 更适合放主要逻辑

因为它运行在可调度上下文里，通常更适合：

1. 读更多寄存器。
2. 做较慢的数据搬运。
3. 使用锁。
4. 和其他内核子系统交互。

这会让中断处理更接近“工程上可维护”的结构，而不是把所有复杂度塞在一个硬 handler 里。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| IRQ 获取 | 通过设备树或平台资源获取 | 在驱动里硬编码 IRQ 号 | 不可移植且易错 |
| 来源判断 | 先确认是不是自己的中断 | 不判断就直接处理 | 共享 IRQ 时会误处理 |
| 上下文分工 | top half 最小化，复杂逻辑下放线程 | 所有逻辑都塞硬中断 | 延迟和可维护性都差 |
| 触发类型 | 结合手册配置边沿/电平 | 只要能注册就忽略触发方式 | 容易丢中断或风暴 |
| 清中断顺序 | 按设备语义清 pending | 随便找个地方清 | 很容易重复触发 |
| 运行期验证 | 看 `/proc/interrupts` 和日志 | 只盯注册代码 | 看不到真实运行状态 |

## 边界条件与适用范围

1. 这篇笔记聚焦嵌入式 Linux 驱动最常见的 platform / SoC 设备中断主线，不展开所有 irqchip 内部实现细节。
2. 不是所有驱动都必须用 threaded IRQ，极轻量且严格时延要求的场景可以只用 top half。
3. 如果设备本质上更适合轮询，例如低频状态检查且中断代价更高，也不必强行上 IRQ。
4. 某些复杂控制器会有级联中断、MSI、GPIO 扩展中断，这时还要进一步理解控制器层。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| `platform_get_irq()` 失败 | 设备树 `interrupts` 或 `interrupt-parent` 错 | 查 dts 和启动日志 |
| IRQ 能注册但完全不触发 | 设备侧没使能；线没接；触发方式错 | 看寄存器、波形、`/proc/interrupts` |
| 一触发就风暴 | 电平触发状态没清；清除顺序错 | 看计数暴涨和状态寄存器 |
| 共享中断时误响应 | 没判断来源就处理 | 在 handler 里先读状态确认 |
| handler 进了但功能不对 | 只清了控制器，没清设备侧 pending | 联合查设备寄存器 |
| 系统抖动大 | top half 太重；日志太多 | tracing 或统计 handler 耗时 |

排查 Linux IRQ 问题时，推荐按这条顺序走：

1. 设备树和 IRQ 资源是否正确。
2. 驱动是否成功申请 IRQ。
3. `/proc/interrupts` 计数是否增长。
4. handler 是否真的进。
5. 设备状态位是否正确读取和清除。
6. top half 和 threaded handler 分工是否合理。

## 工程落地建议

1. 新驱动优先考虑 `devm_request_threaded_irq()`，特别是需要较多处理逻辑时。
2. 共享中断场景一定把“来源判断”写清楚，不要省。
3. 把设备侧 pending 清除语义写进注释或笔记里，后续维护很容易踩坑。
4. 中断调通后，顺手把 `/proc/interrupts` 计数、触发类型和清除顺序记录下来，后面排障非常省时间。
5. 把 Linux 中断理解和 RTOS 的“ISR 最小化 + 任务处理”放在一起看，会更容易形成迁移能力。

## 性能、稳定性、可维护性影响

1. 中断路径设计得好，系统响应会快且稳。
2. top half 太重，会直接抬高系统延迟和抖动。
3. 清晰的线程化中断结构，比堆在一个硬 handler 里更容易维护和排查。
4. 正确的来源判断和状态清除，是避免中断风暴和误处理中最关键的两步。

## 面试 / 问答怎么讲

### 30 秒版本

Linux 中断子系统的核心，是把硬件中断通过中断控制器和 irq domain 映射成逻辑 IRQ，再由驱动注册 handler 处理。工程上通常采用“top half 最小确认 + threaded handler 完整处理”的方式，重点是设备树描述、触发类型和 pending 清除顺序。

### 3 分钟版本

可以先讲链路：设备事件 -> IRQ controller -> 逻辑 IRQ -> handler。然后讲驱动里如何通过 `platform_get_irq()` 和 `devm_request_threaded_irq()` 注册中断，再讲 top half / 下半部的分工，为什么共享中断要先判断来源，最后补 `/proc/interrupts` 和设备树在排障里的作用。

### 10 分钟版本

可以进一步结合一个真实设备展开：设备树描述 `interrupts`，驱动 `probe` 时取 IRQ 并注册线程化中断，top half 只检查状态并清必要 pending，threaded handler 负责读数据和上报。再往下讲电平触发和边沿触发的区别、`IRQF_ONESHOT` 的意义、共享中断返回 `IRQ_NONE`/`IRQ_HANDLED` 的区别，以及如何通过 `/proc/interrupts`、日志和寄存器联合排障。这会很贴近工作场景。

## 实战练习

1. 给一个 platform 驱动增加线程化中断，要求 top half 只做状态确认，threaded handler 完成主要处理。
2. 故意把设备树中的触发类型改错，观察 `/proc/interrupts` 计数和系统现象变化，并写出排查记录。
3. 设计一个共享中断场景，在 handler 里增加“来源判断”前后对比日志和行为。
4. 用一个 GPIO 中断设备，验证“设备侧清 pending 的顺序”对中断风暴的影响。

## 关键要点

1. Linux 中断问题通常不是单点 API 问题，而是一整条链的问题。
2. 逻辑 IRQ、设备树、触发类型和设备侧 pending 语义必须一起理解。
3. 线程化中断是非常常见且实用的工程方案。
4. `/proc/interrupts`、日志和寄存器状态，是 Linux IRQ 排障的三件套。

## 关联笔记

1. `Linux-总览`
2. `Linux-设备树`
3. `Linux-内核日志与排障`
4. `驱动开发-platform驱动基础`
5. `RTOS-中断与任务协作`
6. `硬件-寄存器与地址映射`
