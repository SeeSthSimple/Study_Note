# 驱动开发-devm资源管理

## 原始问题

为什么很多现代 Linux 驱动都用 `devm_kzalloc` 而不是 `kmalloc`？为什么 `probe` 失败时不用手动释放资源？`devm_*` 系列函数到底做了什么，什么时候该用它，什么时候不该用？

## 先给结论

`devm`（Device Resource Management）的本质是"把资源的生命周期绑定到设备生命周期"：

1. `devm_*` 申请的资源在设备移除或 `probe` 失败时自动释放，不需要手动 `free`。
2. 它解决的核心问题是"驱动错误路径和 `remove` 路径里的资源泄漏"。
3. 常用 `devm_*` 接口覆盖了内存、IRQ、IO 映射、时钟、GPIO、PWM 等大部分资源。
4. 不是所有资源都适合 `devm`，有自定义释放逻辑或特殊时序要求的资源仍需手动管理。
5. `devm` 的核心收益不是"少写几行代码"，而是"让错误路径和 `remove` 路径更不容易出错"。

如果只能背"devm 就是自动释放"，但讲不清它怎么绑定到设备、什么资源适合用它、什么场景不适合，就还没有真正掌握 `devm` 资源管理。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. `probe` 里申请了很多资源，失败时回滚代码又长又容易漏。
2. `remove` 里忘记释放某个资源，导致内存泄漏或中断残留。
3. 不确定哪些资源有 `devm` 版本，哪些必须手动管理。
4. 不理解 `devm` 的实现原理，担心它会不会有性能问题或隐藏风险。
5. 面试时知道"现代驱动推荐用 devm"，但讲不清为什么和怎么用。

它在AI时代依然重要，因为AI生成驱动代码时经常混用 `devm` 和手动管理，导致资源释放不一致。理解 `devm` 的边界后，你才能判断AI给的代码是否在资源管理上自洽。

## 核心概念 / 本质机制

### 1. 没有 devm 时的痛点

传统驱动里，`probe` 和 `remove` 的资源管理很繁琐：

```c
static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    int ret;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) return -ENOMEM;

    priv->base = ioremap(res->start, resource_size(res));
    if (!priv->base) { ret = -ENOMEM; goto fail_ioremap; }

    ret = request_irq(priv->irq, my_handler, 0, "mydev", priv);
    if (ret) goto fail_irq;

    priv->clk = clk_get(&pdev->dev, NULL);
    if (IS_ERR(priv->clk)) { ret = PTR_ERR(priv->clk); goto fail_clk; }

    ret = clk_prepare_enable(priv->clk);
    if (ret) goto fail_clk_enable;

    return 0;

fail_clk_enable:
    clk_put(priv->clk);
fail_clk:
    free_irq(priv->irq, priv);
fail_irq:
    iounmap(priv->base);
fail_ioremap:
    kfree(priv);
    return ret;
}

static int my_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);

    clk_disable_unprepare(priv->clk);
    clk_put(priv->clk);
    free_irq(priv->irq, priv);
    iounmap(priv->base);
    kfree(priv);
    return 0;
}
```

问题：

1. `probe` 失败路径要按反序释放，容易漏。
2. `remove` 要跟 `probe` 完全对称，新增资源时容易忘。
3. 代码量大，维护成本高。

### 2. devm 如何解决

`devm` 把资源注册到设备的资源列表里，设备移除时自动按反序释放：

```c
static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    int ret;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) return -ENOMEM;

    priv->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(priv->base)) return PTR_ERR(priv->base);

    ret = devm_request_irq(&pdev->dev, priv->irq, my_handler, 0, "mydev", priv);
    if (ret) return ret;

    priv->clk = devm_clk_get(&pdev->dev, NULL);
    if (IS_ERR(priv->clk)) return PTR_ERR(priv->clk);

    ret = clk_prepare_enable(priv->clk);
    if (ret) return ret;

    return 0;
}

static int my_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);

    clk_disable_unprepare(priv->clk);
    return 0;
}
```

对比：

1. `probe` 失败路径只需要 `return ret`，不需要 goto 链。
2. `remove` 只需要处理非 devm 资源。
3. 代码量大幅减少，遗漏风险大幅降低。

### 3. devm 的实现原理

