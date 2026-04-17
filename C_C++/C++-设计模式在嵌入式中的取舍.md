# C++-设计模式在嵌入式中的取舍

## 原始问题

嵌入式项目里该不该用设计模式？有人说"设计模式是桌面端的东西，嵌入式用不上"，又有人说"不用设计模式的代码根本没法维护"。到底哪些模式在嵌入式里真正有价值，哪些反而会增加开销和复杂度？

## 先给结论

设计模式在嵌入式里不是"全用"或"全不用"的问题，而是要按三个标准逐个判断：

1. **内存开销**：模式是否引入了额外的堆分配或虚函数表。
2. **实时性影响**：模式是否引入了不确定的运行时开销（如动态分发、异常）。
3. **可维护性收益**：模式是否真正降低了模块间耦合、提高了可测试性或可扩展性。

按这个标准，嵌入式里的模式取舍大致是：

1. **值得用**：单例（硬件抽象）、观察者（事件分发）、状态机（协议解析）、策略（算法替换）、适配器（接口转换）。
2. **需要谨慎**：工厂（动态创建开销）、装饰器（层层包装开销）、模板方法（继承耦合）、命令模式（对象化开销）。
3. **不推荐**：重度运行时多态、复杂继承体系、RTTI 依赖、过度抽象的访问者模式。

如果只能背"单例、工厂、观察者"这些名字，但讲不清它们在资源受限环境下的内存代价和实时性影响，就还没有真正理解设计模式在嵌入式中的取舍。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 写嵌入式代码时不知道什么时候该抽象、什么时候该直接写。
2. 用了设计模式后代码反而更复杂、更难调试、内存开销更大。
3. 面试时被问"你用过哪些设计模式"，只能说出一两个名字但讲不清取舍。
4. 看别人代码里用了模式，不确定是合理设计还是过度设计。
5. 不理解为什么有些模式在桌面端很好用，在嵌入式里却成了负担。

它在AI时代依然重要，因为AI生成代码时倾向于使用"教科书式"的设计模式，但经常忽略嵌入式场景下的内存约束、实时性要求和可维护性平衡。你越理解模式的代价和收益，越能判断AI给的方案是否适合你的目标平台。

## 核心概念 / 本质机制

### 1. 设计模式的本质

设计模式不是"必须遵守的规则"，而是"在特定约束下解决特定问题的经验总结"。

它的核心价值是：

1. 提供一套通用词汇，让团队沟通更高效。
2. 把"已经验证过的解决方案"沉淀成可复用的思路。
3. 帮助你识别代码中的坏味道，找到重构方向。

但在嵌入式里，模式的价值必须跟资源约束一起权衡。

### 2. 嵌入式里评估模式的三条标准

#### 标准1：内存开销

1. 模式是否引入了虚函数表（每个含虚函数的类一个 vtable，每个对象一个 vptr）。
2. 模式是否需要堆分配（`new`/`make_unique`/`make_shared`）。
3. 模式是否增加了代码体积（模板实例化膨胀、内联失败）。

#### 标准2：实时性影响

1. 虚函数调用比直接调用多一次间接寻址，在极端实时路径里可能有影响。
2. 动态创建对象的时间不确定，不适合硬实时路径。
3. 异常抛出和捕获的开销不确定，嵌入式通常禁用异常。

#### 标准3：可维护性收益

1. 模式是否真正降低了耦合。
2. 模式是否让代码更容易测试。
3. 模式是否让扩展更安全。
4. 模式是否让代码更容易理解（而不是更难理解）。

### 3. 值得用的模式

#### 单例模式（硬件抽象）

嵌入式里很多硬件外设天然就是单例：一个 UART、一个 SPI 控制器、一个 ADC。

```cpp
class UartDriver {
public:
    static UartDriver& instance() {
        static UartDriver inst;
        return inst;
    }

    void send(const uint8_t* data, size_t len) { /* ... */ }
    bool receive(uint8_t* buf, size_t len) { /* ... */ }

    UartDriver(const UartDriver&) = delete;
    UartDriver& operator=(const UartDriver&) = delete;

private:
    UartDriver() { init_hardware(); }
    void init_hardware() { /* 配置寄存器 */ }
};
```

