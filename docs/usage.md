# ddhub usage

English | [简体中文](usage.zh-CN.md)

ddhub (Data Dispatch Hub) is a C99 component with static storage, caller-owned intrusive subscriptions, borrowed payloads, and C++ compatible public headers.

## Configuration and build

Define consecutive `ddhub_id_t` values starting at zero, followed by `DDHUB_ID_COUNT`, in the default configuration or a build-wide `DDHUB_CONFIG_HEADER`. All translation units must share configuration.

| Setting | Default | Meaning |
|---|---|---|
| `DDHUB_ASYNC_PROCESS_ENABLE` | `1` | `1`: enqueue and process globally; `0`: dispatch directly in send |
| `DDHUB_FIFO_DEPTH` | `8` | Positive waiting capacity per asynchronous hub |
| `DDHUB_CONFIG_HEADER` | `"ddhub_config.h"` | Application configuration header |

Synchronous builds contain no FIFO storage and neither declare nor export `ddhub_process()` or `ddhub_set_process_notify()`. The old per-ID process and separate process_all APIs are removed.

```sh
cmake -G Ninja -S . -B build/library
cmake --build build/library
cmake -G Ninja -S . -B build/library-sync -DDDHUB_ASYNC_PROCESS_ENABLE=0
cmake --build build/library-sync
cmake -G Ninja -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Use `add_subdirectory(path/to/ddhub)` and link target `ddhub`. Supply a PUBLIC configuration include directory and `DDHUB_CONFIG_HEADER="my_config.h"` definition as needed. An explicitly supplied CMake `DDHUB_ASYNC_PROCESS_ENABLE` setting propagates a PUBLIC definition; otherwise the configuration header controls the mode.

## Platform exclusion

Bind `ddhub_set_lock_ops(lock, unlock)` once during serialized startup, before other use. Lock must successfully acquire exclusion and return a per-acquisition `uintptr_t` token; unlock restores that token in the same context. The pair supplies acquire/release ordering and must not reenter ddhub. Unbound operations return NOT_READY; rebinding returns BUSY.

Thread, ISR, multicore support and possible blocking depend on the adapter. A single-core Cortex-M adapter can save/mask/restore interrupts; a desktop adapter can use a mutex. Consumer, completion and notification callbacks run outside locks.

Sorted writer scans run outside platform exclusion under a per-hub writer reservation. Link publication/removal, descriptors and cursor selection/advancement use platform exclusion. Eligibility filtering may scan multiple subscriptions, and sequence exhaustion performs rare rebasing under exclusion. Critical-section duration therefore depends on subscription count; it never includes payload copies or user callbacks.

## API

| Operation | Purpose |
|---|---|
| `ddhub_set_lock_ops(lock, unlock)` | Bind permanent platform exclusion |
| `ddhub_register(id, node, event, priority, callback, arg1, arg2, arg3)` | Register a caller-owned subscription |
| `ddhub_unregister(node)` | Unlink an inactive subscription safely |
| `ddhub_set_process_done(id, done)` | Configure completion on an idle, empty hub |
| `ddhub_send(id, event, data, len, done_arg)` | Direct dispatch or enqueue, according to the build |
| `ddhub_set_process_notify(notify)` | Async only: bind the global scheduling hook once |
| `ddhub_process()` | Async only: drain every hub on one consumer |

IDs are in `[0, DDHUB_ID_COUNT)`. Event masks are nonzero; all 32 bits are available. Positive length requires non-NULL data; zero permits NULL. Consumer callbacks are required; contexts and completion callbacks are optional.

## Direct synchronous send

With `DDHUB_ASYNC_PROCESS_ENABLE=0`, send dispatches directly and returns success only after this message's completion callback returns. No FIFO is used. Overlapping or recursive sends to the same hub return BUSY; different idle hubs may dispatch concurrently. A → B → A recursion fails on the second A. Completion cannot be replaced during processing.

Callbacks inherit the sender's context. **Prefer asynchronous mode for ISR sends to shorten interrupt execution.** Synchronous ISR use requires every consumer and completion callback to support that context.

## Async enqueue and drain

Async send stores only `event/data/len/done_arg`, not payload contents. Each hub has an independent FIFO; capacity excludes the currently dispatched message. QUEUE_FULL rejects the message. List writers do not block FIFO access.

Exactly one consumer calls `ddhub_process()`. It never waits for messages. Each round consumes finite current quotas from every hub, then checks for more work. Messages sent during callbacks or completion are also drained before return. Continuous producers may keep the call running indefinitely.

Overlapping/reentrant process calls return BUSY. OK means every queue was empty at the final locked check that also released processing rights. Later sends belong to subsequent work; queues need not remain empty until the caller physically observes the return. FIFO ordering is per hub, not global across hubs.

## One global notification

The optional hook has type `void (*)(void)`, with no hub ID. Every successful enqueue calls the same hook after unlocking. Releasing a list writer may also notify when queued work remains. The hook runs in the sender/writer's context, including supported ISRs.

Bind it once after platform locking and before concurrent use. Leaving it unbound or binding NULL selects polling; rebinding returns BUSY. Use it to schedule the consumer, rather than relying on recursive process inside the hook. The component supplies no wait, ready or thread-creation interface.

A bare-metal main loop can call process directly. An RTOS worker can wait on one retained binary semaphore, consume the signal before calling process, and then repeat. The Chinese guide includes a Zephyr example. Notifications arriving during processing may remain after queues drain and trigger a harmless empty call. Do not unconditionally reset the semaphore after process: that could erase a notification for a send after the final empty check.

## Live subscriptions

Initialize nodes with static zero initialization or `DDHUB_SUBSCRIPTION_INIT`. Registered fields belong to ddhub; do not copy, modify or reinitialize them. One node belongs to at most one hub. Unregister before changing its parameters.

Smaller priorities execute first; ties preserve registration order. Each subscription receives one callback for a nonzero intersection of the message and subscription masks. All consumers share the original mutable payload. Messages are not merged.

**Registration is allowed during dispatch. New subscriptions skip the message already being dispatched and participate from the next message, even within the same process call.** Synchronous sends use the same message boundary. Registration does not replay a message or invoke callbacks by itself. Re-registration receives a new sequence; rare exhaustion rebases sequences while preserving current eligibility.

Only the node still pinned by dispatch is protected from unregistration. Removing it returns BUSY. Other nodes can be removed, cancelling their current-message delivery if removal wins before selection. The pin lasts through the callback's final next-link read; after successful removal the dispatcher never accesses that node again.

Competing list writers on one hub may return BUSY; processing does not freeze the entire list. Writers on different hubs operate independently. Keep nodes and contexts alive until successful unregistration and until other concurrent pointer users finish.

## Completion and ownership

Failed send accepts nothing and never calls done. Successful send borrows payload and producer context until completion; the producer must keep them valid and stop modifying payload. After all matching callbacks, done runs once, including messages with no matching subscriptions, and receives the original hub, mask, payload, length and done_arg. No payload/context access follows done, so it may release or reuse them.

Sync done runs before send returns. Async done runs on the process caller and may enqueue the same hub again for the current drain call; sync same-hub send from done returns BUSY. Done has no active subscription pin, so nodes may be registered or unregistered, but processing rights still prevent completion replacement.

NULL done leaves lifetime management to the application. Temporary stack buffers usually cannot survive async consumption. Shared mutable business objects require application synchronization, including across hubs.

## Results and validation

| Result | Meaning |
|---|---|
| `DDHUB_OK` | Success; empty process is successful |
| `DDHUB_INVALID` | Invalid ID, event, node, callback or data/length combination |
| `DDHUB_BUSY` | Writer contention, pinned node, repeated binding, overlapping processing or same-hub sync send |
| `DDHUB_QUEUE_FULL` | Async waiting FIFO is full |
| `DDHUB_ALREADY_REGISTERED` | Node already registered or reserved by registration |
| `DDHUB_NOT_REGISTERED` | Node is unregistered |
| `DDHUB_NOT_READY` | Platform exclusion is not bound |

Host tests build both modes and cover parameters, ordering, masks, FIFO, lifetimes, live changes, sequence rebasing, controlled thread interleavings, final-check handoff, 2000 messages from multiple producers with binary wakeup, C++ interoperability, and synchronous API/symbol exclusion.
