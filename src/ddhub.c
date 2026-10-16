/**
 * @file ddhub.c
 * @brief Implement static message queues and event-based data dispatch.
 * @details ddhub - Data Dispatch Hub, an event-driven data distribution center.
 *          Manage intrusive subscriptions, stable priorities, per-hub FIFOs, completion callbacks, and injected locks.
 * @version 0.2.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-14： Implement static dispatch, nonblocking reservations, and consumption notifications.
 * - Codex 2026-10-15： Document file responsibilities, development dates, and modification history.
 * - Codex 2026-10-15： Inject platform locks to protect shared state without atomic API dependencies.
 * - Codex 2026-10-15： Isolate hub metadata locks and protect node ownership and ALL snapshot admission.
 * - Codex 2026-10-15： Group FIFO state and let ALL processing skip busy hubs while dispatching others.
 * - Codex 2026-10-15： Unify hub lock and processing helpers with caller-managed platform exclusion.
 * - Codex 2026-10-15： Merge fixed-size queue and state transactions to reduce platform lock calls.
 * - Codex 2026-10-15： Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Implement dispatch modes, drain notification, message boundaries, and safe removal.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#include "ddhub.h"
#include DDHUB_CONFIG_HEADER

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if DDHUB_ASYNC_PROCESS_ENABLE
#ifndef DDHUB_FIFO_DEPTH
#define DDHUB_FIFO_DEPTH                                    8U
#elif DDHUB_FIFO_DEPTH == 0
#error "DDHUB_FIFO_DEPTH must be positive"
#endif
#endif

/** @brief Borrowed payload descriptor shared by direct and queued dispatch. */
struct ddhub_message {
    uint32_t event;
    void    *data;
    size_t   len;
    void    *done_arg;
};

/** @brief Selected callback fields copied while its node is protected. */
struct ddhub_delivery {
    ddhub_callback_t callback;
    uint32_t         event;
    void            *arg1;
    void            *arg2;
    void            *arg3;
};

/** @brief Links and cursors use platform exclusion; writer only serializes list writers. */
struct ddhub_object {
    ddhub_subscription_t *subscriptions;
    ddhub_subscription_t *active;
    ddhub_process_done_t  done;
    uint64_t              generation;
    uint64_t              cutoff;
    bool                  writer;
    bool                  processing;
#if DDHUB_ASYNC_PROCESS_ENABLE
    struct {
        size_t               head;
        size_t               count;
        size_t               pending;
        struct ddhub_message msg[DDHUB_FIFO_DEPTH];
    } fifo;
#endif
};

static struct ddhub_object hubs[DDHUB_ID_COUNT];
static ddhub_lock_t        platform_lock;
static ddhub_unlock_t      platform_unlock;
#if DDHUB_ASYNC_PROCESS_ENABLE
static ddhub_process_notify_t process_notify;
static bool                   notify_bound;
static bool                   process_active;
#endif

/**
 * @brief Validate an actual hub, including negative enum values.
 * @param hub_id Candidate ID.
 * @return true for an actual hub.
 */
static bool valid_hub(ddhub_id_t hub_id)
{
    return (unsigned int)hub_id < (unsigned int)DDHUB_ID_COUNT;
}

/**
 * @brief Allocate a registration sequence without losing an active message's eligibility boundary.
 * @param hub Target hub; caller holds platform exclusion and its writer reservation.
 * @return Nonzero generation for the newly published node.
 */
static uint64_t next_generation(struct ddhub_object *hub)
{
    if (hub->generation == UINT64_MAX) {
        /* Preserve the eligible/ineligible partition instead of wrapping numeric order. */
        for (ddhub_subscription_t *node = hub->subscriptions; node != NULL; node = node->next) {
            node->generation = (node->generation <= hub->cutoff) ? UINT64_C(1) : UINT64_C(2);
        }
        hub->cutoff = UINT64_C(1);
        hub->generation = UINT64_C(2);
    }
    return ++hub->generation;
}

/**
 * @brief Select and protect the next eligible matching node inside platform exclusion.
 * @param hub Target hub with this message's captured cutoff.
 * @param node First candidate; no unprotected successor survives this call.
 * @param event Published mask.
 * @param delivery Receives immutable callback fields if a node is selected.
 * @return Selected protected node, or NULL at the end of this message's recipients.
 */
static ddhub_subscription_t *select_subscription(struct ddhub_object *hub, ddhub_subscription_t *node, uint32_t event,
                                                 struct ddhub_delivery *delivery)
{
    while (node != NULL) {
        uint32_t matched = event & node->event;
        if ((node->generation <= hub->cutoff) && (matched != 0U)) {
            delivery->callback = node->callback;
            delivery->event = matched;
            delivery->arg1 = node->arg1;
            delivery->arg2 = node->arg2;
            delivery->arg3 = node->arg3;
            break;
        }
        node = node->next;
    }
    hub->active = node;
    return node;
}

/**
 * @brief Dispatch one message while the caller owns the target hub's processing rights.
 * @param hub_id Actual target hub.
 * @param message Borrowed descriptor, no longer accessed after its completion callback.
 */
