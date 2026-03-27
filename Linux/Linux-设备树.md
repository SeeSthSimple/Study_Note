# Linux-设备树

## 原始问题

Linux 设备树到底是什么？为什么很多 ARM 板级开发离不开它？一个 `.dts` 节点最终是怎么让内核找到并驱动设备的？

## 先给结论

设备树的核心作用，不是“写一份硬件配置文件”，而是把“板级硬件描述”从驱动代码里拆出来，让同一份驱动能复用于不同硬件布局。

先记住四个结论：

1. 设备树描述的是“这块板子上有什么硬件，以及这些硬件怎么连”，不是描述驱动算法。
2. 驱动里最关键的匹配点通常是 `compatible`，它决定内核能不能把某个节点交给对应驱动。
3. `dts` 是源码，`dtb` 是编译后的二进制，启动时一般由 bootloader 把 `dtb` 传给内核。
4. 设备树问题的本质排查链路通常是：节点有没有进内核、属性有没有被正确解析、驱动有没有匹配上、资源有没有申请成功。

如果只会抄节点格式，但讲不清 `compatible`、`reg`、`interrupts`、`status`、驱动匹配和 `/proc/device-tree` 的关系，说明还没有真正掌握设备树。

## 这个知识解决什么问题

设备树主要解决下面几类问题：

1. 同一个 SoC 可以做很多不同开发板，但外设挂接方式、引脚复用、地址、时钟、GPIO 连接不一样，不能把这些板级差异硬编码进驱动。
2. 驱动开发需要知道设备的寄存器地址、中断号、时钟、复位脚、电源、总线从地址等资源来源。
3. 板子启动后设备不工作时，需要快速判断是“设备树写错了”，还是“驱动没匹配上”，还是“资源申请失败”。
4. 面试或工作中，经常要解释“驱动和硬件描述是怎么解耦的”，设备树就是核心入口。

它在 AI 时代也很重要，因为 AI 可以生成一个看起来像样的 `.dts` 片段，但它经常不知道你的 SoC 手册、地址空间、时钟树、引脚复用和驱动匹配表是否真实一致。设备树基础越扎实，你越能校验 AI 生成内容是不是“格式对但语义错”。

## 核心概念 / 本质机制

### 1. 设备树的本质

设备树可以理解为一棵描述硬件拓扑的树：

1. 每个节点表示一个硬件设备、总线、控制器或功能块。
2. 节点里的属性表示这个硬件的资源和约束。
3. 父子关系通常表示总线层级或从属关系，比如 I2C 控制器下面挂 I2C 从设备。

它不是“谁都要靠设备树发现”。

通常更适合用设备树描述的是：

1. SoC 内部外设。
2. 板级固定连接的设备。
3. 不能靠枚举自动发现的硬件。

通常不靠设备树描述的是：

1. PCIe 设备，因为它能枚举发现。
2. USB 外设，因为它能枚举发现。
3. 纯软件逻辑，不对应真实硬件资源时不应该滥用设备树。

### 2. 为什么设备树能让驱动复用

没有设备树时，驱动往往会把板级信息写死：

1. 寄存器地址写死。
2. GPIO 编号写死。
3. 中断号写死。
4. 不同开发板就得改驱动源码。

有了设备树之后：

1. 驱动只关心“我要哪些资源”。
2. 板级 `.dts` 负责提供这些资源。
3. 驱动通过设备树接口读 `reg`、`interrupts`、`gpios`、`clocks` 等属性。
4. 这样同一个驱动能复用于多块板子。

### 3. 最常见的关键属性

1. `compatible`：设备和驱动的匹配关键字。
2. `reg`：寄存器地址范围，通常和 `#address-cells`、`#size-cells` 一起解释。
3. `interrupts`：中断资源。
4. `status`：节点是否启用，最常见是 `"okay"` 和 `"disabled"`。
5. `clocks`、`clock-names`：时钟依赖。
6. `resets`、`reset-names`：复位依赖。
7. `gpios`：GPIO 资源。
8. `pinctrl-names`、`pinctrl-0`：引脚复用配置。

