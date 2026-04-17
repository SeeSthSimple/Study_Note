# 硬件-GPIO与电平

## 原始问题

GPIO 到底是什么？推挽输出和开漏输出有什么区别？为什么 I2C 必须用开漏？上拉下拉电阻到底起什么作用？3.3V 和 5V 系统混接会怎样？悬空引脚为什么读值不稳定？active-low 是什么意思？

## 先给结论

GPIO 的本质是"可配置的数字信号接口"，电平的本质是"电压范围表示的 0 和 1"：

1. GPIO 不是一个简单的"引脚"，而是一组可配置的硬件单元：方向、驱动方式、上下拉、复用功能、中断能力。
2. 推挽输出能主动驱动高低电平，开漏输出只能拉低，需要外部上拉才能输出高。
3. 上拉下拉电阻的作用是给悬空信号一个确定默认电平，防止漂移。
4. 不同电压域的 IO 不能直接连接，需要电平转换或确认兼容性。
5. active-low 意味着低电平是"有效"状态，软件逻辑要反转理解。

如果只知道"GPIO 就是输入输出引脚"，但讲不清推挽和开漏的区别、上下拉的必要性、电平兼容的约束，就还没有真正理解 GPIO 与电平。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 不理解推挽和开漏输出的区别，选错驱动方式导致总线冲突或信号异常。
2. 不知道上拉下拉电阻的作用，遇到悬空输入抖动问题无从下手。
3. 不理解 IO 电平兼容性，5V 信号直接接 3.3V IO 导致芯片损坏。
4. 不理解 active-low，软件逻辑写反。
5. 面试时被问"为什么 I2C 要用开漏+上拉"，只能背结论讲不清原理。

它在 AI 时代依然重要，因为 AI 生成设备树和驱动代码时经常搞错 GPIO 极性、忽略上下拉配置、不检查电平兼容性。理解 GPIO 和电平的本质后，你才能判断 AI 给的配置在物理上是否正确。

## 核心概念 / 本质机制

### 1. GPIO 的硬件结构

一个 GPIO 引脚内部通常包含这些模块：

```text
                    +-- 上拉电阻 (可选)
                    |
输入路径:  引脚 -->|--> 输入施密特触发器 --> 输入数据寄存器
                    |
                    +-- 下拉电阻 (可选)

输出路径:  输出数据寄存器 --> 输出驱动器 --> 引脚

复用路径:  外设功能信号 --> 复用选择器 --> 引脚

中断路径:  输入施密特触发器 --> 中断检测逻辑 --> 中断控制器
```

### 2. 推挽输出（Push-Pull）

推挽输出有两个 MOS 管（上管 PMOS、下管 NMOS）交替导通：

```text
        VDD
         |
      [PMOS]  <-- 上管
         |---+--- 引脚
      [NMOS]  <-- 下管
         |
        GND

输出高: PMOS 导通, NMOS 关断 --> 引脚被拉向 VDD
输出低: PMOS 关断, NMOS 导通 --> 引脚被拉向 GND
```

推挽输出的特点：

1. 能主动驱动高电平和低电平。
2. 驱动能力强（取决于 MOS 管尺寸）。
3. 不需要外部上拉电阻。
4. 不能多个推挽输出直接连在一起（会短路）。

适用场景：LED 驱动、SPI 信号、UART TX、普通输出控制。

### 3. 开漏输出（Open-Drain / Open-Collector）

开漏输出只有下管（NMOS），没有上管：

```text
        VDD
         |
     [外部上拉电阻]  <-- 必须外部提供
         |
         +--- 引脚
         |
      [NMOS]  <-- 下管
         |
        GND

输出低: NMOS 导通 --> 引脚被拉向 GND
输出高: NMOS 关断 --> 引脚被外部上拉电阻拉向 VDD
```

开漏输出的特点：

1. 只能主动拉低，不能主动拉高。
2. 必须外部上拉电阻才能输出高电平。
3. 多个开漏输出可以连在一起（线与逻辑）。
4. 上升沿速度取决于上拉电阻和寄生电容（RC 时间常数）。

