/**
 * @file ddhub.h
 * @brief Declare the portable Data Dispatch Hub API.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Define static subscription nodes, event callbacks, completion notifications, and C/C++ platform lock APIs.
 * @version 0.1.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-14: Introduce the static event dispatch API.
 * - Codex 2026-10-15: Add platform lock injection, a separate ALL processing API, and C++ initialization support.
 * - Codex 2026-10-15: Document per-hub concurrency, node ownership contention, and callback reentry.
 * - Codex 2026-10-15: Document busy-hub skipping, partial consumption, and selected-hub exclusion.
 * - Codex 2026-10-15: Document descriptor transactions and list traversal and callbacks outside platform exclusion.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#ifndef DDHUB_H_
#define DDHUB_H_

#include <stddef.h>
#include <stdint.h>

/** @brief Optional build-wide replacement for the user configuration header. */
#ifndef DDHUB_CONFIG_HEADER
#define DDHUB_CONFIG_HEADER                                 "ddhub_config.h"
#endif
#include DDHUB_CONFIG_HEADER

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initial value for an unregistered node; static zero initialization is equivalent. */
#ifdef __cplusplus
#define DDHUB_SUBSCRIPTION_INIT                             { }
#else
#define DDHUB_SUBSCRIPTION_INIT                             {0}
#endif

/** @brief Operation results, independent of errno and the operating system. */
typedef enum {
    DDHUB_OK = 0,
    DDHUB_INVALID = -1,
    DDHUB_BUSY = -2,
    DDHUB_QUEUE_FULL = -3,
    DDHUB_ALREADY_REGISTERED = -4,
    DDHUB_NOT_REGISTERED = -5,
    DDHUB_NOT_READY = -6,
} ddhub_status_t;

/** @brief Opaque per-acquisition token, such as an interrupt-state key. */
typedef uintptr_t                                           ddhub_lock_key_t;

/**
 * @brief Enter a short exclusion region for shared state and fixed-size message descriptors.
 * @return Token for this acquisition, passed unchanged to the matching unlock callback.
 *
 * Must acquire exclusion successfully and provide acquire/release memory ordering together
 * with unlock. It must not call ddhub. Blocking behavior and supported thread/ISR contexts
 * depend on the adapter; nonblocking use requires a nonblocking adapter.
 * Regions cover state checks and FIFO descriptor updates, never payload copying, subscription
 * traversal, or user callbacks. ALL entry/exit regions visit the configured number of hubs.
 */
typedef ddhub_lock_key_t (*ddhub_lock_t)(void);

/**
 * @brief Leave the platform's exclusion region and restore its previous state.
 * @param key Token returned by the matching lock call in the same execution context.
 *
 * Must provide release ordering and must not call ddhub. No shared global token may replace
 * this per-call value, because distinct callers may use the adapter concurrently.
 */
typedef void (*ddhub_unlock_t)(ddhub_lock_key_t key);

/**
 * @brief Bind the platform lock callbacks once, before concurrent use of any ddhub API.
 * @param lock Non-NULL exclusion-entry function.
 * @param unlock Non-NULL matching exclusion-exit function.
 * @return DDHUB_OK, DDHUB_INVALID for a NULL callback, or DDHUB_BUSY if already bound.
 *
 * Call from serialized startup code before any other ddhub operation or worker starts.
 * Callbacks remain fixed for the lifetime of the component. Unbound operations return
 * DDHUB_NOT_READY. Binding does not reset objects, nodes, or queues.
 */
ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);

/**
 * @brief Consume the intersection of a message's events and a subscription's events.
 * @param event Nonzero event intersection; all 32 bits are available to the application.
 * @param data Original mutable payload pointer, shared by all consumers of this message.
 * @param len Payload size in bytes; zero permits a NULL data pointer.
 * @param arg1 First mutable subscriber context supplied during registration.
 * @param arg2 Second mutable subscriber context supplied during registration.
 * @param arg3 Third mutable subscriber context supplied during registration.
 *
 * No internal lock is held. Use const pointer parameters in the callback definition too
 * if local pointer reassignment must be rejected by the compiler. Pointees remain mutable.
 * The same function may run concurrently for different hubs; synchronize shared pointees
 * in application code. Publishing from a callback is allowed. Subscription changes to
 * the currently processing hub and processing it recursively return DDHUB_BUSY.
 */
typedef void (*ddhub_callback_t)(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                                 void *const arg3);

/**
 * @brief Notify the producer that one accepted message has been fully consumed.
 * @param hub_id Actual hub that consumed the message, in [0, DDHUB_ID_COUNT).
 * @param event Original published event mask, without subscription filtering.
 * @param data Original payload pointer; it may be released or reused in this callback.
 * @param len Original payload size in bytes.
 * @param arg Per-message producer context supplied as done_arg to ddhub_send().
 *
 * Called once per consumed message, including messages with no matching subscriptions.
 * No internal lock is held. The hub remains processing until its current batch finishes.
 */
typedef void (*ddhub_process_done_t)(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg);

/**
 * @brief Caller-owned intrusive subscription; fields are private to ddhub after initialization.
 *
 * Keep the node and its contexts valid until successful unregistration and until every
 * concurrent operation referring to this node has returned. Coordinate reuse with other
 * callers. Do not copy, reinitialize, or modify a registered node. Concurrent registration
 * attempts cannot place one node in multiple hubs; one node belongs to at most one hub.
 */
typedef struct ddhub_subscription {
    struct ddhub_subscription *next;
    ddhub_id_t                 hub_id;
    uint32_t                   event;
    uint8_t                    priority;
    uint8_t                    registered;
    ddhub_callback_t           callback;
    void                      *arg1;
    void                      *arg2;
    void                      *arg3;
} ddhub_subscription_t;