`devm` 的核心机制是"设备资源列表"：

1. 每个 `struct device` 内部有一个 `devres_head` 链表。
2. `devm_*` 申请资源时，把资源和释放函数注册到这个链表。
3. 设备移除时（`device_del`），内核遍历链表，按注册反序调用释放函数。

简化版实现：

```c
struct devres_node {
    struct list_head entry;
    dr_release_t release;  /* 释放函数 */
};

struct devres {
    struct devres_node node;
    unsigned long long data[];  /* 资源数据 */
};
```

### 4. 常用 devm 接口

| 手动接口 | devm 接口 | 释放时机 |
| --- | --- | --- |
| `kmalloc` / `kzalloc` | `devm_kzalloc` | 设备移除时自动 `kfree` |
| `ioremap` | `devm_ioremap_resource` | 设备移除时自动 `iounmap` |
| `request_irq` | `devm_request_irq` | 设备移除时自动 `free_irq` |
| `clk_get` | `devm_clk_get` | 设备移除时自动 `clk_put` |
| `gpio_request` | `devm_gpio_request` | 设备移除时自动 `gpio_free` |
| `pwm_request` | `devm_pwm_get` | 设备移除时自动 `pwm_free` |
| `regulator_get` | `devm_regulator_get` | 设备移除时自动 `regulator_put` |
| `phy_get` | `devm_phy_get` | 设备移除时自动 `phy_put` |
| `reset_control_get` | `devm_reset_control_get` | 设备移除时自动释放 |
| `ioremap_uc` / `ioremap_wc` | `devm_ioremap_uc` / `devm_ioremap_wc` | 设备移除时自动 `iounmap` |

### 5. 什么资源不适合 devm

1. **需要提前释放的资源**：如果资源需要在 `remove` 之前就释放（比如先关时钟再断电），`devm` 的自动释放时序可能不满足。
2. **有自定义释放逻辑的资源**：如果释放前需要做额外操作（如等待硬件就绪），纯 `devm` 不够用。
3. **资源生命周期跟设备不一致**：如果资源在设备移除后还需要存在，不能用 `devm`。
4. **`clk_prepare_enable`**：注意 `clk_get` 有 `devm_clk_get`，但 `clk_prepare_enable` 没有 `devm` 版本，需要手动 `clk_disable_unprepare`。

### 6. 自定义 devm 资源

如果需要把自定义资源纳入 `devm` 管理，可以用 `devm_add_action`：

```c
static void my_custom_release(void *data)
{
    struct my_resource *res = data;
    /* 自定义释放逻辑 */
    my_resource_cleanup(res);
}

static int my_probe(struct platform_device *pdev)
{
    struct my_resource *res;

    res = allocate_my_resource();
    if (!res) return -ENOMEM;

    ret = devm_add_action_or_reset(&pdev->dev, my_custom_release, res);
    if (ret) return ret;

    /* res 会在设备移除时自动通过 my_custom_release 释放 */
    return 0;
}
```

`devm_add_action_or_reset`：

1. 注册成功后，设备移除时自动调用 `my_custom_release`。
2. 如果注册失败，立即调用 `my_custom_release` 释放资源。
3. 是把自定义资源纳入 `devm` 管理的标准方式。

### 7. devm 和 probe 失败路径

`devm` 最大的价值在 `probe` 失败路径：

```c
static int my_probe(struct platform_device *pdev)
{
    void __iomem *base;
    int irq, ret;

    base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(base)) return PTR_ERR(base);  // 失败直接返回，无需清理

    irq = platform_get_irq(pdev, 0);
    if (irq < 0) return irq;  // 失败直接返回，base 自动释放

    ret = devm_request_irq(&pdev->dev, irq, handler, 0, "mydev", priv);
    if (ret) return ret;  // 失败直接返回，base 和之前资源自动释放

    return 0;
}
```

没有 `devm` 时，每一步失败都需要手动释放之前申请的所有资源。

### 8. devm 的释放顺序

`devm` 资源按注册的反序释放，这很重要：

1. 最后注册的最先释放。
2. 这跟手动管理的"先申请后释放"顺序一致。
3. 如果释放顺序有特殊要求，可以用 `devm_add_action` 显式控制。