适用场景：I2C 总线、1-Wire、共享中断线、电平转换。

### 4. 为什么 I2C 必须用开漏

I2C 协议要求：

1. 多个设备共享同一根 SDA/SCL 线。
2. 任何设备都可以拉低总线。
3. 没有设备拉低时，总线被上拉电阻拉高。

如果用推挽输出：

1. 一个设备输出高，另一个输出低 → 短路 → 大电流 → 可能烧芯片。
2. 无法实现"线与"逻辑。

开漏 + 上拉电阻实现了"线与"：

```text
所有设备都不拉低 → 上拉电阻拉高 → 总线为高
任何一个设备拉低 → 总线为低 → "线与"逻辑
```

### 5. 上拉电阻和下拉电阻

上拉电阻：把信号默认拉向高电平。

下拉电阻：把信号默认拉向低电平。

为什么需要：

1. **防止悬空**：CMOS 输入引脚悬空时，栅极电压不确定，可能漂移在中间区域，导致：
   - 读值随机跳变。
   - 输入缓冲器 PMOS 和 NMOS 同时微导通，增加功耗。
   - 可能产生振荡。
2. **总线默认状态**：I2C、SPI 等总线需要明确的空闲状态。
3. **按键去抖**：按键一端接 GPIO，另一端接 GND，需要上拉电阻给默认高电平。

上拉电阻的取值：

| 场景 | 典型阻值 | 说明 |
| --- | --- | --- |
| I2C 标准模式 | 4.7kΩ | 100kHz，上升时间要求宽松 |
| I2C 快速模式 | 2.2kΩ | 400kHz，需要更快上升沿 |
| 按键上拉 | 10kΩ~100kΩ | 不需要快速响应 |
| SPI 片选上拉 | 10kΩ | 防止 CS 悬空导致 Flash 误操作 |
| 复位引脚上拉 | 10kΩ | 确保复位释放 |

### 6. 高阻态（High-Z / Hi-Z）

高阻态是引脚"不参与"的状态：

1. 引脚既不输出高，也不输出低，相当于"断开"。
2. 外部信号可以自由驱动这条线。
3. 多设备共享总线时，非活动设备必须进入高阻态。

典型场景：

1. SPI 总线上未选中的从设备，MISO 引脚进入高阻态。
2. 三态缓冲器（74HC245）的输出使能关闭时。
3. GPIO 配置为输入模式时，输出驱动器关闭，相当于高阻。

### 7. 数字电平标准

不同电压系统的逻辑电平范围不同：

```text
3.3V LVTTL 电平标准:
  输入高电平 (VIH): > 2.0V
  输入低电平 (VIL): < 0.8V
  输出高电平 (VOH): > 2.4V
  输出低电平 (VOL): < 0.4V

5V TTL 电平标准:
  输入高电平 (VIH): > 2.0V
  输入低电平 (VIL): < 0.8V
  输出高电平 (VOH): > 2.4V
  输出低电平 (VOL): < 0.4V

1.8V LVCMOS 电平标准:
  输入高电平 (VIH): > 1.17V (0.65 × VDD)
  输入低电平 (VIL): < 0.63V (0.35 × VDD)
```

关键结论：

1. 3.3V LVTTL 输出可以直接驱动 5V TTL 输入（因为 VIH 都是 2.0V）。
2. 5V TTL 输出不能直接接 3.3V 输入（除非 3.3V IO 是 5V 容忍的）。
3. 1.8V 和 3.3V 之间通常需要电平转换。

### 8. 5V 容忍（5V Tolerant）

有些 3.3V IO 引脚标注为"5V Tolerant"，意思是：

1. 这个引脚的 VDD 是 3.3V。
2. 但输入引脚可以承受 5V 电压而不损坏。
3. 输出高电平仍然是 3.3V，不是 5V。

注意：5V 容忍 ≠ 5V 输出。输出仍然是 3.3V 电平。

### 9. Active-Low 信号

Active-Low 表示低电平是"有效"状态：

