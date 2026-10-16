/**
 * @file generation_test.c
 * @brief Verify registration generation rebasing during an active message.
 * @version 0.2.0
 * @date 2026-10-16
 *
 * @par 修改记录
 * - Codex 2026-10-16：Verify registration generation rebasing during an active message.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#include "ddhub.h"
#include "ddhub_test_config.h"
#include "test_lock.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* White-box boundary test, without adding a test-only production API. */
#include "../src/ddhub.c"

static ddhub_subscription_t nodes[4];
static unsigned int         first_calls;
static unsigned int         old_calls;
static unsigned int         new_calls;

/**
 * @brief Count delivery to a subscription eligible before sequence rebasing.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void count_old(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                      void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    ++old_calls;
}

/**
 * @brief Count delivery to subscriptions added after the current message boundary.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void count_new(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                      void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    ++new_calls;
}

/**
 * @brief Force sequence exhaustion while preserving old and new recipient eligibility.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void register_at_wrap(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                             void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    if (first_calls++ == 0U) {
        assert(ddhub_register(TEST_HUB_A,
                              &nodes[2],
                              1U,
                              10U,
                              count_new,
                              NULL,
                              NULL,
                              NULL) == DDHUB_OK);
        ddhub_lock_key_t key = platform_lock();
        hubs[TEST_HUB_A].generation = UINT64_MAX;
        platform_unlock(key);
        assert(ddhub_register(TEST_HUB_A,
                              &nodes[3],
                              1U,
                              15U,
                              count_new,
                              NULL,
                              NULL,
                              NULL) == DDHUB_OK);
    }
}

/**
 * @brief Execute this suite and fail on any violated dispatch contract.
 * @return Zero after all assertions pass.
 */
int main(void)
{
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          register_at_wrap,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[1],
                          1U,
                          20U,
                          count_old,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_OK);
#endif
    assert((first_calls == 1U) && (old_calls == 1U) && (new_calls == 0U));
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_OK);
#endif
    assert((first_calls == 2U) && (old_calls == 2U) && (new_calls == 2U));
    puts("ddhub registration generation passed");
    return 0;
}