### 4. `compatible` 为什么最重要

`compatible` 本质上是“硬件身份标签”。驱动里会有一个匹配表：

```c
static const struct of_device_id mydrv_of_match[] = {
    { .compatible = "vendor,my-device" },
    { }
};
MODULE_DEVICE_TABLE(of, mydrv_of_match);
```

当内核扫描到设备树节点：

```dts
mydev@10000000 {
    compatible = "vendor,my-device";
    reg = <0x10000000 0x1000>;
};
```

它就有机会把这个节点和这个驱动匹配起来。

如果 `compatible` 写错，最典型的现象就是：

1. 节点明明在设备树里。
2. 驱动也编进内核了。
3. 但 `probe` 根本没进。

## 数据流 / 控制流 / 时序关系

设备树从源码到驱动运行，大致经过下面这条链路：

1. 开发者编写 `.dts` 或 `.dtsi`。
2. `dtc` 把它编译成 `.dtb`。
3. bootloader 在启动内核时把 `dtb` 地址传给内核。
4. 内核解包设备树，把节点转换成内核里的设备描述结构。
5. 平台总线或对应子系统根据节点创建设备。
6. 驱动注册自己的 `of_match_table`。
7. 内核按 `compatible` 等信息做匹配。
8. 匹配成功后进入驱动 `probe`。
9. `probe` 再解析 `reg`、`interrupts`、`gpios`、`clocks` 等资源。
10. 资源申请和初始化成功，设备开始工作。

可以把它理解成下面这个最小时序：

```text
DTS -> DTB -> Bootloader -> Kernel unflatten -> 创建设备 -> 驱动匹配 -> probe -> 解析属性 -> 硬件初始化
```

这条链路里任何一环出错，最后现象都可能是“设备不工作”，所以排障必须分层。

## 最小可运行示例

下面给一个最小闭环：设备树节点 + 最小 platform driver。

### 1. 设备树节点示例

```dts
/ {
    mydev@10000000 {
        compatible = "demo,my-device";
        reg = <0x10000000 0x1000>;
        status = "okay";
    };
};
```

这个节点表达的意思很简单：

1. 板子上有一个名为 `mydev` 的设备。
2. 它的寄存器基地址是 `0x10000000`。
3. 寄存器空间大小是 `0x1000`。
4. 它希望由兼容字符串 `demo,my-device` 对应的驱动接管。

### 2. 最小驱动示例

```c
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

struct mydev_priv {
    void __iomem *base;
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

    platform_set_drvdata(pdev, priv);
    dev_info(&pdev->dev, "my device probed\n");
    return 0;
}

static const struct of_device_id mydev_of_match[] = {
    { .compatible = "demo,my-device" },
    { }
};
MODULE_DEVICE_TABLE(of, mydev_of_match);

static struct platform_driver mydev_driver = {
    .probe = mydev_probe,
    .driver = {
        .name = "mydev",
        .of_match_table = mydev_of_match,
    },
};
module_platform_driver(mydev_driver);

MODULE_LICENSE("GPL");
```

这个示例虽然小，但已经体现了设备树驱动的核心闭环：

1. 设备树节点提供 `compatible` 和 `reg`。
2. 平台驱动用 `of_match_table` 做匹配。
3. `platform_get_resource()` 从节点里的 `reg` 拿资源。
4. `devm_ioremap_resource()` 把物理寄存器映射成内核可访问地址。

## 代码解读

### 1. 为什么 `platform_get_resource()` 能拿到 `reg`

因为平台设备在创建设备对象时，内核已经把设备树节点中的 `reg` 转换成了对应 `resource`。驱动不是直接去解析原始文本，而是通过内核资源接口访问。

### 2. 为什么 `devm_ioremap_resource()` 很常见

它做了两件事：

1. 检查资源范围是否合法。
2. 把寄存器地址映射成内核虚拟地址，并在设备释放时自动回收。

这比手工 `ioremap()` 更安全，也更适合工程代码。

### 3. `of_match_table` 和 `.name` 的关系

很多人会误以为 `.driver.name = "mydev"` 才决定匹配，这不对。

