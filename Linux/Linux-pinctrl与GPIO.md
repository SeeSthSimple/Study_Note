# Linux-pinctrl与GPIO

## 原始问题

为什么 Linux 里很多设备树节点明明 `compatible`、`reg`、`interrupts` 都写对了，驱动也 `probe` 成功了，外设还是不工作？为什么有时候明明拿到了 GPIO，实际电平却不对，或者根本没有波形？`pinctrl` 和 `GPIO` 到底各自管什么，它们为什么总是一起出现却又不是一回事？

## 先给结论

`pinctrl` 和 `GPIO` 的关系，是嵌入式 Linux bring-up 里最容易混淆、也最容易出暗坑的一条主线。

先记住下面几个结论：

1. `pinctrl` 解决的是“这根物理引脚当前属于哪个功能、用什么电气配置”，GPIO 解决的是“把这根线当输入还是输出，以及逻辑值怎么读写”。
2. 一根脚先要被切到正确功能，后面的 GPIO 控制或外设功能才有意义。
3. `pinctrl` 配错时，常见现象是驱动 `probe` 成功但硬件没反应；GPIO 配错时，常见现象是能拿到描述符但电平、方向或有效极性不对。
4. 设备树里 `pinctrl-*` 和 `*-gpios` 看起来都在写引脚，其实服务的是两层完全不同的语义。
5. 真正稳定的排障，不是只会改节点，而是能把“引脚复用 -> GPIO 语义 -> 驱动接口 -> 运行时波形/中断现象”串成一条链。

如果只知道“引脚相关都写进 dts”，却讲不清 pinmux、bias、drive strength、GPIO 极性、`gpiod_get()` 和 `pinctrl` 状态切换之间的区别，就还没有真正掌握 Linux 里的 `pinctrl` 与 GPIO。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 设备树里既写 `pinctrl-0` 又写 `reset-gpios`，不知道这两者为什么不能互相替代。
2. 驱动拿到了 GPIO 描述符，但外设 reset 还是不成功，不知道是脚没切对、极性反了，还是电气配置不对。
3. 中断脚配置了 `interrupts`，但 `/proc/interrupts` 计数始终不涨，不知道该先查 pinctrl 还是查 IRQ。
4. 面试和工作里经常被问“pinctrl 和 GPIO 有什么区别”“为什么 probe 成功设备还是不工作”，但回答容易停在“引脚没配好”这种表层说法。

它在 AI 时代仍然重要，因为 AI 很容易生成一段“节点长得像对”的 pinctrl 和 GPIO 配置，但经常不知道某个 SoC 的复用寄存器、电平极性、上拉下拉、开漏、睡眠状态和驱动消费接口到底怎么对应。你越理解这组概念，越能校验 AI 生成内容是不是只是模板拼接。

## 核心概念 / 本质机制

### 1. `pinctrl` 和 GPIO 分别在管什么

可以先粗略理解成两层：

1. `pinctrl` 负责“这根物理引脚当前是什么身份，以及附带的电气属性”。
2. GPIO 子系统负责“把某根线作为通用数字输入输出时，怎么按逻辑语义操作它”。

这意味着同一根物理脚，在不同时间可能有完全不同角色：

1. 作为 UART TX/RX。
2. 作为 SPI CLK/MOSI/MISO。
3. 作为普通 GPIO 输出 reset。
4. 作为 GPIO 输入中断线。

所以它不是“脚就是 GPIO”，而是“脚先有复用身份，再谈能不能当 GPIO 用”。

### 2. 为什么 `pinctrl` 是高频暗坑

很多设备树和驱动问题看起来都像资源拿到了，但功能就是不通。

这时常见根因往往是：

1. pinmux 没切到外设功能。
2. bias 上拉下拉不对。
3. 驱动能力或电气强度不匹配。
4. 睡眠态和默认态切换错了。

所以 `pinctrl` 的问题通常不像 `reg` 错那样直接报错，而是更容易表现成：

1. 串口没波形。
2. I2C 总线被拉死。
3. 中断脚一直保持错误电平。
4. 复位脚逻辑看起来执行了，但外设没有真正被拉低/拉高。

### 3. GPIO 的本质不是“编号”，而是“逻辑线”

