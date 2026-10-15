# ddhub 数据分发中心

[English](usage.md) | 简体中文

版本：**0.1.0**；说明：**初版，首次发布**。

ddhub 是 **Data Dispatch Hub** 的缩写：Data（数据）+ Dispatch（分发）+ Hub（中心），即数据分发中心。

ddhub 是独立的 C 数据分发组件。对象、消息 FIFO、调度状态使用静态存储，订阅节点由业务提供；
不使用动态分配，不依赖 Zephyr、其他链表库或 errno。对象无需资源初始化，首次使用前需绑定平台锁接口。

## 文件与配置

组件位于 `components/ddhub/`：

- `include/ddhub.h`：节点、回调类型、平台锁类型、状态码和七个公开操作。
- `include/ddhub_config.h`：用户对象枚举和 FIFO 容量。
- `src/ddhub.c`：侵入式订阅链表、静态 FIFO、非阻塞锁和分发逻辑。
- `docs/`：本使用说明。
- `tests/`：可独立构建的行为和并发测试，也接入工程级主机测试。
- `examples/`：[Linux、Windows 和 STM32 裸机示例](../examples/README.zh-CN.md)。

在 `ddhub_config.h` 中修改 `ddhub_id_t`，实际对象从 0 开始连续编号，在 `DDHUB_ID_COUNT` 之前添加。
遍历全部对象调用 `ddhub_process_all()`，对象数量不受事件位图的 32 位限制。

`DDHUB_FIFO_DEPTH` 指定每个对象独立的 FIFO 容量，必须大于 0，默认 8 条。
FIFO 只保存 `event/data/len/done_arg`，不复制 data 内容。容量不包含已经出队、正在执行回调的一条消息。
总静态消息存储随 `DDHUB_ID_COUNT * DDHUB_FIFO_DEPTH` 增长，订阅数量由业务提供的节点数量决定。

需要在其他工程或测试中使用另一份配置时，可统一定义
`DDHUB_CONFIG_HEADER="my_ddhub_config.h"`。所有使用 ddhub 的编译单元必须使用相同配置。
默认配置提供一个通用对象 `DDHUB_DEFAULT`，没有自动注册业务回调。

公开头文件仅需要 `<stddef.h>`、`<stdint.h>` 和配置头文件。实现还使用 `<stdbool.h>`。
核心代码只依赖 C99，不调用标准原子接口、编译器原子内建接口或操作系统函数。
共享状态使用普通 bool 和计数变量，互斥及内存可见性由业务注入的平台 lock/unlock 保证。
组件与主机测试单独声明 C99 编译要求，不改变其他应用源码的语言标准。

公共及配套测试头文件均提供 `__cplusplus` 条件编译保护。C++ 业务可直接包含公共头文件，
并链接以 C 编译的 ddhub 实现；实现文件本身使用 C 编译。
静态订阅节点由 C/C++ 的语言规则自动零初始化，可直接声明为未注册状态。

组件及配套测试的 C 源码、头文件均包含文件级 Doxygen 注释，记录 `@file`、`@brief`、`@details`、
`@version 0.1.0`、英文首次发布说明、初始开发日期和英文修改记录。全部源码及头文件使用英文。
原有文件保留 2026-10-14 的开发日期，新增平台锁测试文件的开发日期为 2026-10-15。

## 平台锁移植

启动时，在其他 ddhub 操作及并发调用开始之前配置一次：

```c
typedef uintptr_t ddhub_lock_key_t;
typedef ddhub_lock_key_t (*ddhub_lock_t)(void);
typedef void (*ddhub_unlock_t)(ddhub_lock_key_t key);

ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);
```

lock 必须成功进入真实的互斥区域，返回本次获取的状态值；unlock 原样接收该值并离开互斥区域。
状态值是每次调用的局部变量，可表示中断屏蔽状态，不能用共享全局变量保存它。
Linux 互斥锁等不需要状态值的实现可以返回 0。

