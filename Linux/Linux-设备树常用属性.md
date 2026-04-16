# Linux-设备树常用属性

## 原始问题

设备树节点里为什么总在反复写 `compatible`、`reg`、`interrupts`、`status`、`clocks`、`gpios`、`pinctrl-0` 这些属性？这些属性到底分别在表达什么，内核又是怎么把它们转成驱动真正能用的资源的？

## 先给结论

设备树属性的本质，不是“把信息堆进节点里”，而是用一组约定好的字段，把板级硬件事实和驱动资源需求接起来。

先记住下面几个结论：

1. `compatible` 决定“这个节点该由谁接管”，是匹配入口。
2. `reg`、`interrupts`、`clocks`、`resets`、`gpios` 这类属性，决定“驱动初始化到底能拿到什么资源”。
3. `status`、`pinctrl-names`、`pinctrl-0` 这类属性，决定“这个节点是否启用、引脚是否切到正确功能”。
4. 很多设备树问题不是“语法不合法”，而是“属性名对了、格式也像对，但语义和硬件事实不一致”。
5. 真正高效的排障，不是背属性表，而是把“属性含义 -> 内核解析路径 -> 驱动资源获取 -> 运行期现象”串成一条主线。

如果只会抄模板，却讲不清某个属性是给谁看的、最终被哪个驱动接口消费、写错后会出现什么现象，就还没有真正掌握 Linux 设备树常用属性。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 看得懂一个 `dts` 节点，但不知道每个属性最后落到驱动哪一层。
2. 驱动 `probe` 不进、资源申请失败、GPIO 不翻转、中断不触发时，不知道先看哪个属性。
3. 设备树节点能编过，但启动后设备还是不工作，不知道是 `compatible`、地址、时钟、pinctrl 还是 GPIO 依赖出了问题。
4. 面试和工作交流里，经常会被问“设备树里最关键的属性有哪些”“怎么判断属性写错了”，但回答容易停留在表面。

它在 AI 时代仍然重要，因为 AI 很容易生成一个“结构看起来像对”的节点，但经常会把 phandle、地址、时钟、GPIO 极性、父节点层级和驱动期望混在一起。你越理解常用属性的真实语义，越能校验 AI 生成内容到底是不是板级事实。

## 核心概念 / 本质机制

### 1. 设备树属性本质上在表达三类信息

大多数常用属性，本质上都在表达下面三类信息：

1. 身份信息：这个节点是谁，应该匹配给谁。
2. 资源信息：这个节点占哪些寄存器、中断、时钟、GPIO、电源、复位资源。
3. 状态与约束信息：这个节点当前是否启用、引脚怎么复用、属性名和资源之间如何一一对应。

所以属性不是“附属说明”，而是驱动初始化链上的输入接口。

### 2. `compatible` 是匹配入口，不是普通描述文本

最核心的属性通常就是：

```dts
compatible = "vendor,my-device";
```

它的价值不是“起个名字”，而是：

1. 告诉内核这个节点的硬件身份。
2. 让驱动的 `of_match_table` 有机会匹配到它。
3. 决定后续是否会创建设备对象并进入 `probe`。

如果 `compatible` 不匹配，后面的 `reg`、`interrupts`、`gpios` 写得再对，驱动也可能根本不会进。

### 3. `reg` 表示地址资源，但它不是孤立字段

典型写法：

```dts
reg = <0x10000000 0x1000>;
```

它通常表示：

1. 设备寄存器基地址。
2. 这段寄存器空间大小。

但 `reg` 的解释并不总是“两个数就结束”，它还依赖父节点里的：

1. `#address-cells`
2. `#size-cells`

这意味着同样一行 `reg`，在不同总线层级下，解释方式可能不同。

所以很多地址问题不是 `reg` 本身写错，而是没有回到父节点理解它怎么被解释。

### 4. `interrupts` 表示中断资源，但必须结合中断控制器语义

很多节点里会看到：

```dts
interrupt-parent = <&gic>;
interrupts = <0 42 4>;
```

这里不能把 `interrupts` 简单理解成“中断号=42”。

它通常还编码了：

