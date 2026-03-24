# C/C++ 类型转换总览

## 1. 先抓本质

类型转换不是“换个类型名字”这么简单，它本质上在做下面几类事情：

  **改：值、解释方式、限定符、对象视角、值类别**

对应到代码里，可能分别意味着：

- `3.14 -> 3`
- `Derived* -> Base*`
- `int* -> const int*`
- `void* -> T*`
- `T& -> T&&`

所以判断一个转换是否正确，至少要同时看 4 个维度：

1. 转换后值是否还正确
2. 内存布局是否支持这样解释
3. 生命周期是否仍然有效
4. 语言规则是否允许这样做

很多 bug 不是“转换语法不会写”，而是“转换行为理解错了”。

---

## 2.笔记大纲

文件现在按 “**C 与 C++ 共通机制** + **C 特有习惯** + **C++ 特有机制**+ **工程实践**” 四层

1. 左值到右值转换
2. 数组到指针转换
3. 函数到函数指针转换
4. `const` / `volatile` 限定相关转换
5. 整型提升
6. 浮点提升
7. 默认实参提升
8. usual arithmetic conversions
9. 整数与浮点互转
10. `bool` 转换
11. 枚举转换
12. 空指针常量转换
13. 指针到 `void*` / `void*` 到对象指针
14. 指针到 `bool`
15. 指针与整数互转
16. C 风格强制转换
---
17. C++ 函数风格转换
18. `static_cast`
19. `dynamic_cast`
20. `const_cast`
21. `reinterpret_cast`
22. `std::bit_cast`
23. 基类/派生类指针与引用转换
24. 对象切片
25. 引用绑定与临时对象
26. 用户自定义转换
27. 列表初始化中的窄化检查
28. `std::move` / `std::forward` 这种“值类别转换”
29. 少见但可能遇到的成员指针转换

---

## 3. 先做总分类

从工程视角看，C/C++ 类型转换可以先分成 4 大组：

### 3.1 自动发生的隐式转换

你没有写 cast，但编译器帮你做了。
易被忽略和丢失信息

例如：

```cpp
double d = 10;
int x = d;
```


### 3.2 显式写出来的强制转换

你明确告诉编译器“我要这样转”。

例如：

```cpp
int x = static_cast<int>(3.14);
```

### 3.3 标准定义的值转换

这类转换主要发生在表达式求值、赋值、参数传递时，例如：

- 整型提升
- usual arithmetic conversions
- `bool` 转换
- 枚举转换

### 3.4 视角转换或底层重解释

这类转换最危险，因为它可能不是在“变值”，而是在“换一种方式看同一份内存”。

例如：

- `void* -> int*`
- `Derived* -> Base*`
- `reinterpret_cast<int*>(&d)`

---

## 4. C 和 C++ 共同存在的核心隐式转换

这一组是最基础也最容易出 bug 的部分。

### 4.1 左值到右值转换

当一个对象出现在“需要值”的地方时，编译器会把“对象本身”转换成“这个对象当前保存的值”。

```cpp
#include <iostream>

int main() {
    int x = 42;
    int y = x;  // 这里会读取 x 的值
    std::cout << y << '\n';
}
```

这看起来平常，但要注意一个关键点：

- 左值到右值转换意味着“读对象”
- 如果对象还没初始化，这一步就可能触发未定义行为

例如：

```cpp
int main() {
    int x;
    int y = x;  // 读取未初始化对象，危险
}
```

---

### 4.2 数组到指针转换

数组在大多数表达式里会退化成指向首元素的指针。

```c
#include <stdio.h>

void print_first(const int* p) {
    printf("%d\n", p[0]);
}

int main(void) {
    int arr[3] = {10, 20, 30};
    print_first(arr);  // int[3] -> int*
}
```

但要记住：

- 数组不是指针
- 只是很多场景下会自动转换成指针

不会发生数组退化的典型场景：

1. `sizeof(arr)`
2. `&arr`
3. 用字符串字面量初始化字符数组

示例：

```c
#include <stdio.h>

int main(void) {
    int arr[3] = {1, 2, 3};
    printf("%zu\n", sizeof(arr));   // 整个数组大小
    printf("%p\n", (void*)&arr);    // 指向整个数组
    printf("%p\n", (void*)arr);     // 指向首元素
}
```

---

### 4.3 函数到函数指针转换

函数名在很多场景下也会转成函数指针。

