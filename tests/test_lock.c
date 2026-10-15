/**
 * @file test_lock.c
 * @brief Implement the pthread platform lock used by ddhub host tests.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Validate lock tokens, callbacks outside exclusion, controlled contention, and platform acquisition counts.
 * @version 0.1.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-15: Add a host synchronization adapter without atomic API dependencies.
 * - Codex 2026-10-15: Add staged pauses to verify per-hub locks and ownership publication.
 * - Codex 2026-10-15: Expose acquisition counts to verify short transaction lock costs.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#include "test_lock.h"
#include "ddhub.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

static pthread_mutex_t  port_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t   keys_once = PTHREAD_ONCE_INIT;
static pthread_key_t    held_key;
static pthread_key_t    pause_key;
static char             marker;
static ddhub_lock_key_t last_key;
static ddhub_lock_key_t active_key;
static pthread_mutex_t  pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   pause_condition = PTHREAD_COND_INITIALIZER;
static bool             pause_entered;
static bool             pause_released;
static size_t           pause_count;

/** @brief Create per-thread lock-state and pause keys exactly once. */
static void create_keys(void)
{
    assert(pthread_key_create(&held_key, NULL) == 0);
    assert(pthread_key_create(&pause_key, NULL) == 0);
}

ddhub_lock_key_t ddhub_test_lock(void)
{
    assert(pthread_once(&keys_once, create_keys) == 0);
    assert(pthread_getspecific(held_key) == NULL);
    assert(pthread_mutex_lock(&port_mutex) == 0);
    active_key = ++last_key;
    assert(pthread_setspecific(held_key, &marker) == 0);
    return active_key;
}

void ddhub_test_unlock(ddhub_lock_key_t key)
{
    assert(pthread_getspecific(held_key) == &marker);
    assert(key == active_key);
    assert(pthread_setspecific(held_key, NULL) == 0);
    assert(pthread_mutex_unlock(&port_mutex) == 0);

    if (pthread_getspecific(pause_key) != NULL) {
        assert(pthread_mutex_lock(&pause_mutex) == 0);
        if (--pause_count == 0U) {
            assert(pthread_setspecific(pause_key, NULL) == 0);
            pause_entered = true;
            assert(pthread_cond_broadcast(&pause_condition) == 0);
            while (!pause_released) {
                assert(pthread_cond_wait(&pause_condition, &pause_mutex) == 0);
            }
            pause_entered = false;
        }
        assert(pthread_mutex_unlock(&pause_mutex) == 0);
    }
}

void ddhub_test_assert_unlocked(void)
{
    assert(pthread_once(&keys_once, create_keys) == 0);
    assert(pthread_getspecific(held_key) == NULL);
}

ddhub_lock_key_t ddhub_test_lock_count(void)
{
    ddhub_test_assert_unlocked();
    assert(pthread_mutex_lock(&port_mutex) == 0);
    ddhub_lock_key_t count = last_key;
    assert(pthread_mutex_unlock(&port_mutex) == 0);
    return count;
}

void ddhub_test_pause_next_unlock(void)
{
    ddhub_test_pause_after_unlocks(1U);
}

void ddhub_test_pause_after_unlocks(size_t count)
{
    assert(count != 0U);
    assert(pthread_once(&keys_once, create_keys) == 0);
    assert(pthread_mutex_lock(&pause_mutex) == 0);
    pause_entered = false;
    pause_released = false;
    pause_count = count;
    assert(pthread_mutex_unlock(&pause_mutex) == 0);
    assert(pthread_setspecific(pause_key, &marker) == 0);
}

void ddhub_test_wait_paused(void)
{
    assert(pthread_mutex_lock(&pause_mutex) == 0);
    while (!pause_entered) {
        assert(pthread_cond_wait(&pause_condition, &pause_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&pause_mutex) == 0);
}

void ddhub_test_resume(void)
{
    assert(pthread_mutex_lock(&pause_mutex) == 0);
    pause_released = true;
    assert(pthread_cond_broadcast(&pause_condition) == 0);
    assert(pthread_mutex_unlock(&pause_mutex) == 0);
}
