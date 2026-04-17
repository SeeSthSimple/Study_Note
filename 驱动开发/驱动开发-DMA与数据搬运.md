# 驱动开发-DMA与数据搬运

## 原始问题

为什么高波特率 UART 和 SPI 用中断模式会丢数据？DMA 到底怎么用？一致性 DMA 和流式 DMA 有什么区别？`dma_alloc_coherent` 和 `dma_map_single` 各自适用什么场景？scatter-gather 又是什么？

## 先给结论

DMA 的核心是"让硬件自己搬数据，CPU 不参与"：

1. 一致性 DMA（`dma_alloc_coherent`）：CPU 和 DMA 共享同一块内存，硬件自动维护缓存一致性，适合控制流等小量频繁访问的数据。
2. 流式 DMA（`dma_map_single` / `dma_map_sg`）：CPU 先准备好数据，映射给 DMA 用，用完取消映射，适合大数据量单向传输。
3. scatter-gather DMA：用一张链表描述多个不连续内存片段，DMA 自动遍历，减少内存整理和拷贝。
4. DMA engine 框架是 Linux 提供的通用 DMA 控制器抽象，驱动通过 `dma_request_channel` + `dmaengine_prep_*` + `dmaengine_submit` 提交传输。
5. 使用 DMA 时必须注意缓存一致性、对齐约束和传输完成回调。

如果只能背"DMA 就是硬件搬数据"，但讲不清一致性 DMA 和流式 DMA 的区别、scatter-gather 的用法、缓存一致性问题，就还没有真正掌握 Linux DMA 编程。

## 这个知识解决什么问题

这篇笔记主要解决下面几类问题：

1. 中断模式 UART/SPI 在高波特率下丢数据，不知道怎么改用 DMA。
2. 不理解一致性 DMA 和流式 DMA 的区别，不知道该用哪个。
3. 不知道怎么在驱动里使用 DMA engine 框架提交传输。
4. 不理解 scatter-gather 的作用和用法。
5. DMA 传输偶发数据错乱，不知道是缓存一致性问题还是对齐问题。

它在AI时代依然重要，因为AI生成 DMA 代码时经常漏掉缓存一致性操作、忽略对齐约束、忘记错误处理。理解 DMA 的底层约束后，你才能判断AI给的代码是否在真实硬件上可靠工作。

## 核心概念 / 本质机制

### 1. 为什么需要 DMA

CPU 搬数据的问题：

1. 每字节一次中断，中断开销大。
2. 高波特率下 CPU 大部分时间在搬数据，没空做别的。
3. 中断延迟可能导致 FIFO 溢出丢数据。

DMA 的优势：

1. CPU 完全不参与数据搬运。
2. 传输速度只受总线带宽限制。
3. CPU 可以同时做计算。
4. 中断次数从 N 次减少到 1 次。

### 2. 一致性 DMA vs 流式 DMA

| 特性 | 一致性 DMA | 流式 DMA |
| --- | --- | --- |
| 分配方式 | `dma_alloc_coherent` | `dma_map_single` / `dma_map_sg` |
| 缓存一致性 | 硬件自动维护 | 驱动手动维护（map/unmap） |
| 适用场景 | 控制描述符、小量频繁访问 | 大数据量单向传输 |
| 性能开销 | 较大（每次访问都过缓存） | 较小（批量同步） |
| 内存来源 | 专用分配 | 已有缓冲区映射 |
| 典型用途 | DMA 描述符、控制寄存器镜像 | 音视频缓冲、网络包、UART 批量数据 |

### 3. 一致性 DMA 用法

```c
void *virt_addr;
dma_addr_t dma_handle;

/* 分配 */
virt_addr = dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL);
if (!virt_addr)
    return -ENOMEM;

/* CPU 写数据 */
memset(virt_addr, 0, size);

/* DMA 硬件使用 dma_handle 访问 */
writel(dma_handle, base + REG_DMA_SRC);

/* 传输完成后 CPU 读取 */
val = readl(virt_addr);

/* 释放 */
dma_free_coherent(dev, size, virt_addr, dma_handle);
```

关键点：