为什么值得用：

1. 硬件资源本身就是唯一的，单例模式跟物理现实一致。
2. 避免多处初始化同一硬件导致的冲突。
3. 提供全局访问点，不需要到处传指针。

注意事项：

1. 不要把"只是想全局访问"的东西都做成单例。
2. 单例的构造时机要可控，避免静态初始化顺序问题。
3. 测试时需要能替换单例，可以结合接口和依赖注入。

#### 观察者模式（事件分发）

嵌入式里很多场景需要"事件发生时通知多个模块"：按键事件、传感器数据就绪、通信帧接收完成。

```c
typedef void (*EventHandler)(void *context, int event_id);

#define MAX_HANDLERS 8

typedef struct {
    EventHandler handlers[MAX_HANDLERS];
    void *contexts[MAX_HANDLERS];
    int count;
} EventDispatcher;

void dispatcher_init(EventDispatcher *disp) {
    disp->count = 0;
}

int dispatcher_subscribe(EventDispatcher *disp, EventHandler handler, void *context) {
    if (disp->count >= MAX_HANDLERS || handler == NULL) {
        return -1;
    }
    disp->handlers[disp->count] = handler;
    disp->contexts[disp->count] = context;
    ++disp->count;
    return 0;
}

void dispatcher_notify(EventDispatcher *disp, int event_id) {
    for (int i = 0; i < disp->count; ++i) {
        disp->handlers[i](disp->contexts[i], event_id);
    }
}
```

为什么值得用：

1. 解耦事件源和事件处理者，模块不需要互相知道。
2. 固定大小的处理器数组，没有堆分配。
3. 用函数指针实现，没有虚函数开销。

注意事项：

1. 处理器数量要设上限，避免动态扩容。
2. 通知顺序通常不应被依赖。
3. 处理器里不能做耗时操作，否则会阻塞后续处理器。

#### 状态机模式（协议解析）

嵌入式里协议解析、设备状态管理、UI 流程控制几乎都适合用状态机。

```c
typedef enum { STATE_IDLE, STATE_HEADER, STATE_DATA, STATE_CRC } ParseState;

typedef struct {
    ParseState state;
    uint8_t buffer[64];
    size_t index;
    uint8_t expected_len;
} ProtocolParser;

void parser_init(ProtocolParser *p) {
    p->state = STATE_IDLE;
    p->index = 0;
    p->expected_len = 0;
}

void parser_feed(ProtocolParser *p, uint8_t byte) {
    switch (p->state) {
        case STATE_IDLE:
            if (byte == 0xAA) {
                p->state = STATE_HEADER;
                p->index = 0;
            }
            break;
        case STATE_HEADER:
            p->expected_len = byte;
            p->state = STATE_DATA;
            p->index = 0;
            break;
        case STATE_DATA:
            p->buffer[p->index++] = byte;
            if (p->index >= p->expected_len) {
                p->state = STATE_CRC;
            }
            break;
        case STATE_CRC:
            /* 校验处理 */
            p->state = STATE_IDLE;
            break;
    }
}
```

为什么值得用：

1. 协议解析天然就是状态驱动的，状态机是最自然的表达。
2. 状态转换逻辑集中在一处，容易审查和调试。
3. 没有堆分配，没有虚函数，纯数据驱动。

注意事项：

1. 状态多时 switch-case 会变长，可以考虑用状态表驱动。
2. 状态转换时要注意清理上一个状态的临时数据。
3. 异常帧的处理要回到安全状态。

#### 策略模式（算法替换）

嵌入式里经常需要在运行时或编译时切换算法：不同的滤波算法、不同的编码方式、不同的通信协议。

```cpp
class FilterStrategy {
public:
    virtual ~FilterStrategy() = default;
    virtual int process(int sample) = 0;
};

class MovingAverageFilter : public FilterStrategy {
public:
    int process(int sample) override { /* 滑动平均 */ return 0; }
};

class MedianFilter : public FilterStrategy {
public:
    int process(int sample) override { /* 中值滤波 */ return 0; }
};

class Sensor {
public:
    explicit Sensor(FilterStrategy& filter) : filter_(filter) {}
    int read() {
        int raw = read_hardware();
        return filter_.process(raw);
    }
private:
    FilterStrategy& filter_;
    int read_hardware() { return 0; }
};
```

