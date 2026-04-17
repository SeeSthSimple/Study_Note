# C与C++-单元测试与可测试性

## 原始问题

为什么嵌入式代码总是"改一处坏三处"？为什么修了一个bug，过两天又冒出来？为什么很多人觉得"嵌入式代码没法测试，只能上板验证"？单元测试在嵌入式里到底怎么做，可测试性又该怎么设计？

## 先给结论

嵌入式代码不是"没法测试"，而是"没有按可测试的方式写"。关键结论：

1. 单元测试的目标不是"覆盖所有硬件行为"，而是"验证每个纯逻辑模块在给定输入下的输出是否正确"。
2. 可测试性不是测试框架的事，而是代码设计的事：依赖注入、接口分离、硬件抽象层是三大核心手段。
3. C 项目可以用 Unity + CMock + Ceedling，C++ 项目可以用 Google Test 或 Catch2。
4. Mock 和 Stub 不是"造假骗自己"，而是"隔离被测单元的外部依赖"。
5. 测试驱动开发（TDD）在嵌入式里不是必须的，但"先想怎么测再写代码"的习惯非常有价值。

如果只能背"单元测试很重要"，但讲不清可测试设计、Mock 原理和嵌入式测试策略，就还没有真正掌握这个主题。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 写完代码后不知道怎么测，只能上板手动验证，效率极低。
2. 改了一个模块，不确定是否影响了其他模块，只能全量回归。
3. 代码里硬件操作和业务逻辑混在一起，没法脱离硬件单独测试。
4. 不知道该选什么测试框架，不知道 Mock 和 Stub 怎么用。
5. 面试时被问"你怎么保证代码质量"，只能说"我仔细看"。

它在AI时代依然重要，因为AI生成代码的速度很快，但代码是否正确需要人来验证。单元测试是验证AI生成代码最系统化的手段，也是"人机协作"里人最不可替代的环节——判断什么是对的。

## 核心概念 / 本质机制

### 1. 单元测试的本质

单元测试是对代码中最小可测试单元的自动化验证：

1. **最小可测试单元**：一个函数、一个类的方法、一个模块的公开接口。
2. **自动化**：测试代码可以一键运行，不需要人工干预。
3. **验证**：对比实际输出和期望输出，自动判定通过或失败。

单元测试不是：

1. 不是集成测试（不测模块间交互）。
2. 不是系统测试（不测整体功能）。
3. 不是硬件测试（不测真实外设行为）。

### 2. 为什么嵌入式代码也需要单元测试

很多人觉得"嵌入式代码跟硬件强相关，没法单元测试"，这是误解：

1. 不是所有代码都直接操作硬件。协议解析、数据处理、状态机逻辑、算法实现，这些都可以脱离硬件测试。
2. 硬件相关代码可以通过抽象层隔离，让上层逻辑可测试。
3. 真正难测的只是"硬件行为本身"，而不是"调用硬件的代码"。

嵌入式代码的典型分层：

```text
应用逻辑层     ← 可单元测试
协议/算法层    ← 可单元测试
硬件抽象层     ← 需要 Mock
寄存器/硬件    ← 需要上板验证
```

### 3. 可测试设计的三大核心手段

#### 手段1：依赖注入

不要在模块内部硬编码依赖，而是通过参数传入。

不可测试的写法：

```c
int read_sensor(void) {
    return ADC->DR;  // 直接读硬件寄存器
}
```

可测试的写法：

```c
typedef int (*SensorReader)(void);

int read_sensor(SensorReader reader) {
    return reader();  // 通过函数指针读取
}

// 生产代码
int real_reader(void) { return ADC->DR; }
int value = read_sensor(real_reader);

// 测试代码
int mock_reader(void) { return 42; }
int value = read_sensor(mock_reader);  // 返回可控值
```

#### 手段2：接口分离

把"做什么"和"怎么做"分开，让测试可以替换"怎么做"。

```c
/* transport.h - 接口定义 */
typedef struct {
    int (*send)(const uint8_t *data, size_t len);
    int (*receive)(uint8_t *buf, size_t len);
} Transport;

/* 业务逻辑只依赖接口 */
int send_command(Transport *transport, uint8_t cmd) {
    uint8_t frame[] = {0xAA, cmd, cmd};
    return transport->send(frame, sizeof(frame));
}

/* 测试代码 */
int mock_send(const uint8_t *data, size_t len) {
    /* 记录调用参数，验证帧格式 */
    return 0;
}

Transport mock_transport = { .send = mock_send, .receive = NULL };
send_command(&mock_transport, 0x01);  // 不需要真实硬件
```

#### 手段3：硬件抽象层（HAL）

把所有硬件操作封装到一个统一接口后面，让上层代码只依赖接口。