```c
#include <stdio.h>

void hello(void) {
    printf("hello\n");
}

int main(void) {
    void (*fp)(void) = hello;
    fp();
}
```

要注意：

- 函数本身不是指针
- 只是很多表达式里，函数标识符会转换成对应函数指针

---

### 4.4 限定符转换：`T* -> const T*`

可以把“可写对象的指针”转成“只读视角的指针”：

```cpp
int x = 42;
int* p = &x;
const int* cp = p;
```

这个方向安全，因为“我本来能写，现在决定先不写”。

但反方向不允许隐式做：

```cpp
const int x = 42;
const int* cp = &x;
// int* p = cp;  // 不允许
```

更重要的坑在多级指针：

```cpp
int value = 0;
int* p = &value;
int** pp = &p;

// const int** cpp = pp;  // 不允许
```

为什么？

因为如果允许，就可能绕过 `const` 保护，把一个 `const int` 间接改掉。

---

### 4.5 整型提升

`char`、`short` 这类比 `int` 小的整数类型，在表达式计算里通常会先提升成 `int` 或 `unsigned int`。

```c
#include <stdio.h>

int main(void) {
    char a = 100;
    char b = 30;
    int c = a + b;  // 实际运算通常先提升到 int
    printf("%d\n", c);
}
```

常见会发生整型提升的类型：

- `char`
- `signed char`
- `unsigned char`
- `short`
- `unsigned short`
- `_Bool` / `bool`

为什么它重要：

- 解释了为什么很多表达式结果不是你以为的小类型
- 解释了为什么 `printf` / 变参函数里 `char` 不会按 `char` 传

---

### 4.6 浮点提升

`float` 在某些上下文里会提升成 `double`，最典型就是变参函数。

```c
#include <stdio.h>

int main(void) {
    float f = 3.14f;
    printf("%f\n", f);  // 实际上传给 printf 的是 double
}
```

---

### 4.7 默认实参提升

这组规则在 C 和 C++ 的变参函数 `...` 中都很重要：

1. `char` / `short` 先整型提升
2. `float` 提升成 `double`

例如：

```c
#include <stdio.h>

int main(void) {
    char c = 'A';
    short s = 7;
    float f = 1.5f;

    printf("%c %d %f\n", c, s, f);
}
```

你看到 `%c %d %f`，但底层传参并不是按原始小类型逐个传过去。

这也是为什么：

- 变参函数类型检查弱
- 格式串错了经常直接出大问题

---

### 4.8 usual arithmetic conversions

只要两个算术类型一起参与运算，编译器就会找一个“共同类型”。

```cpp
#include <iostream>

int main() {
    int a = 10;
    double b = 2.5;
    auto c = a + b;  // int -> double
    std::cout << c << '\n';
}
```

这里的核心不是“哪个类型更大”，而是：

- 先做整型提升
- 再根据规则找共同类型

最常见风险是有符号和无符号混算：

```cpp
#include <iostream>

int main() {
    int a = -1;
    unsigned int b = 1;
    std::cout << (a < b) << '\n';
}
```

这类代码经常和直觉不一致，因为 `a` 可能会被转换成无符号数。

---

### 4.9 整数和浮点互转

#### 整数转浮点

```cpp
int x = 42;
double d = x;
```

通常语义清晰，但并不保证大整数一定能被精确表示。

#### 浮点转整数

```cpp
double d = 3.99;
int x = d;  // 结果是 3
```

特点：

- 截断，不是四舍五入
- 值超出目标整数类型范围时，行为不安全或不可表示

工程里应尽量写成：

```cpp
int x = static_cast<int>(d);
```

这样意图更清楚。

---

### 4.10 转成 `bool`

在 C 里通常对应 `_Bool` 或 `stdbool.h` 的 `bool`，在 C++ 里就是内建 `bool`。

数值转 `bool` 的规则很简单：

- 0 -> `false`
- 非 0 -> `true`

```c
#include <stdio.h>
#include <stdbool.h>

int main(void) {
    int a = 0;
    int b = 7;
    printf("%d %d\n", (bool)a, (bool)b);
}
```

指针也可以转成 `bool`：

```cpp
int x = 1;
int* p = &x;
bool ok = p;  // 非空为 true
```

---

### 4.11 枚举转换

这是 C 和 C++ 容易混淆但差异很大的点。

#### C 里的枚举

C 里的 `enum` 本质上和整数更接近，很多场景会自然参与整数运算。