static void dispatch_message(ddhub_id_t hub_id, const struct ddhub_message *message)
{
    struct ddhub_object  *hub = &hubs[hub_id];
    struct ddhub_delivery delivery;
    ddhub_lock_key_t      key = platform_lock();
    hub->cutoff = hub->generation;
    ddhub_process_done_t  done = hub->done;
    ddhub_subscription_t *node = select_subscription(hub,
                                                     hub->subscriptions,
                                                     message->event,
                                                     &delivery);
    platform_unlock(key);
    while (node != NULL) {
        delivery.callback(delivery.event,
                          message->data,
                          message->len,
                          delivery.arg1,
                          delivery.arg2,
                          delivery.arg3);
        key = platform_lock();
        /* The old active node stays pinned until this final next-link read completes. */
        node = select_subscription(hub,
                                   node->next,
                                   message->event,
                                   &delivery);
        platform_unlock(key);
    }
    if (done != NULL) {
        done(hub_id,
             message->event,
             message->data,
             message->len,
             message->done_arg);
    }
}

/**
 * @brief Bind permanent platform exclusion before component use.
 * @param lock Exclusion-entry callback.
 * @param unlock Matching exclusion-exit callback.
 * @return Binding status.
 */
ddhub_status_t ddhub_set_lock_ops(ddhub_lock_t lock, ddhub_unlock_t unlock)
{
    if ((lock == NULL) || (unlock == NULL)) {
        return DDHUB_INVALID;
    }
    if (platform_lock != NULL) {
        return DDHUB_BUSY;
    }
    platform_unlock = unlock;
    platform_lock = lock;
    return DDHUB_OK;
}

#if DDHUB_ASYNC_PROCESS_ENABLE
/**
 * @brief Release a list writer and snapshot any optional scheduling hook under platform exclusion.
 * @param hub Hub whose list writer is owned by this caller.
 * @return Optional async notification, or NULL without queued work.
 */
static ddhub_process_notify_t release_writer(struct ddhub_object *hub)
{
    hub->writer = false;
    return (hub->fifo.count != 0U) ? process_notify : NULL;
}
#else
/**
 * @brief Release a synchronous list writer inside platform exclusion.
 * @param hub Hub whose list writer is owned by this caller.
 */
static void release_writer(struct ddhub_object *hub)
{
    hub->writer = false;
}
#endif

/**
 * @brief Publish a sorted node, including during message dispatch.
 * @param hub_id Target hub.
 * @param subscription Caller-owned node.
 * @param event Nonzero subscribed mask.
 * @param priority Ascending priority.
 * @param callback Consumer callback.
 * @param arg1 First context.
 * @param arg2 Second context.
 * @param arg3 Third context.
 * @return Registration status; processing alone never rejects registration.
 */
ddhub_status_t ddhub_register(ddhub_id_t hub_id, ddhub_subscription_t *subscription, uint32_t event, uint8_t priority,
                              ddhub_callback_t callback, void *arg1, void *arg2, void *arg3)
{
    if ((!valid_hub(hub_id)) || (subscription == NULL) || (event == 0U) || (callback == NULL)) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    struct ddhub_object *hub = &hubs[hub_id];
    ddhub_lock_key_t     key = platform_lock();
    if (subscription->registered != 0U) {
        platform_unlock(key);
        return DDHUB_ALREADY_REGISTERED;
    }
    if (hub->writer) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    hub->writer = true;
    subscription->hub_id = hub_id;
    subscription->registered = 1U;
    platform_unlock(key);

    ddhub_subscription_t **link = &hub->subscriptions;
    while ((*link != NULL) && ((*link)->priority <= priority)) {
        link = &(*link)->next;
    }
    subscription->event = event;
    subscription->priority = priority;
    subscription->callback = callback;
    subscription->arg1 = arg1;
    subscription->arg2 = arg2;
    subscription->arg3 = arg3;
    subscription->next = *link;

    key = platform_lock();
    subscription->generation = next_generation(hub);
    *link = subscription;
#if DDHUB_ASYNC_PROCESS_ENABLE
    ddhub_process_notify_t notify = release_writer(hub);
    platform_unlock(key);
    if (notify != NULL) {
        notify();
    }
#else
    release_writer(hub);
    platform_unlock(key);
#endif
    return DDHUB_OK;
}

/**
 * @brief Unlink a node that is not still pinned by dispatch.
 * @param subscription Caller-owned registered node.
 * @return Unregistration status.
 */