```c
/* hal_gpio.h */
void hal_gpio_init(int pin, int mode);
void hal_gpio_write(int pin, int value);
int hal_gpio_read(int pin);

/* 生产代码：hal_gpio.c 实现真实寄存器操作 */
/* 测试代码：hal_gpio.c 实现为 Mock，记录调用 */
```

### 4. Mock 和 Stub 的区别

这两个概念经常被混淆：

1. **Stub**：返回预设值，不验证交互。用于"让被测代码能跑下去"。
2. **Mock**：验证交互是否正确（是否被调用、调用参数是否正确）。用于"验证被测代码的行为"。

示例：

```c
/* Stub：只返回固定值 */
int stub_read(void) { return 42; }

/* Mock：记录并验证调用 */
typedef struct {
    int read_called;
    int last_value;
} MockSensor;

int mock_read(MockSensor *m) {
    m->read_called = 1;
    return m->last_value;
}

/* 测试中验证 */
MockSensor mock = {0, 42};
int result = read_sensor_with_context(&mock);
assert(mock.read_called == 1);
assert(result == 42);
```

### 5. C 测试框架选择

#### Unity

最轻量的 C 单元测试框架，适合嵌入式：

1. 一个 `.c` 文件和一个 `.h` 文件，极小体积。
2. 提供 `TEST_ASSERT_EQUAL`、`TEST_ASSERT_TRUE` 等断言宏。
3. 支持 `setUp` 和 `tearDown`。
4. 可以在主机上运行，也可以在目标板上运行。

#### CMock

为 C 函数自动生成 Mock：

1. 读取头文件，自动生成 Mock 函数。
2. 支持设定返回值和验证调用参数。
3. 跟 Unity 配合使用。

#### Ceedling

Unity + CMock 的构建管理工具：

1. 自动发现测试文件。
2. 自动生成 Mock。
3. 一键运行所有测试。
4. 生成覆盖率报告。

### 6. C++ 测试框架选择

#### Google Test (gtest)

最流行的 C++ 测试框架：

1. 提供 `EXPECT_EQ`、`ASSERT_TRUE` 等断言。
2. 支持测试夹具（Test Fixtures）。
3. 支持参数化测试。
4. 跟 Google Mock (gmock) 配合做 Mock。

#### Catch2

更轻量的 C++ 测试框架：

1. 单头文件即可使用。
2. 用自然语言写测试用例名。
3. 不需要手动注册测试。
4. 支持 BDD 风格（GIVEN/WHEN/THEN）。

### 7. 嵌入式测试策略

嵌入式代码的测试通常分三层：

1. **主机测试**：在开发机上编译运行，测试纯逻辑。最快、最方便。
2. **模拟器测试**：在 QEMU 或指令集模拟器上运行，测试接近真实环境的行为。
3. **目标板测试**：在真实硬件上运行，测试硬件相关行为。最慢但最真实。

优先级：**先在主机上把纯逻辑测完，再上板验证硬件交互**。

## 数据流 / 控制流 / 时序关系

单元测试的典型流程：

```text
编写测试用例
-> 构造输入数据
-> 调用被测函数
-> 收集输出结果
-> 断言验证
-> 通过/失败
```

可测试代码的设计流程：

```text
识别模块的依赖
-> 把依赖抽象成接口
-> 通过参数注入依赖
-> 生产代码用真实实现
-> 测试代码用 Mock/Stub
```

## 最小可运行示例

### C 风格：用 Unity 测试协议解析器

```c
/* parser.h */
typedef enum { PARSE_OK, PARSE_EINVAL, PARSE_ESHORT } ParseResult;

ParseResult parse_frame(const uint8_t *data, size_t len, uint8_t *out_id, uint8_t *out_value);

/* parser.c */
ParseResult parse_frame(const uint8_t *data, size_t len, uint8_t *out_id, uint8_t *out_value) {
    if (data == NULL || out_id == NULL || out_value == NULL) {
        return PARSE_EINVAL;
    }
    if (len < 3) {
        return PARSE_ESHORT;
    }
    *out_id = data[0];
    *out_value = data[1];
    return PARSE_OK;
}

/* test_parser.c */
#include "unity.h"
#include "parser.h"

void setUp(void) {}
void tearDown(void) {}

void test_parse_valid_frame(void) {
    uint8_t data[] = {0x01, 0x42, 0x00};
    uint8_t id = 0, value = 0;
    ParseResult result = parse_frame(data, sizeof(data), &id, &value);
    TEST_ASSERT_EQUAL(PARSE_OK, result);
    TEST_ASSERT_EQUAL(0x01, id);
    TEST_ASSERT_EQUAL(0x42, value);
}

void test_parse_null_input(void) {
    uint8_t id = 0, value = 0;
    ParseResult result = parse_frame(NULL, 3, &id, &value);
    TEST_ASSERT_EQUAL(PARSE_EINVAL, result);
}

void test_parse_short_frame(void) {
    uint8_t data[] = {0x01};
    uint8_t id = 0, value = 0;
    ParseResult result = parse_frame(data, sizeof(data), &id, &value);
    TEST_ASSERT_EQUAL(PARSE_ESHORT, result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid_frame);
    RUN_TEST(test_parse_null_input);
    RUN_TEST(test_parse_short_frame);
    return UNITY_END();
}
```

