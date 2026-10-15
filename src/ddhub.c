/**
 * @file ddhub.c
 * @brief Implement static message queues and event-based data dispatch.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Manage intrusive subscriptions, stable priorities, per-hub FIFOs, completion callbacks, and injected locks.
 * @version 0.1.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-14: Implement static dispatch, nonblocking reservations, and consumption notifications.
 * - Codex 2026-10-15: Document file responsibilities, development dates, and modification history.
 * - Codex 2026-10-15: Inject platform locks to protect shared state without atomic API dependencies.
 * - Codex 2026-10-15: Isolate hub metadata locks and protect node ownership and ALL snapshot admission.
 * - Codex 2026-10-15: Group FIFO state and let ALL processing skip busy hubs while dispatching others.
 * - Codex 2026-10-15: Unify hub lock and processing helpers with caller-managed platform exclusion.
 * - Codex 2026-10-15: Merge fixed-size queue and state transactions to reduce platform lock calls.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#include "ddhub.h"
#include DDHUB_CONFIG_HEADER

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Number of waiting messages stored independently by each hub. */
#ifndef DDHUB_FIFO_DEPTH
#define DDHUB_FIFO_DEPTH                                    8U
#elif DDHUB_FIFO_DEPTH == 0
#error "DDHUB_FIFO_DEPTH must be positive > zero"
#endif

/** @brief One borrowed payload and its producer's per-message completion context. */
struct ddhub_message {
    uint32_t event;
    void    *data;
    size_t   len;
    void    *done_arg;
};

/** @brief FIFO/list fields use lock; pending belongs to the admitted processor. */
struct ddhub_object {
    ddhub_subscription_t *subscriptions;
    ddhub_process_done_t  done;
    struct {
        size_t               head;                  /**< Index of the oldest queued message. */
        size_t               count;                 /**< Current queue size, including new sends. */
        size_t               pending;               /**< Remaining quota owned by the current processor. */
        struct ddhub_message msg[DDHUB_FIFO_DEPTH]; /**< Static storage for borrowed message descriptors. */
    } fifo;
    bool lock;           /**< A caller owns this hub's FIFO/list metadata protection. */
    bool processing;     /**< A single-hub or ALL call owns this hub's processing rights. */
    bool process_by_all; /**< The active ALL call owns this hub; only that call changes this flag. */
};

static struct ddhub_object hubs[DDHUB_ID_COUNT];
static ddhub_lock_t        platform_lock;
static ddhub_unlock_t      platform_unlock;
/* Serialize ALL calls only; per-hub processing flags arbitrate their selected objects. */
static bool process_all_active;

/**
 * @brief Check whether an ID names an actual hub, including rejection of negative IDs.
 * @param hub_id Candidate ID.
 * @return true for an actual hub, false for an invalid ID.
 */
static inline bool valid_hub(ddhub_id_t hub_id)
{
    return (unsigned int)hub_id < (unsigned int)DDHUB_ID_COUNT;
}

/**
 * @brief Attempt to acquire one hub's metadata lock inside platform exclusion.
 * @param hub_id Valid target hub ID; caller must already hold the platform lock.
 * @return true if the caller now owns this hub's lock, otherwise false.
 */
static inline bool try_hub_lock(ddhub_id_t hub_id)
{
    if (hubs[hub_id].lock) {
        return false;
    }
    hubs[hub_id].lock = true;
    return true;
}

/**
 * @brief Release one hub's metadata lock inside platform exclusion.
 * @param hub_id Valid hub ID whose lock is owned by the caller; platform lock must be held.
 */
static inline void release_hub_lock(ddhub_id_t hub_id)
{
    hubs[hub_id].lock = false;
}

/**
 * @brief Read one hub's processing state inside platform exclusion.
 * @param hub_id Valid actual hub ID; caller must already hold the platform lock.
 * @return true while a single-hub or ALL call owns this hub's processing rights.
 */
static inline bool hub_is_processing(ddhub_id_t hub_id)
{
    return hubs[hub_id].processing;
}

/**
 * @brief Reserve both processing rights and the metadata lock inside platform exclusion.
 * @param hub_id Valid target hub ID; caller must already hold the platform lock.
 * @return true if both reservations succeed, otherwise false without changing either state.
 */