在 Linux 里，更推荐把 GPIO 理解成“有方向、有值、有极性语义的一根逻辑控制线”，而不是“某个全局编号”。

这也是为什么现代驱动更推荐使用：

1. `gpiod_get()`
2. `devm_gpiod_get_optional()`
3. `gpiod_set_value_cansleep()`

而不是老式“硬编码全局 GPIO 编号”的方式。

因为真正重要的是：

1. 这根线在设备语义上叫什么，比如 `reset`、`enable`、`irq`。
2. 它是高有效还是低有效。
3. 它能不能睡眠上下文操作。
4. 它属于哪个 GPIO 控制器。

### 4. `pinctrl` 配的是“功能状态”，`*-gpios` 配的是“驱动要拿的线”

设备树里这两类属性长得都和引脚有关，但语义完全不同。

例如：

```dts
pinctrl-names = "default";
pinctrl-0 = <&mydev_pins>;
reset-gpios = <&gpio3 5 GPIO_ACTIVE_LOW>;
```

它们分别在表达：

1. `pinctrl-0`：这组脚默认切成什么功能、带什么电气配置。
2. `reset-gpios`：驱动要额外拿一根逻辑 reset 线来做控制。

所以常见误区是：

1. 以为配了 `reset-gpios` 就不需要 pinctrl。
2. 以为 pinctrl 已经切到功能后，驱动就自动拥有 GPIO 控制权。

这两个理解都不对。

### 5. GPIO 极性和驱动逻辑常常一起把人带偏

典型写法：

```dts
reset-gpios = <&gpio3 5 GPIO_ACTIVE_LOW>;
```

这表示：

1. 从逻辑语义上看，这是一根低有效 reset 线。
2. 驱动通过 gpiod 接口操作它时，最好按“assert/deassert”或逻辑值理解，而不是死盯物理高低电平。

如果极性写错，常见后果是：

1. 驱动代码看起来执行了 reset。
2. 实际硬件却一直处在复位态，或者根本没被复位。

这类问题特别容易被误判成“寄存器初始化错”或“时钟没开”。

### 6. `pinctrl` 还有“状态切换”语义，不只有默认态

很多设备不止一个引脚状态：

1. `default`
2. `sleep`
3. `idle`

例如：

```dts
pinctrl-names = "default", "sleep";
pinctrl-0 = <&uart0_default>;
pinctrl-1 = <&uart0_sleep>;
```

这意味着驱动或框架在 suspend/resume 时，可能会把同一组脚切到不同配置。

所以一些“运行正常、休眠唤醒后异常”的问题，本质上可能不在核心功能代码，而在 pinctrl 状态切换。

### 7. GPIO 中断链路常常跨了三层

一个 GPIO 中断类问题，通常至少跨了这几层：

1. pinctrl：脚是否切成输入/中断相关功能。
2. GPIO 控制器：能否把该脚暴露成中断来源。
3. IRQ 子系统：`interrupt-parent`、触发类型、handler 是否正确。

所以“中断没进”不能只查 `request_irq()` 或 `platform_get_irq()`，也不能只查 `interrupts` 属性。

真正高效的路径是把 pinctrl、GPIO 和 IRQ 一起看。

## 数据流 / 控制流 / 时序关系

把 `pinctrl` 与 GPIO 放进驱动初始化主线里，通常是这样一条链：

```text
设备树定义 pinctrl 组和 *-gpios 属性
-> 内核解析节点并准备 pinctrl 状态
-> 驱动 probe 开始
-> pinctrl 默认状态被选择或由框架切入
-> 驱动通过 gpiod 接口获取 reset/enable/irq 等逻辑线
-> 驱动设置方向、初值或切换逻辑状态
-> 外设复位、上电、开始通信或等待中断
-> 运行期根据波形、电平、中断计数判断链路是否真正打通
```

这条链里最容易出错的点有：

1. pinmux 没切到正确功能。
2. GPIO 极性写反。
3. 脚既被当外设功能用，又被错误地当普通 GPIO 控。
4. 方向配置或初始电平不符合硬件时序要求。
5. 睡眠态切换后引脚状态丢了。

## 最小可运行示例