### C++ 风格：用 Google Test 测试带依赖的模块

```cpp
#include <gtest/gtest.h>

class SensorReader {
public:
    virtual ~SensorReader() = default;
    virtual int read() = 0;
};

class DataProcessor {
public:
    explicit DataProcessor(SensorReader& reader) : reader_(reader) {}
    int process() {
        int raw = reader_.read();
        return raw * 2;
    }
private:
    SensorReader& reader_;
};

class MockSensorReader : public SensorReader {
public:
    int read_value = 0;
    int read() override { return read_value; }
};

TEST(DataProcessorTest, DoublesInput) {
    MockSensorReader mock;
    mock.read_value = 21;
    DataProcessor proc(mock);
    EXPECT_EQ(proc.process(), 42);
}

TEST(DataProcessorTest, HandlesZero) {
    MockSensorReader mock;
    mock.read_value = 0;
    DataProcessor proc(mock);
    EXPECT_EQ(proc.process(), 0);
}
```

## 代码解读

### 1. 为什么测试用例要覆盖正常路径和异常路径

因为嵌入式代码的很多 bug 恰恰出在异常路径：空指针、长度不足、校验失败。如果只测 happy path，这些边界问题就会被遗漏。

### 2. 为什么 `MockSensorReader` 要继承接口

因为 `DataProcessor` 依赖的是 `SensorReader` 接口，不是具体实现。测试时用 Mock 替换真实实现，就能控制输入、验证逻辑。

### 3. 为什么 C 风格用函数指针而 C++ 风格用虚函数

这是两种语言的惯用方式：

1. C 没有虚函数和继承，函数指针是唯一的运行时多态手段。
2. C++ 有虚函数和继承，用接口类更符合语言习惯。
3. 两种方式的本质相同：让依赖可替换。

### 4. 为什么 `setUp` 和 `tearDown` 很重要

它们保证每个测试用例都在干净状态下运行，不会因为前一个用例的副作用影响后一个用例。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 依赖管理 | 依赖注入、接口分离 | 在函数内部直接调用硬件 | 无法替换，无法测试 |
| 测试范围 | 只测纯逻辑 | 试图在单元测试里验证硬件行为 | 单元测试不适合测硬件 |
| Mock 使用 | 只 Mock 直接依赖 | Mock 所有东西 | 过度 Mock 让测试变得脆弱 |
| 测试数据 | 边界值、异常值 | 只测正常输入 | 遗漏边界和异常路径 |
| 测试命名 | 描述被测行为 | `test1`、`test2` | 看不出在测什么 |
| 测试独立性 | 每个用例独立运行 | 用例之间有依赖 | 一个失败导致后续全挂 |
| 覆盖率目标 | 关键路径100% | 追求行覆盖率数字 | 高覆盖率不等于高质量 |

## 边界条件与适用范围

1. 单元测试适合验证纯逻辑，不适合验证时序、中断和硬件行为。
2. Mock 不是万能的，过度 Mock 会让测试跟实现强耦合，一改实现测试就挂。
3. 测试覆盖率是参考指标，不是目标。100% 覆盖率不等于没有 bug。
4. 嵌入式里有些代码确实只能上板验证，但应该尽量减少这部分代码的比例。
5. 测试代码也是代码，需要维护。测试写得太复杂反而会成为负担。
6. TDD 不是强制的，但"先想怎么测"的习惯对代码设计有正面影响。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 测试经常因为无关改动失败 | 过度 Mock，测试跟实现耦合 | 减少 Mock 层次，只 Mock 直接依赖 |
| 测试写了但没发现 bug | 只测了 happy path | 补充边界值和异常路径 |
| 不敢重构因为怕测试挂 | 测试太脆弱 | 重构测试，让测试验证行为而不是实现 |
| 没时间写测试 | 代码不可测试，写测试成本太高 | 先重构代码提高可测试性 |
| 覆盖率很高但还是有 bug | 覆盖率只看行不看路径 | 用 MC/DC 或路径覆盖分析 |

### 推荐排查顺序

1. 先确认代码是否可测试（依赖是否可注入）。
2. 再确认测试是否覆盖了关键路径和边界。
3. 再确认 Mock 是否合理（不过度、不遗漏）。
4. 再确认测试是否独立运行（没有用例间依赖）。
5. 最后看测试运行效率和持续集成配置。

