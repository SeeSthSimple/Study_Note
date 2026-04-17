# 驱动开发-GPIO驱动基础

## 原始问题

Linux 里怎么操作 GPIO？为什么有 `gpio_request`、`gpiod_get`、`pinctrl` 这么多接口？设备树里 GPIO 怎么描述？用户态怎么控制 GPIO？从裸机直接操作寄存器到 Linux GPIO 子系统，到底发生了什么变化？

## 先给结论

Linux GPIO 子系统的核心是"把裸机直接操作寄存器的方式，变成通过抽象层安全访问"：

1. 旧接口（`gpio_request` / `gpio_direction_output` 等）已废弃，新驱动应使用基于描述符的 `gpiod_*` 接口。
2. GPIO 在设备树里通过 `xxx-gpios` 属性描述，驱动用 `devm_gpiod_get` 获取。
3. `pinctrl` 子系统负责引脚复用和配置，GPIO 子系统负责引脚电平读写，两者配合工作。
4. 用户态可以通过 `libgpiod` 或 `/sys/class/gpio` 访问 GPIO，但生产代码推荐用驱动。
5. GPIO 中断通过 `gpiod_to_irq` 转换成 IRQ 号，再用标准中断接口注册。

如果只能背"GPIO 就是控制引脚高低电平"，但讲不清 gpiod 接口、设备树描述、pinctrl 关系和中断处理，就还没有真正掌握 Linux GPIO 驱动。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 知道裸机怎么操作 GPIO，但不知道 Linux 下怎么写 GPIO 驱动。
2. 不理解 `gpiod_*` 和旧 `gpio_*` 接口的区别，不知道该用哪个。
3. 设备树里 GPIO 属性怎么写、驱动里怎么获取，缺少完整链路。
4. 不知道怎么把 GPIO 中断接入 Linux 中断子系统。
5. 面试时知道"GPIO 是最基本的外设"，但讲不清 Linux GPIO 子系统的分层和接口。

它在AI时代依然重要，因为AI生成 GPIO 代码时经常混用新旧接口、忽略设备树描述、忘记 pinctrl 配置。理解 GPIO 子系统后，你才能判断AI给的代码是否在现代内核上正确工作。

## 核心概念 / 本质机制

### 1. Linux GPIO 子系统的分层

```text
用户态应用
  |
  V
libgpiod / sysfs / 驱动接口
  |
  V
GPIO 子系统 (gpiod 核心层)
  |
  V
GPIO 控制器驱动 (厂商提供)
  |
  V
硬件寄存器
```

关键分层：

1. **GPIO 控制器驱动**：厂商提供，操作具体硬件寄存器，注册 `gpio_chip`。
2. **GPIO 核心层**：提供统一的 `gpiod_*` 接口，管理所有 GPIO 控制器。
3. **消费者驱动**：使用 GPIO 的功能驱动，通过 `gpiod_*` 接口访问。

### 2. 旧接口 vs 新接口

| 旧接口（已废弃） | 新接口（推荐） | 区别 |
| --- | --- | --- |
| `gpio_request(nr)` | `devm_gpiod_get(dev, con_id)` | 旧接口用全局编号，新接口用设备树描述符 |
| `gpio_direction_output(nr, val)` | `gpiod_direction_output(desc, val)` | 新接口基于描述符，类型安全 |
| `gpio_set_value(nr, val)` | `gpiod_set_value(desc, val)` | 同上 |
| `gpio_get_value(nr)` | `gpiod_get_value(desc)` | 同上 |
| `gpio_free(nr)` | 自动释放（devm） | 新接口配合 devm 自动管理 |
| `gpio_to_irq(nr)` | `gpiod_to_irq(desc)` | 同上 |

为什么旧接口被废弃：

1. 全局 GPIO 编号不可移植，不同平台编号不同。
2. 没有跟设备树绑定，配置和代码分离。
3. 没有自动资源管理。

### 3. 设备树里的 GPIO 描述

```dts
my_device {
    compatible = "vendor,mydev";
    
    /* GPIO 描述：<phandle pin_num flags> */
    enable-gpios = <&gpio0 12 GPIO_ACTIVE_LOW>;
    reset-gpios = <&gpio1 5 GPIO_ACTIVE_HIGH>;
    irq-gpios = <&gpio0 8 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>;
};
```