lock/unlock 必须同时提供互斥和正确的内存顺序，不能只是空函数或普通 bool 的赋值，且不能重入 ddhub。
平台临界区覆盖状态检查和固定大小的消息描述更新，不复制 data 内容，不执行链表遍历或用户回调。
ALL 的入口及清理临界区按已配置对象数遍历；增加对象数量时，应一并评估裸机中断屏蔽的最长持续时间。
适配器需要覆盖所有调用上下文：单核 MCU 可使用短临界区，多核及线程环境使用适当的系统同步机制。
需要非阻塞或中断调用时，适配器本身也必须满足相应约束；Linux 的阻塞互斥锁适配不能用于 MCU 中断。

空函数指针返回 `DDHUB_INVALID`。未绑定时，有效的注册、注销、通知配置、发送及处理请求返回
`DDHUB_NOT_READY`。绑定仅允许在串行启动阶段执行一次，重复绑定返回 `DDHUB_BUSY`，
以避免运行期间更换同步域。平台绑定不清空对象、节点或队列。

Zephyr/STM32 适配示例：

```c
#include "ddhub.h"
#include <zephyr/irq.h>

/**
 * @brief Save interrupt state and enter platform exclusion.
 * @return Interrupt-state token for this acquisition.
 */
static ddhub_lock_key_t platform_irq_lock(void)
{
    return (ddhub_lock_key_t)irq_lock();
}

/**
 * @brief Restore the interrupt state saved by the corresponding entry.
 * @param key Per-acquisition interrupt-state token.
 */
static void platform_irq_unlock(ddhub_lock_key_t key)
{
    irq_unlock((unsigned int)key);
}

/**
 * @brief Bind the board adapter once from serialized startup code.
 * @return ddhub binding status.
 */
ddhub_status_t example_bind_irq_lock(void)
{
    return ddhub_set_lock_ops(platform_irq_lock, platform_irq_unlock);
}
```

该适配保存并恢复原来的中断状态，支持正确的嵌套，避免退出时无条件开中断。
不受 IRQ 锁屏蔽的中断不能并发访问同一份 ddhub 状态。

Linux/POSIX 线程适配示例：

```c
#include "ddhub.h"
#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t platform_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Enter the process-wide metadata exclusion region.
 * @return Zero, because a pthread mutex needs no restoration token.
 */
static ddhub_lock_key_t platform_mutex_lock(void)
{
    if (pthread_mutex_lock(&platform_mutex) != 0) {
        abort();
    }
    return 0;
}

/**
 * @brief Leave the matching process-wide exclusion region.
 * @param key Unused token returned by platform_mutex_lock().
 */
static void platform_mutex_unlock(ddhub_lock_key_t key)
{
    (void)key;
    if (pthread_mutex_unlock(&platform_mutex) != 0) {
        abort();
    }
}

/**
 * @brief Bind the POSIX adapter once before starting workers.
 * @return ddhub binding status.
 */
ddhub_status_t example_bind_mutex_lock(void)
{
    return ddhub_set_lock_ops(platform_mutex_lock, platform_mutex_unlock);
}
```

示例在平台锁失败时终止进程；实际产品可采用自己的致命错误处理，但不能在加锁失败后继续无保护访问。
pthread 及其错误处理依赖属于平台适配器，不属于 ddhub 核心。

## 订阅与事件

业务声明长期有效的节点：

```c
static ddhub_subscription_t subscription;
```

静态零初始化使节点处于未注册状态。一个节点同时只能属于一个对象；不同节点可以使用同一个回调。
注册期间节点字段由 ddhub 管理，业务不能修改、复制或重新初始化节点。
注销成功后清除链表链接和登记标志，节点可以复用；其余私有字段在下次注册时重新赋值。
修改事件、优先级、回调或 arg 时，先注销再注册。

不同对象使用不同节点时，可从多个线程并发注册和注销。内部锁按对象划分，A 对象的链表操作
不会让 B 对象因内部锁竞争而返回忙。同一对象的元数据操作发生冲突时，仍然返回 DDHUB_BUSY。

同一节点的归属在平台临界区内仲裁：并发向不同对象注册同一节点时，不能同时挂入两条链表。
注销在平台临界区内解析当前所属对象并取得该对象的锁，摘链后才发布节点可重新注册。
节点可能在旧注销调用返回前被另一个调用重新注册，所以业务必须协调同一节点的复用和生命周期，
不能在仍有并发请求引用节点时回收、复制或重新初始化它。