## 数据流 / 控制流 / 时序关系

```text
probe 成功路径:
  devm_kzalloc -> devm_ioremap_resource -> devm_request_irq -> devm_clk_get
  所有资源注册到设备资源列表

probe 失败路径:
  任何一步失败 -> return ret
  -> 内核自动释放已注册的 devm 资源（按反序）

remove 路径:
  手动释放非 devm 资源（如 clk_disable_unprepare）
  -> 内核自动释放所有 devm 资源（按反序）
```

## 最小可运行示例

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/clk.h>

struct mydev_priv {
    void __iomem *base;
    int irq;
    struct clk *clk;
};

#define REG_CTRL   0x00
#define CTRL_ENABLE BIT(0)

static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    return IRQ_HANDLED;
}

static void my_clk_disable(void *data)
{
    struct clk *clk = data;
    clk_disable_unprepare(clk);
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

    ret = devm_request_irq(&pdev->dev, priv->irq, my_irq_handler, 0,
                           "mydev", priv);
    if (ret) return ret;

    priv->clk = devm_clk_get(&pdev->dev, NULL);
    if (IS_ERR(priv->clk)) return PTR_ERR(priv->clk);

    ret = clk_prepare_enable(priv->clk);
    if (ret) return ret;

    ret = devm_add_action_or_reset(&pdev->dev, my_clk_disable, priv->clk);
    if (ret) return ret;

    writel(CTRL_ENABLE, priv->base + REG_CTRL);

    platform_set_drvdata(pdev, priv);
    dev_info(&pdev->dev, "probed\n");
    return 0;
}

static int my_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);
    writel(0, priv->base + REG_CTRL);
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

### 1. 为什么 `clk_prepare_enable` 没有 devm 版本

因为时钟的使能/禁用需要精确控制时序：先禁用时钟，再断电，再释放其他资源。如果用 `devm` 自动释放，释放时序可能不对。

### 2. 为什么用 `devm_add_action_or_reset` 包装 `clk_disable_unprepare`

这是把"需要手动管理"的资源纳入 `devm` 体系的标准方式。设备移除时，`my_clk_disable` 会被自动调用。

### 3. 为什么 `devm_add_action_or_reset` 而不是 `devm_add_action`

`devm_add_action_or_reset` 在注册失败时会立即调用释放函数，避免资源泄漏。`devm_add_action` 不会。

### 4. 为什么 `remove` 里只需要禁用硬件

因为所有资源（内存、IO映射、中断、时钟）都通过 `devm` 管理，设备移除时自动释放。`remove` 只需要做"让硬件停止工作"这件事。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 内存分配 | `devm_kzalloc` | `kmalloc` + 手动 `kfree` | 错误路径容易漏释放 |
| IO 映射 | `devm_ioremap_resource` | `ioremap` + 手动 `iounmap` | 同上 |
| 中断注册 | `devm_request_irq` | `request_irq` + 手动 `free_irq` | 同上 |
| 时钟使能 | `clk_prepare_enable` + `devm_add_action_or_reset` | `clk_prepare_enable` 不配对禁用 | 时钟泄漏 |
| 自定义资源 | `devm_add_action_or_reset` | 不注册释放函数 | 资源泄漏 |
| 混合管理 | devm 和手动管理不要交叉管理同一资源 | 同一资源既 devm 又手动释放 | 双重释放 |

## 边界条件与适用范围

1. `devm` 资源的生命周期跟 `struct device` 绑定，不是跟模块绑定。
2. 如果设备被多次 probe/remove（如热插拔），`devm` 会正确处理每次的资源。
3. `devm_ioremap_resource` 跟 `devm_ioremap` 不同：前者会检查资源冲突，后者不会。
4. `devm_kzalloc` 分配的内存是 GFP_KERNEL，不适合原子上下文。
5. 不要在 `devm` 释放回调里做可能失败的操作。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 设备移除后内存泄漏 | 混用了非 devm 分配 | 检查是否有裸 `kmalloc` |
| 双重释放 | 同一资源既 devm 又手动释放 | 确认每个资源只用一种管理方式 |
| 释放顺序错误 | 依赖隐式的 devm 释放顺序 | 用 `devm_add_action` 显式控制 |
| 时钟未禁用 | `clk_prepare_enable` 没有 `devm` 包装 | 加 `devm_add_action_or_reset` |
| probe 失败后资源残留 | 用了非 devm 接口 | 改用 devm 版本 |