```c
enum Color { RED = 1, GREEN = 2 };

int main(void) {
    enum Color c = RED;
    int x = c;
    return x;
}
```

#### C++ 里的非强类型枚举

```cpp
enum Color { Red = 1, Green = 2 };

int x = Red;  // 可转成 int
```

#### C++ 的强类型枚举 `enum class`

```cpp
enum class Color { Red = 1, Green = 2 };

// int x = Color::Red;  // 不允许隐式转成 int
int x = static_cast<int>(Color::Red);
```

这就是 `enum class` 的价值之一：减少无意的隐式转换。

---

### 4.12 空指针常量转换

#### C

C 里常见空指针常量有：

- `0`
- `NULL`

#### C++

C++11 以后更推荐：

- `nullptr`

示例：

```cpp
int* p = nullptr;
if (p == nullptr) {
}
```

为什么 `nullptr` 更好：

- 它有独立类型
- 不会像 `0` 那样和整数重载混淆

---

### 4.13 对象指针和 `void*`

这是 C 和 C++ 的一条高频差异。

#### 在 C 里

对象指针和 `void*` 之间可隐式转换：

```c
#include <stdlib.h>

int main(void) {
    int* p = malloc(sizeof(int));  // C 里不需要强转
    free(p);
}
```

#### 在 C++ 里

`T* -> void*` 可以隐式转：

```cpp
int x = 42;
void* vp = &x;
```

但 `void* -> T*` 需要显式转换：

```cpp
int* p = static_cast<int*>(vp);
```

这是为什么：

- C 里 `malloc` 不用 cast
- C++ 里如果硬要用 `malloc`，就得 cast

不过现代 C++ 通常不推荐 `malloc` + 强转来管理对象。

---

### 4.14 指针到整数、整数到指针

这类转换很底层，也很容易出可移植性问题。

```cpp
#include <cstdint>
#include <iostream>

int main() {
    int x = 42;
    int* p = &x;
    std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(p);
    int* q = reinterpret_cast<int*>(raw);
    std::cout << *q << '\n';
}
```

要点：

- 不要把它当普通业务操作
- 应优先使用 `std::uintptr_t` / `uintptr_t`
- 不是所有整数类型都适合承载指针
- 指针做整数运算后再转回，不一定仍然合法

---

### 4.15 函数指针转换

函数名可以转函数指针，但不同函数指针类型之间不能随便乱转。

```cpp
void f(int) {}
void (*pf)(int) = f;
```

危险做法：

```cpp
using A = void(*)(int);
using B = void(*)(double);

A pa = f;
// B pb = reinterpret_cast<B>(pa);  // 即使能转，调用也非常危险
```

还要记住：

- 对象指针和函数指针不是一回事
- 两者之间转换不具有良好可移植性

---

## 5. C 语言里的显式类型转换

### 5.1 C 里主要就是 C 风格强制转换

写法：

```c
(T)expr
```

示例：

```c
double d = 3.14;
int x = (int)d;
```

在 C 里这是主要显式转换方式。

它既能做：

- 数值转换
- 指针转换
- 去掉限定符
- 把结果丢给 `void`

也正因为它“什么都能干”，所以风险也最大。

---

### 5.2 C 里常见的 `void` 转换

把表达式转成 `void`，含义是：

- 我只想执行副作用
- 我不要这个返回值

```c
int main(void) {
    int x = 0;
    (void)x;  // 避免未使用告警
    return 0;
}
```

这是很常见也很合理的转换。

---

### 5.3 C 里不要给 `malloc` 强转

在 C 里：

```c
int* p = malloc(sizeof(int));
```

就够了。

如果写成：

```c
int* p = (int*)malloc(sizeof(int));
```

坏处是：

- 掩盖缺少 `#include <stdlib.h>` 的问题
- 容易把本来应由编译器暴露出来的错误藏掉

---

### 5.4 C 中字符串字面量相关的坑

C 里字符串字面量类型是字符数组，但它不是让你安全修改的可写缓存。

```c
char* p = "hello";
```

这类代码在很多旧代码中能看到，但修改它是未定义行为：

```c
// p[0] = 'H';  // 不安全
```

更稳妥：

```c
const char* p = "hello";
char s[] = "hello";
```

C++ 对这个问题更严格，见后文。

---

## 6. C++ 里的标准转换序列和重载决议