| 信号名 | 含义 | 有效电平 |
| --- | --- | --- |
| RESET_N | 复位信号 | 低有效（低电平 = 复位中） |
| CS_N / CS# | 片选信号 | 低有效（低电平 = 选中） |
| EN_N | 使能信号（低有效版本） | 低有效 |
| LED_N | LED 控制（低亮） | 低有效 |

为什么很多信号用 active-low：

1. 早期 TTL 逻辑下拉能力比上拉强。
2. 断电或断线时默认进入安全状态（复位、不选中）。
3. 开漏输出天然适合低有效。

软件处理 active-low：

```c
/* LED 低有效：写 0 点亮，写 1 熄灭 */
gpiod_set_value(led_gpio, 0);  /* 点亮 */
gpiod_set_value(led_gpio, 1);  /* 熄灭 */

/* 或者用 GPIO_ACTIVE_LOW 标志，让 gpiod 框架自动反转 */
/* 设备树: led-gpios = <&gpio0 5 GPIO_ACTIVE_LOW>; */
gpiod_set_value(led_gpio, 1);  /* 框架自动反转，实际输出低 → 点亮 */
```

### 10. GPIO 的寄存器级操作

典型 GPIO 寄存器组：

| 寄存器 | 作用 |
| --- | --- |
| MODER | 模式选择（输入/输出/复用/模拟） |
| OTYPER | 输出类型（推挽/开漏） |
| OSPEEDR | 输出速度（低/中/高/极高） |
| PUPDR | 上下拉配置（无/上拉/下拉） |
| IDR | 输入数据（只读） |
| ODR | 输出数据 |
| BSRR | 位设置/清除（原子操作） |
| LCKR | 配置锁定 |
| AFRL/AFRH | 复用功能选择 |

BSRR 寄存器的设计特别巧妙：

```c
/* 传统读改写方式（非原子，可能被中断打断） */
uint32_t val = readl(base + GPIO_ODR);
val |= (1 << 5);
writel(val, base + GPIO_ODR);

/* BSRR 方式（原子操作，一次写入完成） */
writel(1 << 5, base + GPIO_BSRR);     /* 置位 bit5 */
writel(1 << (5 + 16), base + GPIO_BSRR); /* 清除 bit5 */
```

BSRR 的低 16 位是置位操作，高 16 位是清除操作，写 1 执行，写 0 无效。

## 数据流 / 控制流 / 时序关系

### GPIO 输出路径

```text
软件写 ODR/BSRR 寄存器
  |
  V
输出数据锁存
  |
  V
输出类型选择 (推挽/开漏)
  |
  V
驱动 MOS 管导通/关断
  |
  V
引脚电平变化
  |
  V
外部电路响应 (LED 亮灭、总线信号变化等)
```

### GPIO 输入路径

```text
外部信号改变引脚电平
  |
  V
施密特触发器整形 (消除缓慢边沿和噪声)
  |
  V
输入数据寄存器 (IDR) 更新
  |
  V
软件读取 IDR 或中断检测逻辑响应
```

### I2C 总线信号流

```text
主机发起:
  主机开漏输出拉低 SDA/SCL
    → 总线变低
  主机释放开漏输出
    → 上拉电阻拉高总线
    → 总线变高

从机应答:
  从机开漏输出拉低 SDA
    → 总线变低 (ACK)
  从机释放开漏输出
    → 上拉电阻拉高总线
```

## 最小可运行示例

### 裸机 GPIO 配置（STM32 风格）

