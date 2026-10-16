# ddhub 使用说明

[English](usage.md) | 简体中文

ddhub（Data Dispatch Hub，数据分发中心）是静态内存、侵入式订阅节点、借用 payload 的 C99 组件，公共头支持 C++。

## 配置与构建

在 `ddhub_config.h` 或统一选择的 `DDHUB_CONFIG_HEADER` 中定义连续的 `ddhub_id_t`，实际 hub 从 0 开始，在 `DDHUB_ID_COUNT` 前添加。所有编译单元必须使用相同配置。

| 配置 | 默认值 | 含义 |
|---|---|---|
| `DDHUB_ASYNC_PROCESS_ENABLE` | `1` | `1` 为发送入队、统一 process；`0` 为 send 直接分发 |
| `DDHUB_FIFO_DEPTH` | `8` | 异步模式每 hub 的等待队列容量，必须大于零 |
| `DDHUB_CONFIG_HEADER` | `"ddhub_config.h"` | 构建级用户配置头 |

同步构建不包含 FIFO 存储，也不声明或导出 `ddhub_process()`、`ddhub_set_process_notify()`。本次接口调整移除了带 ID 的 process 和独立的 process_all。

```sh
cmake -G Ninja -S . -B build/library
cmake --build build/library
cmake -G Ninja -S . -B build/library-sync -DDDHUB_ASYNC_PROCESS_ENABLE=0
cmake --build build/library-sync
cmake -G Ninja -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

父工程通过 `add_subdirectory(path/to/ddhub)` 和 `target_link_libraries(app PRIVATE ddhub)` 接入。自定义配置使用 PUBLIC 包含路径及 `DDHUB_CONFIG_HEADER="my_config.h"` 编译定义。只有显式设置 CMake 变量 `DDHUB_ASYNC_PROCESS_ENABLE` 时，构建系统才通过 PUBLIC 定义覆盖配置头默认值。

## 平台互斥

在串行启动阶段、任何其他操作之前绑定一次：

```c
ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);
```

lock 必须成功取得互斥，返回本次获取专属的 `uintptr_t` token；unlock 在相同上下文恢复 token。适配器共同提供 acquire/release 内存顺序，不能重入 ddhub。未绑定时有效操作返回 NOT_READY，重复绑定返回 BUSY。

线程、ISR、多核支持由适配器决定。单核 Cortex-M 可以保存、屏蔽、恢复中断；普通桌面线程可以使用互斥锁。回调、完成通知、全局通知都在锁外执行。

排序查找在平台锁外用每 hub 写者标志串行化。链接提交、消息描述符操作、活动节点选择和推进在平台锁内进行。选择下一节点可能扫描跳过不匹配或本条消息开始后注册的节点；注册序号耗尽时也在锁内重排序号。临界区不会复制 payload 或调用业务代码，其耗时受订阅数量影响。

## 公开接口

| 接口 | 用途 |
|---|---|
| `ddhub_set_lock_ops(lock, unlock)` | 一次性配置平台互斥 |
| `ddhub_register(id, node, event, priority, callback, arg1, arg2, arg3)` | 注册调用方提供的节点 |
| `ddhub_unregister(node)` | 安全摘除非活动节点 |
| `ddhub_set_process_done(id, done)` | 在 hub 空闲且队列为空时配置完成函数 |
| `ddhub_send(id, event, data, len, done_arg)` | 按编译模式直接分发或入队 |
| `ddhub_set_process_notify(notify)` | 仅异步：启动阶段一次性绑定全局调度通知 |
| `ddhub_process()` | 仅异步：单消费者统一排空所有 hub |

ID 必须在 `[0, DDHUB_ID_COUNT)`。事件掩码不能为零，全部 32 位都可使用。`len > 0` 要求 data 非空；零长度允许 NULL。消费回调非空，其他上下文及 done 可以为空。

## 同步发送

`DDHUB_ASYNC_PROCESS_ENABLE=0` 时，send 在调用者上下文直接分发目标 hub，完成回调返回后 send 才成功返回。同步 send 不使用 FIFO。

同一 hub 已在分发时，再次发送返回 BUSY；其他空闲 hub 可以分发，可能在不同线程中并行执行。A → B → A 的递归在再次发送到 A 时返回 BUSY。完成函数在处理期间不能替换。

同步回调继承发送者上下文。**从 ISR 发送推荐开启异步模式，以缩短中断处理时间。** 如选择同步 ISR 发送，全部订阅与完成回调都必须支持 ISR 上下文。

## 异步发送、通知与排空

异步 send 只保存 `event/data/len/done_arg` 描述符，不复制 payload。每 hub FIFO 独立，容量不包含已出队、正在回调的消息。满时返回 QUEUE_FULL，不接受消息。链表写者不阻挡发送和出队。

全库只有一个消费执行者。`ddhub_process()` 不等待消息；按轮处理各 hub 的有限当前批次，再复查新消息，直到全部排空。回调和 done 中发送的新消息也在本次调用继续处理。持续发送可能使 process 长期不返回。

并发或递归 process 返回 BUSY。OK 表示最终锁内检查时全部队列为空，并已在同一临界区释放处理权；之后新发送的消息由后续调用处理。因此不能推断调用者实际观察到返回值时队列仍为空。各 hub 内按 FIFO，不保证跨 hub 的全局发送顺序。

通知类型为 `void (*)(void)`，所有成功的 send 调用同一个全局通知，不传 hub ID。注册、注销释放写者后，如果队列仍有消息，也可以补发相同通知。通知在解锁后、发送者或写者上下文执行，适配器必须支持实际的线程/ISR 上下文。

通知在绑定锁之后、并发使用之前一次性配置；未配置或绑定 NULL 可用于轮询。配置后不能更换。通知用于安排后续 process，不应依赖在通知中递归 process 完成排空。

## 平台调用方式

裸机异步模式可以直接从主循环调用 `ddhub_process()`，没有数据就返回，不需要通知钩子。同步模式只调用 send。

RTOS 可以使用一个能保留通知的二值信号量。以下为 Zephyr 消费线程示意；先提供适合发送上下文的平台锁，再绑定通知，最后启动生产者与唯一消费线程：

```c
#include "ddhub.h"
#include <zephyr/kernel.h>

