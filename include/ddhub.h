/**
 * @file ddhub.h
 * @brief Declare the portable Data Dispatch Hub API.
 * @details ddhub - Data Dispatch Hub, an event-driven data distribution center.
 *          Define static subscription nodes, event callbacks, completion notifications, and C/C++ platform lock APIs.
 * @version 0.2.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-14： Introduce the static event dispatch API.
 * - Codex 2026-10-15： Add platform lock injection, a separate ALL processing API, and C++ initialization support.
 * - Codex 2026-10-15： Document per-hub concurrency, node ownership contention, and callback reentry.
 * - Codex 2026-10-15： Document busy-hub skipping, partial consumption, and selected-hub exclusion.
 * - Codex 2026-10-15： Document descriptor transactions and list traversal and callbacks outside platform exclusion.
 * - Codex 2026-10-15： Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Add dispatch modes, global notification, draining, and live subscriptions.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#ifndef DDHUB_H_
#define DDHUB_H_

#include <stddef.h>
#include <stdint.h>

/** @brief Build-wide replacement for the application configuration header. */
#ifndef DDHUB_CONFIG_HEADER
#define DDHUB_CONFIG_HEADER                                 "ddhub_config.h"
#endif
#include DDHUB_CONFIG_HEADER

/** @brief Enable queued dispatch; zero selects direct synchronous send. */
#ifndef DDHUB_ASYNC_PROCESS_ENABLE
#define DDHUB_ASYNC_PROCESS_ENABLE                          1
#endif
#if (DDHUB_ASYNC_PROCESS_ENABLE != 0) && (DDHUB_ASYNC_PROCESS_ENABLE != 1)
#error "DDHUB_ASYNC_PROCESS_ENABLE must be 0 or 1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initial value for a caller-owned unregistered node. */
#ifdef __cplusplus
#define DDHUB_SUBSCRIPTION_INIT                             { }
#else
#define DDHUB_SUBSCRIPTION_INIT                             {0}
#endif

/** @brief Portable operation results; failures never accept a sent message. */
typedef enum {
    DDHUB_OK = 0,
    DDHUB_INVALID = -1,
    DDHUB_BUSY = -2,
    DDHUB_QUEUE_FULL = -3,
    DDHUB_ALREADY_REGISTERED = -4,
    DDHUB_NOT_REGISTERED = -5,
    DDHUB_NOT_READY = -6,
} ddhub_status_t;

/** @brief Per-acquisition platform token, such as saved interrupt state. */
typedef uintptr_t                                           ddhub_lock_key_t;

/**
 * @brief Acquire platform exclusion and acquire memory ordering successfully.
 * @return Token restored by the matching unlock call in the same context.
 *
 * Must not call ddhub. Supported thread/ISR contexts and possible blocking depend on
 * the adapter. Regions cover descriptors, link commits, and selecting the next eligible
 * subscription, never payload copying or user callbacks. Sorted writer scans run outside
 * exclusion; eligibility filtering and rare registration-sequence rebasing run inside it.
 */
typedef ddhub_lock_key_t (*ddhub_lock_t)(void);

/**
 * @brief Release platform exclusion with release memory ordering.
 * @param key Token returned by this acquisition's matching lock callback.
 *
 * Must not call ddhub or substitute a shared global token for the per-call key.
 */
typedef void (*ddhub_unlock_t)(ddhub_lock_key_t key);

/**
 * @brief Bind permanent platform lock callbacks before any concurrent use.
 * @param lock Non-NULL exclusion-entry callback.
 * @param unlock Non-NULL matching exclusion-exit callback.
 * @return DDHUB_OK, DDHUB_INVALID for NULL callbacks, or DDHUB_BUSY if already bound.
 */
ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock);

/**
 * @brief Consume one message's intersection with this subscription's event mask.
 * @param event Nonzero matched mask; all 32 bits are application-owned.
 * @param data Original mutable borrowed payload, shared by this message's consumers.
 * @param len Payload size in bytes; zero allows NULL data.
 * @param arg1 First subscriber context.
 * @param arg2 Second subscriber context.
 * @param arg3 Third subscriber context.
 *
 * Runs outside locks. Registration is allowed and takes effect from the next message.
 * Unregistering the active node returns BUSY; other nodes may be removed. Synchronous
 * callbacks inherit the sender's context and may overlap on different hubs. Asynchronous
 * callbacks run on the unique process caller. Synchronize shared application objects.
 */
typedef void (*ddhub_callback_t)(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                                 void *const arg3);

/**
 * @brief Complete one accepted message after all matching callbacks return.
 * @param hub_id Actual consuming hub ID.
 * @param event Original unfiltered event mask.
 * @param data Original payload; it may be released or reused here.
 * @param len Original payload size.
 * @param arg Per-message producer context supplied as done_arg.
 *
 * Runs once even without matching subscriptions, outside locks. No payload/context access
 * follows this callback. The hub keeps processing rights until this callback returns.
 */
typedef void (*ddhub_process_done_t)(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg);