## 工程落地建议

### 1. 把可测试性当成设计约束

写代码之前先问：

1. 这个函数能脱离硬件单独测试吗？
2. 它的依赖能通过参数注入吗？
3. 它的输出能通过断言验证吗？

如果答案是否定的，先重构代码再写测试。

### 2. 优先测试这些模块

1. 协议解析器：帧格式、校验、状态机。
2. 数据处理：滤波、转换、计算。
3. 状态机逻辑：状态转换、事件处理。
4. 配置解析：参数校验、默认值。
5. 错误处理：异常路径、恢复逻辑。

### 3. 硬件相关代码的测试策略

1. HAL 层以下：在目标板或模拟器上测试。
2. HAL 层：用 Mock 替换，测试上层逻辑。
3. HAL 层以上：在主机上用 Mock 测试。

### 4. 持续集成

1. 每次提交自动运行所有测试。
2. 测试失败不允许合并。
3. 定期检查覆盖率变化。
4. 把测试纳入构建流程，不是可选步骤。

### 5. 测试代码的组织

推荐目录结构：

```text
project/
├── src/
│   ├── parser.c
│   └── filter.c
├── include/
│   ├── parser.h
│   └── filter.h
├── test/
│   ├── test_parser.c
│   ├── test_filter.c
│   └── mock/
│       └── mock_hal.c
└── Makefile
```

## 性能、稳定性、可维护性影响

1. 单元测试的短期成本是"多写代码"，长期收益是"改代码更有信心"。
2. 可测试代码通常也是模块化更好的代码，可维护性自然更高。
3. 测试覆盖率每提升一点，回归验证的效率就提升一点。
4. 没有测试的代码，修改成本会随时间指数增长；有测试的代码，修改成本相对稳定。
5. 真正成熟的嵌入式项目，不是"没有测试"，而是"测试跟代码一起演进"。

## 面试 / 问答怎么讲

### 30 秒版本

嵌入式单元测试的关键是可测试设计：依赖注入、接口分离、硬件抽象层。C 用 Unity + CMock，C++ 用 Google Test。Mock 用于隔离外部依赖，Stub 用于提供预设返回值。优先在主机上测纯逻辑，硬件相关代码用 Mock 隔离后测上层。

### 3 分钟版本

可以从"为什么嵌入式代码也需要测试"讲起：不是所有代码都直接操作硬件，纯逻辑部分完全可以脱离硬件测试。然后说明可测试设计的三大手段：依赖注入、接口分离、硬件抽象层。再举一个协议解析器的例子，展示如何在主机上用 Unity 测试。最后补充测试策略：主机测试优先，Mock 隔离硬件，持续集成保障。

### 10 分钟版本

可以结合一个完整项目展开：传感器数据采集系统里，协议解析用 Unity 测试帧格式和校验逻辑，数据处理用 Google Test 测试滤波算法，HAL 层用 CMock 生成 Mock 替换硬件操作。然后说明如何组织测试代码目录、如何配置 Ceedling 构建系统、如何在持续集成中自动运行测试。再讨论测试覆盖率的合理目标和常见误区。这种讲法更接近真实工程能力。

## 实战练习

1. 用 Unity 为一个 CRC 校验函数写单元测试，覆盖正常输入、空指针和长度不足。
2. 把一个直接读硬件寄存器的函数改成依赖注入风格，然后用 Mock 测试。
3. 用 CMock 为一个 UART 发送接口生成 Mock，验证帧格式是否正确。
4. 用 Google Test 为一个 C++ 滤波类写测试，包括构造、处理和边界情况。
5. 给一个现有模块补上测试，记录"发现几个之前没注意到的边界问题"。

## 关键要点

1. 嵌入式代码不是"没法测试"，而是"没有按可测试的方式写"。
2. 可测试设计三大手段：依赖注入、接口分离、硬件抽象层。
3. Mock 验证交互，Stub 提供预设值，两者用途不同。
4. C 用 Unity + CMock + Ceedling，C++ 用 Google Test 或 Catch2。
5. 优先在主机上测纯逻辑，硬件相关代码用 Mock 隔离。
6. 测试覆盖率是参考指标，不是目标；关键路径100%比全局80%更有价值。
7. 可测试代码通常也是模块化更好的代码，可维护性自然更高。

## 关联笔记

1. `C语言-模块化与头文件组织`
2. `C与C++-接口设计与错误处理`
3. `C++-多态与虚函数`
4. `C语言-函数指针与回调`
5. `C++-设计模式在嵌入式中的取舍`
6. `C与C++-构建链接与工程组织`
7. `项目与架构-模块边界划分`