static inline bool try_hub_processing(ddhub_id_t hub_id)
{
    if (hub_is_processing(hub_id) || (!try_hub_lock(hub_id))) {
        return false;
    }
    hubs[hub_id].processing = true;
    return true;
}

/**
 * @brief Clear the caller's batch quota and release processing rights inside platform exclusion.
 * @param hub_id Valid hub ID owned by this processor; caller must hold the platform lock.
 *
 * Metadata protection is acquired and released separately; leave its state unchanged here.
 */
static inline void release_hub_processing(ddhub_id_t hub_id)
{
    hubs[hub_id].fifo.pending = 0U;
    hubs[hub_id].processing = false;
}

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

ddhub_status_t ddhub_register(ddhub_id_t hub_id, ddhub_subscription_t *subscription, uint32_t event, uint8_t priority,
                              ddhub_callback_t callback, void *arg1, void *arg2, void *arg3)
{
    if ((!valid_hub(hub_id)) || (subscription == NULL) || (event == 0U) || (callback == NULL)) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    ddhub_status_t   status = DDHUB_OK;
    ddhub_lock_key_t key = platform_lock();
    if (!try_hub_lock(hub_id)) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    if (subscription->registered != 0U) {
        status = DDHUB_ALREADY_REGISTERED;
    } else if (hub_is_processing(hub_id)) {
        status = DDHUB_BUSY;
    } else {
        /* Publish the owner while its metadata lock prevents access to a partial node. */
        subscription->hub_id = hub_id;
        subscription->registered = 1U;
    }
    if (status != DDHUB_OK) {
        release_hub_lock(hub_id);
        platform_unlock(key);
        return status;
    }
    platform_unlock(key);

    ddhub_subscription_t **link = &hubs[hub_id].subscriptions;
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
    *link = subscription;
    key = platform_lock();
    release_hub_lock(hub_id);
    platform_unlock(key);
    return status;
}

ddhub_status_t ddhub_unregister(ddhub_subscription_t *subscription)
{
    if (subscription == NULL) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }

    ddhub_status_t   status = DDHUB_OK;
    ddhub_id_t       hub_id = (ddhub_id_t)0;
    ddhub_lock_key_t key = platform_lock();
    if (subscription->registered == 0U) {
        status = DDHUB_NOT_REGISTERED;
    } else {
        hub_id = subscription->hub_id;
        if (hub_is_processing(hub_id) || (!try_hub_lock(hub_id))) {
            status = DDHUB_BUSY;
        }
    }
    platform_unlock(key);
    if (status != DDHUB_OK) {
        return status;
    }

    ddhub_subscription_t **link = &hubs[hub_id].subscriptions;
    while ((*link != NULL) && (*link != subscription)) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        status = DDHUB_NOT_REGISTERED;
    } else {
        *link = subscription->next;
        subscription->next = NULL;
    }
    key = platform_lock();
    if (status == DDHUB_OK) {
        subscription->registered = 0U;
    }
    release_hub_lock(hub_id);
    platform_unlock(key);
    /* The node can now move to another hub; do not access it after publication. */
    return status;
}

ddhub_status_t ddhub_set_process_done(ddhub_id_t hub_id, ddhub_process_done_t done)
{
    if (!valid_hub(hub_id)) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    ddhub_lock_key_t key = platform_lock();
    if (!try_hub_lock(hub_id)) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }

    ddhub_status_t status = DDHUB_OK;
    if (hub_is_processing(hub_id) || (hubs[hub_id].fifo.count != 0U)) {
        status = DDHUB_BUSY;
    } else {
        hubs[hub_id].done = done;
    }
    release_hub_lock(hub_id);
    platform_unlock(key);
    return status;
}

