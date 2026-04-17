# 驱动开发-IRQ处理基础

## 原始问题

Linux 驱动里中断处理到底怎么写？为什么有"上半部"和"下半部"之分？`request_irq` 的参数怎么填？为什么中断里不能做耗时操作？中断共享、中断线程化、工作队列、tasklet 这些概念到底怎么选？

## 先给结论

Linux 中断处理的核心原则是"上半部快进快出，下半部延迟处理"：

1. 中断处理程序（上半部）在关中断或部分关中断状态下执行，必须尽可能短。
2. 耗时操作必须推迟到下半部处理：软中断、tasklet、工作队列、线程化中断。
3. `request_irq` 注册中断，`free_irq` 释放，必须成对出现。
4. 中断里不能睡眠、不能调用可能阻塞的函数、不能访问用户态内存。
5. 共享中断必须检查是否是自己的设备产生的中断。
6. 现代内核推荐用 `devm_request_irq` 自动管理中断资源。

如果只能背"中断分上下半部"，但讲不清为什么、怎么选下半部机制、中断里什么能做什么不能做、共享中断怎么处理，就还没有真正掌握 Linux IRQ 处理。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 知道要注册中断，但不确定 `request_irq` 各参数含义和 flags 怎么选。
2. 中断里做了耗时操作导致系统卡死或丢中断。
3. 不清楚软中断、tasklet、工作队列各自的特点和选择标准。
4. 共享中断时驱动不知道怎么判断中断来源。
5. 面试时知道"中断不能睡眠"，但讲不清为什么、什么场景下会出问题。

它在AI时代依然重要，因为AI生成驱动代码时经常在中断处理里做不安全的操作（如加锁睡眠、调用可能阻塞的函数），理解 IRQ 处理规则后，你才能判断AI给的代码是否在中断上下文里做了不该做的事。

## 核心概念 / 本质机制

### 1. Linux 中断处理为什么要分上下半部

原因很简单：**中断处理期间，系统可能无法响应其他中断**。

如果中断处理程序执行时间过长：

1. 其他中断被延迟响应，可能导致数据丢失。
2. 系统实时性下降。
3. 看门狗可能超时复位。

所以 Linux 把中断处理分成两部分：

1. **上半部（Top Half）**：中断处理程序，只做最紧急的工作（读取数据、确认中断、调度下半部）。
2. **下半部（Bottom Half）**：延迟处理，做剩余的耗时工作（数据处理、唤醒等待队列、通知用户态）。

### 2. `request_irq` 详解

```c
int request_irq(unsigned int irq,
                irq_handler_t handler,
                unsigned long flags,
                const char *name,
                void *dev_id);
```

参数含义：

1. `irq`：中断号，从设备树或平台资源获取。
2. `handler`：中断处理函数，上半部。
3. `flags`：中断属性标志。
4. `name`：中断名称，出现在 `/proc/interrupts` 里。
5. `dev_id`：设备私有数据，传给 handler，共享中断时必须唯一。

常用 flags：

| Flag | 含义 |
| --- | --- |
| `IRQF_SHARED` | 共享中断，多个设备共用一个中断线 |
| `IRQF_TRIGGER_RISING` | 上升沿触发 |
| `IRQF_TRIGGER_FALLING` | 下降沿触发 |
| `IRQF_TRIGGER_HIGH` | 高电平触发 |
| `IRQF_TRIGGER_LOW` | 低电平触发 |
| `IRQF_ONESHOT` | 线程化中断时，上半部处理后不重新使能，等线程函数执行完再使能 |

### 3. 中断处理函数的写法

```c
static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    struct mydev_priv *priv = dev_id;

    if (!is_my_interrupt(priv)) {
        return IRQ_NONE;  // 不是我的中断
    }

    /* 上半部：只做最紧急的事 */
    uint32_t status = readl(priv->base + REG_STATUS);
    writel(status, priv->base + REG_ACK);  // 清中断

    /* 调度下半部 */
    schedule_work(&priv->work);

    return IRQ_HANDLED;
}
```

返回值：

1. `IRQ_HANDLED`：中断已处理。
2. `IRQ_NONE`：不是我的中断（共享中断时必须检查）。

### 4. 中断里绝对不能做的事

| 不能做 | 原因 |
| --- | --- |
| 睡眠（`msleep`、`schedule`） | 中断上下文没有进程调度，睡眠会导致系统死锁 |
| 调用可能阻塞的函数 | 同上 |
| 访问用户态内存（`copy_to_user`） | 中断上下文没有用户态地址空间 |
| 获取可能睡眠的锁（`mutex`） | `mutex` 可能睡眠，只能用 `spinlock` |
| 执行耗时操作 | 影响系统响应性 |
| 调用 `printk` 过多 | `printk` 可能阻塞，大量输出影响性能 |