1. 中断控制器类型或 SPI/PPI 类别。
2. 中断号。
3. 触发类型，如上升沿、低电平、高电平等。

所以中断问题经常不是“驱动不会申请 IRQ”，而是：

1. `interrupt-parent` 指错控制器。
2. 触发类型写错。
3. 节点没挂在正确中断域下。

### 5. `status` 决定节点是否真的参与系统

最常见写法：

```dts
status = "okay";
```

或者：

```dts
status = "disabled";
```

它的价值是：

1. 明确这个节点当前是否启用。
2. 允许 SoC `.dtsi` 里先放默认禁用节点，再由板级 `.dts` 开启。

很多人改完设备树发现驱动不 `probe`，根因并不是 `compatible` 错，而是节点仍然处在 `"disabled"` 状态。

### 6. `clocks` / `clock-names` 是“时钟资源引用 + 名字绑定”

典型写法：

```dts
clocks = <&clkctrl 5>;
clock-names = "core";
```

它通常在表达：

1. 这个设备依赖一个或多个时钟提供者。
2. 每个时钟在驱动里对应哪个逻辑名称。

驱动里常见消费方式是：

```c
clk = devm_clk_get(dev, "core");
```

所以如果 `clock-names` 和驱动里请求的名字对不上，现象往往就是：

1. 驱动 `probe` 进了。
2. `devm_clk_get()` 失败。
3. 日志里提示找不到时钟或 defer。

### 7. `resets` / `reset-names` 与时钟是同一类依赖思路

典型写法：

```dts
resets = <&resetctrl 7>;
reset-names = "core";
```

它的本质和 `clocks` 很像：

1. 用 phandle 指向资源提供者。
2. 用名字把多个资源和驱动里的逻辑角色对齐。

所以名字和顺序都很重要，不能只觉得“写上了 reset 就行”。

### 8. `gpios` 不只是编号，还包含控制器和极性语义

典型写法：

```dts
reset-gpios = <&gpio3 5 GPIO_ACTIVE_LOW>;
```

这类属性表达的是：

1. GPIO 控制器是谁。
2. 用的是哪根线。
3. 有效极性是什么。

真正容易出问题的地方通常不是编号本身，而是：

1. 极性写反。
2. 把输入脚和输出脚语义混了。
3. 控制器节点本身没启用或没准备好。

### 9. `pinctrl-names` / `pinctrl-0` 决定“脚有没有切到正确功能”

很多外设节点即使 `reg`、`interrupts` 都对，如果 pinmux 没切过去，设备还是不会正常工作。

常见写法：

```dts
pinctrl-names = "default";
pinctrl-0 = <&uart0_pins>;
```

它在表达：

1. 当前默认状态要用哪组引脚配置。
2. 这些引脚配置通常定义在 pinctrl 控制器节点里。

很多“驱动 probe 成功但外设没反应”的问题，本质上不是驱动逻辑，而是脚根本没切到外设功能。

### 10. phandle 是设备树里最关键的“引用关系”

无论是：

1. `clocks`
2. `resets`
3. `gpios`
4. `pinctrl-0`
5. `dmas`
6. `power-domains`

背后都绕不开 phandle。

phandle 的本质可以理解成“设备树节点之间的引用指针”。这也是为什么很多属性看起来不是简单整数，而是：

```dts
<&node_label ...>
```

如果 label 写错、引用关系不成立、目标节点未启用，最后就会变成资源申请失败。

## 数据流 / 控制流 / 时序关系

把常用属性放进驱动初始化链里，通常是这样一条主线：

```text
板级 dts 写入 compatible / reg / interrupts / clocks / gpios / pinctrl
-> dtc 编译成 dtb
-> bootloader 把 dtb 传给内核
-> 内核解析节点并创建设备对象
-> 驱动根据 compatible 匹配
-> probe 里通过平台资源接口和 of 接口获取 reg / irq / clk / gpio / reset / pinctrl
-> 硬件初始化
-> 设备开始工作或在某个资源环节失败
```

这条链里最容易出错的点有：