ddhub_status_t ddhub_send(ddhub_id_t hub_id, uint32_t event, void *data, size_t len, void *done_arg)
{
    if ((!valid_hub(hub_id)) || (event == 0U) || ((data == NULL) && (len != 0U))) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }
    ddhub_lock_key_t key = platform_lock();
    if (!try_hub_lock(hub_id)) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }

    struct ddhub_object *hub = &hubs[hub_id];
    ddhub_status_t       status = DDHUB_OK;
    if (hub->fifo.count == DDHUB_FIFO_DEPTH) {
        status = DDHUB_QUEUE_FULL;
    } else {
        size_t tail = (hub->fifo.head + hub->fifo.count) % DDHUB_FIFO_DEPTH;
        hub->fifo.msg[tail].event = event;
        hub->fifo.msg[tail].data = data;
        hub->fifo.msg[tail].len = len;
        hub->fifo.msg[tail].done_arg = done_arg;
        ++hub->fifo.count;
    }
    release_hub_lock(hub_id);
    platform_unlock(key);
    return status;
}

/**
 * @brief Consume a bounded FIFO prefix while the caller owns this hub's processing rights.
 * @param hub_id Valid actual hub ID with a frozen subscription list and done function.
 * @return DDHUB_OK, or DDHUB_BUSY if a queue access conflicts with another operation.
 */
static ddhub_status_t process_hub(ddhub_id_t hub_id)
{
    struct ddhub_object *hub = &hubs[hub_id];
    while (hub->fifo.pending != 0U) {
        ddhub_lock_key_t key = platform_lock();
        if (!try_hub_lock(hub_id)) {
            platform_unlock(key);
            return DDHUB_BUSY;
        }
        struct ddhub_message message = hub->fifo.msg[hub->fifo.head];
        hub->fifo.head = (hub->fifo.head + 1U) % DDHUB_FIFO_DEPTH;
        --hub->fifo.count;
        --hub->fifo.pending;
        release_hub_lock(hub_id);
        platform_unlock(key);

        for (ddhub_subscription_t *node = hub->subscriptions; node != NULL; node = node->next) {
            uint32_t matched = message.event & node->event;
            if (matched != 0U) {
                node->callback(matched,
                               message.data,
                               message.len,
                               node->arg1,
                               node->arg2,
                               node->arg3);
            }
        }
        if (hub->done != NULL) {
            hub->done(hub_id,
                      message.event,
                      message.data,
                      message.len,
                      message.done_arg);
        }
        /* No payload/context access is permitted after the completion callback. */
    }
    return DDHUB_OK;
}

ddhub_status_t ddhub_process(ddhub_id_t hub_id)
{
    if (!valid_hub(hub_id)) {
        return DDHUB_INVALID;
    }
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }

    struct ddhub_object *hub = &hubs[hub_id];
    ddhub_lock_key_t     key = platform_lock();
    if (!try_hub_processing(hub_id)) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }

    hub->fifo.pending = hub->fifo.count;
    release_hub_lock(hub_id);
    platform_unlock(key);
    ddhub_status_t status = process_hub(hub_id);

    /* Release processing rights after the final user callback has returned. */
    key = platform_lock();
    release_hub_processing(hub_id);
    platform_unlock(key);
    return status;
}

ddhub_status_t ddhub_process_all(void)
{
    if (platform_lock == NULL) {
        return DDHUB_NOT_READY;
    }

    ddhub_lock_key_t key = platform_lock();
    if (process_all_active) {
        platform_unlock(key);
        return DDHUB_BUSY;
    }
    process_all_active = true;
    ddhub_status_t status = DDHUB_OK;
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        struct ddhub_object *hub = &hubs[i];
        hub->process_by_all = try_hub_processing((ddhub_id_t)i);
        if (hub->process_by_all) {
            hub->fifo.pending = hub->fifo.count;
            release_hub_lock((ddhub_id_t)i);
        } else {
            status = DDHUB_BUSY;
        }
    }
    platform_unlock(key);

    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        if (hubs[i].process_by_all && (process_hub((ddhub_id_t)i) != DDHUB_OK)) {
            status = DDHUB_BUSY;
        }
    }

    /* Release only this ALL call's hubs, even if another processor finished meanwhile. */
    key = platform_lock();
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        if (hubs[i].process_by_all) {
            release_hub_processing((ddhub_id_t)i);
            hubs[i].process_by_all = false;
        }
    }
    process_all_active = false;
    platform_unlock(key);
    return status;
}