### 5. 下半部机制选择

| 机制 | 执行上下文 | 可睡眠 | 适用场景 |
| --- | --- | --- | --- |
| 软中断（softirq） | 软中断上下文 | 否 | 网络子系统等极高频率场景 |
| tasklet | 软中断上下文 | 否 | 中等频率，不能睡眠的延迟处理 |
| 工作队列（workqueue） | 进程上下文 | 是 | 需要睡眠或调用的函数可能睡眠 |
| 线程化中断（threaded IRQ） | 内核线程 | 是 | 驱动整体中断处理都放在线程里 |

选择原则：

1. 不需要睡眠 → tasklet 或软中断。
2. 需要睡眠 → 工作队列或线程化中断。
3. 不确定 → 用工作队列，最安全。

### 6. 工作队列用法

```c
struct mydev_priv {
    struct work_struct work;
    /* ... */
};

static void my_work_handler(struct work_struct *work)
{
    struct mydev_priv *priv = container_of(work, struct mydev_priv, work);

    /* 下半部：可以做耗时操作 */
    process_data(priv);
    wake_up_interruptible(&priv->wait_queue);
}

/* 初始化 */
INIT_WORK(&priv->work, my_work_handler);

/* 在中断上半部调度 */
schedule_work(&priv->work);
```

工作队列的特点：

1. 在进程上下文执行，可以睡眠。
2. 可以调用可能阻塞的函数。
3. 可以访问用户态内存（如果需要）。
4. 是最通用的下半部机制。

### 7. 线程化中断

```c
int ret = request_threaded_irq(irq,
                               my_hard_handler,   // 上半部，可为 NULL
                               my_thread_fn,      // 线程函数
                               IRQF_ONESHOT,
                               "mydev", priv);
```

线程化中断的特点：

1. 上半部在硬中断上下文执行，线程函数在内核线程执行。
2. `IRQF_ONESHOT` 保证线程函数执行完才重新使能中断。
3. 如果上半部为 NULL，内核提供一个默认的"唤醒线程"处理。
4. 线程函数里可以睡眠，适合复杂中断处理。

### 8. 共享中断的处理

```c
static irqreturn_t my_shared_handler(int irq, void *dev_id)
{
    struct mydev_priv *priv = dev_id;

    /* 必须先检查是不是自己的设备产生的中断 */
    if (!(readl(priv->base + REG_STATUS) & STATUS_IRQ_PENDING)) {
        return IRQ_NONE;  // 不是我的中断，让其他 handler 处理
    }

    /* 处理中断 */
    writel(STATUS_IRQ_PENDING, priv->base + REG_ACK);
    schedule_work(&priv->work);

    return IRQ_HANDLED;
}

/* 注册时必须用 IRQF_SHARED */
request_irq(irq, my_shared_handler, IRQF_SHARED | IRQF_TRIGGER_FALLING,
            "mydev", priv);
```

共享中断的关键：

1. `flags` 必须包含 `IRQF_SHARED`。
2. `dev_id` 必须唯一（通常用设备私有数据指针）。
3. handler 必须先检查是否是自己的设备产生的中断。
4. 不是自己的中断必须返回 `IRQ_NONE`。

### 9. 中断号从哪里来

在设备树场景里：

```c
/* 设备树 */
mydev@10000000 {
    compatible = "vendor,mydev";
    reg = <0x10000000 0x1000>;
    interrupts = <0 25 4>;  /* GIC SPI 25, 触发类型 4 */
};

/* 驱动里获取 */
int irq = platform_get_irq(pdev, 0);
if (irq < 0) return irq;

ret = devm_request_irq(&pdev->dev, irq, my_handler,
                       IRQF_TRIGGER_FALLING, "mydev", priv);
```

## 数据流 / 控制流 / 时序关系

中断处理的完整链路：

```text
硬件产生中断
-> CPU 响应中断，进入内核中断入口
-> 内核查找中断号对应的 handler 链表
-> 依次调用链表上的 handler（共享中断）
-> handler 检查是否是自己的设备
  -> 不是：返回 IRQ_NONE
  -> 是：读取状态、清中断、调度下半部、返回 IRQ_HANDLED
-> 下半部在适当时机执行
  -> tasklet：软中断上下文，不能睡眠
  -> workqueue：进程上下文，可以睡眠
  -> threaded IRQ：内核线程，可以睡眠
-> 下半部完成数据处理、唤醒等待队列、通知用户态
```

## 最小可运行示例

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/io.h>

