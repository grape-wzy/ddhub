# ddhub 数据分发中心

[English](usage.md) | 简体中文

版本：**0.1.0**；说明：**初版，首次发布**。

ddhub 是 **Data Dispatch Hub** 的缩写：Data（数据）+ Dispatch（分发）+ Hub（中心），即数据分发中心。

ddhub 是可移植的 C99 组件，对象和消息队列使用静态存储。
业务提供侵入式订阅节点和平台锁适配，消息通过借用的数据指针传递，并按事件分发给订阅者，无需动态分配。
C++ 应用可以使用公共 C 接口，并链接以 C 编译的实现。

## 文件与配置

组件文件：

- `include/ddhub.h`：节点、回调类型、平台锁类型、状态码和七个公开操作。
- `include/ddhub_config.h`：用户对象枚举和 FIFO 容量。
- `src/ddhub.c`：订阅管理、静态 FIFO 和事件分发。
- `docs/`：本使用说明。
- `tests/`：组件自动化测试。
- `examples/`：[Linux、Windows 和 STM32 裸机示例](../examples/README.zh-CN.md)。

在 `ddhub_config.h` 中修改 `ddhub_id_t`，实际对象从 0 开始连续编号，在 `DDHUB_ID_COUNT` 之前添加。
遍历全部对象调用 `ddhub_process_all()`，对象数量不受事件位图的 32 位限制。

`DDHUB_FIFO_DEPTH` 指定每个对象独立的 FIFO 容量，必须大于 0，默认 8 条。
FIFO 只保存 `event/data/len/done_arg`，不复制 data 内容。容量不包含已经出队、正在执行回调的一条消息。
总静态消息存储随 `DDHUB_ID_COUNT * DDHUB_FIFO_DEPTH` 增长，订阅数量由业务提供的节点数量决定。

需要在其他工程或测试中使用另一份配置时，可统一定义
`DDHUB_CONFIG_HEADER="my_ddhub_config.h"`。所有使用 ddhub 的编译单元必须使用相同配置。
默认配置提供一个通用对象 `DDHUB_DEFAULT`，没有自动注册业务回调。

## CMake 接入

在仓库根目录构建 `ddhub` C99 静态库：

```sh
cmake -G Ninja -S . -B build/library
cmake --build build/library
```

其他 CMake 工程可这样接入：

```cmake
add_subdirectory(path/to/ddhub)
target_link_libraries(my_application PRIVATE ddhub)
```

应用使用自定义配置头时，由父工程添加其包含路径和编译定义：

```cmake
target_include_directories(ddhub PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/config")
target_compile_definitions(ddhub PUBLIC DDHUB_CONFIG_HEADER="my_ddhub_config.h")
```

库和使用方共享所选配置，目标平台的编译参数通过应用工具链设置。

## 平台锁移植

启动时，在其他 ddhub 操作及并发调用开始之前配置一次：

```c
typedef uintptr_t ddhub_lock_key_t;
typedef ddhub_lock_key_t (*ddhub_lock_t)(void);
typedef void (*ddhub_unlock_t)(ddhub_lock_key_t key);

ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);
```

lock 进入互斥区域并返回本次操作的状态值，unlock 在同一执行上下文中原样接收该值。
中断适配可用它恢复之前的屏蔽状态，不需要恢复状态的适配器可以返回 0。

适配器为所有支持的调用上下文提供互斥和内存可见性，加锁需要成功，lock/unlock 回调不能重入 ddhub。
中断调用使用支持中断上下文的适配器，多核调用使用跨核同步机制。
业务回调和完成通知均在平台临界区外执行。

空函数指针返回 `DDHUB_INVALID`。未绑定时，有效的注册、注销、通知配置、发送及处理请求返回
`DDHUB_NOT_READY`。绑定仅允许在串行启动阶段执行一次，重复绑定返回 `DDHUB_BUSY`，
以避免运行期间更换同步域。平台绑定不清空对象、节点或队列。

Zephyr 适配示例：

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

该示例使用 pthread 互斥锁，锁操作失败时终止进程。