/**
 * @brief Register a node in ascending priority order, preserving registration order for ties.
 * @param hub_id Actual target hub ID, in [0, DDHUB_ID_COUNT).
 * @param subscription Initialized, unregistered caller-owned node.
 * @param event Nonzero subscription event mask.
 * @param priority Priority in the range 0..255; smaller values execute first.
 * @param callback Non-NULL consumer function.
 * @param arg1 First caller-owned context; NULL is permitted.
 * @param arg2 Second caller-owned context; NULL is permitted.
 * @param arg3 Third caller-owned context; NULL is permitted.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, DDHUB_ALREADY_REGISTERED, or DDHUB_NOT_READY.
 *
 * Different hubs with distinct nodes can register concurrently. Same-hub metadata
 * contention returns DDHUB_BUSY. Processing freezes only its selected hubs' subscriptions;
 * ddhub_process_all() skips busy hubs. Node ownership is claimed under the platform lock,
 * so competing registrations of the same node cannot both succeed while it remains registered.
 */
ddhub_status_t ddhub_register(ddhub_id_t hub_id, ddhub_subscription_t *subscription, uint32_t event, uint8_t priority,
                              ddhub_callback_t callback, void *arg1, void *arg2, void *arg3);

/**
 * @brief Unlink a subscription and mark it unregistered so the caller may reuse it.
 * @param subscription Initialized caller-owned node.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, DDHUB_NOT_REGISTERED, or DDHUB_NOT_READY.
 *
 * Resolves and reserves the current owner under the platform lock. Different owners can
 * unlink distinct nodes concurrently. The node may be claimed by another caller once it
 * has been unlinked; the caller must coordinate its lifetime and subsequent reuse.
 */
ddhub_status_t ddhub_unregister(ddhub_subscription_t *subscription);

/**
 * @brief Set a hub's completion function only while its FIFO is empty and it is not processing.
 * @param hub_id Actual target hub ID, in [0, DDHUB_ID_COUNT).
 * @param done Per-hub completion function; NULL disables completion notification.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, or DDHUB_NOT_READY.
 *
 * Affects only the selected hub; metadata work on another hub does not cause bool-lock
 * contention here. An active ddhub_process_all() freezes only its selected hubs' functions.
 */
ddhub_status_t ddhub_set_process_done(ddhub_id_t hub_id, ddhub_process_done_t done);

/**
 * @brief Enqueue a borrowed payload without executing callbacks or allocating memory.
 * @param hub_id Actual target hub ID, in [0, DDHUB_ID_COUNT).
 * @param event Nonzero published event mask.
 * @param data Mutable payload, kept valid and not changed by the producer until completion.
 * @param len Payload size in bytes; a positive size requires non-NULL data.
 * @param done_arg Producer context retained until completion; NULL is permitted.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, DDHUB_QUEUE_FULL, or DDHUB_NOT_READY.
 *
 * Failure leaves ownership with the producer and never invokes the completion function.
 * Queue capacity excludes the message currently executing callbacks. Lock contention
 * on the internal bool lock returns immediately; the adapter itself may wait. Shared
 * payload/context access across hubs is the caller's responsibility.
 * Each hub has its own metadata lock. The entire enqueue transaction uses one short
 * platform exclusion region. Another hub's list operation does not cause DDHUB_BUSY;
 * an unfinished list operation on this hub can. The platform adapter itself may wait.
 */
ddhub_status_t ddhub_send(ddhub_id_t hub_id, uint32_t event, void *data, size_t len, void *done_arg);

/**
 * @brief Process only messages pending at entry for one specified hub.
 * @param hub_id Actual hub ID, in [0, DDHUB_ID_COUNT).
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, or DDHUB_NOT_READY; BUSY may follow partial consumption.
 *
 * Different hubs may process concurrently; the same hub cannot. This call freezes only
 * that hub's list. An ALL call skips hubs already processing; conversely, this call may
 * process a hub omitted by an active ALL call once it becomes idle. Callbacks and completion
 * notifications execute in the caller's context without internal or platform locks.
 * Reentrant calls follow the same exclusion rules. Metadata work on other hubs does not
 * interfere with this hub's FIFO snapshot or consumption.
 *
 * New messages wait for the next call. On a mid-batch lock conflict, remaining messages
 * stay queued; already completed messages are not replayed. Completion notifications,
 * not this function's status, determine when individual payloads can be reused.
 */
ddhub_status_t ddhub_process(ddhub_id_t hub_id);

/**
 * @brief Process entry-pending messages for available hubs in enumeration order, skipping busy hubs.
 * @return DDHUB_OK, DDHUB_BUSY, or DDHUB_NOT_READY; BUSY may follow partial consumption.
 *
 * Only one ALL call may run at a time. At entry, reserve hubs that have neither an active
 * processor nor a busy metadata lock. Freeze only those hubs' lists and completion functions
 * until this call returns, and capture all their pending counts before any callbacks start.
 * Skipped hubs remain available to other calls once their current operation finishes;
 * this ALL call never revisits them. New messages wait for the next processing call.
 * Any skipped hub or mid-batch queue contention makes the result DDHUB_BUSY, but processing
 * continues for other selected hubs. A second ALL call returns DDHUB_BUSY without consuming.
 * Callbacks and notifications run without internal or platform locks. Remaining messages
 * stay queued; completion notifications determine payload reuse, even on DDHUB_BUSY.
 */
ddhub_status_t ddhub_process_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_H_ */
