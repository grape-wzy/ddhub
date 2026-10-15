# ddhub user guide

English | [Simplified Chinese](usage.zh-CN.md)

Version: **0.1.0**. Release notes: **Initial version; first release**.

**ddhub** stands for **Data Dispatch Hub**: Data is the payload, Dispatch is routing to interested subscribers,
and Hub is the central coordination point. Together they describe a data distribution center.

ddhub is an independent C component. Hub objects, FIFO descriptors, and processing state use static storage;
applications provide intrusive subscription nodes. It allocates no heap memory and depends on neither Zephyr,
an external list library, nor errno. Hub objects require no resource initialization, but platform lock callbacks
must be bound before the first operation.

## Files and configuration

When embedded in the firmware project, the component resides in `components/ddhub/`:

- `include/ddhub.h`: nodes, callbacks, platform lock types, status codes, and seven public operations.
- `include/ddhub_config.h`: application-owned hub IDs and FIFO capacity.
- `src/ddhub.c`: intrusive subscriptions, static FIFOs, nonblocking reservations, and dispatch.
- `docs/`: this guide in English and Simplified Chinese.
- `tests/`: standalone behavior and concurrency tests, also included by the parent host-test project.
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

The public header needs only `<stddef.h>`, `<stdint.h>`, and the configuration header; the implementation
also uses `<stdbool.h>`. The core requires C99 and calls no standard atomic APIs, compiler atomic builtins,
or operating-system functions. Injected platform lock/unlock callbacks provide exclusion and memory visibility
for ordinary bool flags and counters. Component and host-test C99 requirements do not change the language standard
of other application sources.

Public and test headers provide `__cplusplus` linkage guards. C++ applications may include the public API
and link the implementation compiled as C. Static subscription nodes are zero-initialized by C and C++.

All source files and headers use English Doxygen documentation, including `@file`, `@brief`, `@details`,
`@version 0.1.0`, initial-release notes, development dates, and modification history.
Existing files retain their 2026-10-14 development date; new platform adapters and examples use 2026-10-15.

## Platform lock adapters

Bind the adapter once during serialized startup, before any other ddhub operation or concurrent caller starts:

```c
typedef uintptr_t ddhub_lock_key_t;
typedef ddhub_lock_key_t (*ddhub_lock_t)(void);
typedef void (*ddhub_unlock_t)(ddhub_lock_key_t key);

ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);
```

The lock callback must successfully enter real mutual exclusion and return a token for that acquisition.
Unlock receives that token unchanged and leaves the region. Keep the token in a per-call local variable;
it may represent a saved interrupt mask and must not be replaced by a shared global token.
Adapters that require no restoration state, such as a Linux mutex adapter, may return zero.

Both callbacks must provide exclusion and correct memory ordering. Empty functions and ordinary bool assignments
do not meet this contract. Neither callback may reenter ddhub.
Regions protect shared-state checks and fixed-size descriptor updates; they never copy payload bytes,
traverse subscription lists, or execute user callbacks. ALL admission and cleanup visit the configured number
of hubs, so consider that bound when evaluating interrupt latency on bare metal.

The adapter must cover every supported execution context. A single-core MCU can use short critical sections;
multiple cores and concurrent threads require an appropriate system synchronization mechanism.
Interrupt or nonblocking use requires an adapter with matching properties. A blocking Linux mutex adapter
cannot serve an MCU interrupt context.

A NULL callback returns `DDHUB_INVALID`. Before binding, otherwise valid registration, unregistration,
completion configuration, send, and processing calls return `DDHUB_NOT_READY`.
Binding is allowed only once from serialized startup; repeated binding returns `DDHUB_BUSY`.
It does not reset hubs, nodes, or queues and cannot switch synchronization domains at runtime.

Example Zephyr/STM32 adapter:

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

The example terminates the process if a mutex operation fails. Product code may use another fatal-error policy,
but must not continue with unprotected shared-state access. pthread and its error handling belong to the adapter,
not the ddhub core.