## 订阅与事件

业务声明长期有效的节点：

```c
static ddhub_subscription_t subscription;
```

静态零初始化使节点处于未注册状态。一个节点同时只能属于一个对象；不同节点可以使用同一个回调。
注册期间节点字段由 ddhub 管理，业务不能修改、复制或重新初始化节点。
注销成功后节点可以复用。
修改事件、优先级、回调或 arg 时，先注销再注册。

不同对象使用不同节点时，可从多个线程并发注册和注销。
在 A 上注册或注销不会让 B 变忙；同一对象的操作发生冲突时，返回 `DDHUB_BUSY`。

并发注册不能将同一个节点挂入两个对象。业务协调节点的复用和生命周期，
使其保持有效直到注销成功，且所有引用它的并发操作均已返回。

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
| `ddhub_unregister(node)` | 注销订阅并允许复用节点 |
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

`void * const` 描述局部指针形参，不表示数据只读，所指内容仍可修改。
回调 typedef 中的顶层 const 不会强制回调定义保留相同的局部限定符。
指针按值传递，局部形参重新赋值不会修改调用方的指针变量。

## 处理范围与并发

`ddhub_process(id)` 只处理指定对象在本轮开始时已经排队的消息。
`ddhub_process_all()` 选择空闲对象，在执行任何回调前记录这些对象的待处理消息数量，再按枚举顺序分发。
忙对象直接跳过，即使它随后空闲，本次 ALL 也不会重新选择；其他调用可以独立处理它。
快照之后发送的消息留待下一轮，包括回调中新发出的消息。
同一对象内部保持 FIFO 顺序，对象之间不保证全局发送顺序。

同时只允许一个 `ddhub_process_all()` 执行；另一个 ALL 调用立即返回 busy，不消费消息。
任一对象被跳过，或处理中遇到忙状态，最终返回 busy，
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
| register | 不同对象使用不同节点可同时注册 | 同对象操作冲突返回 busy；同一节点不能同时归属多个对象 |
| unregister | 不同对象可以独立注销各自节点 | 目标对象需要可用；节点保持有效直到引用它的并发操作结束 |
| send | 每个对象拥有独立 FIFO | 目标对象暂不可用时返回 busy，满时返回 queue full；平台适配器可能等待 |
| set_process_done | 可分别配置空闲对象 | 目标 FIFO 必须为空且未处理；ALL 仅冻结选中对象的配置 |
| process | 不同对象可同时执行回调，也可与跳过本对象的 ALL 并行 | 同一对象只允许一个处理调用；被 ALL 选中的对象返回 busy |
| process_all | 跳过正在处理或被其他操作占用的对象，继续处理其他对象 | 同时只有一个 ALL；跳过或遇到忙状态时返回 busy，其余对象继续处理 |
| set_lock_ops | 启动阶段串行配置一次 | 不支持并发首次绑定或运行期更换同步域 |

回调和完成通知在内部对象锁、平台临界区之外执行，因此回调可以 send 同一对象，
也可以操作其他空闲对象。当前处理对象的 register/unregister/set_process_done 返回 busy，
递归 process 当前对象也返回 busy。单对象回调中可以调用 ALL，它跳过当前对象并处理其他空闲对象，
最终返回 busy。ALL 回调中再次调用 ALL 返回 busy；process 被本轮 ALL 选中的对象也返回 busy，
而本轮跳过的对象在空闲后仍可独立 process。

对象暂不可用时返回 `DDHUB_BUSY`，调用方可以稍后重试。
接口是否可能等待由平台适配器决定；需要非阻塞调用时，使用非阻塞适配器。

同一个回调函数可能在不同对象的处理线程中同时执行。如果不同对象共享可变 data 或 arg，
业务负责这些内容的并发同步及数据生命周期；不同 hub_id 并不能自动保证业务数据彼此独立。

如果处理中途返回 busy，已经完成的消息不回滚、不重放，
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
| `DDHUB_BUSY` | -2 | 目标对象忙、处理调用重叠，或当前对象不能修改 |
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