为什么值得用：

1. 算法可以独立变化，不影响使用方。
2. 新增算法不需要修改已有代码。
3. 通过引用注入，不拥有对象，没有额外内存开销。

注意事项：

1. 如果算法在编译期就能确定，用模板比虚函数更高效。
2. 如果策略对象很小且数量固定，可以不用堆分配。
3. 虚函数调用的间接开销在大多数嵌入式场景下可以忽略。

#### 适配器模式（接口转换）

嵌入式里经常需要把不同硬件或不同协议的接口统一成一致形式。

```c
typedef struct {
    int (*init)(void);
    int (*send)(const uint8_t *data, size_t len);
    int (*receive)(uint8_t *buf, size_t len);
} TransportAdapter;

/* UART 适配器 */
int uart_init(void) { return 0; }
int uart_send(const uint8_t *data, size_t len) { return 0; }
int uart_receive(uint8_t *buf, size_t len) { return 0; }

TransportAdapter uart_adapter = {
    .init = uart_init,
    .send = uart_send,
    .receive = uart_receive
};

/* 使用方只依赖 TransportAdapter 接口 */
void communicate(TransportAdapter *transport) {
    transport->init();
    uint8_t data[] = {0x01, 0x02};
    transport->send(data, sizeof(data));
}
```

为什么值得用：

1. 统一接口，上层代码不需要关心底层差异。
2. 切换底层实现只需要换一个适配器实例。
3. 用函数指针实现，没有虚函数和堆分配。

### 4. 需要谨慎的模式

#### 工厂模式

问题：动态创建对象需要堆分配，创建时间不确定。

嵌入式里的替代方案：

1. 对象池：预分配一组对象，从池中获取而不是 `new`。
2. 静态创建 + 初始化：先静态分配对象，再调用初始化函数。
3. 编译期工厂：用模板在编译期选择具体类型。

#### 装饰器模式

问题：层层包装会增加对象数量和间接调用层数。

嵌入式里的替代方案：

1. 用组合代替装饰：把功能直接放在一个类里。
2. 用策略代替装饰：通过注入不同策略来改变行为。
3. 如果确实需要，用固定层数的包装，不要无限嵌套。

#### 模板方法模式

问题：继承耦合，子类必须理解父类的骨架逻辑。

嵌入式里的替代方案：

1. 用策略模式替代：把变化的部分抽取成策略接口。
2. 用回调函数替代：C 风格里更常见的做法。

### 5. 不推荐的模式

1. **重度运行时多态**：大量虚函数、深层继承、动态转型（`dynamic_cast`）。嵌入式通常禁用 RTTI，且深层继承难以理解和维护。
2. **复杂继承体系**：3 层以上的继承、多重继承、菱形继承。嵌入式代码应该优先用组合而不是继承。
3. **访问者模式**：双重分发的实现复杂，在类型稳定的嵌入式数据结构上几乎没有必要。
4. **代理模式的远程变体**：嵌入式里没有 RPC 场景，远程代理没有用武之地。

## 数据流 / 控制流 / 时序关系

设计模式在嵌入式里的典型应用主线：

```text
识别问题类型
-> 判断是否需要模式（还是直接写更清晰）
-> 选择合适的模式
-> 评估内存和实时性代价
-> 实现模式（优先用 C 风格或轻量 C++ 风格）
-> 验证模式确实带来了可维护性收益
```

最常见的错误是：**还没确认问题是否需要模式，就先套了一个模式进去**。

## 最小可运行示例

