# 驱动开发-platform驱动基础

## 原始问题

Linux 里的 platform 驱动到底是什么？为什么很多 SoC 内建外设都用 platform 驱动，驱动 `probe` 又是怎样和设备树、资源申请、初始化逻辑串起来的？

## 先给结论

platform 驱动的本质，是让“平台固定存在、无法自动枚举的设备”通过统一模型接入内核。

先记住下面几个结论：

1. platform 驱动最常服务于 SoC 内建外设或板级固定设备，比如 GPIO 控制器、UART、SPI 控制器、自定义 MMIO 设备。
2. 在设备树场景里，platform 驱动是否能 `probe`，关键往往不在 `.name`，而在 `compatible` 与 `of_match_table` 是否匹配。
3. `probe` 的核心任务不是“写一坨初始化代码”，而是按顺序完成资源获取、硬件初始化、错误处理和注册对外能力。
4. 新驱动优先使用 `devm_*` 资源管理接口，这会显著降低错误路径复杂度。
5. platform 驱动排障的主线通常是：节点是否存在 -> 驱动是否匹配 -> 资源是否申请成功 -> 初始化时序是否正确。

如果只能抄一个 `platform_driver` 模板，却讲不清平台设备从哪里来、`reg` 怎么变成 `resource`、为什么 `probe` 不进、为什么推荐 `devm_*`，说明这部分还没有真正掌握。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 学 Linux 驱动时知道有 `platform_driver`，但不知道它在系统里到底扮演什么角色。
2. `probe` 不进、资源申请失败、中断不工作时，没有稳定定位主线。
3. 面试时会背 platform 驱动，但讲不清它和设备树、platform bus、资源管理的关系。
4. 工作里需要给板级固定硬件写驱动，但不知道一份最小可维护的 platform 驱动应该长什么样。

它在 AI 时代仍然重要，因为 AI 能很快生成一份“能编译”的 `platform_driver` 模板，但它经常不知道你的 `compatible` 是否真的匹配、`reg` 是否正确、时钟复位依赖是否完整、错误路径是否足够稳。理解 platform 驱动主线后，你才能判断 AI 给出的代码到底只是样子像，还是工程上真的可用。

## 核心概念 / 本质机制

### 1. 什么样的设备适合 platform 驱动

platform 设备通常有一个共同点：

1. 它们不会像 PCIe、USB 那样自己枚举出来。
2. 它们通常是 SoC 内建或板级固定挂接的硬件资源。

例如：

1. SoC 内建 UART 控制器。
2. 某个固定地址的自定义 FPGA 寄存器块。
3. 固定挂在某平台上的控制器或辅助设备。

这类设备需要平台层提前把“这里有个设备，它的地址、中断、时钟、GPIO 等资源是什么”告诉内核，驱动再来接管。

### 2. platform 设备和 platform 驱动怎么遇到一起

设备和驱动能匹配，一般要经过这条链：

1. 设备树描述硬件节点。
2. 内核把节点转成平台设备。
3. 驱动注册 `platform_driver`。
4. 内核根据 `compatible` 或其他匹配条件把设备和驱动配对。
5. 匹配成功后进入 `probe`。

所以 platform 驱动不是凭空执行的，它必须先有对应的平台设备对象。

### 3. `probe` 到底在做什么

一个稳妥的 `probe` 通常会做下面几类事情：

1. 分配私有数据结构。
2. 获取 MMIO、IRQ、GPIO、clock、reset、regulator 等资源。
3. 做必要的硬件初始化。
4. 注册字符设备、子设备、中断处理或上层接口。
5. 在任一阶段失败时，能优雅返回错误。

所以 `probe` 的关键不是“代码写得多”，而是：

1. 初始化顺序合理。
2. 资源依赖清楚。
3. 错误路径可控。

### 4. 为什么 `devm_*` 很重要

platform 驱动特别容易在失败路径里拿很多资源：

1. `kzalloc`
2. `ioremap`
3. `request_irq`
4. `clk_get`
5. `gpiod_get`

如果都手工释放，错误路径很快就会变得很乱。

`devm_*` 的核心价值是：

1. 把资源生命周期绑定到设备。
2. 降低失败回滚复杂度。
3. 降低重构时引入泄漏的风险。

### 5. platform 驱动里的资源从哪里来

以设备树场景为例：

1. `reg` 通常会变成 `IORESOURCE_MEM`。
2. `interrupts` 会变成 `IORESOURCE_IRQ` 或相关 IRQ 信息。
3. `clocks`、`resets`、`gpios` 等由对应子系统接口解析。