```c
#include <stdint.h>

#define GPIOA_BASE    0x48000000UL
#define GPIOA_MODER   (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_OTYPER  (*(volatile uint32_t *)(GPIOA_BASE + 0x04))
#define GPIOA_OSPEEDR (*(volatile uint32_t *)(GPIOA_BASE + 0x08))
#define GPIOA_PUPDR   (*(volatile uint32_t *)(GPIOA_BASE + 0x0C))
#define GPIOA_IDR     (*(volatile uint32_t *)(GPIOA_BASE + 0x10))
#define GPIOA_ODR     (*(volatile uint32_t *)(GPIOA_BASE + 0x14))
#define GPIOA_BSRR    (*(volatile uint32_t *)(GPIOA_BASE + 0x18))

#define LED_PIN       5
#define KEY_PIN       0

static void gpio_init(void)
{
    /* PA5: 推挽输出，高速，无上下拉 */
    GPIOA_MODER   = (GPIOA_MODER & ~(3 << (LED_PIN * 2))) | (1 << (LED_PIN * 2));
    GPIOA_OTYPER &= ~(1 << LED_PIN);
    GPIOA_OSPEEDR = (GPIOA_OSPEEDR & ~(3 << (LED_PIN * 2))) | (3 << (LED_PIN * 2));
    GPIOA_PUPDR  &= ~(3 << (LED_PIN * 2));

    /* PA0: 输入，下拉（按键接 VCC） */
    GPIOA_MODER  &= ~(3 << (KEY_PIN * 2));
    GPIOA_PUPDR  = (GPIOA_PUPDR & ~(3 << (KEY_PIN * 2))) | (2 << (KEY_PIN * 2));
}

static void led_on(void)
{
    GPIOA_BSRR = (1 << LED_PIN);
}

static void led_off(void)
{
    GPIOA_BSRR = (1 << (LED_PIN + 16));
}

static uint32_t key_read(void)
{
    return (GPIOA_IDR >> KEY_PIN) & 1;
}

int main(void)
{
    gpio_init();

    while (1) {
        if (key_read())
            led_on();
        else
            led_off();
    }
}
```

### Linux 设备树 GPIO 配置

```dts
/* LED: 低有效，推挽输出 */
led {
    gpios = <&gpio0 5 GPIO_ACTIVE_LOW>;
    default-state = "off";
};

/* 按键: 高有效，内部下拉 */
button {
    gpios = <&gpio0 0 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>;
    interrupt-parent = <&gpio0>;
    interrupts = <0 IRQ_TYPE_EDGE_RISING>;
};

/* I2C: 开漏 + 外部上拉 */
&i2c1 {
    pinctrl-0 = <&i2c1_pins>;
    pinctrl-names = "default";
    status = "okay";
};

/* pinctrl 配置 I2C 引脚为开漏 */
i2c1_pins: i2c1-pins {
    pins = "GPIO0_A3", "GPIO0_A4";
    function = "i2c1";
    bias-pull-up;
    drive-open-drain;
};
```

### Linux 驱动中使用 gpiod

```c
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

struct mydev_priv {
    struct gpio_desc *led;
    struct gpio_desc *key;
};

static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    int val;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->led = devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
    if (IS_ERR(priv->led))
        return PTR_ERR(priv->led);

    priv->key = devm_gpiod_get(&pdev->dev, "key", GPIOD_IN);
    if (IS_ERR(priv->key))
        return PTR_ERR(priv->key);

    val = gpiod_get_value(priv->key);
    dev_info(&pdev->dev, "key=%d\n", val);

    gpiod_set_value(priv->led, 1);

    platform_set_drvdata(pdev, priv);
    return 0;
}

static const struct of_device_id my_match[] = {
    { .compatible = "vendor,mydev" },
    {}
};
MODULE_DEVICE_TABLE(of, my_match);

static struct platform_driver my_driver = {
    .probe  = my_probe,
    .driver = {
        .name = "mydev-gpio",
        .of_match_table = my_match,
    },
};
module_platform_driver(my_driver);

MODULE_LICENSE("GPL");
```

## 代码解读

### 1. 为什么用 BSRR 而不是 ODR 做位操作

BSRR 是原子操作：一次写就能设置或清除指定位，不需要读改写。ODR 需要先读、再改、再写，如果中间被中断打断，可能覆盖中断里的修改。

### 2. 为什么 `devm_gpiod_get` 的第二个参数是 "led" 而不是引脚号

gpiod 框架通过设备树里的 `led-gpios` 属性名匹配，而不是直接指定引脚号。这样驱动代码不依赖具体引脚号，换板子只需要改设备树。

### 3. 为什么 `GPIOD_OUT_LOW` 是推荐的初始状态

LED 通常是低有效，初始输出低（逻辑 0）对应物理低电平，LED 不亮。如果用 `GPIOD_OUT_HIGH`，配合 `GPIO_ACTIVE_LOW`，框架会自动反转，实际输出低电平，LED 点亮——这可能不是期望的初始状态。

