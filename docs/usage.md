# ddhub user guide

English | [Simplified Chinese](usage.zh-CN.md)

Version: **0.1.0**. Release notes: **Initial version; first release**.

**ddhub** stands for **Data Dispatch Hub**: Data is the payload, Dispatch is routing to interested subscribers,
and Hub is the central coordination point. Together they describe a data distribution center.

ddhub is a portable C99 component with static hub objects and message queues.
Applications provide intrusive subscription nodes and platform lock callbacks.
Messages carry borrowed payload pointers, and subscribers receive matching events without heap allocation.
C++ applications can use the public C API and link the implementation compiled as C.

## Files and configuration

Component files:

- `include/ddhub.h`: nodes, callbacks, platform lock types, status codes, and seven public operations.
- `include/ddhub_config.h`: application-owned hub IDs and FIFO capacity.
- `src/ddhub.c`: subscriptions, static FIFOs, and event dispatch.
- `docs/`: this guide in English and Simplified Chinese.
- `tests/`: automated component tests.
- `examples/`: [Linux, Windows, and STM32 bare-metal examples](../examples/README.md).

Edit `ddhub_id_t` in `ddhub_config.h`. Actual hub IDs must be consecutive, starting at zero;
insert application IDs before `DDHUB_ID_COUNT`. Use `ddhub_process_all()` to visit all available hubs.
The 32-bit event mask does not limit the number of hub objects.

`DDHUB_FIFO_DEPTH` sets an independent queue capacity for each hub. It must be positive; the default is eight messages.
The FIFO stores only `event/data/len/done_arg`, without copying payload bytes. Capacity excludes the message
already dequeued and executing callbacks. Static descriptor storage grows with `DDHUB_ID_COUNT * DDHUB_FIFO_DEPTH`;
the number of subscriptions is limited by application-provided nodes.

For another project or test configuration, define `DDHUB_CONFIG_HEADER="my_ddhub_config.h"` consistently
for every translation unit using ddhub. The default configuration provides one `DDHUB_DEFAULT` hub
and registers no business callbacks automatically.

## CMake integration

Build the `ddhub` C99 static library from the repository root:

```sh
cmake -G Ninja -S . -B build/library
cmake --build build/library
```

To use it from another CMake project:

```cmake
add_subdirectory(path/to/ddhub)
target_link_libraries(my_application PRIVATE ddhub)
```

If the application provides its own configuration header, apply its include directory and definition from the parent:

```cmake
target_include_directories(ddhub PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/config")
target_compile_definitions(ddhub PUBLIC DDHUB_CONFIG_HEADER="my_ddhub_config.h")
```

The library and its consumers share the selected configuration.
Use the application's toolchain settings for the target platform.

## Platform lock adapters

Bind the adapter once during serialized startup, before any other ddhub operation or concurrent caller starts:

```c
typedef uintptr_t ddhub_lock_key_t;
typedef ddhub_lock_key_t (*ddhub_lock_t)(void);
typedef void (*ddhub_unlock_t)(ddhub_lock_key_t key);

ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);
```

The lock callback enters mutual exclusion and returns an opaque token. Unlock receives the matching token
unchanged in the same execution context. An interrupt adapter can use it to restore the previous mask;
an adapter that needs no restoration state may return zero.

The adapter provides exclusion and memory visibility for every supported calling context.
Lock acquisition must succeed, and the adapter callbacks must not reenter ddhub.
For interrupt access, use an interrupt-compatible adapter; for multiple cores, use cross-core synchronization.
Consumer callbacks and completion notifications execute outside platform exclusion.

A NULL callback returns `DDHUB_INVALID`. Before binding, otherwise valid registration, unregistration,
completion configuration, send, and processing calls return `DDHUB_NOT_READY`.
Binding is allowed only once from serialized startup; repeated binding returns `DDHUB_BUSY`.
It does not reset hubs, nodes, or queues and cannot switch synchronization domains at runtime.

Example Zephyr adapter:

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

This adapter saves and restores the previous interrupt state, supports correctly paired nesting,
and does not unconditionally enable interrupts on exit. Interrupts outside the IRQ lock's masking coverage
must not access the same ddhub state concurrently.

Example Linux/POSIX thread adapter:

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

This example uses a pthread mutex and terminates the process if a mutex operation fails.

## Subscriptions and events

Declare a subscription node with sufficient lifetime:

```c
static ddhub_subscription_t subscription;
```

Static zero initialization leaves it unregistered. A node may belong to only one hub at a time;
different nodes may use the same callback function. ddhub owns its fields while registered.
Do not modify, copy, or reinitialize a registered node.
Successful unregistration makes the node available for reuse.
To change events, priority, callback, or context pointers, unregister first and then register again.

Distinct nodes belonging to different hubs can be registered or unregistered concurrently.
Registering or unregistering on A does not make B busy. Conflicting operations on the same hub return `DDHUB_BUSY`.

Concurrent registration cannot attach the same node to two hubs. Applications coordinate node reuse and lifetime,
keeping it valid until unregistration succeeds and all concurrent operations referring to it have returned.

Each hub has an application-defined `uint32_t` event mask. Neither a subscription mask nor a published mask may be zero.
All 32 bits, including bit 31, are available.
For each subscription:

```c
matched = published_event & subscribed_event;
```

When `matched != 0`, invoke that subscription once with the intersection. Multiple matching bits still cause
only one invocation. Events are neither accumulated nor latched, and messages are not merged.
Messages retain FIFO order within each hub.

`uint8_t priority` ranges from 0 to 255; lower numbers execute first. Equal priorities retain registration order.
After unregistering and re-registering, a node moves behind existing nodes of equal priority.

## Public API

| Operation | Purpose |
| --- | --- |
| `ddhub_set_lock_ops(lock, unlock)` | Bind platform exclusion callbacks once at startup |
| `ddhub_register(id, node, event, priority, callback, arg1, arg2, arg3)` | Register an intrusive subscription |
| `ddhub_unregister(node)` | Unregister a subscription and make its node reusable |
| `ddhub_set_process_done(id, done)` | Configure one hub's completion callback |
| `ddhub_send(id, event, data, len, done_arg)` | Enqueue a borrowed descriptor without invoking user callbacks |
| `ddhub_process(id)` | Consume the specified hub's messages present at batch entry |
| `ddhub_process_all()` | Consume available hubs' entry-pending messages, skipping busy hubs |

Callback types:

```c
typedef void (*ddhub_callback_t)(uint32_t event, void * const data, size_t len,
                                void * const arg1, void * const arg2, void * const arg3);

typedef void (*ddhub_process_done_t)(ddhub_id_t hub_id, uint32_t event,
                                    void * const data, size_t len, void * const arg);
```

Pointees remain mutable. Every consumer of a message shares the original `data` pointer, so later callbacks
can observe earlier modifications. The three subscriber contexts are supplied at registration;
`done_arg` is provided per message. Applications own all context lifetimes.

`void * const` describes a local pointer parameter, not a read-only payload. Pointees remain mutable,
and top-level const in the callback typedef does not enforce the callback definition's local qualifiers.
Pointers are passed by value; reassigning a local parameter does not change the caller's pointer variable.

## Processing scope and concurrency

`ddhub_process(id)` consumes only the specified hub's messages queued when the batch begins.
`ddhub_process_all()` selects available hubs and captures their queued message counts before invoking any callbacks.
Busy hubs are skipped; selected hubs are processed in enumeration order.
A skipped hub is not revisited by that ALL call even if it becomes idle, but another caller may process it independently.
Messages published after the snapshot wait for the next batch, including messages sent by callbacks.
Ordering is FIFO within each hub; there is no global publication order across hubs.

Only one ALL call may execute at a time. A second ALL call immediately returns busy without consuming messages.
Skipping any hub, or encountering a busy hub during processing, makes the final result busy,
but other selected hubs continue processing. Thus busy can accompany partial consumption of several hubs.
Use per-message completion notifications to determine buffer reuse, then retry processing remaining messages later.

For example, thread 1 is processing A while thread 2 calls ALL. ALL skips A, processes available B/C,
and returns busy. Once A finishes, another call can process A even before ALL returns.
ALL cleanup releases only the processing rights it owns.

Subscription changes before processing affect that batch's receivers. For example, after a successful send,
unregistering subscriber A and registering B means B receives matching events when processing later starts.
A hub's subscriptions and completion callback remain stable throughout its batch.
ALL freezes only its selected hubs until the whole ALL call returns.