每个对象有自己的 `uint32_t` 事件位图，各 bit 含义由用户定义。
注册和发送的事件掩码都不能为 0，包含 bit 31 在内的全部 32 位均可使用。

对每条订阅计算：

```c
matched = published_event & subscribed_event;
```

`matched != 0` 时调用一次回调，传入 matched。命中多个 bit 也只调用一次。
事件不累计、不锁存；不同消息不合并。消息之间保持对象内部的 FIFO 顺序。

注册参数 `uint8_t priority` 越小越先调用，范围为 0～255。同优先级按注册顺序调用，
注销后重新注册的节点排到同优先级节点末尾。

## 公开接口

| 操作 | 用途 |
| --- | --- |
| `ddhub_set_lock_ops(lock, unlock)` | 启动时一次性绑定平台互斥操作 |
| `ddhub_register(id, node, event, priority, callback, arg1, arg2, arg3)` | 注册侵入式订阅节点 |
| `ddhub_unregister(node)` | 注销并重置节点 |
| `ddhub_set_process_done(id, done)` | 配置对象的完成通知函数 |
| `ddhub_send(id, event, data, len, done_arg)` | 保存消息描述并返回，不调用业务回调 |
| `ddhub_process(id)` | 处理所选对象本轮已有消息 |
| `ddhub_process_all()` | 处理空闲对象本轮已有消息，跳过忙对象 |

业务回调类型：

```c
typedef void (*ddhub_callback_t)(uint32_t event, void * const data, size_t len,
                                void * const arg1, void * const arg2, void * const arg3);

typedef void (*ddhub_process_done_t)(ddhub_id_t hub_id, uint32_t event,
                                    void * const data, size_t len, void * const arg);
```

所有指针所指内容均可修改。同一消息的所有订阅回调共享原始 data，后执行的回调可以看到先前的修改。
三个 arg 在注册时提供，done_arg 按消息提供。所有上下文都由调用方维护生命周期。

`void * const` 约束回调实现中的局部指针形参，允许修改所指结构体成员。
函数指针类型中的顶层 const 不会强制业务函数的定义保留 const；若要编译器阻止局部指针重新赋值，
业务回调的形参也应按上述方式声明。指针按值传递，局部形参赋值本身不会修改调用方的指针变量。

## 处理范围与并发

`ddhub_process(id)` 在取得该对象的处理权和元数据锁后记录待处理数量。
`ddhub_process_all()` 在平台临界区内检查所有对象，一次性选中未处理且元数据锁空闲的对象，
取得这些对象的处理权和元数据锁；忙对象直接跳过，不改变其队列、本轮额度或处理权。
然后记录所有选中对象的数量，再释放元数据锁并按枚举顺序分发。
入口时跳过的对象即使随后空闲，本次 ALL 也不会重新选择；业务可以独立处理它。
该快照之后向选中对象入队的新消息全部留待下一次处理，包括回调或完成通知中新发出的消息。
对象之间不保证全局发送顺序。

一个 `ddhub_object` 中的 `fifo` 统一保存 `head`、`count`、`pending` 和消息数组 `msg`。
`count` 是实时排队数量，`pending` 从本轮入口快照开始逐条递减，限制本轮最多消费多少条消息。
消息出队时两个数量同时递减；发送只增加 `count`，因此本轮新消息不会扩大处理额度。
处理结束时清除本轮剩余额度，未消费消息仍在队列中，下次调用重新记录额度。

同时只允许一个 `ddhub_process_all()` 执行；另一个 ALL 调用立即返回 busy，不消费消息。
任一对象在入口被跳过，或某个选中对象中途发生队列锁争用，最终返回 busy，
但其余选中对象仍继续处理。因此 busy 可以伴随部分甚至多个对象的消息消费，
应依靠逐消息完成通知判断数据何时可以复用，之后再次调用处理接口重试剩余消息。