在设备树场景里，更关键的是：

1. 节点里的 `compatible`。
2. 驱动里的 `of_match_table`。

`.name` 更像驱动自身名字，不是设备树匹配的主依据。

### 4. 为什么 `status` 也会导致 probe 不进

如果节点写成：

```dts
status = "disabled";
```

即使 `compatible` 完全正确，很多情况下这个节点也不会被当成可用设备创建出来，最终现象仍然是驱动不 probe。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 驱动匹配 | 节点 `compatible` 和驱动 `of_match_table` 一致 | 两边字符串不一致 | 驱动根本匹配不上 |
| 启用节点 | `status = "okay"` | 忘记写或保留 `"disabled"` | 节点可能不会启用 |
| 地址空间 | `reg` 与硬件手册一致 | 抄错基地址或大小 | 驱动能 probe，但访问寄存器异常 |
| 资源获取 | 用 `platform_get_resource()`、`devm_*` 接口 | 手写固定地址或裸 `ioremap()` | 可移植性和可维护性差 |
| 节点归属 | I2C 设备挂在 I2C 控制器下面 | 把从设备直接挂到根节点 | 总线拓扑错误，子系统可能无法识别 |
| 可选属性处理 | 驱动里判断属性是否存在并给默认值 | 默认假设属性一定存在 | 不同板卡容易启动失败 |

## 边界条件与适用范围

1. 设备树主要用于 ARM、RISC-V 等平台的板级硬件描述，不是所有架构都同样依赖它。
2. 能自动枚举的总线设备通常不靠设备树发现，例如 PCIe、USB。
3. 设备树描述的是硬件事实，不应该塞入策略、业务逻辑或运行时状态。
4. 同一个驱动可以匹配多个 `compatible`，常见做法是从具体到通用，例如 `"vendor,soc-uart-v2"`、`"vendor,soc-uart"`。
5. `dts` 只是描述入口，真正资源是否可用还取决于时钟、复位、电源域、pinmux、IOMMU 等依赖是否完整。
6. overlay 适合运行时增量加载某些硬件描述，但不能把它当成“随便补丁一下就一定能工作”的万能修复手段。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 驱动 `probe` 不进 | `compatible` 不匹配；`status` 不是 `"okay"`；驱动没编进内核 | `dmesg`、检查驱动匹配表、反编译 dtb |
| `probe` 进了但资源申请失败 | `reg` 错；地址冲突；资源未声明正确 | `dmesg`、看 `resource` 错误日志 |
| GPIO/中断申请失败 | 节点属性名错； phandle 写错；控制器未启用 | 查 `gpios`、`interrupt-parent`、相关控制器节点 |
| 设备节点存在但功能不工作 | pinctrl、clock、reset、电源域没配齐 | 查时钟、复位、引脚复用相关节点和日志 |
| 改了 dts 但现象没变 | 实际启动没用新的 dtb；bootloader 仍加载旧文件 | 看启动日志、校验 `/proc/device-tree` 内容 |
| overlay 加载失败 | 符号未导出；目标节点路径不对；基础 dtb 不支持 overlay | 看 overlay 加载日志和 `dtc` 输出 |

### 推荐排查顺序

1. 先确认运行中的系统到底加载的是哪份设备树。
2. 再确认节点是否真的存在于运行中的设备树里。
3. 再确认 `compatible`、`status`、`reg`、`interrupts`、`gpios` 是否正确。
4. 再确认驱动是否真的注册了对应 `of_match_table`。
5. 最后再看 probe 里资源申请、时钟、pinctrl、电源依赖是否成功。

### 常用排障命令

```bash
dmesg | grep -i mydev
ls /proc/device-tree
ls /sys/firmware/devicetree/base
dtc -I dtb -O dts -o running.dts /boot/board.dtb
hexdump -C /proc/device-tree/model
```

如果你不确定设备树是否真的生效，优先不要猜，直接去看运行中的设备树内容和启动日志。

## 工程落地建议

### 1. 写设备树时先保证“硬件事实正确”

先核对这些内容，再谈驱动：