这意味着驱动看到的不是原始设备树文本，而是经过内核框架整理后的资源接口。

### 6. 为什么 `probe` 不进是高频问题

因为这条链任何一处错了都可能导致：

1. 节点没启用。
2. `compatible` 不匹配。
3. 驱动没注册成功。
4. 运行中的系统根本没加载你修改后的设备树。

所以看到 `probe` 不进时，不要一上来就怀疑 `platform_driver` 结构体写错了，要先把设备出现这条链跑通。

## 数据流 / 控制流 / 时序关系

一条典型 platform 驱动链路可以先这样理解：

```text
设备树节点存在
-> 内核解析设备树并创建 platform_device
-> 驱动注册 platform_driver
-> of_match_table 匹配成功
-> 进入 probe
-> 获取寄存器 / IRQ / 时钟 / GPIO 等资源
-> 初始化硬件
-> 注册对外接口
-> 设备进入可用状态
```

这条链路里任何一环出错，最后现象都可能只是“设备不工作”。

## 最小可运行示例

### 1. 设备树节点示例

```dts
mydev@10010000 {
    compatible = "demo,my-platform-dev";
    reg = <0x10010000 0x1000>;
    interrupts = <12>;
    status = "okay";
};
```

### 2. 最小 platform 驱动示例

```c
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct mydev_priv {
    void __iomem *base;
    int irq;
};

static int mydev_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    struct resource *res;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    priv->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(priv->base))
        return PTR_ERR(priv->base);

    priv->irq = platform_get_irq(pdev, 0);
    if (priv->irq < 0)
        return priv->irq;

    platform_set_drvdata(pdev, priv);
    dev_info(&pdev->dev, "platform device probed\n");
    return 0;
}

static int mydev_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "platform device removed\n");
    return 0;
}

static const struct of_device_id mydev_of_match[] = {
    { .compatible = "demo,my-platform-dev" },
    { }
};
MODULE_DEVICE_TABLE(of, mydev_of_match);

static struct platform_driver mydev_driver = {
    .probe = mydev_probe,
    .remove = mydev_remove,
    .driver = {
        .name = "my_platform_dev",
        .of_match_table = mydev_of_match,
    },
};
module_platform_driver(mydev_driver);

MODULE_LICENSE("GPL");
```

这个示例体现了 platform 驱动最核心的闭环：

1. 设备树节点描述硬件。
2. 驱动通过 `of_match_table` 匹配节点。
3. `platform_get_resource()` 获取 `reg` 资源。
4. `devm_ioremap_resource()` 完成映射和自动回收。
5. `platform_get_irq()` 获取中断资源。

## 代码解读

### 1. 为什么 `platform_get_resource()` 能拿到 `reg`

因为平台设备在创建时，内核已经把设备树里的 `reg` 转成了 `resource` 结构。驱动不需要自己去解析文本。

### 2. 为什么 `devm_ioremap_resource()` 很常见

它同时做了两件事：

1. 检查资源合法性。
2. 把寄存器映射到内核可访问地址，并把释放过程交给设备管理框架。

这比手工 `ioremap()` 更稳妥。

### 3. 为什么 `.name` 不是设备树匹配主依据

很多人误以为 `.driver.name` 决定 platform 设备树匹配，这在设备树场景里并不准确。

更关键的是：

1. 节点里的 `compatible`
2. 驱动里的 `of_match_table`

`.name` 更像驱动自身的标识。

### 4. 为什么示例里还没有真正“让硬件工作”

因为这里只展示了 platform 驱动骨架。

真实项目里往往还要继续补：

1. 时钟使能。
2. 复位释放。
3. IRQ 注册。
4. 寄存器初始化。
5. 对外接口注册。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 匹配 | 用 `of_match_table` 对齐 `compatible` | 只改 `.name` 不看 `compatible` | 设备树场景下可能根本不匹配 |
| MMIO 获取 | `platform_get_resource()` + `devm_ioremap_resource()` | 驱动里硬编码物理地址 | 可移植性和维护性差 |
| 私有数据 | 用 `devm_kzalloc()` 和 `platform_set_drvdata()` | 到处用全局变量 | 多实例和维护都差 |
| 资源释放 | 优先 `devm_*` | 手工资源申请却漏释放 | 错误路径容易出问题 |
| 错误处理 | 失败后立即返回明确错误 | 失败继续往后跑 | 容易引入更隐蔽问题 |
| 排障路径 | 先看节点和匹配，再看初始化 | 一上来只盯 `probe` 代码细节 | 容易在错误层面耗时 |

## 边界条件与适用范围