字段含义：

1. `&gpio0`：GPIO 控制器的 phandle。
2. `12`：引脚编号（在控制器内的偏移）。
3. `GPIO_ACTIVE_LOW`：有效电平标志，低电平有效时 GPIO 子系统自动取反。

常用 flags：

| Flag | 含义 |
| --- | --- |
| `GPIO_ACTIVE_HIGH` | 高电平有效（默认） |
| `GPIO_ACTIVE_LOW` | 低电平有效 |
| `GPIO_OPEN_DRAIN` | 开漏输出 |
| `GPIO_OPEN_SOURCE` | 开源输出 |
| `GPIO_PULL_UP` | 内部上拉 |
| `GPIO_PULL_DOWN` | 内部下拉 |

### 4. 驱动里获取和使用 GPIO

```c
struct mydev_priv {
    struct gpio_desc *enable_gpio;
    struct gpio_desc *reset_gpio;
};

static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) return -ENOMEM;

    /* 获取 GPIO，con_id 对应设备树里的 "enable-gpios" */
    priv->enable_gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(priv->enable_gpio))
        return PTR_ERR(priv->enable_gpio);

    priv->reset_gpio = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(priv->reset_gpio))
        return PTR_ERR(priv->reset_gpio);

    /* 使能设备 */
    gpiod_set_value(priv->enable_gpio, 1);

    /* 复位脉冲 */
    gpiod_set_value(priv->reset_gpio, 0);
    msleep(10);
    gpiod_set_value(priv->reset_gpio, 1);

    return 0;
}
```

关键点：

1. `devm_gpiod_get` 的第二个参数 `"enable"` 对应设备树里的 `enable-gpios`。
2. 第三个参数指定初始方向和值：`GPIOD_OUT_LOW`、`GPIOD_OUT_HIGH`、`GPIOD_IN`。
3. `gpiod_set_value` 的 1 表示"有效"，不是"高电平"。如果设备树标了 `GPIO_ACTIVE_LOW`，1 会输出低电平。

### 5. 有效电平（Active Level）的自动处理

这是 `gpiod` 接口最重要的设计之一：

```c
/* 设备树：enable-gpios = <&gpio0 12 GPIO_ACTIVE_LOW>; */

gpiod_set_value(priv->enable_gpio, 1);  /* "使能" → 输出低电平 */
gpiod_set_value(priv->enable_gpio, 0);  /* "禁能" → 输出高电平 */
```

驱动代码不需要关心物理电平是高还是低，只需要表达"逻辑上使能还是禁能"。GPIO 子系统根据设备树的 flags 自动处理取反。

这带来的好处：

1. 同一份驱动代码可以适配不同硬件（有的使能脚高有效，有的低有效）。
2. 只需要改设备树，不需要改驱动代码。
3. 代码意图更清晰：`1` 表示"开"，`0` 表示"关"。

### 6. GPIO 中断

```c
static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    int irq, ret;

    /* 获取 GPIO */
    priv->irq_gpio = devm_gpiod_get(&pdev->dev, "irq", GPIOD_IN);
    if (IS_ERR(priv->irq_gpio))
        return PTR_ERR(priv->irq_gpio);

    /* GPIO 转 IRQ 号 */
    irq = gpiod_to_irq(priv->irq_gpio);
    if (irq < 0)
        return irq;

    /* 注册中断 */
    ret = devm_request_irq(&pdev->dev, irq, my_irq_handler,
                           IRQF_TRIGGER_FALLING, "mydev_irq", priv);
    if (ret)
        return ret;

    return 0;
}
```

注意：

1. 不是所有 GPIO 都支持中断，需要查阅 SoC 数据手册。
2. `gpiod_to_irq` 返回的 IRQ 号跟 GPIO 控制器的中断控制器有关。
3. 触发类型要跟设备树 flags 一致，或者在 `request_irq` 时指定。

### 7. pinctrl 和 GPIO 的关系