1. `virt_addr` 是 CPU 访问的虚拟地址。
2. `dma_handle` 是 DMA 硬件使用的总线地址。
3. 两者之间硬件自动维护缓存一致性，驱动不需要手动 sync。
4. 分配的内存是页对齐的，可能浪费空间。

### 4. 流式 DMA 用法

```c
dma_addr_t dma_handle;
void *buf = kmalloc(size, GFP_KERNEL);

/* 映射：CPU → DMA 方向 */
dma_handle = dma_map_single(dev, buf, size, DMA_TO_DEVICE);
if (dma_mapping_error(dev, dma_handle)) {
    kfree(buf);
    return -EIO;
}

/* DMA 硬件使用 dma_handle */
writel(dma_handle, base + REG_DMA_SRC);

/* 传输完成后取消映射 */
dma_unmap_single(dev, dma_handle, size, DMA_TO_DEVICE);

kfree(buf);
```

方向参数：

| 方向 | 含义 |
| --- | --- |
| `DMA_TO_DEVICE` | CPU 写好数据，DMA 读走发送 |
| `DMA_FROM_DEVICE` | DMA 写入数据，CPU 读取 |
| `DMA_BIDIRECTIONAL` | 双向传输（性能最低，尽量避免） |

关键点：

1. `dma_map_single` 做三件事：分配总线地址、刷新 CPU 缓存、返回 DMA 地址。
2. `dma_unmap_single` 做三件事：释放总线地址、使 CPU 缓存失效（FROM 方向）、返回所有权给 CPU。
3. 映射期间 CPU 不能访问该缓冲区（否则缓存一致性被破坏）。
4. 如果需要在映射期间 CPU 访问，必须用 `dma_sync_single_for_cpu` / `dma_sync_single_for_device`。

### 5. 流式 DMA 的同步操作

```c
/* DMA 正在写入缓冲区... */

/* 传输完成中断里 */
dma_unmap_single(dev, dma_handle, size, DMA_FROM_DEVICE);

/* 现在 CPU 可以安全读取 buf */

/* 如果不 unmap 而是想继续让 DMA 用同一块缓冲区 */
dma_sync_single_for_cpu(dev, dma_handle, size, DMA_FROM_DEVICE);
/* CPU 读取数据 */
val = buf[0];
dma_sync_single_for_device(dev, dma_handle, size, DMA_FROM_DEVICE);
/* DMA 继续使用 */
```

### 6. scatter-gather DMA

当数据分散在多个不连续的内存区域时，传统 DMA 需要先拷贝到一块连续内存。scatter-gather DMA 用一张描述符表告诉 DMA 控制器"依次访问这些地址"，避免拷贝。

```c
struct scatterlist sg[4];
int nents;

/* 准备 scatterlist */
sg_init_table(sg, 4);
sg_set_buf(&sg[0], buf0, len0);
sg_set_buf(&sg[1], buf1, len1);
sg_set_buf(&sg[2], buf2, len2);
sg_set_buf(&sg[3], buf3, len3);

/* 映射 */
nents = dma_map_sg(dev, sg, 4, DMA_TO_DEVICE);
if (nents == 0)
    return -EIO;

/* DMA 硬件遍历 sg 列表传输 */
/* ... */

/* 取消映射 */
dma_unmap_sg(dev, sg, nents, DMA_TO_DEVICE);
```

scatter-gather 的典型场景：

1. 网络发送：skb 的数据可能分散在多个页面。
2. 块设备读写：bio 结构包含多个 segment。
3. 大数据传输：避免分配大块连续内存。

### 7. DMA Engine 框架

Linux 提供了通用的 DMA 控制器抽象：DMA engine。

```c
struct dma_chan *chan;
struct dma_async_tx_descriptor *desc;
dma_cookie_t cookie;

/* 1. 申请 DMA 通道 */
chan = dma_request_slave_channel(dev, "rx");
if (IS_ERR(chan))
    return PTR_ERR(chan);

/* 2. 准备传输描述符 */
desc = dmaengine_prep_slave_single(chan, dma_handle, size,
                                    DMA_DEV_TO_MEM,
                                    DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
if (!desc) {
    dma_release_channel(chan);
    return -EIO;
}

/* 3. 设置完成回调 */
desc->callback = my_dma_callback;
desc->callback_param = priv;

/* 4. 提交传输 */
cookie = dmaengine_submit(desc);

/* 5. 启动传输 */
dma_async_issue_pending(chan);
```