struct mydev_priv {
    void __iomem *base;
    int irq;
    struct work_struct work;
    spinlock_t lock;
    int event_count;
};

#define REG_STATUS  0x00
#define REG_ACK     0x04
#define STATUS_IRQ_PENDING BIT(0)

static void my_work_handler(struct work_struct *work)
{
    struct mydev_priv *priv = container_of(work, struct mydev_priv, work);

    spin_lock(&priv->lock);
    priv->event_count++;
    spin_unlock(&priv->lock);

    pr_info("mydev: work processed, count=%d\n", priv->event_count);
}

static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    struct mydev_priv *priv = dev_id;
    u32 status;

    status = readl(priv->base + REG_STATUS);
    if (!(status & STATUS_IRQ_PENDING)) {
        return IRQ_NONE;
    }

    writel(STATUS_IRQ_PENDING, priv->base + REG_ACK);
    schedule_work(&priv->work);

    return IRQ_HANDLED;
}

static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    struct resource *res;
    int ret;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    priv->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(priv->base)) return PTR_ERR(priv->base);

    priv->irq = platform_get_irq(pdev, 0);
    if (priv->irq < 0) return priv->irq;

    INIT_WORK(&priv->work, my_work_handler);
    spin_lock_init(&priv->lock);

    ret = devm_request_irq(&pdev->dev, priv->irq, my_irq_handler,
                           IRQF_TRIGGER_FALLING, "mydev", priv);
    if (ret) {
        dev_err(&pdev->dev, "request_irq failed: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, priv);
    dev_info(&pdev->dev, "probed, irq=%d\n", priv->irq);
    return 0;
}

static int my_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);
    cancel_work_sync(&priv->work);
    dev_info(&pdev->dev, "removed\n");
    return 0;
}

static const struct of_device_id my_match[] = {
    { .compatible = "vendor,mydev" },
    {}
};
MODULE_DEVICE_TABLE(of, my_match);

static struct platform_driver my_driver = {
    .probe  = my_probe,
    .remove = my_remove,
    .driver = {
        .name = "mydev",
        .of_match_table = my_match,
    },
};
module_platform_driver(my_driver);