```text
pinctrl 子系统：负责引脚复用（这个引脚是做 GPIO 还是 UART TX？）
GPIO 子系统：  负责引脚电平控制（这个 GPIO 输出高还是低？）
```

典型流程：

1. 设备树里定义引脚复用配置（pinctrl 节点）。
2. 驱动 `probe` 时，pinctrl 子系统自动把引脚配置成 GPIO 模式。
3. 驱动通过 `gpiod_*` 接口控制引脚电平。

大多数情况下，只要设备树里正确配置了 pinctrl，驱动代码不需要直接操作 pinctrl。

### 8. 用户态访问 GPIO

#### 方式1：libgpiod（推荐）

```bash
# 安装
apt install gpiod

# 命令行工具
gpioinfo              # 列出所有 GPIO
gpioset gpio0 12=1    # 设置 GPIO
gpioget gpio0 12      # 读取 GPIO
gpiomon gpio0 12      # 监控 GPIO 事件
```

C 语言接口：

```c
#include <gpiod.h>

struct gpiod_chip *chip = gpiod_chip_open_by_name("gpio0");
struct gpiod_line *line = gpiod_chip_get_line(chip, 12);

gpiod_line_request_output(line, "myapp", 0);
gpiod_line_set_value(line, 1);

gpiod_line_release(line);
gpiod_chip_close(chip);
```

#### 方式2：sysfs（旧接口，已废弃但仍然常见）

```bash
echo 12 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio12/direction
echo 1 > /sys/class/gpio/gpio12/value
cat /sys/class/gpio/gpio12/value
echo 12 > /sys/class/gpio/unexport
```

### 9. 多个 GPIO 的批量获取

```c
/* 设备树 */
my_device {
    compatible = "vendor,mydev";
    data-gpios = <&gpio0 0 GPIO_ACTIVE_HIGH>,
                 <&gpio0 1 GPIO_ACTIVE_HIGH>,
                 <&gpio0 2 GPIO_ACTIVE_HIGH>,
                 <&gpio0 3 GPIO_ACTIVE_HIGH>;
};

/* 驱动 */
struct gpio_descs *data_gpios;

data_gpios = devm_gpiod_get_array(&pdev->dev, "data", GPIOD_OUT_LOW);
if (IS_ERR(data_gpios))
    return PTR_ERR(data_gpios);

/* 设置所有 GPIO */
int values[] = {1, 0, 1, 1};
gpiod_set_array_value(data_gpios->ndescs, data_gpios->desc, data_gpios->info, values);
```

## 数据流 / 控制流 / 时序关系

GPIO 操作的完整链路：

```text
设备树定义 GPIO
-> pinctrl 配置引脚复用
-> 驱动 probe 时 devm_gpiod_get 获取描述符
-> gpiod_direction_* 设置方向
-> gpiod_set_value / gpiod_get_value 读写电平
-> gpiod_to_irq 转中断号（如需中断）
-> devm_request_irq 注册中断
-> 设备移除时自动释放
```

## 最小可运行示例

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>

struct mydev_priv {
    struct gpio_desc *enable_gpio;
    struct gpio_desc *reset_gpio;
    struct gpio_desc *status_gpio;
};

static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    int val;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) return -ENOMEM;

    priv->enable_gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(priv->enable_gpio)) {
        dev_err(&pdev->dev, "failed to get enable gpio\n");
        return PTR_ERR(priv->enable_gpio);
    }

    priv->reset_gpio = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(priv->reset_gpio))
        return PTR_ERR(priv->reset_gpio);

    priv->status_gpio = devm_gpiod_get(&pdev->dev, "status", GPIOD_IN);
    if (IS_ERR(priv->status_gpio))
        return PTR_ERR(priv->status_gpio);

    /* 复位序列 */
    gpiod_set_value(priv->reset_gpio, 0);
    msleep(10);
    gpiod_set_value(priv->reset_gpio, 1);
    msleep(50);

    /* 使能设备 */
    gpiod_set_value(priv->enable_gpio, 1);

    /* 读取状态 */
    val = gpiod_get_value(priv->status_gpio);
    dev_info(&pdev->dev, "status=%d\n", val);

    platform_set_drvdata(pdev, priv);
    return 0;
}