1. `compatible` 对不上，驱动不进。
2. `status` 没启用，设备对象都没出现。
3. `reg` 或父节点地址语义错，映射失败。
4. `clock-names`、`reset-names`、GPIO 极性和驱动期望不一致。
5. pinctrl 配置缺失，导致“probe 成功但功能不通”。

## 最小可运行示例

下面给一个相对完整但仍然最小化的设备树节点示例，再配上驱动中对应的资源获取代码。

### 1. 设备树节点示例

```dts
myuart@10000000 {
    compatible = "demo,my-uart";
    reg = <0x10000000 0x1000>;
    interrupts = <0 42 4>;
    clocks = <&clkctrl 5>;
    clock-names = "core";
    reset-gpios = <&gpio3 5 GPIO_ACTIVE_LOW>;
    pinctrl-names = "default";
    pinctrl-0 = <&myuart_pins>;
    status = "okay";
};
```

这个节点至少表达了五件事：

1. 它应该由 `demo,my-uart` 对应驱动接管。
2. 它有一段寄存器空间。
3. 它依赖一个中断。
4. 它依赖一个名为 `core` 的时钟。
5. 它依赖一根低电平有效的 reset GPIO，以及一组默认 pinctrl 配置。

### 2. 驱动里的资源获取示例

```c
static int myuart_probe(struct platform_device *pdev)
{
    void __iomem *base;
    int irq;
    struct clk *clk;
    struct gpio_desc *reset_gpio;

    base = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(base))
        return PTR_ERR(base);

    irq = platform_get_irq(pdev, 0);
    if (irq < 0)
        return irq;

    clk = devm_clk_get(&pdev->dev, "core");
    if (IS_ERR(clk))
        return dev_err_probe(&pdev->dev, PTR_ERR(clk),
                             "failed to get core clock\n");

    reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(reset_gpio))
        return dev_err_probe(&pdev->dev, PTR_ERR(reset_gpio),
                             "failed to get reset gpio\n");

    return 0;
}
```

这个最小示例体现了设备树属性和驱动资源接口的一一对应关系：

1. `reg` -> `devm_platform_ioremap_resource()`
2. `interrupts` -> `platform_get_irq()`
3. `clock-names = "core"` -> `devm_clk_get(..., "core")`
4. `reset-gpios` -> `devm_gpiod_get_optional(..., "reset", ...)`

## 代码解读

### 1. 为什么 `reg` 最终不是直接手工解析字符串

因为在平台设备场景里，内核已经把设备树里的 `reg` 转成了 `resource`。驱动更常用的是：

1. `platform_get_resource()`
2. `devm_platform_ioremap_resource()`

这说明设备树属性并不是“文本配置而已”，而是会进入内核设备模型和资源模型。

### 2. 为什么 `clock-names` 很容易被忽略，却很关键

如果节点里写的是：

```dts
clock-names = "bus";
```

而驱动里写的是：

```c
devm_clk_get(dev, "core");
```

那即使 `clocks = <...>` 本身存在，驱动照样可能拿不到资源。

所以很多问题不是“有没有写 clocks”，而是“名字和驱动约定是否一致”。

### 3. 为什么 `reset-gpios` 在驱动里用 `"reset"`

因为 gpiod 接口有一套“属性前缀 -> 逻辑名字”的约定。

像下面这种属性：

```dts
reset-gpios = <...>;
```

在驱动里通常对应：

```c
devm_gpiod_get(dev, "reset", ...)
```

如果你把属性写成别的前缀，驱动侧也要跟着改，不然就是名字对不上。

### 4. 为什么 pinctrl 问题常常不会在第一时间报成明显错误

很多平台上，pinctrl 缺失的现象不是“驱动完全起不来”，而是：

1. `probe` 成功。
2. 寄存器能映射。
3. 时钟也能拿到。
4. 但实际串口没波形、I2C 没响应、中断不触发。