下面给一个“设备默认 pinctrl + 一根 reset GPIO”的最小闭环示例。

### 1. 设备树示例

```dts
mydev@10000000 {
    compatible = "demo,mydev";
    reg = <0x10000000 0x1000>;

    pinctrl-names = "default";
    pinctrl-0 = <&mydev_default_pins>;

    reset-gpios = <&gpio3 5 GPIO_ACTIVE_LOW>;
    irq-gpios = <&gpio3 6 GPIO_ACTIVE_HIGH>;

    status = "okay";
};
```

这个节点表达的意思是：

1. 设备默认使用一组名为 `mydev_default_pins` 的引脚配置。
2. 设备有一根低有效 reset 线。
3. 设备还有一根高有效 IRQ 相关 GPIO 线。

### 2. 驱动资源获取示例

```c
static int mydev_probe(struct platform_device *pdev)
{
    struct gpio_desc *reset_gpio;
    struct gpio_desc *irq_gpio;

    reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(reset_gpio))
        return dev_err_probe(&pdev->dev, PTR_ERR(reset_gpio),
                             "failed to get reset gpio\n");

    irq_gpio = devm_gpiod_get_optional(&pdev->dev, "irq", GPIOD_IN);
    if (IS_ERR(irq_gpio))
        return dev_err_probe(&pdev->dev, PTR_ERR(irq_gpio),
                             "failed to get irq gpio\n");

    gpiod_set_value_cansleep(reset_gpio, 1);
    usleep_range(1000, 2000);
    gpiod_set_value_cansleep(reset_gpio, 0);

    return 0;
}
```

这个最小示例体现了三件关键事情：

1. `pinctrl-0` 负责默认引脚功能状态，不是通过代码手动拿到的 GPIO 描述符。
2. `reset-gpios` 和 `irq-gpios` 分别被驱动通过 `"reset"`、`"irq"` 逻辑名获取。
3. 驱动对 reset 的操作，应该按逻辑语义和极性理解，而不是只盯物理高低电平。

## 代码解读

### 1. 为什么 `devm_gpiod_get_optional(..., "reset", ...)` 里只写 `"reset"`

因为设备树属性名通常是：

```dts
reset-gpios = <...>;
```

gpiod 接口会按“前缀名 + `-gpios`”的约定去匹配资源。

这意味着：

1. 设备树前缀和驱动逻辑名必须一致。
2. 不能随手把属性改成 `rst-gpios`，却还指望驱动里 `"reset"` 能照常获取成功。

### 2. 为什么 reset 线用逻辑值思考更稳

如果设备树写的是 `GPIO_ACTIVE_LOW`，那么：

1. 驱动写逻辑 `1`，代表“assert 这根 reset 线”。
2. 物理电平可能实际被驱到低电平。

如果你只盯物理电平，很容易把驱动逻辑和硬件语义混在一起。

### 3. 为什么 `irq-gpios` 和 `interrupts` 不是一回事

`irq-gpios` 常见于一些通过 GPIO 控制器转成中断源的场景，或者驱动先拿一根 GPIO 再映射成 IRQ。

而 `interrupts` 更直接表示节点的中断资源描述。

两者可能相关，但并不是简单等价关系，也不能互相替换。

### 4. 为什么只拿到 GPIO 还不够

即使驱动 `gpiod_get()` 成功，也不代表链路已经完全正确，因为还可能有这些问题：

1. 脚没切到 GPIO 模式。
2. pinctrl 默认态没生效。
3. 电气上拉下拉不对。
4. 这根脚被别的功能复用了。

所以“描述符拿到了”只是资源入口成功，不是功能闭环成功。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 功能理解 | `pinctrl` 管复用和电气属性，GPIO 管逻辑线控制 | 把 pinctrl 和 GPIO 当一回事 | 很容易排错方向跑偏 |
| 外设默认引脚 | 配 `pinctrl-names` / `pinctrl-0` | 只配 `*-gpios`，不配 pinctrl | 设备可能 probe 成功但功能不通 |
| GPIO 获取 | 用 `devm_gpiod_get_optional(dev, "reset", ...)` 等逻辑名接口 | 硬编码全局 GPIO 编号 | 可移植性和维护性差 |
| 极性处理 | 在 dts 里明确 `GPIO_ACTIVE_LOW/HIGH`，驱动按逻辑语义使用 | 极性不写清，代码里手工反转猜测 | 容易出现“看起来执行了但硬件不对” |
| 状态切换 | 区分 `default` / `sleep` 等 pinctrl 状态 | 所有场景只配一个模糊状态 | suspend/resume 后容易出问题 |
| 中断排查 | pinctrl、GPIO、IRQ 三层联动看 | 只看 `request_irq()` 或只看设备树 `interrupts` | 容易漏掉真正断链位置 |

