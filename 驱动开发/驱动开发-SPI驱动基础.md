# 驱动开发-SPI驱动基础

## 原始问题

Linux 里的 SPI 驱动到底和 platform 驱动、I2C 驱动有什么不同？为什么 SPI 设备已经挂在总线上了，驱动还是会遇到模式不对、`probe` 成功但读 ID 失败、片选时序不对、数据全是 `0xFF` 这种问题？真正写一个 SPI 驱动时，主线应该怎么抓？

## 先给结论

SPI 驱动的本质，是让一个挂在 SPI 控制器下的从设备通过 Linux SPI 总线模型接入内核，并按目标器件要求组织事务完成寄存器或数据访问。

先记住下面几个结论：

1. SPI 驱动的关键不只是“能发字节”，而是 `mode`、位宽、位序、片选时序、最大时钟、dummy 周期这些约束必须和器件手册匹配。
2. SPI 总线层通了，不代表驱动就对了；很多问题发生在“事务格式对不对”，而不是“有没有波形”。
3. `probe` 的核心不是写完整功能，而是先建立一个最小闭环，例如成功读出 chip ID。
4. SPI 驱动排障主线通常是：控制器工作正常 -> 设备树 / board info 创建设备 -> 驱动匹配 -> SPI 参数配置正确 -> 最小读写事务正确 -> 再扩展功能。
5. AI 可以快速生成 `spi_driver` 模板，但它不知道你的器件是 `Mode 0` 还是 `Mode 3`，不知道读寄存器前是否要 dummy byte，也不知道 CS 拉低后要不要延时。真正决定驱动能不能跑的是你对设备事务格式的理解。

如果只能写出一个 `spi_write_then_read()` 示例，却讲不清 `spi_device` 上的 `mode`、`bits_per_word`、`max_speed_hz` 为什么重要，也讲不清为什么 SPI 比 I2C 更依赖器件手册，就说明这部分还没有真正掌握。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 知道 SPI 外设原理，但不会把它写成 Linux 驱动。
2. 驱动已经 `probe` 了，但读不到寄存器、chip ID 全错、数据全是 `0x00/0xFF`，不知道先查哪一层。
3. 面试和工作中经常会被问到“SPI 驱动怎么写”“和 I2C 驱动有什么区别”，缺少稳定主线。
4. 后续做 SPI Flash、屏幕、ADC、传感器或带中断的 SPI 外设时，需要一篇连接外设知识和 Linux 驱动模型的桥梁笔记。

它在 AI 时代仍然重要，因为 AI 很容易生成一个能注册到总线上的驱动模板，但它不会自动知道你的器件命令字里读写位在哪一位，也不会替你判断一次事务里 CS 应该保持多久。SPI 理解不够时，AI 只会让你更快地得到一堆“看起来合理”的错误驱动。

## 核心概念 / 本质机制

### 1. SPI 驱动和 platform 驱动的本质区别

platform 驱动面对的通常是：

1. 固定 MMIO 设备。
2. 通过寄存器窗口直接访问。

SPI 驱动面对的则是：

1. 挂在某个 SPI 控制器下面的从设备。
2. 通过 SPI 事务和从设备交换数据。

所以 SPI 驱动真正拿到的核心对象是：

1. `struct spi_device`

它不仅表达“这是哪个设备”，还表达：

1. 用哪个控制器。
2. 片选是谁。
3. 当前模式、位宽、最大频率是多少。

### 2. SPI 和 I2C 驱动的关键差异

I2C 更像：

1. 总线层约束较强。
2. 地址和 ACK 机制比较统一。

SPI 更像：

1. 总线电气形式统一，但设备事务格式差异极大。
2. 没有统一地址和 ACK 语义。
3. 真正能否通信高度依赖器件手册给出的命令和时序。

所以 SPI 驱动更容易出现：

1. 波形有了，但事务格式不对。
2. `probe` 了，但读出来全错。
3. 某芯片能跑，换个兼容型号就不对。

### 3. `spi_device` 上最关键的几个参数

#### 3.1 `mode`

它通常决定：

1. `CPOL`
2. `CPHA`
3. 某些特殊模式位