如果你以后要读模板、重载、泛型代码，这一节非常重要。

C++ 在重载匹配时，不只是看“能不能转”，还看“哪种转换更优”。

大致优先级可以先这样理解：

1. 精确匹配
2. 提升
3. 一般标准转换
4. 用户自定义转换
5. 省略号匹配

示例：

```cpp
#include <iostream>

void f(int) {
    std::cout << "f(int)\n";
}

void f(double) {
    std::cout << "f(double)\n";
}

int main() {
    char c = 1;
    f(c);  // 常见结果是调用 f(int)，因为 char -> int 属于提升
}
```

这就是为什么“提升”通常比“一般转换”更受偏好。

---

## 7. C++ 里的显式转换总表

现代 C++ 里最重要的是这 6 类：

1. C 风格转换 `(T)expr`
2. 函数风格转换 `T(expr)`
3. `static_cast`
4. `dynamic_cast`
5. `const_cast`
6. `reinterpret_cast`
7. `std::bit_cast`（C++20）

其中真正推荐优先用的是命名转换：

- `static_cast`
- `dynamic_cast`
- `const_cast`
- `reinterpret_cast`

---

## 8. C 风格转换 `(T)expr`

```cpp
double d = 3.14;
int x = (int)d;
```

问题不在于它“完全不能用”，而在于它太模糊。

在 C++ 里，一个 C 风格转换背后可能等价于：

- `static_cast`
- `const_cast`
- `reinterpret_cast`
- 或这些的组合

也就是说，你一眼看不出它到底是在：

- 截断数值
- 去掉 `const`
- 重解释地址

所以：

- 现代 C++ 不推荐优先用它
- 尤其不要在复杂代码里用它来掩盖危险转换

---

## 9. 函数风格转换 `T(expr)`

```cpp
int x = int(3.14);
```

它和 C 风格转换一样，也可能让真实意图不够清晰。

但它有一个非常常见且合理的用途：

- 调用构造函数

```cpp
#include <string>

int main() {
    std::string s("hello");
}
```

所以要区分：

- `int(3.14)` 更像强制转换
- `std::string("hello")` 更像构造对象

---

## 10. `static_cast`

### 10.1 适用范围

`static_cast` 适合“规则明确、编译期能说清楚”的转换。

典型场景：

1. 数值类型互转
2. 枚举和整数互转
3. `void*` 转回原对象指针
4. 派生类和基类之间某些已知安全的转换
5. 显式调用转换构造函数或转换运算符
6. 转成 `void`
7. 构造右值引用，`std::move` 本质上就是它

---

### 10.2 数值转换示例

```cpp
#include <iostream>

int main() {
    double pi = 3.14159;
    int x = static_cast<int>(pi);
    std::cout << x << '\n';
}
```

---

### 10.3 枚举转换示例

```cpp
enum class Color { Red = 1, Green = 2 };

int x = static_cast<int>(Color::Red);
Color c = static_cast<Color>(2);
```

注意：

- 从整数转回枚举，语法上可以
- 但语义上不代表这个整数一定是“有效枚举值”

---

### 10.4 `void*` 转回具体类型

```cpp
int x = 42;
void* vp = &x;
int* p = static_cast<int*>(vp);
```

前提必须是：

- `vp` 原本真的来自 `int*`

---

### 10.5 基类和派生类指针/引用转换

向上转型：

```cpp
struct Base {};
struct Derived : Base {};

Derived d;
Base* pb = &d;  // 隐式即可
```

向下转型：

```cpp
Base* pb = &d;
Derived* pd = static_cast<Derived*>(pb);
```

这里的前提非常强：

- `pb` 真正指向的就是 `Derived`

如果判断错了，后果是未定义行为。

所以：

- 编译器允许，不代表运行时安全

---

### 10.6 转成 `void`

```cpp
int x = 0;
static_cast<void>(x);
```

这比 `(void)x` 更明确，也更符合现代 C++ 风格。

---

## 11. `dynamic_cast`

### 11.1 它解决什么问题

`dynamic_cast` 主要解决：

- 多态继承体系里的安全下转
- 运行时检查对象真实类型

要求：

- 基类必须是多态类型
- 通常意味着基类里至少有一个虚函数

```cpp
struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {
    void run() {}
};
```

---

### 11.2 指针形式