MODULE_LICENSE("GPL");
```

## 代码解读

### 1. 为什么用 `devm_request_irq` 而不是 `request_irq`

`devm_request_irq` 在设备移除时自动释放中断，不需要在 `remove` 里手动调用 `free_irq`。

### 2. 为什么上半部用 `readl` / `writel`

这是访问 MMIO 寄存器的标准函数，保证内存访问顺序。

### 3. 为什么 `spin_lock` 而不是 `mutex`

因为 `event_count` 可能被中断处理程序和工作队列同时访问。中断上下文里不能用 `mutex`（可能睡眠），只能用 `spinlock`。

### 4. 为什么 `cancel_work_sync` 在 `remove` 里调用

确保工作队列里的任务在设备移除前执行完，避免 use-after-free。

### 5. 为什么 handler 里要检查 `STATUS_IRQ_PENDING`

即使不是共享中断，检查中断状态也是好习惯，可以防止误触发。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 中断里做耗时操作 | 上半部只清中断，下半部做处理 | 全部放在 handler 里 | 影响系统响应性 |
| 中断里加锁 | `spin_lock` / `spin_lock_irqsave` | `mutex_lock` | mutex 可能睡眠 |
| 共享中断 | 检查中断来源，返回 `IRQ_NONE` | 不检查直接处理 | 可能处理了别人的中断 |
| 中断注册 | `devm_request_irq` | `request_irq` 不配对 `free_irq` | 资源泄漏 |
| 工作队列取消 | `cancel_work_sync` | 不取消直接移除设备 | use-after-free |
| 中断触发类型 | 从设备树获取或显式指定 | 不指定触发类型 | 中断可能不触发或误触发 |

## 边界条件与适用范围

1. 中断处理函数不能睡眠，这是硬性约束，违反会导致系统死锁。
2. `spinlock` 在中断上下文里必须用 `spin_lock_irqsave` 保存中断状态，避免死锁。
3. 工作队列不是实时的，从调度到执行有延迟，不适合硬实时场景。
4. 线程化中断的线程优先级可以配置，适合需要保证处理时序的场景。
5. 中断号在不同平台上可能不同，必须从设备树或平台资源获取，不能硬编码。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 中断不触发 | 触发类型配置错、中断控制器未使能 | `cat /proc/interrupts` 看计数 |
| 系统卡死 | 中断里睡眠或做耗时操作 | 检查 handler 里是否有阻塞调用 |
| 中断丢失 | 上半部处理太慢 | 缩短上半部，推迟到下半部 |
| 共享中断处理错 | 不检查中断来源 | 加状态检查，返回 `IRQ_NONE` |
| 死锁 | 中断上下文用 `mutex` 或 `spin_lock` 不关中断 | 改用 `spin_lock_irqsave` |
| `free_irq` 崩溃 | `dev_id` 跟注册时不一致 | 确保释放时传同一个 `dev_id` |

### 推荐排查顺序

1. 先确认中断号是否正确：`cat /proc/interrupts`。
2. 再确认中断是否触发：看计数是否增加。
3. 再确认 handler 是否被调用：加 `pr_info`。
4. 再确认是否返回了正确的值（`IRQ_HANDLED` vs `IRQ_NONE`）。
5. 最后看下半部是否执行、是否有死锁或睡眠。

## 工程落地建议

### 1. 默认用 `devm_request_irq`

自动管理中断资源，减少 `remove` 里的清理代码和遗漏风险。

### 2. 上半部只做三件事

1. 读取中断状态。
2. 清除中断标志。
3. 调度下半部。

### 3. 下半部优先用工作队列

除非有极端性能要求，否则工作队列是最安全的选择。

### 4. 给中断处理加日志

```c
pr_debug("mydev: irq=%d, status=0x%x\n", irq, status);
```

用 `pr_debug` 而不是 `pr_info`，避免生产环境日志过多。

### 5. 测试中断是否正常

```bash
cat /proc/interrupts        # 看中断号和计数
cat /proc/irq/25/smp_affinity  # 看 CPU 亲和性
echo 1 > /proc/irq/25/smp_affinity  # 绑定到 CPU0
```

## 性能、稳定性、可维护性影响

1. 上半部处理时间直接影响系统响应性，越短越好。
2. 下半部机制的选择影响延迟和可维护性：工作队列最安全但延迟最大。
3. 共享中断如果处理不当，可能导致中断风暴或误处理。
4. 线程化中断可以设置优先级，适合实时性要求高的场景。
5. 中断里的 bug 最难排查，因为不可预测、不可重现，所以预防比排障更重要。

## 面试 / 问答怎么讲

### 30 秒版本

Linux 中断分上下半部：上半部在硬中断上下文执行，只做最紧急的事（读状态、清中断、调度下半部）；下半部在软中断或进程上下文执行，做耗时处理。中断里不能睡眠、不能用 mutex、不能访问用户态内存。下半部机制选工作队列最安全，需要睡眠就用工作队列或线程化中断。

### 3 分钟版本

可以从"为什么要分上下半部"讲起：中断处理期间系统可能无法响应其他中断，所以上半部必须快。然后说明 `request_irq` 的参数和 flags。再对比四种下半部机制：软中断、tasklet、工作队列、线程化中断。最后强调中断里的禁忌：不能睡眠、不能用 mutex、不能做耗时操作。

### 10 分钟版本

可以结合一个完整驱动展开：platform 驱动在 `probe` 里用 `devm_request_irq` 注册中断，handler 里读状态、清中断、调度工作队列。工作队列里做数据处理并唤醒等待队列。然后讨论共享中断的处理方式、线程化中断的适用场景、`spin_lock_irqsave` 为什么比 `spin_lock` 更安全。最后说明排障主线：`/proc/interrupts` → handler 日志 → 下半部执行 → 死锁排查。

## 实战练习

1. 写一个 platform 驱动，注册一个中断，在中断 handler 里打印日志并用工作队列延迟处理。
2. 故意在中断 handler 里调用 `msleep`，观察系统行为。
3. 用 `request_threaded_irq` 替代 `request_irq`，把处理逻辑移到线程函数里。
4. 配置一个共享中断，两个驱动都注册，验证 `IRQ_NONE` 的作用。
5. 用 `cat /proc/interrupts` 观察中断计数，确认中断是否正常触发。

## 关键要点

1. 中断分上下半部：上半部快进快出，下半部延迟处理。
2. 中断里不能睡眠、不能用 mutex、不能访问用户态内存。
3. `request_irq` 注册中断，`devm_request_irq` 自动管理资源。
4. 下半部机制：软中断 → tasklet → 工作队列 → 线程化中断，按需选择。
5. 共享中断必须检查中断来源，不是自己的返回 `IRQ_NONE`。
6. `spin_lock_irqsave` 是中断上下文里最安全的锁用法。
7. 中断里的 bug 最难排查，预防比排障更重要。

## 关联笔记

1. `驱动开发-platform驱动基础`
2. `驱动开发-devm资源管理`
3. `RTOS-中断与任务协作`
4. `C与C++-并发基础`
5. `Linux-核心基础`
6. `硬件-时钟与复位`