1. 这篇笔记聚焦设备树场景下的 platform 驱动基础，不覆盖所有总线类型。
2. I2C、SPI、PCIe 等自带总线语义的设备，虽然也和 platform 有关联，但驱动模型会更具体。
3. 某些 platform 设备是代码静态注册的，不一定来自设备树，但总体资源管理主线相似。
4. platform 驱动只是内核设备模型的一部分，真正复杂驱动还会叠加 DMA、runtime PM、子设备和并发控制。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| `probe` 不进 | `compatible` 不匹配；节点没启用；驱动没注册 | 看运行中的设备树、`dmesg` 和匹配表 |
| MMIO 映射失败 | `reg` 错；地址冲突；资源非法 | 看错误日志和设备树地址 |
| IRQ 获取失败 | `interrupts` 写错；控制器没启用 | 看设备树和 `platform_get_irq()` 返回值 |
| 驱动加载了但硬件不工作 | 时钟、复位、GPIO 依赖没补齐 | 查依赖资源和初始化顺序 |
| 改了设备树没效果 | 系统没加载新 dtb | 看启动日志和运行中设备树内容 |

### 推荐排查顺序

1. 先确认运行中的设备树是否真的有这个节点。
2. 再确认节点的 `compatible` 和驱动匹配表是否一致。
3. 再确认 `reg`、`interrupts`、`status` 等核心资源是否正确。
4. 再看 `probe` 里具体是卡在资源申请还是硬件初始化。
5. 最后再看更细的寄存器、中断和时序问题。

## 工程落地建议

### 1. `probe` 里只做必要初始化

推荐顺序通常是：

1. 申请资源。
2. 初始化硬件到安全可用状态。
3. 注册对外接口。

不要把大量无关逻辑都堆到 `probe` 里。

### 2. 错误日志要明确指出是哪类资源失败

比如：

1. `failed to map registers`
2. `failed to get irq`
3. `failed to enable clock`

这类日志对排障价值很高。

### 3. 驱动代码里尽量保留清晰的初始化顺序

像“时钟 -> 复位 -> MMIO -> IRQ -> 子模块注册”这条主线要尽量写清楚，否则后续维护很容易引入顺序类 bug。

## 性能、稳定性、可维护性影响

1. platform 驱动写法是否规范，直接影响新板适配和长期维护成本。
2. 使用 `devm_*`、标准资源接口和清晰错误日志，能显著提高驱动稳定性和可维护性。
3. 很多驱动问题不是代码不够多，而是初始化顺序、资源依赖和错误路径没设计好。

## 面试 / 问答怎么讲

### 30 秒版本

platform 驱动主要用于 SoC 内建或板级固定设备。设备树先描述硬件，内核把它变成 platform_device，驱动通过 `of_match_table` 匹配后进入 `probe`，再申请寄存器、中断、时钟等资源并初始化硬件。

### 3 分钟版本

可以先讲为什么需要 platform 驱动：很多板级设备不能像 PCIe、USB 那样枚举。然后讲设备树节点如何生成 platform_device，驱动如何通过 `compatible` 匹配进入 `probe`，再说明 `platform_get_resource()`、`devm_ioremap_resource()`、`platform_get_irq()` 这类接口各自解决什么问题。最后强调错误路径和依赖资源的重要性。

### 10 分钟版本

可以再结合一个真实驱动案例展开：节点通过设备树提供 `reg`、`interrupts`、`clocks` 等信息，平台驱动 `probe` 时先申请资源，再做时钟使能、复位释放、寄存器初始化和 IRQ 注册。然后说明 `probe` 不进时该怎么排查，为什么很多问题其实卡在节点没生效或资源依赖没配齐，而不是代码逻辑本身。这样讲更有工程味道。

## 实战练习

1. 写一个最小 platform 驱动，只获取 MMIO 资源并打印日志。
2. 故意把设备树里的 `compatible` 改错一次，验证为什么 `probe` 不进。
3. 再给驱动补一个 IRQ 资源申请，设计一条完整排查链路。
4. 把手工 `ioremap()` 改成 `devm_ioremap_resource()`，比较错误路径复杂度。

## 关键要点

1. platform 驱动承接的是“固定存在设备”的驱动接入主线。
2. 在设备树场景里，`compatible` 和 `of_match_table` 是匹配关键。
3. `probe` 的核心是资源获取、初始化顺序和错误处理。
4. 排障时先查设备出现链，再查驱动初始化链。

## 关联笔记

1. `驱动开发-总览`
2. `Linux-设备树`
3. `Linux-pinctrl与GPIO`
4. `板级与启动-Bootloader启动流程`