例如线程 1 正在 `ddhub_process(A)`，线程 2 调用 `ddhub_process_all()`：
ALL 跳过 A，处理空闲的 B、C，最终返回 busy。A 完成后，即使 ALL 尚未结束，
其他调用也可以再次处理 A；ALL 结束时仅释放它选中对象的处理权。

处理开始前的注册、注销会影响本轮接收者。例如 send 成功后注销 A、注册 B，之后 process 时
由 B 按事件匹配接收。指定对象的整批处理期间，该对象订阅链表和完成通知函数保持稳定。
全对象处理的整批期间，仅选中对象的订阅链表和完成通知函数保持稳定。

| 接口 | 不同对象的并发 | 同对象或共享节点的限制 |
| --- | --- | --- |
| register | 不同节点可同时注册，不受其他对象元数据锁影响 | 同对象锁竞争返回 busy；同一节点不能同时归属多个对象 |
| unregister | 依照各节点的当前所属对象独立摘链 | 原子地解析所属关系并取得对象锁；节点由调用方保持有效 |
| send | 各 FIFO 独立，短入队事务由平台锁保护 | 目标对象的链表操作持锁时返回 busy，满时返回 queue full；平台适配器可能等待 |
| set_process_done | 可分别配置空闲对象 | 目标 FIFO 必须为空且未处理；ALL 仅冻结选中对象的配置 |
| process | 不同对象可同时执行回调，也可与跳过本对象的 ALL 并行 | 同一对象只允许一个处理调用；被 ALL 选中的对象返回 busy |
| process_all | 跳过正在处理或元数据锁被占用的对象，继续处理其他对象 | 同时只有一个 ALL；跳过或中途争锁时返回 busy，其余对象继续处理 |
| set_lock_ops | 启动阶段串行配置一次 | 不支持并发首次绑定或运行期更换同步域 |

回调和完成通知在内部对象锁、平台临界区之外执行，因此回调可以 send 同一对象，
也可以操作其他空闲对象。当前处理对象的 register/unregister/set_process_done 返回 busy，
递归 process 当前对象也返回 busy。单对象回调中可以调用 ALL，它跳过当前对象并处理其他空闲对象，
最终返回 busy。ALL 回调中再次调用 ALL 返回 busy；process 被本轮 ALL 选中的对象也返回 busy，
而本轮跳过的对象在空闲后仍可独立 process。

每个对象拥有自己的 bool 元数据锁，检查、置位和清除由平台 lock/unlock 保护。
内部统一使用 `try_hub_lock()` / `release_hub_lock()` 操作对象锁，
使用 `hub_is_processing()` 查询处理状态，使用 `try_hub_processing()` / `release_hub_processing()`
取得和释放处理权。以上辅助函数均要求调用方已经进入平台临界区，本身不再次调用平台 lock/unlock。
取得处理权时同时取得该对象的元数据锁，保证状态检查、处理权预留和快照保护在同一临界区完成。
入队、出队及 done 配置在一个平台临界区内完成对象锁申请、固定大小的状态更新和对象锁释放。
注册在入口临界区中完成对象锁申请、处理状态检查和节点归属预留，随后在临界区外遍历并插入链表。
注销在临界区外摘链，结束时将节点可重新注册及对象锁释放合并到同一个临界区。
每个对象的 bool 锁保护这些较长的链表操作；不同对象的链表遍历及回调仍可并行。
平台临界区还保护节点归属、对象 processing 标志、ALL 选中标记及 ALL 调用活动标志。
`process_by_all` 记录哪些对象由本次 ALL 持有，以免与其他对象的处理权混淆。
不同对象的 FIFO 描述更新和共享控制状态由平台锁短时串行化，不扩大到链表遍历或回调。
处理结束直接通过平台锁清理处理权，不受其他对象的元数据活动影响。

以下按正常完成路径统计，每次表示一对平台 lock/unlock；N 为本次实际出队消息数，
不包含用户回调再次调用 ddhub 产生的额外平台操作：

| 操作 | 平台临界区次数 |
| --- | --- |
| send | 1 |
| 单条消息出队 | 1 |
| set_process_done | 1 |
| register / unregister | 2 |
| process / process_all | 2 + N，包含入口快照、逐条出队及结束清理 |