K_SEM_DEFINE(hub_wakeup, 0, 1);

/** @brief Arrange processing from a thread or supported ISR. */
static void process_notify(void)
{
    k_sem_give(&hub_wakeup);
}

/**
 * @brief Run the unique asynchronous consumer.
 * @param arg1 Unused thread context.
 * @param arg2 Unused thread context.
 * @param arg3 Unused thread context.
 */
static void process_thread(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    for (;;) {
        k_sem_take(&hub_wakeup, K_FOREVER);
        ddhub_process();
    }
}
```

启动时调用 `ddhub_set_process_notify(process_notify)` 并检查状态。处理期间的通知可以在排空后仍保留，导致下一次立即醒来做一次空处理；这是允许的。不能在 process 返回后无条件重置信号量，否则可能抹掉最终空检查之后发生的新发送。组件没有 wait/ready 或线程创建接口。

## 动态订阅与生命周期

节点用静态零初始化或 `DDHUB_SUBSCRIPTION_INIT` 初始化。注册后字段归组件管理，不允许修改、复制或重新初始化。一个节点同时只能属于一个 hub；修改订阅参数时先注销再注册。

priority 数值越小越先执行，同优先级按注册顺序。每条消息对订阅掩码计算交集，非零调用一次，传交集；消息不会合并。所有订阅共享同一可变 payload，后执行者可观察前执行者的修改。

**处理期间允许注册，新订阅从下一条消息开始生效，包括同一次 process 中的后续消息。** 同步模式遵循相同消息边界。注册不重放当前消息，也不独立触发回调；每次重新注册取得新序号，仍保持优先级顺序。序号耗尽时保留当前消息的可接收集合后重新编号。

process 只保护仍在使用的活动节点。注销当前节点返回 BUSY；其他节点可以摘除。若在被选中前成功注销，则当前消息也跳过它。活动保护覆盖回调后的最后一次 next 读取，成功注销后调度器不再访问节点。

注册/注销彼此竞争同一 hub 的写者标志时仍可能 BUSY；与处理中的整 hub 冻结不同。不同 hub 的写者可以独立操作。节点及上下文应有效至注销成功且其他持有指针的并发调用全部结束。

## 完成与所有权

send 失败：消息没有被接受，不调用 done，发送者继续拥有 payload。

send 成功：保持 data、done_arg 有效，生产者停止修改 data，直至完成。所有匹配订阅执行完后，done 被调用一次，包括无匹配订阅的情况。done 收到原始 hub、事件、data、len 和 done_arg；此后组件不再访问 payload 或生产者上下文，可以在 done 中释放或复用。

同步 done 在 send 返回前执行。异步 done 在 process 上下文执行，同 hub 的新发送可以再次入队并被本次 process 消费。同步 done 中再次发送同 hub 则 BUSY。done 时没有活动订阅节点，可以注册或注销节点，但 hub 的处理权仍有效，不能替换完成函数。

done 为 NULL 时，数据有效期由应用自行保证。异步发送函数栈上的临时数据通常无法满足消费前的有效期要求。不同 hub 共享的可变业务对象仍需应用同步。

## 返回值

| 状态 | 含义 |
|---|---|
| `DDHUB_OK` | 成功；空 process 也是成功 |
| `DDHUB_INVALID` | ID、事件、节点、消费回调或 data/len 组合无效 |
| `DDHUB_BUSY` | 链表写者竞争、注销活动节点、重复绑定、重叠处理或同步同 hub 发送 |
| `DDHUB_QUEUE_FULL` | 仅异步：目标 FIFO 已满 |
| `DDHUB_ALREADY_REGISTERED` | 节点已注册或被另一次注册预留 |
| `DDHUB_NOT_REGISTERED` | 节点未注册 |
| `DDHUB_NOT_READY` | 尚未绑定平台锁 |

## 验证

主机测试同时编译两种模式，覆盖参数、排序、事件交集、FIFO、完成生命周期、动态注册/注销、序号重排、受控线程交错、最终检查后的发送交接、2000 条多生产者消息与二值通知、C++ 互操作，以及同步构建的 API/符号屏蔽。