如果模式错了，常见现象就是：

1. 数据位错位。
2. 读回值稳定但完全不对。
3. 低速偶尔能用，高速就全错。

#### 3.2 `bits_per_word`

默认很多设备按 8 位传输，但也有：

1. 16 位
2. 32 位
3. 特殊打包格式

位宽错了，事务边界和数据解释都会错。

#### 3.3 `max_speed_hz`

太高时常见问题：

1. 器件采样不到。
2. 板级走线边沿太差。
3. 某些 dummy 周期或时序约束不满足。

所以最小闭环阶段常常先低速跑通，再逐步提升。

### 4. 为什么 `spi_setup()` 很关键

在驱动里设置：

1. `spi->mode`
2. `spi->bits_per_word`
3. `spi->max_speed_hz`

之后，通常要调用 `spi_setup()`，让控制器层真正应用这些配置。

如果你只改了字段不 `setup`，表面代码像配了，底层控制器未必真的按新配置跑。

### 5. 什么叫 SPI 驱动的“最小闭环”

通常不是一上来就做完整业务，而是：

1. 匹配成功。
2. 配置正确的 SPI 参数。
3. 发出一个最小读事务。
4. 成功读到固定 chip ID 或版本寄存器。

这一步极其重要，因为它把问题范围大幅缩小到：

1. 总线和模式是否基本正确。
2. 事务格式是否基本正确。
3. 目标芯片是否真的在线。

### 6. 为什么事务格式决定成败

典型 SPI 器件手册会约定：

1. 读命令码是多少。
2. 地址长度几字节。
3. 数据在第几个字节开始。
4. 是否需要 dummy byte 或 dummy cycle。
5. CS 拉低后和拉高前是否要满足保持时间。

所以常见读寄存器链路可能是：

```text
CS 拉低
-> 发命令字
-> 发寄存器地址
-> 发一个 dummy byte
-> 再收数据
-> CS 拉高
```

只要其中任何一步和手册不匹配，波形看起来再“正常”，读回也可能全错。

### 7. 为什么 `spi_sync_transfer()` 很常用

因为一个 SPI 事务往往不是单一方向：

1. 先发命令。
2. 再发地址。
3. 再读数据。

用 `spi_sync_transfer()` 或多个 `spi_transfer` 组合，可以更清晰表达：

1. 每一段传输长度。
2. 每一段的 tx/rx buffer。
3. 是否保持片选不释放。

这比把所有逻辑硬塞进一个模糊的读写函数更清楚。

## 数据流 / 控制流 / 时序关系

下面用“读 chip ID”的典型 SPI 驱动链路来理解：

```text
设备树创建 spi_device
-> SPI 驱动注册并匹配
-> probe 获取 spi_device
-> 设置 mode / bits_per_word / max_speed_hz
-> 调用 spi_setup()
-> 构造最小读 ID 事务
-> 控制器按事务驱动 SCLK / MOSI / MISO / CS
-> 设备返回 chip ID
-> 驱动确认型号正确
-> 再继续初始化中断、缓存和上层接口
```

这条链路里最常见的问题包括：

1. `probe` 成功，但 SPI 参数没真正应用。
2. 模式错误，导致读回值错位。
3. 地址长度或 dummy byte 数量不对。
4. 频率太高，低速能跑高速不行。
5. 片选控制或保持时间不满足器件要求。

## 最小可运行示例

下面给一个最小 SPI 驱动闭环，目标是读出一个 8 位 chip ID：