```c
#include <stdio.h>
#include <stddef.h>

typedef void (*FilterFunc)(int *output, const int *input, size_t len);

static void moving_average(int *output, const int *input, size_t len) {
    int sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += input[i];
        output[i] = (int)((long)sum / (long)(i + 1));
    }
}

static void passthrough(int *output, const int *input, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        output[i] = input[i];
    }
}

typedef struct {
    FilterFunc filter;
    const char *name;
} FilterStrategy;

static void apply_filter(const FilterStrategy *strategy, const int *data, size_t len) {
    int result[8];
    strategy->filter(result, data, len);
    printf("%s: ", strategy->name);
    for (size_t i = 0; i < len; ++i) {
        printf("%d ", result[i]);
    }
    printf("\n");
}

int main(void) {
    int data[] = {10, 20, 30, 40, 50, 60, 70, 80};
    size_t len = sizeof(data) / sizeof(data[0]);

    FilterStrategy strategies[] = {
        {moving_average, "moving_avg"},
        {passthrough, "passthrough"}
    };

    for (size_t i = 0; i < sizeof(strategies) / sizeof(strategies[0]); ++i) {
        apply_filter(&strategies[i], data, len);
    }

    return 0;
}
```

编译和运行：

```bash
gcc -Wall -Wextra -Werror -std=c11 demo.c -o demo
./demo
```

预期输出：

```text
moving_avg: 10 15 20 25 30 35 40 45
passthrough: 10 20 30 40 50 60 70 80
```

这个示例展示了策略模式的 C 风格实现：用函数指针替代虚函数，用结构体替代类，没有堆分配和虚函数表。

## 代码解读

### 1. 为什么用函数指针而不是虚函数

在 C 风格嵌入式代码里，函数指针是更自然的选择：

1. 不需要 C++ 编译器。
2. 没有虚函数表开销。
3. 策略对象可以静态分配。
4. 跟 C 接口和回调机制天然兼容。

### 2. 为什么 `FilterStrategy` 用结构体而不是类

因为结构体更轻量，语义更直接：它就是"一组函数指针 + 名称"，不需要封装、继承和多态。

### 3. 为什么策略数组可以静态分配

因为策略数量在编译期已知，不需要动态创建。这避免了堆分配和时间不确定性。

### 4. 为什么 `apply_filter` 接收指针而不是值

因为 `FilterStrategy` 包含函数指针，传指针更高效，也符合 C 的接口风格。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 硬件抽象 | 单例 + 接口，或 C 风格全局驱动对象 | 到处 `extern` 全局变量 | 耦合严重，难以测试和替换 |
| 事件通知 | 固定大小观察者列表 | 链表动态注册观察者 | 堆分配和遍历时间不确定 |
| 算法切换 | 函数指针或策略引用 | 到处 if-else 硬编码 | 新增算法要改多处代码 |
| 协议解析 | 状态机 | 大量嵌套 if-else 和标志位 | 逻辑散乱，难以审查 |
| 接口统一 | 适配器 + 函数指针表 | 每种硬件单独写一套上层逻辑 | 代码重复，切换困难 |
| 对象创建 | 对象池或静态分配 | 直接 `new` | 堆分配时间不确定 |
| 功能扩展 | 组合或策略 | 深层继承 | 耦合严重，修改风险大 |

## 边界条件与适用范围

1. 设计模式不是万能药，如果直接写更清晰，就不要套模式。
2. 嵌入式里优先用 C 风格实现模式（函数指针、结构体、回调），而不是 C++ 虚函数体系。
3. 模式的价值在于"让代码更容易理解和维护"，不在于"让代码看起来更高级"。
4. 如果一个模式引入的开销（内存、时间、复杂度）大于它带来的收益，就不用。
5. 在资源极度受限的 MCU 上，可能连函数指针的间接调用都要避免，更不用说虚函数。
6. 模式选择要跟团队水平匹配，过于复杂的模式可能让团队其他成员更难理解代码。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 代码比不用模式更难理解 | 过度抽象，模式套模式 | 回到问题本身，看是否真的需要 |
| 内存占用增加 | 引入了虚函数表、动态分配 | 检查对象大小和分配次数 |
| 实时性下降 | 动态创建、虚函数分发、异常 | 分析热路径的调用链 |
| 测试反而更难 | 单例无法替换、继承耦合 | 检查是否可以用依赖注入 |
| 新增功能要改很多地方 | 抽象边界选错了 | 重新审视变化点和稳定点 |

### 推荐排查顺序

1. 先确认问题是不是"模式选择错误"，而不是"实现有bug"。
2. 如果是模式选择错误，回到三条标准重新评估。
3. 如果模式本身没问题但实现有问题，检查是否有不必要的堆分配或虚函数。
4. 如果模式让代码更难理解，考虑简化或换一种更直接的实现方式。