static int my_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);

    gpiod_set_value(priv->enable_gpio, 0);
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
        .name = "mydev-gpio",
        .of_match_table = my_match,
    },
};
module_platform_driver(my_driver);

MODULE_LICENSE("GPL");
```

对应设备树：

```dts
mydev {
    compatible = "vendor,mydev";
    enable-gpios = <&gpio0 12 GPIO_ACTIVE_LOW>;
    reset-gpios = <&gpio1 5 GPIO_ACTIVE_HIGH>;
    status-gpios = <&gpio0 8 GPIO_ACTIVE_HIGH>;
};
```

## 代码解读

### 1. 为什么 `devm_gpiod_get` 的 con_id 是 "enable"

`devm_gpiod_get` 会自动拼接 `-gpios` 后缀，所以 `"enable"` 对应设备树里的 `enable-gpios`。

### 2. 为什么 `GPIOD_OUT_LOW` 和 `GPIOD_OUT_HIGH` 同时指定方向和初始值

获取 GPIO 时就设置好方向和初始值，避免引脚处于不确定状态。这比先获取再设置方向更安全。

### 3. 为什么 `gpiod_set_value(priv->enable_gpio, 1)` 可能输出低电平

因为设备树里 `enable-gpios` 标了 `GPIO_ACTIVE_LOW`。`gpiod_set_value` 的参数是逻辑值，1 表示"使能"，GPIO 子系统自动取反输出低电平。

### 4. 为什么 `remove` 里只需要禁用设备

所有 GPIO 通过 `devm_gpiod_get` 获取，设备移除时自动释放。`remove` 只需要让硬件进入安全状态。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 获取 GPIO | `devm_gpiod_get` | `gpio_request` + 硬编码编号 | 旧接口已废弃，不可移植 |
| 设置电平 | `gpiod_set_value(desc, 1)` | 直接写寄存器 | 绕过子系统，破坏抽象 |
| 有效电平 | 设备树标 `GPIO_ACTIVE_LOW`，代码传逻辑值 | 代码里手动取反 | 不可移植，硬件改了要改代码 |
| 初始状态 | `devm_gpiod_get` 时指定 `GPIOD_OUT_LOW` | 获取后再设置 | 中间有不确定状态 |
| 中断获取 | `gpiod_to_irq` | 自己算 IRQ 号 | 不同平台映射不同 |
| 多 GPIO | `devm_gpiod_get_array` | 逐个获取 | 代码冗余，不原子 |

## 边界条件与适用范围

1. 不是所有引脚都能做 GPIO，有些引脚可能被其他外设占用。
2. 不是所有 GPIO 都支持中断，需要查阅 SoC 数据手册。
3. `gpiod_set_value` 不能在中断上下文中睡眠，某些 GPIO 控制器可能通过 I2C/SPI 扩展，操作时会睡眠。
4. `gpiod_get_value` 返回的是物理电平还是逻辑值取决于上下文：输入时返回物理电平，输出时返回逻辑值。
5. GPIO 控制器通过 I2C/SPI 扩展时，操作可能睡眠，不能在硬中断上下文使用。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| `devm_gpiod_get` 返回错误 | 设备树属性名不匹配 | 检查 con_id 和 `-gpios` 后缀 |
| GPIO 电平不对 | ACTIVE_LOW 配置错误 | 用示波器量物理电平，对比设备树 |
| 中断不触发 | GPIO 不支持中断或触发类型错 | `cat /proc/interrupts` 看计数 |
| 操作 GPIO 时崩溃 | 在中断上下文操作 I2C 扩展 GPIO | 改用工作队列 |
| pinctrl 冲突 | 引脚被其他外设占用 | `cat /sys/kernel/debug/pinctrl/` |
| 引脚状态不确定 | 获取和设置方向之间有间隙 | 获取时指定初始值 |

### 推荐排查顺序

1. 先确认设备树里 GPIO 属性是否正确。
2. 再确认 `devm_gpiod_get` 是否成功（检查返回值）。
3. 再用 `gpioinfo` 命令确认引脚状态。
4. 再用示波器或万用表量物理电平。
5. 最后看 pinctrl 和中断配置。

## 工程落地建议

### 1. 新驱动一律用 gpiod 接口

不要用旧 `gpio_*` 接口，不要直接操作寄存器。

### 2. 有效电平在设备树里标清楚

驱动代码只传逻辑值（1=使能，0=禁能），硬件差异全部在设备树里处理。

### 3. 获取时指定初始状态

避免引脚在获取和设置方向之间处于不确定状态。

### 4. 用 `cat /sys/kernel/debug/gpio` 调试

这个文件列出所有 GPIO 的当前状态，是排查 GPIO 问题的第一站。

### 5. 注意 I2C/SPI 扩展 GPIO 的睡眠问题

如果 GPIO 控制器通过 I2C 或 SPI 连接，操作可能睡眠。不能在硬中断上下文使用这类 GPIO。

## 性能、稳定性、可维护性影响

1. `gpiod` 接口的运行时开销极小，不影响性能。
2. 有效电平自动处理让同一份驱动适配不同硬件，可维护性显著提升。
3. `devm_gpiod_get` 自动管理资源，减少泄漏风险。
4. 通过 GPIO 子系统操作比直接写寄存器更安全，不会意外影响其他引脚。
5. 设备树 + gpiod 的组合让硬件变更不需要改驱动代码，只需要改设备树。

## 面试 / 问答怎么讲

### 30 秒版本

Linux GPIO 驱动使用 `gpiod_*` 接口，通过设备树描述 GPIO 属性，驱动用 `devm_gpiod_get` 获取描述符。`gpiod_set_value` 传逻辑值，GPIO 子系统根据设备树的 ACTIVE_LOW 标志自动处理物理电平取反。GPIO 中断通过 `gpiod_to_irq` 转换后注册。

### 3 分钟版本

可以从分层讲起：GPIO 控制器驱动注册 `gpio_chip`，核心层提供 `gpiod_*` 接口，消费者驱动通过设备树获取 GPIO。然后说明旧 `gpio_*` 接口为什么被废弃：全局编号不可移植。再讲有效电平自动处理：驱动只传逻辑值，硬件差异在设备树里解决。最后补充 GPIO 中断和 pinctrl 的关系。

### 10 分钟版本

可以结合一个完整驱动展开：设备树定义 enable/reset/status 三个 GPIO，驱动在 `probe` 里获取并执行复位序列。然后讨论 ACTIVE_LOW 的自动取反机制、I2C 扩展 GPIO 的睡眠问题、pinctrl 和 GPIO 的配合。再展示用户态通过 libgpiod 访问 GPIO 的方式。最后说明排障主线：设备树 → gpiod_get → gpioinfo → 示波器。

## 实战练习

1. 写一个 platform 驱动，获取一个 GPIO 并控制 LED 闪烁。
2. 把设备树里的 `GPIO_ACTIVE_HIGH` 改成 `GPIO_ACTIVE_LOW`，验证驱动代码不需要改。
3. 用 `gpiod_to_irq` 把一个 GPIO 配置成中断，在中断里打印日志。
4. 用 `libgpiod` 命令行工具读取和设置 GPIO。
5. 用 `devm_gpiod_get_array` 批量获取 4 个 GPIO 并同时设置值。

## 关键要点

1. 新驱动一律用 `gpiod_*` 接口，不要用旧 `gpio_*` 接口。
2. GPIO 在设备树里通过 `xxx-gpios` 属性描述，驱动用 `devm_gpiod_get` 获取。
3. `gpiod_set_value` 传逻辑值，GPIO 子系统根据 ACTIVE_LOW 自动处理物理电平。
4. 获取 GPIO 时指定初始方向和值，避免不确定状态。
5. GPIO 中断通过 `gpiod_to_irq` 转换后用标准中断接口注册。
6. pinctrl 负责引脚复用，GPIO 负责电平控制，两者配合工作。
7. I2C/SPI 扩展 GPIO 的操作可能睡眠，不能在硬中断上下文使用。

## 关联笔记

1. `外设-GPIO基础`
2. `驱动开发-platform驱动基础`
3. `驱动开发-devm资源管理`
4. `驱动开发-IRQ处理基础`
5. `Linux-pinctrl与GPIO`
6. `Linux-设备树`
