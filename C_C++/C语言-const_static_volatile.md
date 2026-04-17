# C语言-const_static_volatile

## 原始问题

为什么 `const`、`static`、`volatile` 看起来只是三个关键字，但在嵌入式项目里总能决定代码是“稳定可控”还是“偶发诡异”？为什么很多 bug 不是逻辑错，而是这三个关键字语义没用对？

## 先给结论

`const`、`static`、`volatile` 不是“语法装饰”，而是三个不同维度的约束：

1. `const` 约束“是否允许通过该名字修改”。
2. `static` 约束“存储期和链接可见性”。
3. `volatile` 约束“每次访问都必须真实读写，不可被优化器省略或重排访问次数”。

先记住下面几个结论：

1. `const` 不等于放到只读存储区，也不等于绝对不可变。
2. 文件内函数/变量加 `static`，核心价值是收敛符号可见性，降低耦合和命名污染。
3. 函数内 `static` 变量生命周期贯穿程序运行期，不会每次调用都重建。
4. `volatile` 只解决“可见访问”问题，不保证原子性，也不提供线程同步。
5. 嵌入式里 `volatile` 的高频场景是：硬件寄存器、ISR 与主循环共享标志、DMA 更新缓冲区状态位。

如果只能背“const 常量、static 静态、volatile 易变”，却讲不清链接属性、生命周期、优化器行为和并发边界，就说明这部分还没有真正掌握。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 明明变量“会变”，主循环却读不到更新值。
2. 多个 `.c` 文件出现重名符号冲突或接口污染。
3. 函数内部状态希望跨调用保留，但实现不稳定。
4. 面试和工程沟通常问到 `const` 指针、`static` 可见性、`volatile` 适用边界。

它在 AI 时代仍然重要，因为 AI 很容易给出“能编译”的代码，但经常在寄存器映射、共享标志、模块边界上漏掉关键语义。你理解这三个关键字，才能快速判断代码是“表面正确”还是“有时序雷”。

## 核心概念 / 本质机制

### 1. `const` 的本质

`const` 约束的是“通过这个名字是否允许写”。

例如：

```c
const int a = 10;      /* a 只读视图 */
int x = 1;
const int *p = &x;     /* 不能通过 p 改 *p，但 x 仍可被别名修改 */
```

关键点：

1. `const` 是类型系统约束，不是物理只读保证。
2. `const` 可以帮助编译器做接口防误用和优化判断。
3. 接口参数用 `const` 能明确“只读承诺”。

### 2. `static` 在文件作用域

文件作用域下：

```c
static int g_counter;
static void helper(void);
```

效果：

1. 符号只在当前编译单元可见（内部链接）。
2. 外部文件无法直接引用。

工程价值：

1. 缩小暴露面。
2. 降低符号冲突概率。
3. 形成更清晰模块边界。

### 3. `static` 在函数作用域

函数内 `static`：

```c
void tick(void)
{
    static unsigned int cnt;
    cnt++;
}
```

效果：

1. 变量只在该函数名义作用域可见。
2. 生命周期是整个程序运行期。
3. 初次初始化一次，后续保留上次值。

适合场景：

1. 小型状态计数。
2. 局部缓存。
3. 无需外部访问的持久状态。

### 4. `volatile` 的本质

`volatile` 告诉编译器：每次访问都必须执行真实内存/寄存器访问。

高频场景：

1. 内存映射寄存器。
2. ISR 改、主循环读的标志位。
3. 外设/DMA 异步更新状态。

关键边界：

1. `volatile` 不等于原子。
2. `volatile` 不等于互斥。
3. `volatile` 不等于内存屏障完整替代。

### 5. 组合类型怎么读

常见声明：

1. `const int *p`：指向只读 `int` 的指针。
2. `int * const p`：常量指针，指向可改 `int`。
3. `const int * const p`：指针和目标都只读。

读法建议：先看变量名，再向外扩展。

## 数据流 / 控制流 / 时序关系

下面用“中断置位、主循环处理”串主线：

```text
ISR 触发
-> 修改 volatile 标志位
-> 主循环每轮读取标志位
-> 发现置位后处理事件
-> 清标志位
```

如果标志位不加 `volatile`，优化器可能把读取提升或缓存，主循环就可能“看不到变化”。

## 最小可运行示例

```c
#include <stdio.h>

static int g_module_only = 0;

static void module_helper(void)
{
    g_module_only++;
}

volatile int g_irq_flag = 0;

void fake_isr_set_flag(void)
{
    g_irq_flag = 1;
}

int main(void)
{
    const int threshold = 3;
    static int handled = 0;

    module_helper();

    while (handled < threshold) {
        if (g_irq_flag) {
            g_irq_flag = 0;
            handled++;
            printf("handled=%d\n", handled);
        }

        if (handled == 1) {
            fake_isr_set_flag();
        } else if (handled == 2) {
            fake_isr_set_flag();
        } else if (handled == 0) {
            fake_isr_set_flag();
        }
    }

    return 0;
}
```

这个示例里：

1. `g_module_only` 和 `module_helper` 用 `static` 限制文件内可见。
2. `handled` 用函数内 `static` 体现跨循环保留。
3. `g_irq_flag` 用 `volatile` 表达异步可变。
4. `threshold` 用 `const` 表达只读阈值语义。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 只读参数 | `const` 修饰输入参数 | 全部裸指针 | 接口语义不清，误改风险高 |
| 模块私有函数 | 文件内 helper 用 `static` | 默认外部可见 | 符号污染和耦合升高 |
| ISR 共享标志 | `volatile` + 明确时序 | 普通全局变量 | 优化后可能读不到变化 |
| 并发安全 | 用原子/锁解决同步 | 误把 `volatile` 当同步工具 | 竞态仍然存在 |

## 边界条件与适用范围

1. `volatile` 不是多线程同步万能键；并发一致性仍要靠原子和同步原语。
2. `static` 文件内可见性是编译单元级，不是目录级。
3. `const` 只能约束当前类型视图，别名写入仍可能改变底层值。

## 常见坑与排查

1. 主循环读不到中断更新：先检查共享变量是否 `volatile`。
2. 链接重定义：检查本该私有的全局是否缺少 `static`。
3. 误改输入缓冲区：检查接口参数是否应加 `const`。
4. 以为用了 `volatile` 就线程安全：检查是否缺少原子/互斥。

## 工程落地建议

1. 头文件优先表达 `const` 语义，减少误用。
2. `.c` 内默认私有符号加 `static`，只导出必要 API。
3. 对异步共享变量先判断“是否需要 `volatile` + 原子/锁组合”。

## 关键要点

1. `const` 管“可写性语义”。
2. `static` 管“生命周期和链接可见性”。
3. `volatile` 管“访问不可省略”，不管原子同步。
4. 三者经常组合使用，核心是先明确变量角色再下关键字。

## 关联笔记

1. `C语言-函数指针与回调`
2. `C语言-模块化与头文件组织`
3. `C与C++-并发基础`
4. `C语言-位运算与寄存器操作`