## 工程落地建议

### 1. 嵌入式里优先用 C 风格实现模式

1. 函数指针替代虚函数。
2. 结构体 + 函数指针表替代类 + 继承。
3. 回调替代观察者接口。
4. 固定大小数组替代动态容器。

### 2. 先确认问题，再选模式

1. 先问"这段代码的痛点是什么"。
2. 再问"模式能不能解决这个痛点"。
3. 再问"模式的代价是否可接受"。
4. 最后才决定用不用模式、用哪个模式。

### 3. 组合优于继承

嵌入式里几乎总是应该优先用组合：

1. 组合不引入虚函数表。
2. 组合的对象可以独立分配和初始化。
3. 组合的耦合度更低，修改更安全。
4. 组合更容易测试，因为可以单独替换组件。

### 4. 给测试留接口

无论用什么模式，都要确保：

1. 硬件相关代码可以通过接口替换。
2. 依赖可以通过参数注入，而不是硬编码在内部。
3. 模块可以独立测试，不需要完整硬件环境。

## 性能、稳定性、可维护性影响

1. 合理使用模式能显著提升可维护性，让模块边界更清晰、扩展更安全。
2. 不合理使用模式会增加内存开销、运行时开销和代码理解成本。
3. 嵌入式里最怕的不是"没有模式"，而是"模式用错了地方"。
4. 真正好的嵌入式代码，看起来通常比"教科书式"代码更简单，因为每一步取舍都是显式的。
5. 模式的最大价值不是"让代码更优雅"，而是"让修改更安全、让测试更容易"。

## 面试 / 问答怎么讲

### 30 秒版本

设计模式在嵌入式里要按内存开销、实时性影响和可维护性收益三条标准取舍。值得用的有单例（硬件抽象）、观察者（事件分发）、状态机（协议解析）、策略（算法替换）。不推荐重度运行时多态和复杂继承体系。嵌入式里优先用 C 风格实现模式：函数指针替代虚函数，结构体替代类。

### 3 分钟版本

可以从三条评估标准讲起：内存、实时性、可维护性。然后举几个值得用的模式例子：单例用于硬件抽象、状态机用于协议解析、策略用于算法切换。再说明为什么工厂和装饰器要谨慎：动态创建和层层包装的开销。最后强调嵌入式里优先用 C 风格实现模式，组合优于继承，先确认问题再选模式。

### 10 分钟版本

可以结合一个嵌入式项目展开：传感器数据采集系统里，用状态机管理采集流程、用策略切换滤波算法、用观察者分发数据就绪事件、用适配器统一不同传感器的接口。然后分析每个模式的内存和实时性代价，说明为什么不用工厂（避免动态创建）和继承（用组合替代）。再讨论如何让这些模式支持测试（依赖注入、接口替换）。这种讲法更接近真实工程能力。

## 实战练习

1. 用 C 风格函数指针实现一个策略模式，支持两种排序算法切换。
2. 用状态机模式实现一个简单的 UART 帧解析器。
3. 用观察者模式实现一个按键事件分发系统，要求固定处理器数量。
4. 把一个用继承实现的模块改成组合实现，对比代码结构和可测试性。
5. 用适配器模式统一两种不同接口的传感器驱动。

## 关键要点

1. 嵌入式里评估模式的三条标准：内存开销、实时性影响、可维护性收益。
2. 值得用的模式：单例、观察者、状态机、策略、适配器。
3. 需要谨慎的模式：工厂、装饰器、模板方法。
4. 不推荐：重度运行时多态、复杂继承体系、RTTI 依赖。
5. 嵌入式里优先用 C 风格实现模式：函数指针替代虚函数，结构体替代类。
6. 组合优于继承，先确认问题再选模式。
7. 模式的最大价值不是"让代码更优雅"，而是"让修改更安全、让测试更容易"。

## 关联笔记

1. `C++-多态与虚函数`
2. `C++-模板与泛型基础`
3. `C语言-函数指针与回调`
4. `C语言-模块化与头文件组织`
5. `C与C++-接口设计与错误处理`
6. `项目与架构-状态机设计`
7. `项目与架构-模块边界划分`