## 边界条件与适用范围

1. `pinctrl` 控制器的具体属性写法强依赖 SoC 厂商和 binding 文档，不同平台的 pin group 表达方式差异很大。
2. 并不是所有引脚都能在所有功能之间自由切换，很多脚有固定复用限制，必须回到芯片手册确认。
3. 某些 GPIO 控制器访问可能会睡眠，因此应区分 `gpiod_set_value()` 和 `gpiod_set_value_cansleep()` 的使用语境。
4. 用户态旧式 `sysfs GPIO` 接口已逐步被 character device 接口替代，写新代码时不要再把它当首选。
5. 这篇笔记主要讲设备树和驱动里的 pinctrl/GPIO 主线，不展开每个 SoC 的 pin mux 细节寄存器。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 驱动 `probe` 成功但外设没波形 | pinctrl 没切到外设功能；默认态没生效 | 查设备树、`/sys/kernel/debug/pinctrl`、示波器 |
| reset GPIO 获取成功但设备还是没复位 | 极性写反；初值不对；脚被复用占用 | 查 `GPIO_ACTIVE_*`、波形、pinctrl 配置 |
| I2C/SPI/UART 功能时好时坏 | bias、drive strength 或 sleep/default 状态切换有问题 | 查 pinctrl 状态、休眠唤醒流程 |
| GPIO 中断不触发 | pinctrl 没切输入/中断相关功能；IRQ 极性错；GPIO 控制器链路断 | 查 pinctrl、`/proc/interrupts`、设备树中断描述 |
| 驱动拿不到 GPIO | `*-gpios` 前缀和驱动逻辑名不一致； provider 节点没就绪 | 查属性名、日志、provider 节点状态 |
| 表面上都正常但功能不稳定 | 电气层和时序层问题被误当成驱动逻辑问题 | 结合日志、debugfs、波形一起看 |

### 推荐排查顺序

1. 先确认这根脚当前应该承担什么角色：外设功能、普通 GPIO，还是 GPIO 中断。
2. 再确认 pinctrl 是否把它切到了正确功能和电气状态。
3. 再确认 GPIO 属性名、极性、方向和驱动获取接口是否一致。
4. 如果涉及中断，再确认 GPIO 控制器和 IRQ 链路是否打通。
5. 最后结合示波器、逻辑分析仪和 `/proc/interrupts` 做运行期验证。

### 常用验证方式

下面这些命令和路径在 Linux bring-up 里很常用：

```bash
dmesg | grep -Ei "gpio|pinctrl|irq|probe"
cat /sys/kernel/debug/gpio
find /sys/kernel/debug/pinctrl -maxdepth 2 -type f
cat /proc/interrupts
```

它们分别帮助你看：

1. 驱动和资源获取日志。
2. 当前 GPIO 线的占用情况。
3. pinctrl 相关状态和 mux 信息。
4. 中断计数是否真的在增长。

如果系统没有挂载 `debugfs`，要先确认 `/sys/kernel/debug` 是否可用。

## 工程落地建议

### 1. 先分清“哪根脚归谁管”

在板级 bring-up 时，建议先画清楚：

1. 哪些脚是外设主功能脚。
2. 哪些脚是 reset/enable 等 GPIO 控制脚。
3. 哪些脚承担中断输入。

这一步清楚后，设备树和驱动接口会顺很多。

### 2. 设备树命名尽量和驱动逻辑名一致

例如：

1. `reset-gpios` 对应驱动里的 `"reset"`。
2. `enable-gpios` 对应驱动里的 `"enable"`。
3. pinctrl 组名尽量体现功能和状态，比如 `uart0_default`、`uart0_sleep`。