辅助函数本身不增加平台调用。消息批次中的订阅回调和 done 均在平台锁及对象锁外执行，
processing 只用于保留整批处理权。消息描述事务完成后立即释放对象锁，
避免生产者在临界区外暂停时阻碍已开始的消费批次。

同对象内部锁争用立即返回 busy，核心不等待、休眠或自旋。传入的平台 lock 是否等待由适配器决定；
只有非阻塞平台适配器才能保证整次调用满足非阻塞要求。
调用者需要处理可重试返回值，不应在阻止锁持有者运行的上下文中无限重试。

同一个回调函数可能在不同对象的处理线程中同时执行。如果不同对象共享可变 data 或 arg，
业务负责这些内容的并发同步及数据生命周期；不同 hub_id 并不能自动保证业务数据彼此独立。

出队保留对象锁争用的防御性检查。如果中途返回 busy，已经完成的消息不回滚、不重放，
未消费的消息完整保留在 FIFO。下一次 process 从剩余消息继续，以逐条完成通知判断数据可复用时机。

## 数据生命周期与完成通知

1. send 失败：消息未被接收，不调用完成通知，数据仍完全由发送方持有。
2. send 成功：发送方保持 data、done_arg 有效，并停止自行改写 data，直至收到完成通知。
3. 匹配的订阅回调依次执行，可以修改 data 和各自的 arg 所指内容。
4. 全部匹配回调返回后，调用对象的 done 一次，传入原始事件、原始 data、len、done_arg。
5. 即使没有匹配的订阅者，也会完成通知。进入 done 后 ddhub 不再访问该消息的数据或生产者上下文，
   发送方可以释放或复用它们。done 中再次 send 同一个已完成的缓冲区是允许的。

一个对象只有一个 done 函数，可根据 done_arg 区分不同发送方。done 在 process 调用方的上下文中执行。
done 执行期间仍持有处理权，所以同对象的 process、注册、注销及完成函数更换返回 busy。
只有 FIFO 为空且对象未处于处理状态时才能更换或清除 done；ALL 期间选中对象的 done 保持不变。

允许 done 为 NULL，此时数据有效期由业务自行保证。未配置通知时，异步发送的临时栈数据通常不能满足该要求。
订阅节点及三个 arg 必须至少有效到成功注销；其他并发对象仍持有同一上下文时，业务还需要延长其生命周期。

## 返回值与参数边界

| 状态 | 值 | 含义 |
| --- | --- | --- |
| `DDHUB_OK` | 0 | 操作成功；处理空 FIFO 也是成功 |
| `DDHUB_INVALID` | -1 | 无效 ID、零事件、空节点/消费回调、或空 data 配合正长度 |
| `DDHUB_BUSY` | -2 | 锁争用、处理权冲突、或修改处于忙状态的对象 |
| `DDHUB_QUEUE_FULL` | -3 | 目标对象 FIFO 已满，消息没有入队 |
| `DDHUB_ALREADY_REGISTERED` | -4 | 同一节点已经注册到某个对象 |
| `DDHUB_NOT_REGISTERED` | -5 | 节点未注册，不能注销 |
| `DDHUB_NOT_READY` | -6 | 尚未绑定平台 lock/unlock |

hub_id 只接受 `[0, DDHUB_ID_COUNT)` 范围内的实际对象编号。`data == NULL && len > 0` 无效；
len 为 0 时允许 data 为 NULL，
可用于纯事件。三个 arg、done_arg、done 函数均允许为空。订阅消费函数不可为空。

## 单线程主循环示例

以下示例中的节点、数据和上下文均为静态对象。ready 仅适用于这里的单线程主循环；
若生产与消费分属不同线程，应按业务并发模型同步这个标志。