/**
 * @brief Caller-owned intrusive node; all fields are private while registered or being registered.
 *
 * Initialize once before use. Do not copy or modify registered nodes. Keep the node and
 * contexts alive until successful unregistration and all other concurrent users return.
 * One node belongs to at most one hub; successful re-registration is a new subscription.
 */
typedef struct ddhub_subscription {
    struct ddhub_subscription *next;
    ddhub_id_t                 hub_id;
    uint64_t                   generation;
    uint32_t                   event;
    uint8_t                    priority;
    uint8_t                    registered;
    ddhub_callback_t           callback;
    void                      *arg1;
    void                      *arg2;
    void                      *arg3;
} ddhub_subscription_t;

/**
 * @brief Register in ascending priority order, preserving registration order for ties.
 * @param hub_id Actual target hub ID in [0, DDHUB_ID_COUNT).
 * @param subscription Initialized caller-owned node.
 * @param event Nonzero event mask.
 * @param priority Priority 0..255; smaller values execute first.
 * @param callback Non-NULL consumer callback.
 * @param arg1 First optional subscriber context.
 * @param arg2 Second optional subscriber context.
 * @param arg3 Third optional subscriber context.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, DDHUB_ALREADY_REGISTERED, or DDHUB_NOT_READY.
 *
 * Allowed during dispatch. The new subscription skips a message whose dispatch has already
 * begun and participates from the next message, including within the same process call.
 * Same-hub list-writer contention may return BUSY; it does not block FIFO operations.
 */
ddhub_status_t ddhub_register(ddhub_id_t hub_id, ddhub_subscription_t *subscription, uint32_t event, uint8_t priority,
                              ddhub_callback_t callback, void *arg1, void *arg2, void *arg3);

/**
 * @brief Remove an inactive node; dispatch will no longer access it after success.
 * @param subscription Initialized caller-owned subscription node.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, DDHUB_NOT_REGISTERED, or DDHUB_NOT_READY.
 *
 * BUSY protects the node still used by dispatch or a competing list writer. Successful
 * removal before selection also cancels this node's delivery of the current message.
 * Coordinate reuse with other concurrent callers that hold the same node pointer.
 */
ddhub_status_t ddhub_unregister(ddhub_subscription_t *subscription);

/**
 * @brief Configure completion only while the target hub is idle and its queue is empty.
 * @param hub_id Actual target hub ID.
 * @param done Completion callback; NULL disables it.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, or DDHUB_NOT_READY.
 */
ddhub_status_t ddhub_set_process_done(ddhub_id_t hub_id, ddhub_process_done_t done);

/**
 * @brief Send directly in synchronous builds or enqueue and notify in asynchronous builds.
 * @param hub_id Actual target hub ID.
 * @param event Nonzero application event mask.
 * @param data Borrowed mutable payload, kept valid and unchanged by its producer until completion.
 * @param len Payload size in bytes; positive size requires non-NULL data.
 * @param done_arg Optional producer context, kept valid until completion.
 * @return DDHUB_OK, DDHUB_INVALID, DDHUB_BUSY, DDHUB_QUEUE_FULL, or DDHUB_NOT_READY.
 *
 * Failure does not accept the payload or invoke completion. Synchronous success completes
 * this message before return; same-hub overlapping sends return BUSY. Synchronous callbacks
 * inherit the sender's context. Prefer asynchronous builds for ISR sends to shorten ISR
 * execution. Async success stores only a descriptor and invokes the global notify outside
 * locks; the notification adapter must support the sender's thread/ISR context.
 */
ddhub_status_t ddhub_send(ddhub_id_t hub_id, uint32_t event, void *data, size_t len, void *done_arg);

#if DDHUB_ASYNC_PROCESS_ENABLE
/**
 * @brief Arrange later processing after successful enqueue or a list writer releases pending work.
 *
 * One global optional hook, called without locks in the sending/writing context. It must
 * retain wakeups as needed by the application and must support the actual thread/ISR contexts.
 * It should schedule the unique consumer rather than depend on recursive process calls.
 */
typedef void (*ddhub_process_notify_t)(void);

/**
 * @brief Bind the optional notification once during serialized startup, after lock binding.
 * @param notify Global scheduling hook; NULL permanently selects polling-only use.
 * @return DDHUB_OK, DDHUB_BUSY if already bound, or DDHUB_NOT_READY before lock binding.
 *
 * Leaving this function uncalled also permits polling. A bound hook remains fixed.
 */
ddhub_status_t ddhub_set_process_notify(ddhub_process_notify_t notify);

/**
 * @brief Drain every hub by bounded rounds, including messages arriving during processing.
 * @return DDHUB_OK, DDHUB_BUSY for overlapping/reentrant calls, or DDHUB_NOT_READY.
 *
 * Exactly one consumer may be active. This function does not wait for data. OK means all
 * queues were empty at the final locked check that also releases processing rights; later
 * sends schedule subsequent work. Continuous sends may prevent return. Callbacks and done
 * run outside locks. No per-hub processing API or separate process_all API is provided.
 */
ddhub_status_t ddhub_process(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_H_ */