### 4. 为什么 I2C 引脚要配 `drive-open-drain`

I2C 协议要求开漏输出。如果 pinctrl 不配 `drive-open-drain`，引脚可能默认推挽，一个设备输出高、另一个输出低时会产生总线冲突。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| I2C 引脚 | 开漏 + 外部上拉 | 推挽输出 | 多设备共享会短路 |
| 按键输入 | 明确上下拉 | 悬空直接读 | 值会漂移抖动 |
| 位操作 | 用 BSRR 原子操作 | 用 ODR 读改写 | 非原子，可能被中断打断 |
| active-low | 设备树标 GPIO_ACTIVE_LOW | 软件里手动反转 | 逻辑散落各处，容易漏 |
| LED 初始状态 | GPIOD_OUT_LOW + ACTIVE_LOW | 不考虑极性 | 初始状态可能意外点亮 |
| 5V 信号接 3.3V IO | 确认 5V 容忍或加电平转换 | 直接连接 | 可能损坏芯片 |
| 多设备共享线 | 开漏 + 上拉 | 推挽直连 | 总线冲突 |

## 边界条件与适用范围

1. 不同 SoC 的 GPIO 寄存器布局差异很大，STM32 的 MODER/OTYPER/BSRR 不是通用标准。
2. 上拉电阻值影响上升沿速度，I2C 快速模式需要更小的上拉电阻。
3. 开漏输出的上升沿比推挽慢，不适合高速信号（如 SPI CLK）。
4. 某些 GPIO 有电流限制（通常 4~8mA），不能直接驱动大负载。
5. GPIO 输出速度设置影响 EMI：速度越高，边沿越陡，高频噪声越大。
6. 5V 容忍只是输入不怕 5V，输出仍然是 3.3V 电平。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| I2C 通信失败 | 没上拉；开漏没配；地址错 | 万用表量 SDA/SCL 静态电平 |
| 按键读值随机跳 | 没上下拉；悬空 | 万用表量引脚电压 |
| LED 不亮 | 极性反了；GPIO 没输出 | 万用表量引脚电平 |
| 多设备总线冲突 | 用了推挽而不是开漏 | 示波器看总线波形 |
| GPIO 写了没效果 | 方向没配成输出；pinmux 错 | 读回寄存器确认 |
| 3.3V IO 接了 5V 后异常 | IO 不是 5V 容忍 | 查数据手册绝对最大额定值 |
| BSRR 操作没生效 | 写错位（置位/清除搞反） | 查 BSRR 寄存器位定义 |

### 推荐排查顺序

1. 先确认 GPIO 方向配置是否正确。
2. 再确认 pinmux 是否切到了 GPIO 功能。
3. 再确认上下拉配置。
4. 再确认输出类型（推挽/开漏）。
5. 用万用表量引脚实际电平。
6. 最后确认软件逻辑和极性。

## 工程落地建议

### 1. 设备树里明确标注极性

```dts
/* 低有效 LED */
led-gpios = <&gpio0 5 GPIO_ACTIVE_LOW>;

/* 高有效按键，内部上拉 */
key-gpios = <&gpio0 0 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>;
```

让 gpiod 框架自动处理极性，驱动代码里不需要手动反转。

### 2. 用 gpiod 而不是 gpio 号

```c
/* 推荐: 通过设备树属性名获取 */
struct gpio_desc *led = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);

/* 不推荐: 硬编码引脚号 */
int led_gpio = 5;
gpio_request(led_gpio, "led");
gpio_direction_output(led_gpio, 0);
```

gpiod 方式解耦了引脚号，换板子只改设备树。

### 3. I2C 上拉电阻值计算

```text
Rp(min) = (VDD - VOL_max) / IOL

其中:
  VDD = 3.3V
  VOL_max = 0.4V (I2C 规范)
  IOL = 3mA (标准模式) 或 6mA (快速模式)

标准模式: Rp(min) = (3.3 - 0.4) / 0.003 ≈ 967Ω
快速模式: Rp(min) = (3.3 - 0.4) / 0.006 ≈ 483Ω

Rp(max) 由上升时间要求决定:
  tr = 0.8473 × Rp × Cb

其中 Cb 是总线总电容，tr 是允许的上升时间
```