## 工程落地建议

### 1. 新驱动默认全部用 devm

除非有明确理由不用 `devm`，否则所有资源申请都应该用 `devm` 版本。

### 2. 没有 devm 版本的资源用 `devm_add_action_or_reset`

这是把任何资源纳入 `devm` 管理的通用方案。

### 3. `remove` 函数只做"让硬件停止"

不需要手动释放 `devm` 资源，只需要让硬件进入安全状态。

### 4. 代码评审时重点检查

1. 是否有裸 `kmalloc` / `ioremap` / `request_irq` 没有配对释放。
2. 是否有同一资源被 `devm` 和手动双重管理。
3. `clk_prepare_enable` 是否有对应的禁用逻辑。

## 性能、稳定性、可维护性影响

1. `devm` 的运行时开销极小（只是往链表加一个节点），不影响性能。
2. `devm` 的最大收益是稳定性和可维护性：减少资源泄漏和双重释放。
3. `devm` 让 `probe` 失败路径从"容易出错的手动回滚"变成"自动安全回滚"。
4. `devm` 让 `remove` 函数从"冗长的释放链"变成"只做硬件停止"。
5. 真正好的驱动代码，不是"手动管理很仔细"，而是"让机制保证不出错"。

## 面试 / 问答怎么讲

### 30 秒版本

`devm` 把资源生命周期绑定到设备生命周期，设备移除时自动释放。常用接口包括 `devm_kzalloc`、`devm_ioremap_resource`、`devm_request_irq` 等。没有 `devm` 版本的资源可以用 `devm_add_action_or_reset` 包装。核心收益是让 `probe` 失败路径和 `remove` 路径更不容易出错。

### 3 分钟版本

可以从痛点讲起：传统驱动的 `probe` 失败路径需要大量 goto 回滚，`remove` 要跟 `probe` 完全对称，容易遗漏。然后说明 `devm` 的原理：把资源和释放函数注册到设备资源列表，设备移除时自动按反序释放。再列举常用 `devm` 接口。最后补充不适合 `devm` 的场景：需要提前释放或有特殊时序要求的资源。

### 10 分钟版本

可以结合一个完整驱动展开：对比传统写法和 `devm` 写法的 `probe`/`remove` 代码量。然后说明 `clk_prepare_enable` 为什么没有 `devm` 版本，以及如何用 `devm_add_action_or_reset` 包装。再讨论 `devm` 的释放顺序保证和自定义释放逻辑。最后说明代码评审时的重点检查项。

## 实战练习

1. 把一个用 `kmalloc` + `ioremap` + `request_irq` 的驱动改成全 `devm` 版本，对比代码量。
2. 用 `devm_add_action_or_reset` 包装一个自定义资源的释放逻辑。
3. 故意在 `probe` 中间返回错误，观察 `devm` 是否正确释放了之前的资源。
4. 对比 `devm_ioremap_resource` 和 `devm_ioremap` 的区别，说明为什么推荐前者。
5. 写一个混合管理（部分 devm、部分手动）的驱动，标注哪些必须手动、为什么。

## 关键要点

1. `devm` 把资源生命周期绑定到设备生命周期，自动释放。
2. 常用接口：`devm_kzalloc`、`devm_ioremap_resource`、`devm_request_irq`、`devm_clk_get`。
3. 没有 `devm` 版本的资源用 `devm_add_action_or_reset` 包装。
4. `devm` 的核心收益是让 `probe` 失败路径和 `remove` 路径更不容易出错。
5. 不是所有资源都适合 `devm`：需要提前释放或有特殊时序要求的资源仍需手动管理。
6. 不要对同一资源既用 `devm` 又手动管理，避免双重释放。
7. `devm_ioremap_resource` 比 `devm_ioremap` 更推荐，因为它会检查资源冲突。

## 关联笔记

1. `驱动开发-platform驱动基础`
2. `驱动开发-IRQ处理基础`
3. `C++-RAII与资源管理`
4. `C与C++-内存管理`
5. `Linux-设备树`
6. `C与C++-接口设计与错误处理`