| API | Across different hubs | Same-hub or shared-node restrictions |
| --- | --- | --- |
| register | Distinct nodes can register independently | Same-hub contention is busy; one node cannot belong to two hubs |
| unregister | Distinct nodes can be removed from different hubs | The target hub must be available; callers keep nodes valid through concurrent operations |
| send | Each hub has an independent FIFO | An unavailable hub returns busy; a full FIFO rejects the message; the platform adapter may wait |
| set_process_done | Idle hubs may be configured independently | Queue must be empty and hub idle; ALL freezes selected hubs only |
| process | Different hubs and a skipped hub can process alongside ALL | One processor per hub; ALL-selected hubs reject another processor |
| process_all | Skip hubs busy with processing or another operation; consume the others | Only one ALL call; skipped/conflicting hubs cause busy without stopping the others |
| set_lock_ops | Bind once during serialized startup | No concurrent first binding or runtime replacement |

Callbacks and completion notifications execute outside the platform and object locks.
They may send to the same hub and operate on other idle hubs.
Registering, unregistering, changing completion, or recursively processing the active hub returns busy.
A single-hub callback may call ALL: ALL skips that hub and processes other available hubs, returning busy.
Calling ALL from an ALL callback is busy. Processing a hub selected by ALL is also busy;
an omitted hub may be processed once idle.

An unavailable hub returns `DDHUB_BUSY`, which can be retried later.
Whether an API call can wait depends on the platform adapter; nonblocking use needs a nonblocking adapter.

The same callback function may execute concurrently for different hubs. Applications synchronize shared mutable
data/contexts and preserve their lifetimes; different IDs alone do not make business data independent.

If processing returns busy partway through,
completed messages are neither rolled back nor replayed, and unconsumed messages remain intact.
The next processing call starts with the remaining queue. Completion notifications identify reusable payloads.

## Payload lifetime and completion

1. A failed send accepts nothing, invokes no completion callback, and leaves ownership with the producer.
2. After a successful send, keep `data` and `done_arg` valid and stop producer-side modification until completion.
3. Matching subscribers run in order and may modify the shared payload and their own pointed-to contexts.
4. After every matching subscriber returns, invoke the hub's done function once with the original event,
   payload, length, and per-message producer context.
5. A message with no matching subscribers still completes. On entry to done, ddhub no longer accesses its
   payload or producer context, so they may be released or reused. Done may send that completed buffer again.

A hub has one done function; `done_arg` can distinguish producers. Done executes in the processing caller's context.
The hub remains processing during done, so same-hub processing, registration, unregistration, and done replacement
return busy. Replace or clear done only when the FIFO is empty and the hub is idle.
ALL-selected hubs retain their completion function until ALL returns.

Done may be NULL, in which case the application guarantees lifetime itself. A temporary stack buffer usually
cannot remain valid for an asynchronous publication. Subscription nodes and their three contexts must last
through successful unregistration and all concurrent references; shared contexts used by other hubs may require
an even longer lifetime.

## Status codes and argument boundaries

| Status | Value | Meaning |
| --- | --- | --- |
| `DDHUB_OK` | 0 | Success, including processing an empty FIFO |
| `DDHUB_INVALID` | -1 | Invalid ID, zero event, NULL node/callback, or NULL data with positive length |
| `DDHUB_BUSY` | -2 | Target hub is busy, processing overlaps, or an active hub cannot be modified |
| `DDHUB_QUEUE_FULL` | -3 | Destination FIFO is full; message was not accepted |
| `DDHUB_ALREADY_REGISTERED` | -4 | Node already belongs to a hub |
| `DDHUB_NOT_REGISTERED` | -5 | Node is unregistered and cannot be unlinked |
| `DDHUB_NOT_READY` | -6 | Platform lock/unlock callbacks have not been bound |

Only actual IDs in `[0, DDHUB_ID_COUNT)` are valid. `data == NULL && len > 0` is invalid.
Zero length permits NULL data for a pure event. Subscriber contexts, `done_arg`, and done may be NULL;
the subscription callback must not be NULL.

## Single-thread main-loop example

The nodes, payload, and contexts below have static storage. The ready flag is valid for this single-thread loop only;
if production and consumption run in different threads, synchronize it according to the application's concurrency model.

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

For immediate dispatch, call `ddhub_process(DDHUB_DEFAULT)` after successful send.
That call also consumes any earlier queued messages in the entry snapshot.
On busy, an accepted message may still await another processor; the processing status alone does not permit early reuse.