```cpp
#include <iostream>

struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {
    void run() {
        std::cout << "Derived::run\n";
    }
};

int main() {
    Base* pb = new Derived;

    if (Derived* pd = dynamic_cast<Derived*>(pb)) {
        pd->run();
    }

    delete pb;
}
```

转换失败时：

- 返回 `nullptr`

---

### 11.3 引用形式

```cpp
#include <iostream>
#include <typeinfo>

struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {};

int main() {
    Base b;

    try {
        Derived& d = dynamic_cast<Derived&>(b);
        (void)d;
    } catch (const std::bad_cast& e) {
        std::cout << e.what() << '\n';
    }
}
```

转换失败时：

- 抛出 `std::bad_cast`

---

### 11.4 `dynamic_cast<void*>`

这是一个常被忽略但确实存在的用法：把多态对象指针转成“指向最派生对象”的 `void*`。

这类场景不高频，但在读复杂运行时系统代码时可能遇到。

---

### 11.5 什么时候不要滥用

如果你发现代码里到处都要 `dynamic_cast`，通常说明：

- 抽象层次可能不对
- 行为分发本该由虚函数完成
- 设计可能在向“类型判断驱动”倾斜

---

## 12. `const_cast`

### 12.1 它只处理限定符

`const_cast` 只做一类事：

- 增加或去掉 `const`
- 增加或去掉 `volatile`

```cpp
const int* cp = nullptr;
int* p = const_cast<int*>(cp);
```

它不会做数值转换，不会做继承转换，也不会做底层重解释。

---

### 12.2 最大风险

如果对象本身原来就是 `const`，你通过 `const_cast` 去改它，结果是未定义行为。

```cpp
const int x = 42;
const int* cp = &x;
int* p = const_cast<int*>(cp);
// *p = 7;  // 未定义行为
```

相对合理的场景是：

- 对象原本不是 `const`
- 只是经过某个接口后类型上带了 `const`

```cpp
int x = 42;
const int* cp = &x;
int* p = const_cast<int*>(cp);  // 仅在确实知道底层对象非 const 时才考虑
```

即使这样，也应优先反思接口设计。

---

## 13. `reinterpret_cast`

### 13.1 它的本质

`reinterpret_cast` 不是在做“普通值转换”，而是在做“底层重解释”。

```cpp
#include <cstdint>

int main() {
    int x = 42;
    int* p = &x;
    std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(p);
    int* q = reinterpret_cast<int*>(raw);
}
```

它能做的典型事包括：

1. 指针和足够大的整数互转
2. 不同对象指针类型之间重解释
3. 某些函数指针相关重解释

---

### 13.2 为什么它危险

因为它通常不保证：

- 语义正确
- 对齐正确
- 严格别名规则安全
- 可移植性

例如：

```cpp
double d = 3.14;
int* p = reinterpret_cast<int*>(&d);
```

这类代码即使能编译，也不意味着可以安全读取 `*p`。

---

### 13.3 它不会自动帮你创建对象

很多人误以为：

```cpp
char buffer[sizeof(int)];
int* p = reinterpret_cast<int*>(buffer);
```

就意味着这里已经有一个合法的 `int` 对象了。不是。

你还得关心：

- 对齐
- 对象生命周期
- 是否真的构造了对象

这类问题一旦混错，就不再是“语法问题”，而是对象模型问题。

---

## 14. `std::bit_cast`（C++20）

### 14.1 它和 `reinterpret_cast` 的区别

`std::bit_cast` 更像：

- 按位复制
- 用新类型承接同样的比特模式

而不是：

- 直接把同一块对象内存强行当另一种类型引用

```cpp
#include <bit>
#include <cstdint>
#include <iostream>

int main() {
    float f = 1.0f;
    std::uint32_t bits = std::bit_cast<std::uint32_t>(f);
    std::cout << bits << '\n';
}
```

适用前提：

- 源和目标大小相同
- 类型满足相应约束，通常是可平凡拷贝

适用场景：

- 查看位模式
- 协议字段编码解码
- 底层格式转换

---

## 15. C++ 里的用户自定义转换

这部分是 C 没有、C++ 很重要的能力。

### 15.1 转换构造函数

一个单参数构造函数可能让“其他类型 -> 当前类”自动发生。

```cpp
#include <iostream>

class Distance {
public:
    Distance(int meter) : meter_(meter) {}

    int value() const {
        return meter_;
    }

private:
    int meter_;
};

int main() {
    Distance d = 10;  // int -> Distance
    std::cout << d.value() << '\n';
}
```