ddhub_status_t ddhub_unregister(ddhub_subscription_t *subscription)
{
    if (subscription == NULL) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    ddhub_lock_key_t key = platform_lock();
    if (subscription->registered == 0U) {
        platform_unlock(key);
        return DDHUB_NOT_REGISTERED;
    }
    struct ddhub_object *hub = &hubs[subscription->hub_id];
    if (hub->writer || (hub->active == subscription)) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    hub->writer = true;
    platform_unlock(key);

    ddhub_subscription_t **link = &hub->subscriptions;
    while ((*link != NULL) && (*link != subscription)) {
        link = &(*link)->next;
    }
    key = platform_lock();
    ddhub_status_t status = DDHUB_OK;
    if (hub->active == subscription) {
        status = DDHUB_BUSY;
    } else if (*link == NULL) {
        status = DDHUB_NOT_REGISTERED;
    } else {
        *link = subscription->next;
        subscription->next = NULL;
        subscription->registered = 0U;
    }
#if DDHUB_ASYNC_PROCESS_ENABLE
    ddhub_process_notify_t notify = release_writer(hub);
    platform_unlock(key);
    if (notify != NULL) {
        notify();
    }
#else
    release_writer(hub);
    platform_unlock(key);
#endif
    /* Do not access the node after publishing successful unregistration. */
    return status;
}

/**
 * @brief Replace completion only while idle and without queued messages.
 * @param hub_id Target hub.
 * @param done Optional completion callback.
 * @return Configuration status.
 */
ddhub_status_t ddhub_set_process_done(ddhub_id_t hub_id, ddhub_process_done_t done)
{
    if (!valid_hub(hub_id)) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    struct ddhub_object *hub = &hubs[hub_id];
    ddhub_lock_key_t     key = platform_lock();
    bool                 busy = hub->processing;
#if DDHUB_ASYNC_PROCESS_ENABLE
    busy = busy || (hub->fifo.count != 0U);
#endif
    if (!busy) {
        hub->done = done;
    }
    platform_unlock(key);
    return busy ? DDHUB_BUSY : DDHUB_OK;
}

/**
 * @brief Accept one direct or queued message according to the build mode.
 * @param hub_id Target hub.
 * @param event Nonzero published mask.
 * @param data Borrowed payload.
 * @param len Payload size.
 * @param done_arg Producer completion context.
 * @return Send status; failure never accepts the message.
 */
ddhub_status_t ddhub_send(ddhub_id_t hub_id, uint32_t event, void *data, size_t len, void *done_arg)
{
    if ((!valid_hub(hub_id)) || (event == 0U) || ((data == NULL) && (len != 0U))) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    struct ddhub_object *hub = &hubs[hub_id];
    ddhub_lock_key_t     key = platform_lock();
#if DDHUB_ASYNC_PROCESS_ENABLE
    if (hub->fifo.count == DDHUB_FIFO_DEPTH) {
        platform_unlock(key);
        return DDHUB_QUEUE_FULL;
    }
    size_t tail = (hub->fifo.head + hub->fifo.count) % DDHUB_FIFO_DEPTH;
    hub->fifo.msg[tail] = (struct ddhub_message){
        event,
        data,
        len,
        done_arg
    };
    ++hub->fifo.count;
    ddhub_process_notify_t notify = process_notify;
    platform_unlock(key);
    if (notify != NULL) {
        notify();
    }
#else
    if (hub->processing) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    hub->processing = true;
    platform_unlock(key);
    const struct ddhub_message message = {
        event,
        data,
        len,
        done_arg
    };
    dispatch_message(hub_id, &message);
    key = platform_lock();
    hub->processing = false;
    platform_unlock(key);
#endif
    return DDHUB_OK;
}

#if DDHUB_ASYNC_PROCESS_ENABLE
/**
 * @brief Bind a permanent optional global scheduling hook after platform initialization.
 * @param notify Hook; NULL permanently selects polling.
 * @return Binding status.
 */
ddhub_status_t ddhub_set_process_notify(ddhub_process_notify_t notify)
{
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    ddhub_lock_key_t key = platform_lock();
    if (notify_bound) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    process_notify = notify;
    notify_bound = true;
    platform_unlock(key);
    return DDHUB_OK;
}

/**
 * @brief Drain all hub queues through finite rounds on the unique consumer.
 * @return OK at the final empty check, BUSY for reentry, or NOT_READY before initialization.
 */
ddhub_status_t ddhub_process(void)
{
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    ddhub_lock_key_t key = platform_lock();
    if (process_active) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    process_active = true;
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        hubs[i].processing = true;
    }
    for (;;) {
        bool pending = false;
        for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
            hubs[i].fifo.pending = hubs[i].fifo.count;
            pending = pending || (hubs[i].fifo.pending != 0U);
        }
        if (!pending) {
            for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
                hubs[i].processing = false;
            }
            process_active = false;
            platform_unlock(key);
            return DDHUB_OK;
        }
        platform_unlock(key);
        for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
            struct ddhub_object *hub = &hubs[i];
            while (hub->fifo.pending != 0U) {
                key = platform_lock();
                struct ddhub_message message = hub->fifo.msg[hub->fifo.head];
                hub->fifo.head = (hub->fifo.head + 1U) % DDHUB_FIFO_DEPTH;
                --hub->fifo.count;
                --hub->fifo.pending;
                platform_unlock(key);
                dispatch_message((ddhub_id_t)i, &message);
            }
        }
        key = platform_lock();
    }
}
#endif