```c
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

struct demo_spi_priv {
    struct spi_device *spi;
};

static int demo_spi_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
    u8 tx = reg | 0x80; /* 假设最高位表示读 */
    int ret;

    ret = spi_write_then_read(spi, &tx, 1, val, 1);
    if (ret < 0)
        return ret;

    return 0;
}

static int demo_spi_probe(struct spi_device *spi)
{
    struct demo_spi_priv *priv;
    u8 chip_id = 0;
    int ret;

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = 1000000;

    ret = spi_setup(spi);
    if (ret)
        return ret;

    priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->spi = spi;
    spi_set_drvdata(spi, priv);

    ret = demo_spi_read_reg(spi, 0x00, &chip_id);
    if (ret)
        return dev_err_probe(&spi->dev, ret, "read chip id failed\n");

    dev_info(&spi->dev, "chip id = 0x%02x\n", chip_id);
    return 0;
}

static const struct of_device_id demo_spi_of_match[] = {
    { .compatible = "demo,my-spi-dev" },
    { }
};
MODULE_DEVICE_TABLE(of, demo_spi_of_match);

static struct spi_driver demo_spi_driver = {
    .probe = demo_spi_probe,
    .driver = {
        .name = "demo_spi_dev",
        .of_match_table = demo_spi_of_match,
    },
};
module_spi_driver(demo_spi_driver);

MODULE_LICENSE("GPL");
```

这个示例虽然很小，但已经体现了 SPI 驱动的主线：

1. 驱动通过 `spi_device` 接入总线模型。
2. 先设置并应用 SPI 参数。
3. 用最小事务验证设备是否真的在线。
4. 读 chip ID 成功后，才有资格继续做复杂初始化。

## 代码解读

### 1. 为什么 `probe` 里先做 `spi_setup()`

因为：

1. `mode`
2. `bits_per_word`
3. `max_speed_hz`

这些参数不只是字段赋值，而是要真正下发给控制器层。

`spi_setup()` 成功后，后续事务才更有可能按预期时序执行。

### 2. 为什么最小读寄存器函数只有一字节命令

因为很多寄存器型 SPI 设备最小闭环就是：

1. 发一个带读位的寄存器地址。
2. 读回一个固定值。

这特别适合拿来：

1. 确认器件在线。
2. 确认 `mode` 基本正确。
3. 确认事务格式至少第一步没错。

### 3. 为什么 `dev_err_probe()` 依然值得用

虽然 SPI 驱动不像 I2C 那样常有 ACK 语义，但初始化失败一样需要：

1. 带设备上下文打印错误。
2. 在资源依赖失败时统一处理。

所以它仍然是很好的错误边界工具。

### 4. 为什么这还只是最小驱动

真实 SPI 驱动通常还要补：

1. 中断。
2. 缓冲区。
3. DMA。
4. 多字节寄存器访问。
5. dummy 周期处理。
6. 接入对应子系统，例如 IIO、input、mtd、net 等。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 驱动入口 | 通过 `spi_device` 和总线模型工作 | 把 SPI 器件当 platform MMIO 设备写 | 模型不对 |
| 参数配置 | 设置 `mode/bits_per_word/max_speed_hz` 后 `spi_setup()` | 改了字段却不 setup | 控制器未必真的应用 |
| 最小验证 | 先读 chip ID 建立闭环 | 一上来就写整套业务逻辑 | 出问题时范围太大 |
| 排障路径 | 先查模式和事务格式，再查业务逻辑 | 只要有波形就盲信总线正常 | 很容易误判 |
| 事务实现 | 按器件手册组织命令、地址、dummy 和数据 | 把所有 SPI 设备都按同一种读法处理 | 器件差异被忽略 |
| 频率策略 | 先低速跑通再提速 | 一上来拉最高频率 | 很容易把时序问题误判成软件 bug |

## 边界条件与适用范围

1. 这篇笔记聚焦典型寄存器型 SPI 从设备驱动，不覆盖 SPI Flash、显示控制器等更复杂设备的全部细节。
2. 有些设备读写事务并不适合 `spi_write_then_read()`，更适合 `spi_sync_transfer()` 或 `spi_mem` 框架。
3. 某些器件对 CS 拉低保持、片选极性、dummy 周期要求非常严格，必须回到 datasheet 逐项确认。
4. 带中断或 DMA 的 SPI 设备，后续还要把 IRQ、缓存和并发一起纳入设计。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| `probe` 进了但 chip ID 读失败 | `mode` 错；事务格式错；频率过高 | 先降频并抓波形 |
| 读回全 `0xFF` | MISO 悬空；CS 无效；器件没响应 | 查片选和硬件连接 |
| 读回全 `0x00` | 模式错；读命令错；dummy 周期不对 | 查 datasheet 和波形 |
| 某些板子能跑某些不行 | 板级时序、频率、片选极性或电源差异 | 比较设备树和波形 |
| 低速正常高速异常 | 走线边沿差；频率超规格；采样边沿不匹配 | 降频、切换 mode、抓波形 |
| 能写不能读 | 读事务格式和写事务不同；漏了 dummy | 查器件命令格式 |