## Subscriptions and events

Declare a subscription node with sufficient lifetime:

```c
static ddhub_subscription_t subscription;
```

Static zero initialization leaves it unregistered. A node may belong to only one hub at a time;
different nodes may use the same callback function. ddhub owns its fields while registered.
Do not modify, copy, or reinitialize a registered node.
Successful unregistration clears the list link and registration flag so the node can be reused;
the next registration overwrites the remaining private fields.
To change events, priority, callback, or context pointers, unregister first and then register again.

Distinct nodes belonging to different hubs can be registered or unregistered concurrently.
A's list operation does not cause B to return busy merely because A's object reservation is held.
Conflicting metadata operations on the same hub return `DDHUB_BUSY`.

Node ownership is arbitrated under platform exclusion. Competing registrations cannot place the same node
in two lists simultaneously. Unregistration resolves and reserves the owner in the same critical section,
then unlinks the node before publishing its availability.
Another caller may register the node before the previous unregister call returns, so applications must coordinate
reuse and lifetime. Do not free, copy, or reinitialize it while concurrent operations still refer to it.

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
| `ddhub_unregister(node)` | Unlink a subscription and make it reusable |
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

`void * const` constrains local pointer parameters in the callback implementation, while allowing changes
to pointed-to structure members. Top-level const in a function-pointer type does not force the callback
definition to retain that qualifier. Repeat const in the callback definition to reject local pointer reassignment.
Pointers are passed by value, so reassigning a local parameter does not change the caller's pointer variable.

## Processing scope and concurrency

`ddhub_process(id)` reserves that hub's processing rights and metadata protection before capturing the batch quota.
`ddhub_process_all()` selects hubs that are neither processing nor holding a metadata reservation,
captures all selected queues' counts, and releases their metadata reservations within one entry critical section.
Busy hubs are skipped without changing their queues, quotas, or processing rights.
Callbacks start only after all selected snapshots are captured, in enumeration order.
A skipped hub is not revisited even if it becomes idle; another caller may process it independently.
Messages enqueued to selected hubs after the snapshot wait for the next batch, including publications
from consumer or completion callbacks. There is no global publication ordering across hubs.

Each object's `fifo` groups `head`, `count`, `pending`, and the static `msg` array.
`count` is the live queue size; `pending` is the batch quota, decremented for each dequeue.
Dequeue decreases both counters, while sending increases only `count`, so new messages cannot enlarge the current batch.
At exit, discard any unused batch quota; remaining messages stay queued and receive a fresh quota on the next call.

Only one ALL call may execute at a time. A second ALL call immediately returns busy without consuming messages.
Skipping any hub, or defensive detection of a queue reservation conflict, makes the final result busy,
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
| unregister | Nodes unlink independently according to their current owners | Resolve ownership and reserve the hub together; callers keep nodes valid |
| send | FIFOs are independent; platform exclusion serializes brief descriptor updates | An unfinished list operation can cause busy; a full FIFO rejects the message; the adapter may wait |
| set_process_done | Idle hubs may be configured independently | Queue must be empty and hub idle; ALL freezes selected hubs only |
| process | Different hubs and a skipped hub can process alongside ALL | One processor per hub; ALL-selected hubs reject another processor |
| process_all | Skip hubs processing or holding metadata reservations; consume the others | Only one ALL call; skipped/conflicting hubs cause busy without stopping the others |
| set_lock_ops | Bind once during serialized startup | No concurrent first binding or runtime replacement |

Callbacks and completion notifications execute outside the platform and object locks.
They may send to the same hub and operate on other idle hubs.
Registering, unregistering, changing completion, or recursively processing the active hub returns busy.
A single-hub callback may call ALL: ALL skips that hub and processes other available hubs, returning busy.
Calling ALL from an ALL callback is busy. Processing a hub selected by ALL is also busy;
an omitted hub may be processed once idle.