完成回调：

```c
static void my_dma_callback(void *param)
{
    struct mydev_priv *priv = param;

    /* DMA 传输完成，处理数据 */
    dma_unmap_single(priv->dev, priv->dma_handle, priv->size,
                     DMA_FROM_DEVICE);

    /* 通知上层 */
    complete(&priv->dma_done);
}
```

### 8. DMA 和设备树的配合

```dts
mydev@10000000 {
    compatible = "vendor,mydev";
    reg = <0x10000000 0x1000>;
    interrupts = <0 25 4>;
    dmas = <&dma0 2>, <&dma0 3>;
    dma-names = "rx", "tx";
};
```

驱动里获取通道：

```c
chan_rx = dma_request_slave_channel(dev, "rx");
chan_tx = dma_request_slave_channel(dev, "tx");
```

### 9. DMA 的对齐和约束

1. 分配的缓冲区必须对齐到 cache line（通常 32 或 64 字节），否则缓存一致性操作会影响相邻数据。
2. 传输大小通常需要对齐到 DMA 控制器的最小传输单位。
3. `dma_alloc_coherent` 分配的内存自动对齐。
4. `kmalloc` 分配的内存通常也满足 cache line 对齐，但最好用 `dma_get_cache_alignment()` 确认。

## 数据流 / 控制流 / 时序关系

### 一致性 DMA 的数据流

```text
dma_alloc_coherent() 分配内存
-> CPU 写控制描述符到虚拟地址
-> 把 dma_handle 传给 DMA 硬件
-> DMA 硬件读取并执行
-> CPU 通过虚拟地址读取结果
-> dma_free_coherent() 释放
```

### 流式 DMA 的数据流

```text
kmalloc() 分配缓冲区
-> CPU 填充数据
-> dma_map_single() 映射给 DMA
-> 把 dma_handle 传给 DMA 硬件
-> DMA 传输数据
-> 传输完成中断
-> dma_unmap_single() 取消映射
-> CPU 读取/处理数据
-> kfree() 释放
```

### DMA Engine 的数据流

```text
dma_request_slave_channel() 申请通道
-> dmaengine_prep_slave_single() 准备描述符
-> 设置完成回调
-> dmaengine_submit() 提交
-> dma_async_issue_pending() 启动
-> DMA 硬件传输
-> 传输完成中断
-> 完成回调被调用
-> dma_release_channel() 释放通道
```

## 最小可运行示例

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/slab.h>

struct mydev_priv {
    struct device *dev;
    struct dma_chan *rx_chan;
    struct dma_chan *tx_chan;
    void *rx_buf;
    void *tx_buf;
    dma_addr_t rx_dma;
    dma_addr_t tx_dma;
    size_t buf_size;
    struct completion dma_done;
};

static void my_dma_callback(void *param)
{
    struct mydev_priv *priv = param;
    complete(&priv->dma_done);
}

static int my_start_rx_dma(struct mydev_priv *priv)
{
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;

    desc = dmaengine_prep_slave_single(priv->rx_chan, priv->rx_dma,
                                        priv->buf_size,
                                        DMA_DEV_TO_MEM,
                                        DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
    if (!desc) return -EIO;

    desc->callback = my_dma_callback;
    desc->callback_param = priv;

    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie))
        return -EIO;

    dma_async_issue_pending(priv->rx_chan);
    return 0;
}

static int my_probe(struct platform_device *pdev)
{
    struct mydev_priv *priv;
    int ret;

    priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) return -ENOMEM;

    priv->dev = &pdev->dev;
    priv->buf_size = 4096;
    init_completion(&priv->dma_done);

    priv->rx_chan = dma_request_slave_channel(&pdev->dev, "rx");
    if (!priv->rx_chan) {
        dev_err(&pdev->dev, "failed to get rx dma channel\n");
        return -ENODEV;
    }

    priv->rx_buf = dma_alloc_coherent(&pdev->dev, priv->buf_size,
                                       &priv->rx_dma, GFP_KERNEL);
    if (!priv->rx_buf) {
        ret = -ENOMEM;
        goto fail_buf;
    }

    ret = my_start_rx_dma(priv);
    if (ret) goto fail_start;

    platform_set_drvdata(pdev, priv);
    dev_info(&pdev->dev, "probed with DMA\n");
    return 0;