排查 SPI 驱动时，推荐按下面顺序走：

1. 设备树是否创建设备并匹配到驱动。
2. `spi_setup()` 是否成功。
3. `mode`、位宽、频率是否符合器件手册。
4. 最小读 ID 事务是否正确。
5. 波形里的 CS、SCLK、MOSI、MISO 是否和预期一致。
6. 再去看中断、DMA 和上层业务逻辑。

## 工程落地建议

1. 任何新 SPI 驱动都先做“低速读 chip ID”最小闭环。
2. 把器件事务格式整理成笔记或注释，例如“读命令、地址长度、dummy、最大频率、CS 约束”。
3. 优先保留一套能和逻辑分析仪波形对上的测试事务，后续排障特别值钱。
4. 如果设备有现成内核子系统可挂，尽量接入标准框架，不要长期维持裸驱动接口。
5. 一旦读写开始变复杂，优先把“寄存器访问 helper”和“业务流程”拆开，方便定位问题。

## 性能、稳定性、可维护性影响

1. SPI 参数和事务格式理解准确，驱动联调成本会明显下降。
2. 先做最小闭环，再扩功能，能大幅降低“哪里都像有问题”的调试混乱感。
3. 清晰的事务封装和手册约束记录，会直接降低后续芯片替换和维护成本。
4. 由于 SPI 缺少统一 ACK 语义，越是缺乏结构化设计，后期越难排障。

## 面试 / 问答怎么讲

### 30 秒版本

Linux SPI 驱动的核心，是通过 `spi_device` 接入 SPI 总线模型，在 `probe` 里先设置 `mode`、位宽和频率并调用 `spi_setup()`，然后用最小事务读出 chip ID 建立闭环。排障时重点看事务格式、模式和片选时序，而不是只看有没有波形。

### 3 分钟版本

可以先讲 SPI 驱动和 platform、I2C 驱动的区别：SPI 更依赖器件手册定义的事务格式。然后讲 `spi_device`、`spi_setup()` 和最小读 ID 闭环，再补 `mode`、`bits_per_word`、`max_speed_hz` 的意义，以及为什么 SPI 常用逻辑分析仪辅助排障。

### 10 分钟版本

可以进一步结合一个传感器或 ADC 设备展开：设备树创建 `spi_device`，驱动匹配后设置 `Mode 0`、频率和位宽，用 `spi_write_then_read()` 或 `spi_sync_transfer()` 读版本寄存器，确认事务正确后再初始化中断和上层接口。然后往下讲为什么有些设备需要 dummy byte、为什么低速能跑高速不行、为什么 `0xFF` 常常意味着片选或 MISO 问题。这会非常贴近真实联调场景。

## 实战练习

1. 写一个最小 SPI 驱动，只完成 `spi_setup()` 和读 chip ID。
2. 用逻辑分析仪抓一次“读寄存器”事务，对照 datasheet 标出命令、地址、dummy 和数据阶段。
3. 故意把 `mode` 改错，观察读回值和波形的变化，并写出定位过程。
4. 给 SPI 驱动增加一个 IRQ 资源，再设计“SPI 事务 + 中断上报”的排障主线。

## 关键要点

1. SPI 驱动的难点常常在事务格式，不只是总线模型。
2. `spi_setup()` 和最小读 ID 闭环是关键起点。
3. 波形正常不代表事务正确，必须回到器件手册核对。
4. 先低速跑通，再扩功能和提频率，是很稳的工程路径。

## 关联笔记

1. `驱动开发-总览`
2. `驱动开发-I2C驱动基础`
3. `驱动开发-platform驱动基础`
4. `外设-SPI通信基础`
5. `Linux-platform总线与设备模型`
6. `工具链与调试-示波器与逻辑分析仪排障`