这样做最大的价值是排障时不需要二次翻译。

### 3. 把 pinctrl/GPIO 验证前置到最小闭环

推荐 bring-up 时先做最小验证：

1. 看脚有没有切到预期功能。
2. 看 reset/enable 脚逻辑是否正确翻转。
3. 看 IRQ 脚电平和中断计数是否对应。

不要等完整外设协议都跑不通了，才回头怀疑引脚层。

## 性能、稳定性、可维护性影响

1. `pinctrl` 和 GPIO 设计正确时，最直接收益是稳定 bring-up 和更低排障成本。
2. 引脚层配置错误往往不会立刻报成明显内核错误，却会在运行期表现成偶发异常、超时和不稳定行为。
3. 使用 descriptor-based GPIO 接口、清晰的 pinctrl 状态命名和明确极性约定，能显著提升后续维护性。
4. 在 AI 协助写 dts 和驱动的场景下，这部分理解尤其重要，因为它决定你能不能识别“模板正确但板级语义错误”的配置。

## 面试 / 问答怎么讲

### 30 秒版本

`pinctrl` 和 GPIO 不是一回事。`pinctrl` 管的是引脚复用和电气状态，GPIO 管的是逻辑输入输出控制。很多 Linux 驱动 `probe` 成功但设备不工作，本质上不是驱动主流程错，而是 pinmux、极性、方向或 IRQ 引脚链路没打通。

### 3 分钟版本

可以从一个设备树节点讲起：`pinctrl-0` 决定默认引脚状态，`reset-gpios` 和 `irq-gpios` 让驱动拿到逻辑控制线。驱动通过 `devm_gpiod_get_optional()` 获取这些线，再按极性和方向去控制。如果 `pinctrl` 没配好，即使 GPIO 获取成功、寄存器也映射成功，外设仍然可能没波形或中断不进。这个主题真正难的地方在于它跨了 pinmux、GPIO 和 IRQ 三层。

### 10 分钟版本

可以进一步结合板级排障展开：先确认脚在芯片手册里支持什么复用，再看设备树 pinctrl 组是否切对功能和电气属性，然后看 `*-gpios` 前缀和驱动逻辑名是否一致，最后用 `debugfs`、`/proc/interrupts` 和波形验证实际行为。再补充为什么 `GPIO_ACTIVE_LOW` 这类极性配置会改变驱动逻辑语义、为什么 suspend/resume 还要考虑 `sleep` 状态。这种讲法更像真实 bring-up 经验。

## 实战练习

1. 从一份真实 UART、SPI 或 I2C 节点中，分别标出外设功能脚、reset GPIO、irq GPIO 各自属于哪一层。
2. 故意把 `GPIO_ACTIVE_LOW` 改成 `GPIO_ACTIVE_HIGH`，预测并验证 reset 行为会如何出错。
3. 给一个设备补一组 `default` 和 `sleep` pinctrl 状态，思考它在 suspend/resume 时为什么需要两套配置。
4. 用 `cat /sys/kernel/debug/gpio` 和 `cat /proc/interrupts` 观察一根 IRQ 线在触发前后分别有什么变化。
5. 把某个驱动的 `gpiod_get()` 链路画成表格：属性名、驱动逻辑名、方向、极性、失败现象、验证方法。

## 关键要点

1. `pinctrl` 管复用和电气状态，GPIO 管逻辑线控制。
2. 先把脚切到正确功能，后面的 GPIO 或外设功能才有意义。
3. `pinctrl-*` 和 `*-gpios` 看起来都在写引脚，但语义完全不同。
4. GPIO 极性、方向和名字约定经常决定驱动资源能不能真正用对。
5. 中断脚问题通常跨 pinctrl、GPIO、IRQ 三层，不能只盯一层看。
6. 很多“probe 成功但功能不通”的问题，本质上是 pinctrl/GPIO 链路没打通。

## 关联笔记

1. `Linux-总览`
2. `Linux-设备树`
3. `Linux-设备树常用属性`
4. `Linux-内核日志与排障`
5. `硬件-基础知识`
6. `驱动开发-platform驱动基础`