1. 地址是否和手册一致。
2. 中断号是否来自正确控制器。
3. pinmux 是否配置到对应复用功能。
4. GPIO 极性是否正确。
5. 时钟源、复位线、电源域是否完整。

### 2. 驱动里不要把设备树当成万能输入

好的驱动应该做到：

1. 必需属性缺失时明确报错。
2. 可选属性提供默认值。
3. 日志里能指出到底是哪项资源失败。
4. 不把板级细节重新硬编码回驱动里。

### 3. 推荐的代码组织方式

1. SoC 公共部分放到 `.dtsi`。
2. 板级差异放到具体开发板 `.dts`。
3. 驱动匹配表支持多个 `compatible`，方便向后兼容。
4. 节点命名保持稳定，别频繁改 label 和路径，避免影响 overlay 和引用关系。

## 性能、稳定性、可维护性影响

1. 设备树写对了，最大收益往往不是性能，而是可维护性和驱动复用能力。
2. 把板级差异从驱动里拆出来，能显著降低后续支持新板卡的成本。
3. 错误的设备树可能导致系统表面能启动，但外设随机异常，这类问题比“直接起不来”更难排查。
4. 驱动里使用 `devm_*`、标准资源接口和清晰日志，能显著提高设备树问题定位效率。
5. 滥用 overlay、重复定义节点、无注释地复制粘贴属性，短期看快，长期维护成本很高。

## 面试 / 问答怎么讲

### 30 秒版本

设备树就是把板级硬件描述从驱动里拆出来。驱动不再写死寄存器地址和 GPIO，而是通过 `compatible` 匹配节点，再从节点里读取 `reg`、中断、时钟等资源。

### 3 分钟版本

可以从启动流程讲：`.dts` 编译成 `.dtb`，bootloader 把它传给内核，内核根据设备树创建对应设备，驱动通过 `of_match_table` 和节点里的 `compatible` 匹配，匹配成功后进入 `probe`，再解析 `reg`、`interrupts`、`gpios` 等属性。这样驱动和板级差异就解耦了。

### 10 分钟版本

可以再结合一个真实排障案例展开：比如驱动 `probe` 不进，先确认运行中的设备树有没有这个节点，再检查 `status` 是否是 `"okay"`、`compatible` 是否匹配，然后看驱动是否注册了 `of_match_table`。如果 `probe` 进了但设备不工作，再看 `reg`、时钟、pinctrl、GPIO、中断和电源域是否完整。这种讲法能体现你不是只会背格式，而是真的会排查。

## 实战练习

1. 写一个最小 `mydev` 节点，把 `compatible` 改错一次，观察驱动为什么不 probe。
2. 故意把 `status` 改成 `"disabled"`，再从日志和运行中的设备树验证现象。
3. 给一个 I2C 设备写节点，练习把它正确挂到 I2C 控制器下面，而不是错挂到根节点。
4. 反编译一份真实 `dtb`，找出某个 UART、I2C 或 SPI 控制器节点，并解释它的关键属性。
5. 给最小驱动补充 `interrupts` 或 `gpios` 解析逻辑，再设计一条完整排查链路。

## 关键要点

1. 设备树描述的是硬件事实，不是驱动算法和业务逻辑。
2. `compatible` 是设备树驱动匹配的核心入口。
3. `dts -> dtb -> bootloader -> kernel -> device -> driver probe` 是必须讲清的一条主线。
4. 设备树问题不要只盯格式，要同时看节点是否生效、驱动是否匹配、资源是否申请成功。
5. 真实项目里最常见的问题不是语法错误，而是地址、时钟、pinctrl、GPIO 极性和总线挂接关系错误。
6. 会看运行中的设备树、会反编译 dtb、会读 `dmesg`，比会抄节点模板更重要。

## 关联笔记

1. `Linux-核心基础`
2. `硬件-基础知识`
3. `Linux-pinctrl与GPIO`

后续可以继续拆分为这些主题：

1. `Linux-platform总线与设备模型`
2. `Linux-设备树常用属性`
3. `Linux-I2C设备树绑定`
4. `Linux-SPI设备树绑定`
5. `Linux-设备树Overlay`