fail_start:
    dma_free_coherent(&pdev->dev, priv->buf_size, priv->rx_buf, priv->rx_dma);
fail_buf:
    dma_release_channel(priv->rx_chan);
    return ret;
}

static int my_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);

    dmaengine_terminate_sync(priv->rx_chan);
    dma_free_coherent(&pdev->dev, priv->buf_size, priv->rx_buf, priv->rx_dma);
    dma_release_channel(priv->rx_chan);

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
        .name = "mydev-dma",
        .of_match_table = my_match,
    },
};
module_platform_driver(my_driver);

MODULE_LICENSE("GPL");
```

## 代码解读

### 1. 为什么接收用 `dma_alloc_coherent` 而不是 `dma_map_single`

接收场景下 DMA 会频繁写入缓冲区，CPU 需要频繁读取。一致性 DMA 自动维护缓存一致性，不需要每次手动 sync，代码更简单。

### 2. 为什么 `DMA_PREP_INTERRUPT` 标志很重要

没有这个标志，DMA 传输完成后不会触发中断，回调不会被调用，`completion` 永远不会完成。

### 3. 为什么 `dmaengine_terminate_sync` 在 `remove` 里调用

确保正在进行的 DMA 传输被安全终止，不会在设备移除后继续访问已释放的内存。

### 4. 为什么 `dma_request_slave_channel` 用 "rx" 字符串

对应设备树里 `dma-names = "rx", "tx"` 的第一个成员。

## 正确写法 vs 常见错误写法

| 场景 | 正确写法 | 常见错误写法 | 为什么错 |
| --- | --- | --- | --- |
| 缓存一致性 | `dma_map_single` + `dma_unmap_single` | 直接把 `kmalloc` 地址给 DMA | 缓存不一致，数据可能错乱 |
| 流式 DMA 期间 CPU 访问 | `dma_sync_single_for_cpu` | 映射期间直接读写 | 破坏缓存一致性 |
| DMA 地址 | 使用 `dma_handle`（总线地址） | 使用 `virt_addr`（虚拟地址） | DMA 硬件不认识虚拟地址 |
| 完成回调 | 设置 `DMA_PREP_INTERRUPT` 标志 | 不设标志 | 回调不会被调用 |
| 通道释放 | `dma_release_channel` | 不释放 | 通道泄漏 |
| 传输终止 | `dmaengine_terminate_sync` | 直接 `remove` | DMA 可能继续访问已释放内存 |

## 边界条件与适用范围

1. 不是所有 SoC 的所有外设都支持 DMA，需要查阅数据手册。
2. DMA 传输有最小和最大长度限制，超出范围需要分多次传输。
3. 一致性 DMA 的内存是不可缓存（uncached）的，频繁访问性能较低。
4. 流式 DMA 映射期间 CPU 不能访问缓冲区，否则缓存一致性被破坏。
5. scatter-gather 需要硬件支持，不是所有 DMA 控制器都支持。
6. DMA 传输完成回调在软中断上下文执行，不能睡眠。

## 常见坑与排查

| 现象 | 常见根因 | 优先验证方法 |
| --- | --- | --- |
| 数据前几个字节错乱 | 缓存一致性问题 | 检查是否正确 map/unmap |
| DMA 传输不启动 | 通道获取失败或描述符准备失败 | 检查 `dma_request_slave_channel` 返回值 |
| 完成回调不被调用 | 缺少 `DMA_PREP_INTERRUPT` 标志 | 加标志 |
| 传输偶发数据错误 | 对齐问题 | 确认缓冲区 cache line 对齐 |
| oops 或崩溃 | DMA 访问了已释放的内存 | `dmaengine_terminate_sync` 再释放 |
| 性能不如预期 | 用了一致性 DMA 而非流式 DMA | 大数据量改用流式 DMA |

### 推荐排查顺序

1. 先确认 DMA 通道是否获取成功。
2. 再确认描述符是否准备成功。
3. 再确认传输是否启动（`dma_async_issue_pending`）。
4. 再确认完成回调是否被调用。
5. 最后检查数据内容和缓存一致性。

## 工程落地建议

### 1. 小量控制数据用一致性 DMA

如 DMA 描述符本身、控制寄存器镜像等，数据量小、频繁访问，一致性 DMA 更简单。

### 2. 大数据量传输用流式 DMA

如音视频缓冲、网络包、批量传感器数据，流式 DMA 性能更好。

### 3. 优先使用 DMA Engine 框架

不要直接操作 DMA 控制器寄存器，用 `dmaengine_*` 接口，代码可移植性更好。

### 4. 传输完成用 completion 等待

```c
init_completion(&priv->dma_done);
my_start_dma(priv);
wait_for_completion_timeout(&priv->dma_done, msecs_to_jiffies(1000));
```

### 5. 调试时用 `dma_mapping_error` 检查

每次 `dma_map_single` 后都检查返回值，映射失败通常意味着 DMA 地址空间耗尽。

## 性能、稳定性、可维护性影响

1. DMA 能显著降低 CPU 占用，释放的 CPU 时间可以用来做计算。
2. 一致性 DMA 的 uncached 内存访问性能较低，不适合大数据量。
3. 流式 DMA 的 map/unmap 有一定开销，但比 CPU 搬数据快得多。
4. scatter-gather 避免了内存拷贝，对性能敏感场景很重要。
5. DMA 的 bug 通常是偶发性的（缓存一致性、时序），比普通 bug 更难排查。

## 面试 / 问答怎么讲

### 30 秒版本

DMA 让硬件自己搬数据，CPU 不参与。一致性 DMA 用 `dma_alloc_coherent`，自动维护缓存一致性，适合控制描述符。流式 DMA 用 `dma_map_single`，手动维护缓存一致性，适合大数据量传输。scatter-gather 用链表描述多个不连续内存片段，避免拷贝。Linux 通过 DMA engine 框架提供通用接口。

### 3 分钟版本

可以从两种 DMA 的区别讲起：一致性 DMA 自动维护缓存一致性但性能较低，流式 DMA 手动维护但性能更好。然后说明流式 DMA 的 map/unmap 流程和方向参数。再补充 scatter-gather 的作用：避免把分散数据拷贝到连续内存。最后说明 DMA engine 框架的使用流程：申请通道 → 准备描述符 → 提交 → 启动 → 回调。

### 10 分钟版本

可以结合一个 UART DMA 接收场景展开：中断模式下高波特率丢数据，改用 DMA 后 CPU 不参与搬运。然后详细说明流式 DMA 的 map/unmap 流程、缓存一致性操作、完成回调在软中断上下文执行的限制。再讨论 scatter-gather 在网络和块设备里的应用。最后说明排障主线：通道获取 → 描述符准备 → 传输启动 → 回调触发 → 数据验证。

## 实战练习

1. 用一致性 DMA 实现一个简单的内存到内存拷贝，验证数据正确性。
2. 把一个中断模式 UART 驱动改成 DMA 模式，对比 CPU 占用率。
3. 用 `dma_map_sg` 实现 scatter-gather 传输，验证多个不连续缓冲区能正确传输。
4. 故意不调用 `dma_unmap_single`，观察数据是否错乱。
5. 用 `dmaengine_terminate_sync` 安全终止一个正在进行的 DMA 传输。

## 关键要点

1. DMA 的核心是让硬件自己搬数据，CPU 不参与。
2. 一致性 DMA 自动维护缓存一致性，适合控制描述符；流式 DMA 手动维护，适合大数据量。
3. 流式 DMA 映射期间 CPU 不能访问缓冲区，需要 sync 操作。
4. scatter-gather 用链表描述多个不连续内存片段，避免拷贝。
5. DMA engine 框架提供通用接口：申请通道 → 准备描述符 → 提交 → 启动 → 回调。
6. DMA 地址（总线地址）和虚拟地址是两套地址空间，不能混用。
7. DMA 的 bug 通常是偶发性的，预防比排障更重要。

## 关联笔记

1. `驱动开发-IRQ处理基础`
2. `驱动开发-UART驱动基础`
3. `驱动开发-SPI驱动基础`
4. `驱动开发-devm资源管理`
5. `C与C++-内存管理`
6. `C语言-结构体与内存布局`