Internal helpers consistently manage object reservations and processing state:
`try_hub_lock()` / `release_hub_lock()`, `hub_is_processing()`, and
`try_hub_processing()` / `release_hub_processing()`.
Their callers must already hold platform exclusion; the helpers never reacquire it.
Processing admission reserves both the processing right and metadata protection in the same critical section.

Enqueue, dequeue, and completion configuration acquire the object reservation, perform their fixed-size update,
and release the reservation in one platform critical section.
Registration reserves the hub, checks processing state, and claims node ownership at entry,
then traverses and inserts into the list outside platform exclusion.
Unregistration unlinks outside the critical section, then combines publication of node availability
with releasing the object's reservation.
The per-hub bool reservation protects these longer list operations; different hubs' traversals and callbacks can overlap.
Platform exclusion also protects node ownership, processing flags, `process_by_all`, and the global ALL activity flag.
`process_by_all` identifies the hubs owned by that ALL call so cleanup does not clear another processor's state.
Brief FIFO updates and shared control state are serialized, but list traversal and callbacks are not.
Processing cleanup does not depend on other hubs' metadata activity.

Normal-path platform acquisition counts follow. One acquisition means one paired platform lock/unlock;
N is the number of messages dequeued, excluding additional calls made by user callbacks:

| Operation | Platform critical sections |
| --- | --- |
| send | 1 |
| One message dequeue | 1 |
| set_process_done | 1 |
| register / unregister | 2 |
| process / process_all | 2 + N: admission/snapshot, individual dequeues, and cleanup |

Helpers add no platform calls. Business callbacks and completion notifications run outside both locks;
`processing` retains batch ownership only. Descriptor transactions release the bool reservation before
leaving exclusion, so a producer paused after enqueue cannot obstruct an already admitted consumer batch.

Object reservation conflicts immediately return busy. The core never waits, sleeps, or spins;
whether the injected platform lock waits depends on the adapter. End-to-end nonblocking behavior requires
a nonblocking adapter. Handle retryable statuses without spinning indefinitely in a context that prevents
the lock owner from running.

The same callback function may execute concurrently for different hubs. Applications synchronize shared mutable
data/contexts and preserve their lifetimes; different IDs alone do not make business data independent.

Dequeue retains a defensive object-reservation check. If processing returns busy partway through,
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
| `DDHUB_BUSY` | -2 | Object reservation conflict, processing conflict, or modification of an active hub |
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

## Validation

The component's `tests/` uses a separate three-hub, three-slot configuration without changing firmware settings.
From the ddhub repository root:

```powershell
cmake -G Ninja -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

From the parent firmware project's root, its complete host suite remains available:

```powershell
cmake -G Ninja -S tests/unit -B build/unit-ninja
cmake --build build/unit-ninja
ctest --test-dir build/unit-ninja --output-on-failure
```

`ddhub_behavior_test` checks stable priorities, all 32 event bits, mutable pointees, FIFO fullness and wraparound,
ALL snapshots, nested ALL from a single-hub callback, subscription timing, callback publication, completion,
and payload reuse. Acquisition counters verify short transaction and batch lock costs.

`ddhub_concurrency_test` uses real threads and blocked callbacks to verify same-hub exclusion, parallel hubs,
busy-hub skipping, selected-hub exclusion, independent cleanup, and deferred messages after concurrent publication.
Four producers submit 2000 messages to check per-producer FIFO order, context routing, callback order,
and exactly-once completion.
The adapter verifies each acquisition token and uses thread-local state to ensure callbacks run outside exclusion.
Controlled pauses cover object reservation isolation, parallel registration, shared-node contention,
and node transfer during unregistration.
Stress completion counters belong to one processor; controlled tests use separate per-hub counters inspected
after the corresponding thread completes. Tests call no atomic APIs. pthread belongs only to host-test adapters.

The parent firmware uses its existing `local-debug` CMake preset. The component remains integrated through
the components CMake hierarchy; business code must register subscriptions and publish messages explicitly.
ddhub does not alter existing application data paths automatically.