```c
#include "ddhub.h"
#include "ddhub_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static ddhub_subscription_t listener;
static uint32_t received_count;
static uint32_t sample;
static bool ready = true;

/**
 * @brief Count received samples.
 * @param event Matched sample events.
 * @param data Mutable sample buffer.
 * @param len Buffer size in bytes.
 * @param arg1 Mutable reception counter.
 * @param arg2 Unused context.
 * @param arg3 Unused context.
 */
static void on_sample(uint32_t event, void * const data, size_t len,
                      void * const arg1, void * const arg2, void * const arg3)
{
    (void)event;
    (void)data;
    (void)len;
    (void)arg2;
    (void)arg3;
    ++*(uint32_t *)arg1;
}

/**
 * @brief Return the sample buffer to its producer.
 * @param hub_id Consuming hub.
 * @param event Original event mask.
 * @param data Completed buffer.
 * @param len Completed buffer size.
 * @param arg Producer's availability flag.
 */
static void on_done(ddhub_id_t hub_id, uint32_t event,
                    void * const data, size_t len, void * const arg)
{
    (void)hub_id;
    (void)event;
    (void)data;
    (void)len;
    *(bool *)arg = true;
}

/**
 * @brief Set up this example once from application startup code.
 * @details Requires platform lock callbacks to be bound before this function is called.
 * @return ddhub status; continue only after DDHUB_OK.
 */
ddhub_status_t example_start(void)
{
    /* First bind one of the platform adapters above from serialized startup code. */
    ddhub_status_t status = ddhub_set_process_done(DDHUB_DEFAULT, on_done);
    if (status != DDHUB_OK) {
        return status;
    }
    return ddhub_register(DDHUB_DEFAULT, &listener, 1U, 10U,
                          on_sample, &received_count, NULL, NULL);
}

/**
 * @brief Publish if the buffer is available, then process from the main loop.
 * @param next_value Latest value to publish; ignored while the previous buffer is busy.
 * @return ddhub status; retry transient failures from a later loop iteration.
 */
ddhub_status_t example_tick(uint32_t next_value)
{
    if (ready) {
        sample = next_value;
        ready = false;
        ddhub_status_t status = ddhub_send(DDHUB_DEFAULT, 1U, &sample, sizeof(sample), &ready);
        if (status != DDHUB_OK) {
            ready = true;
            return status;
        }
    }
    return ddhub_process_all();
}
```

需要立即分发时，send 成功后调用 `ddhub_process(DDHUB_DEFAULT)`。它会处理本轮该对象已有的消息，
可能包含本次消息之前排队的消息。若返回 busy，发送成功的消息仍可能等待其他调用处理，
不能单凭 process 的返回值提前复用数据。

## 验证

主机测试位于组件自己的 `tests/`，使用另一份含三个对象、每对象三条消息的配置，不改变固件配置。
可从 ddhub 仓库根目录独立构建：

```powershell
cmake -G Ninja -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

在父工程根目录也可继续运行工程级全部主机测试：

```powershell
cmake -G Ninja -S tests/unit -B build/unit-ninja
cmake --build build/unit-ninja
ctest --test-dir build/unit-ninja --output-on-failure
```

`ddhub_behavior_test` 验证优先级稳定性、32 位事件交集、可变指针内容、FIFO 满与回绕、
ALL 入口快照、单对象回调内调用 ALL、处理时订阅列表、回调内发送、完成通知及数据复用，
并通过平台锁调用计数检查短事务及消息批次的临界区次数。

`ddhub_concurrency_test` 使用真实 host 线程和阻塞回调，验证同对象互斥、不同对象并行、
ALL 跳过忙对象、选中对象互斥、独立处理权清理、并发入队时完成原有批次并推迟新增消息，
以及四个生产者共 2000 条消息的 FIFO 顺序、上下文路由、回调顺序和恰好一次完成通知。
测试适配器逐次校验加锁状态值，使用线程局部数据确认业务回调不处于平台临界区内；
受控暂停点验证对象锁隔离、不同对象同时注册、同节点竞争注册及注销后的跨对象转移。
压力测试的完成统计仅由单个处理线程访问；受控并发用例按对象分别统计，在对应线程完成后检查。
测试也不依赖原子操作接口。pthread 仅属于主机测试依赖，不是组件依赖。

固件采用已有的 `local-debug` CMake 预设构建。新组件已接入 components 的分层 CMake，
业务需要按实际场景注册节点和发送消息，组件不会自动修改现有业务数据路径。