### 4. GPIO 输出速度选择

| 速度等级 | 典型应用 | 说明 |
| --- | --- | --- |
| 低 (< 2MHz) | LED、继电器控制 | EMI 最小 |
| 中 (2~25MHz) | UART、I2C | 平衡速度和 EMI |
| 高 (25~50MHz) | SPI 低速模式 | 边沿较陡 |
| 极高 (> 50MHz) | SPI 高速、高速总线 | EMI 最大，需要阻抗匹配 |

不是越快越好：速度越高边沿越陡，高频噪声越大，可能引起 EMI 问题。

## 性能、稳定性、可维护性影响

1. 推挽输出驱动能力强、边沿快，适合高速信号；开漏输出需要上拉、上升沿慢，但支持多设备共享。
2. 上拉电阻值影响 I2C 总线速度：电阻太大上升沿慢，限制最高频率。
3. GPIO 输出速度设置影响 EMI：不必要的高速输出会增加噪声。
4. 正确处理 active-low 能显著减少"逻辑反转"类 bug。
5. 用 gpiod 框架代替硬编码引脚号，能提高代码可移植性和可维护性。

## 面试 / 问答怎么讲

### 30 秒版本

GPIO 有推挽和开漏两种输出方式。推挽能主动驱动高低电平，开漏只能拉低需要外部上拉。I2C 必须用开漏因为多设备共享总线，推挽会短路。上拉下拉电阻给悬空信号提供默认电平。3.3V 和 5V 系统不能直接连，需要确认电平兼容性。

### 3 分钟版本

从 GPIO 内部结构讲起：推挽有上下两个 MOS 管交替导通，开漏只有下管。然后说明 I2C 为什么必须开漏：多设备共享需要"线与"逻辑，推挽会短路。再讲上拉下拉的作用：防止 CMOS 输入悬空漂移。最后讲 active-low 和电平兼容：很多控制信号是低有效，不同电压域不能直接连接。

### 10 分钟版本

可以系统展开：先画 GPIO 内部结构图，详细说明推挽和开漏的工作原理。然后讲 I2C 总线的开漏+上拉机制，包括上拉电阻值计算。再讲电平标准（VIH/VIL/VOH/VOL）和 5V 容忍。然后讲 BSRR 的原子操作设计。再讲 active-low 在设备树和 gpiod 框架中的处理方式。最后讲 GPIO 输出速度对 EMI 的影响。

## 实战练习

1. 在裸机上分别配置推挽和开漏输出，用示波器观察上升沿速度差异。
2. 在 I2C 总线上去掉上拉电阻，观察通信是否失败，然后用逻辑分析仪看波形。
3. 把一个按键输入引脚设为悬空（无上下拉），用万用表观察电压漂移。
4. 在 Linux 驱动里用 gpiod 框架控制一个 active-low LED，验证框架自动反转。
5. 计算 I2C 总线在 400kHz 模式下的上拉电阻取值范围。

## 关键要点

1. GPIO 不是简单引脚，是可配置的硬件单元：方向、驱动方式、上下拉、复用、中断。
2. 推挽能驱动高低，开漏只能拉低需要上拉；I2C 必须开漏。
3. 上拉下拉给悬空信号提供默认电平，防止 CMOS 输入漂移。
4. 不同电压域不能直连，需要确认电平兼容或加电平转换。
5. active-low 信号低电平有效，用 gpiod 框架的 GPIO_ACTIVE_LOW 自动处理。
6. BSRR 提供原子位操作，比 ODR 读改写更安全。
7. GPIO 输出速度不是越快越好，高速会增加 EMI。

## 关联笔记

1. `硬件-基础知识`
2. `硬件-电平转换与接口兼容`
3. `硬件-原理图阅读方法`
4. `外设-GPIO基础`
5. `驱动开发-GPIO驱动基础`
6. `Linux-pinctrl与GPIO`