所以 pinctrl 是典型的“初始化看似成功，但功能链路暗中断掉”的高频点。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 驱动匹配 | `compatible` 和 `of_match_table` 一致 | 节点和驱动字符串不一致 | 驱动不 `probe` |
| 节点启用 | `status = "okay"` | 保留 `"disabled"` 或板级没覆盖默认状态 | 节点可能不参与系统 |
| 地址资源 | 结合父节点 `#address-cells/#size-cells` 理解 `reg` | 只抄两个数，不看父节点语义 | 地址和大小可能被解释错 |
| 时钟资源 | `clocks` 和 `clock-names` 成对设计，并和驱动名字一致 | 只写 `clocks` 不写名字，或名字对不上 | `devm_clk_get()` 失败 |
| GPIO 资源 | 明确控制器、引脚和极性 | 只关心“第几号脚”，忽略极性和控制器 | 逻辑电平和实际行为可能完全相反 |
| pinctrl | `pinctrl-names` / `pinctrl-0` 和功能状态对应 | 只配外设节点，不配 pinctrl 组 | 设备可能 probe 成功但功能不通 |
| 资源命名 | `reset-gpios`、`clock-names` 等前缀与驱动约定一致 | 属性名随意改，驱动侧不同步 | of/gpiod/clk 获取链断掉 |

## 边界条件与适用范围

1. 并不是所有设备树属性都属于通用基础属性，很多总线或子系统还有各自 binding 专属属性。
2. 同一个属性在不同控制器、不同父节点层级下，编码语义可能不同，尤其是 `reg`、`interrupts`、`dmas` 这类复合属性。
3. 某些资源可以是可选的，驱动应明确区分“必须有”和“没有也能工作”的属性。
4. 不同内核版本、binding 文档和驱动实现之间，属性名可能存在历史兼容差异，不能只抄旧板子的节点。
5. 这篇笔记主要讲最常见的板级属性，不展开每个子系统全部 binding 细节。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 驱动完全不 `probe` | `compatible` 错；`status` 不是 `"okay"`；节点没进运行中设备树 | 查 `dmesg`、运行中设备树、驱动匹配表 |
| `probe` 进了但地址资源失败 | `reg` 错；父节点地址语义理解错；资源冲突 | 查日志、`/proc/iomem`、父节点 cells 配置 |
| `devm_clk_get()` 失败 | `clocks` 引用错；`clock-names` 对不上；provider 没就绪 | 查 provider 节点、日志、名字是否一致 |
| GPIO 获取成功但行为不对 | 极性写反；脚没切成 GPIO；输出初值策略不对 | 查 `GPIO_ACTIVE_LOW/HIGH`、pinctrl、实际波形 |
| `probe` 成功但外设完全无响应 | pinctrl 没生效；时钟未打开；中断极性错 | 查 pinctrl、时钟树、`/proc/interrupts` |
| 改了 dts 没效果 | 启动仍在用旧 dtb；实际运行树未变化 | 查 `/sys/firmware/devicetree/base`、启动日志 |

### 推荐排查顺序

1. 先确认节点是否真的在运行中的设备树里存在且状态正确。
2. 再确认 `compatible` 是否和驱动匹配表一致。
3. 再确认 `reg`、`interrupts`、`clocks`、`gpios`、pinctrl 这些资源属性是否语义正确。
4. 再看驱动获取资源时具体卡在第几步。
5. 最后结合 `/sys`、`/proc`、日志和波形验证功能链路。

### 常用验证方式

下面这些命令很适合把“属性写了”和“属性真生效了”分开验证：

```bash
dmesg | grep -Ei "myuart|clk|gpio|irq|probe"
find /sys/firmware/devicetree/base -iname "*myuart*"
hexdump -C /sys/firmware/devicetree/base/soc/myuart@10000000/status
cat /proc/interrupts
cat /proc/iomem | grep -i myuart
```

如果系统支持 `dtc`，也很适合把运行中的设备树反编译出来核对。

## 工程落地建议

### 1. 写设备树属性时，优先按“驱动会怎么拿”来组织

不要只从硬件手册抄字段，更要同时问：

1. 这个资源在驱动里通过哪个接口获取。
2. 这个名字是不是和驱动约定一致。
3. 这个属性是必需的，还是可选的。

### 2. 尽量让节点既能读，也能排障

更稳的习惯是：