如果不想让它隐式发生，应加 `explicit`：

```cpp
class Distance {
public:
    explicit Distance(int meter) : meter_(meter) {}

private:
    int meter_;
};
```

---

### 15.2 转换运算符

类也可以定义“把自己转成别的类型”的规则。

```cpp
#include <iostream>

class Flag {
public:
    explicit Flag(bool v) : value_(v) {}

    explicit operator bool() const {
        return value_;
    }

private:
    bool value_;
};

int main() {
    Flag ok(true);
    if (ok) {
        std::cout << "true\n";
    }
}
```

现代 C++ 中，这类转换通常建议：

- 能 `explicit` 就尽量 `explicit`

否则类对象可能会在你没注意时参与各种隐式转换。

---

## 16. 基类/派生类相关的转换

### 16.1 向上转型

派生类到基类通常安全，可隐式完成。

```cpp
struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {};

int main() {
    Derived d;
    Base* pb = &d;
    Base& rb = d;
}
```

---

### 16.2 向下转型

基类到派生类不是天然安全的。

```cpp
Base* pb = new Derived;
Derived* pd1 = static_cast<Derived*>(pb);   // 依赖你自己保证真实类型
Derived* pd2 = dynamic_cast<Derived*>(pb);  // 运行时检查
```

---

### 16.3 对象切片

这是“按值转换”为基类对象时很容易漏掉的问题。

```cpp
#include <iostream>

struct Base {
    int a = 1;
};

struct Derived : Base {
    int b = 2;
};

int main() {
    Derived d;
    Base base = d;  // 派生部分被切掉
    std::cout << base.a << '\n';
}
```

这不是报错，而是合法但容易误解的行为。

切片意味着：

- 你不再拥有 `Derived` 那部分内容

所以多态场景通常用指针或引用，而不是按值传基类对象。

---

## 17. 引用绑定、临时对象和生命周期

### 17.1 `const T&` 可以绑定临时对象

```cpp
#include <string>

int main() {
    const std::string& ref = std::string("hello");
}
```

这里发生的不是“普通数值转换”，而是：

- 临时对象物化
- 引用绑定
- 生命周期延长到引用作用域结束

这在现代 C++ 里非常重要。

---

### 17.2 非 `const` 左值引用不能绑定右值

```cpp
int main() {
    // int& r = 10;  // 不允许
}
```

因为一个可修改的左值引用，不能直接绑定到纯右值临时量。

---

### 17.3 右值引用绑定

```cpp
int main() {
    int&& rr = 10;
}
```

右值引用更多和移动语义相关，见后文。

---

## 18. `std::move` 和 `std::forward`

这两个经常被误认为“函数”，但更准确地说，它们是帮助你完成值类别转换的工具。

### 18.1 `std::move`

`std::move(x)` 本质上接近：

```cpp
static_cast<T&&>(x)
```

它不是“真的移动了对象”，而是：

- 把 `x` 转成右值视角
- 让后续代码有机会调用移动构造或移动赋值

示例：

```cpp
#include <string>
#include <utility>

int main() {
    std::string s = "hello";
    std::string t = std::move(s);
}
```

---

### 18.2 `std::forward`

`std::forward` 用于模板里“按原来的值类别转发出去”。

```cpp
#include <iostream>
#include <utility>

void consume(const int&) {
    std::cout << "consume lvalue\n";
}

void consume(int&&) {
    std::cout << "consume rvalue\n";
}

template <typename T>
void wrapper(T&& arg) {
    consume(std::forward<T>(arg));
}
```

它不是普通类型转换，而是：

- 保留左值/右值属性
- 避免把右值错误地重新当左值传出去

这类转换更准确地说属于：

- 引用折叠
- 值类别保持

---

## 19. 列表初始化与窄化转换

C++11 后，花括号初始化可以帮你挡住很多危险的隐式窄化。

```cpp
int a = 3.14;    // 可以，发生截断
// int b{3.14};  // 通常直接报错
```

这非常值得养成习惯，因为很多“偷偷丢信息”的转换会直接暴露在编译期。

---

## 20. 少见但应知道的成员指针转换

C++ 里不只有普通指针，还有：

- 指向成员对象的指针
- 指向成员函数的指针

例如：

```cpp
struct Base {
    int x = 1;
};

int Base::* pm = &Base::x;
```

在继承层次中，成员指针也存在特定的基类/派生类相关转换规则。  
它不如普通指针常见，但在模板库、框架或反射式代码里可能出现。

记住一点就够：

- 成员指针和普通地址指针完全不是一回事

---

## 21. C 和 C++ 在类型转换上的关键差异

### 21.1 `void*`

- C：`void*` 和对象指针可隐式互转
- C++：`void* -> T*` 需要显式转换

### 21.2 字符串字面量

- C：历史上常见 `char* p = "abc"`，但修改仍然危险
- C++：字符串字面量是 `const char[N]`，不能转成 `char*`

### 21.3 枚举

- C：更接近整数
- C++：`enum class` 明显更严格

### 21.4 命名转换

- C：主要靠 `(T)expr`
- C++：有 `static_cast`、`dynamic_cast`、`const_cast`、`reinterpret_cast`

### 21.5 用户自定义转换

- C：没有
- C++：有转换构造函数和转换运算符

---

## 22. 常见错误写法 vs 更合理写法

### 22.1 不分场景直接 C 风格转换

错误倾向：

```cpp
int x = (int)3.14;
```

更清晰：

```cpp
int x = static_cast<int>(3.14);
```

---

### 22.2 忽略有符号/无符号混算

风险代码：

```cpp
int a = -1;
size_t b = 1;
if (a < b) {
}
```

更稳妥的思路：

- 统一比较双方类型
- 在进入比较前就把语义说清楚

---

### 22.3 用 `static_cast` 乱做下转

风险代码：

```cpp
Base* pb = get_object();
Derived* pd = static_cast<Derived*>(pb);
```

更稳妥：

```cpp
if (Derived* pd = dynamic_cast<Derived*>(pb)) {
    pd->run();
}
```

---

### 22.4 用 `const_cast` 改真正的常量对象

```cpp
const int x = 1;
int* p = const_cast<int*>(&x);
// *p = 2;  // UB
```

---

### 22.5 把 `reinterpret_cast` 当成“万能转型器”

危险思路：

```cpp
double d = 3.14;
int* p = reinterpret_cast<int*>(&d);
```

这类代码的问题通常不在“能不能编译”，而在：

- 别名规则
- 对齐
- 生命周期
- 可移植性

---

### 22.6 C 里给 `malloc` 乱加 cast

错误倾向：

```c
int* p = (int*)malloc(sizeof(int));
```

更推荐：

```c
int* p = malloc(sizeof(int));
```

---

## 23. 实战里该怎么选

如果你在写 C：

1. 先依赖语言本身的自然转换规则
2. 对隐式数值转换保持警惕
3. `malloc` 不要强转
4. 多检查 `char` / `short` / `float` 在变参中的提升
5. 涉及指针转换时，先想清楚对象真实类型和生命周期

如果你在写 C++：

1. 能不转就不转
2. 能靠接口设计避免转换，就优先改接口
3. 数值转换优先 `static_cast`
4. 多态安全下转优先 `dynamic_cast`
5. 只处理限定符才用 `const_cast`
6. 只有底层场景才碰 `reinterpret_cast`
7. 不优先用 C 风格转换
8. 用户自定义转换尽量 `explicit`
9. 多用花括号初始化挡窄化

---

## 24. 面试时如何又全又不乱地回答

如果别人问你：

“C/C++ 的类型转换有哪些？”

一个结构清楚的回答可以是：

1. 先分隐式转换和显式转换
2. 隐式转换里要会说：左值到右值、数组到指针、函数到函数指针、整型提升、浮点提升、默认实参提升、usual arithmetic conversions、整数浮点互转、`bool` 转换、枚举转换、空指针转换、指针和 `void*` 转换
3. C 里显式转换主要是 C 风格 `(T)expr`
4. C++ 里显式转换重点是：`static_cast`、`dynamic_cast`、`const_cast`、`reinterpret_cast`
5. C++ 还要补：用户自定义转换、引用绑定、对象切片、列表初始化窄化检查、`std::move` / `std::forward`
6. 最后强调：会不会转不重要，重要的是转了是否安全、是否丢信息、是否破坏对象模型

---

## 25. 一句话总结

C/C++ 的类型转换不是零散的语法点，而是一整套关于值、内存、限定符、继承、生命周期和接口设计的规则系统。  
真正的能力不是“把 cast 写出来”，而是能判断这次转换究竟是在改值、改视角、改权限，还是在埋雷。