1. 属性名尽量遵守 binding 规范，不自创随意缩写。
2. 多个同类资源时，总是配套 `*-names`。
3. 节点 label、功能名和日志里打印名尽量能对上。

这样后续排障时更容易从日志追到节点，从节点追到驱动。

### 3. 板级 bring-up 时，把属性验证拆成几层

推荐这样推进：

1. 先验证节点是否生效。
2. 再验证驱动是否匹配。
3. 再验证资源是否获取成功。
4. 最后验证功能是否真的通。

这样比“属性全写完直接上电试”更稳，也更容易缩小范围。

## 性能、稳定性、可维护性影响

1. 正确的属性设计，首先带来的是稳定初始化和可维护性，而不是单点性能提升。
2. 属性名、资源名和驱动约定一致时，后续迁移板卡和排障成本会显著下降。
3. 很多“偶发设备异常”其实不是驱动本身不稳，而是设备树里的极性、时钟、pinctrl 或地址语义有误。
4. 设备树节点越规范，AI 和脚本工具后续辅助生成、检查和迁移的效果越好，因为语义更稳定、更可推理。

## 面试 / 问答怎么讲

### 30 秒版本

Linux 设备树常用属性本质上分三类：身份、资源和状态。`compatible` 负责匹配驱动，`reg`、`interrupts`、`clocks`、`gpios` 负责提供资源，`status` 和 `pinctrl-*` 负责控制启用状态和引脚功能。真正的关键不是背属性名，而是知道每个属性最终被哪个驱动接口消费。

### 3 分钟版本

可以从一个最小节点讲起：先用 `compatible` 让驱动匹配，再用 `reg` 提供寄存器地址、用 `interrupts` 提供 IRQ、用 `clocks` 和 `clock-names` 提供时钟、用 `reset-gpios` 提供控制线、用 `pinctrl-0` 切默认功能。然后补充排障思路：驱动不 `probe` 先看 `compatible` 和 `status`，资源失败再看 `reg`、时钟、GPIO、pinctrl。这样主线就很清晰。

### 10 分钟版本

可以进一步结合驱动代码展开：`reg` 最终通过 platform resource 变成 `devm_platform_ioremap_resource()` 的输入，`interrupts` 变成 `platform_get_irq()` 的输入，`clock-names` 决定 `devm_clk_get(dev, "core")` 能不能成功，`reset-gpios` 决定 gpiod 接口能不能拿到控制线。然后再讲为什么很多问题不是语法错，而是属性语义和驱动约定不一致，比如时钟名字对不上、GPIO 极性写反、pinctrl 缺失。这种讲法更像真实板级开发经验。

## 实战练习

1. 从一份真实 UART、SPI 或 I2C 节点里，标出 `compatible`、`reg`、`interrupts`、`clocks`、`gpios`、pinctrl 分别在解决什么问题。
2. 故意把 `clock-names` 改错一次，观察驱动日志并总结为什么资源明明存在却还是获取失败。
3. 故意把 `status` 改成 `"disabled"`，再验证为什么驱动不再 `probe`。
4. 故意把 `GPIO_ACTIVE_LOW` 改成 `GPIO_ACTIVE_HIGH`，结合实际现象解释“设备树极性错误”和“驱动逻辑错误”的区别。
5. 把一个节点的资源获取链写成表格：属性名、驱动接口、失败现象、验证方法。

## 关键要点

1. 设备树属性本质上是在连接板级硬件事实和驱动资源接口。
2. `compatible` 是匹配入口，`status` 决定节点是否启用。
3. `reg`、`interrupts`、`clocks`、`resets`、`gpios` 是最常见资源属性。
4. `clock-names`、`reset-names`、GPIO 前缀名这类“名字约定”经常决定资源能不能真正拿到。
5. pinctrl 是高频暗坑，很多“probe 成功但功能不通”的问题都和它有关。
6. 设备树问题不要只看语法，要同时看属性语义、驱动消费方式和运行期现象。

## 关联笔记

1. `Linux-总览`
2. `Linux-设备树`
3. `Linux-platform总线与设备模型`
4. `Linux-内核日志与排障`
5. `驱动开发-platform驱动基础`
6. `硬件-时钟与复位`
