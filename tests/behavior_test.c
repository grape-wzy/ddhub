/**
 * @file behavior_test.c
 * @brief Verify deterministic behavior of the public ddhub API.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Cover both modes, stable priorities, FIFO completion, draining, and live subscription changes.
 * @version 0.2.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-14： Add dispatch behavior and payload lifetime tests.
 * - Codex 2026-10-15： Document file responsibilities, development dates, and modification history.
 * - Codex 2026-10-15： Update the hub count enum and separate ALL processing API.
 * - Codex 2026-10-15： Verify platform binding, unbound states, and callbacks outside exclusion.
 * - Codex 2026-10-15： Verify nested ALL skips its calling hub and preserves that hub's batch quota.
 * - Codex 2026-10-15： Check acquisition counts to verify merged short transactions.
 * - Codex 2026-10-15： Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Verify both dispatch modes, FIFO lifetime, drain rounds, and live subscription changes.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#include "ddhub.h"
#include "ddhub_test_config.h"
#include "test_lock.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct record {
    int        tag;
    int        value;
    uint32_t   event;
    ddhub_id_t hub;
};
static struct record        records[128];
static size_t               record_count;
static ddhub_subscription_t nodes[7];
static int                  tags[] = {
    1,
    2,
    3,
    4,
    5,
    6,
    7
};
#if DDHUB_ASYNC_PROCESS_ENABLE
static size_t notifications;
#endif
static int                   dynamic_once;
static ddhub_subscription_t *removed_tail;

/**
 * @brief Record callback identity, matched mask, payload value, and mutable contexts.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void capture(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    ddhub_test_assert_unlocked();
    assert(record_count < (sizeof(records) / sizeof(records[0])));
    records[record_count++] = (struct record){
        *(int *)arg1, ((data != NULL) && (len == sizeof(int))) ? *(int *)data : -1,
        event,
        TEST_HUB_A
    };
    if (arg2 != NULL) {
        ++*(int *)arg2;
    }
    if (arg3 != NULL) {
        ++*(int *)arg3;
    }
}

/**
 * @brief Change the payload so later subscribers must observe the same mutable buffer.
 * @param event Matched application mask.
 * @param data Borrowed integer payload.
 * @param len Integer payload size.
 * @param arg1 Callback identity.
 * @param arg2 First mutable counter.
 * @param arg3 Second mutable counter.
 */
static void mutate(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    capture(event,
            data,
            len,
            arg1,
            arg2,
            arg3);
    ++*(int *)data;
}

/**
 * @brief Validate one completion and its processing-right lifetime.
 * @param hub_id Actual consuming hub.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg Caller-owned context retained until completion.
 */
static void complete(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg)
{
    ddhub_test_assert_unlocked();
    assert(record_count < (sizeof(records) / sizeof(records[0])));
    records[record_count++] = (struct record){
        9, ((data != NULL) && (len == sizeof(int))) ? *(int *)data : -1,
        event,
        hub_id
    };
    if (arg != NULL) {
        ++*(int *)arg;
    }
    assert(ddhub_set_process_done(hub_id, NULL) == DDHUB_BUSY);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_BUSY);
#endif
}

#if DDHUB_ASYNC_PROCESS_ENABLE
/**
 * @brief Retain one pending wakeup outside platform exclusion.
 */
static void notify(void)
{
    ddhub_test_assert_unlocked();
    ++notifications;
}
#endif

/**
 * @brief Finish queued work when asynchronous dispatch is enabled.
 */
static void drain(void)
{
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_OK);
#endif
}

/**
 * @brief Register a recording callback with the requested priority.
 * @param node Caller-owned subscription node.
 * @param hub Actual target hub.
 * @param tag Expected callback identity.
 * @param priority Subscription priority.
 */
static void subscribe(ddhub_subscription_t *node, ddhub_id_t hub, int *tag, uint8_t priority)
{
    assert(ddhub_register(hub,
                          node,
                          UINT32_MAX,
                          priority,
                          capture,
                          tag,
                          NULL,
                          NULL) == DDHUB_OK);
}

/**
 * @brief Test boundaries.
 */
static void test_boundaries(void)
{
    assert(ddhub_set_lock_ops(NULL, ddhub_test_unlock) == DDHUB_INVALID);
    assert(ddhub_set_lock_ops(ddhub_test_lock, NULL) == DDHUB_INVALID);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_NOT_READY);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          &tags[0],
                          NULL,
                          NULL) == DDHUB_NOT_READY);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_NOT_READY);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_NOT_READY);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_NOT_READY);
    assert(ddhub_set_process_notify(notify) == DDHUB_NOT_READY);
#endif
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_OK);
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_BUSY);
    assert(ddhub_send((ddhub_id_t)-1,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_INVALID);
    assert(ddhub_send((ddhub_id_t)DDHUB_ID_COUNT,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_INVALID);
    assert(ddhub_send(TEST_HUB_A,
                      0U,
                      NULL,
                      0U,
                      NULL) == DDHUB_INVALID);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      1U,
                      NULL) == DDHUB_INVALID);
    assert(ddhub_register(TEST_HUB_A,
                          NULL,
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_INVALID);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          0U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_INVALID);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          NULL,
                          NULL,
                          NULL,
                          NULL) == DDHUB_INVALID);
    assert(ddhub_register((ddhub_id_t)-1,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_INVALID);
    assert(ddhub_unregister(NULL) == DDHUB_INVALID);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_NOT_REGISTERED);
    assert(ddhub_set_process_done((ddhub_id_t)DDHUB_ID_COUNT, NULL) == DDHUB_INVALID);
    /* Polling works before any notification hook is bound. */
    assert(ddhub_send(TEST_HUB_C,
                      UINT32_C(1) << 31,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    drain();
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_set_process_notify(notify) == DDHUB_OK);
    assert(ddhub_set_process_notify(NULL) == DDHUB_BUSY);
#endif
}

/**
 * @brief Test order contexts.
 */
static void test_order_contexts(void)
{
    int a = 0;
    int b = 0;
    int done = 0;
    int value = 42;
    record_count = 0U;
    subscribe(&nodes[0],
              TEST_HUB_A,
              &tags[0],
              20U);
    subscribe(&nodes[1],
              TEST_HUB_A,
              &tags[1],
              20U);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[2],
                          UINT32_C(1) << 31,
                          5U,
                          mutate,
                          &tags[2],
                          &a,
                          &b) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_ALREADY_REGISTERED);
    assert(ddhub_set_process_done(TEST_HUB_A, complete) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A, (UINT32_C(1) << 31) | 1U,
                      &value,
                      sizeof(value),
                      &done) == DDHUB_OK);
    drain();
    assert(record_count == 4U);
    assert((records[0].tag == 3) && (records[0].event == (UINT32_C(1) << 31)));
    assert((records[1].tag == 1) && (records[2].tag == 2) && (records[3].tag == 9));
    assert((records[0].value == 42) && (records[1].value == 43) && (records[3].value == 43));
    assert((a == 1) && (b == 1) && (done == 1));
    for (size_t i = 0U; i < 3U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
    assert(ddhub_unregister(&nodes[0]) == DDHUB_NOT_REGISTERED);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
}

/**
 * @brief Test fifo completion.
 */
static void test_fifo_completion(void)
{
    int values[] = {10, 11, 12};
    int done = 0;
    assert(ddhub_set_process_done(TEST_HUB_A, complete) == DDHUB_OK);
    for (size_t round = 0U; round < 5U; ++round) {
        record_count = 0U;
        for (size_t i = 0U; i < 3U; ++i) {
#if DDHUB_ASYNC_PROCESS_ENABLE
            size_t before = notifications;
#endif
            assert(ddhub_send(TEST_HUB_A,
                              1U,
                              &values[i],
                              sizeof(values[i]),
                              &done) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
            assert(notifications == (before + 1U));
#endif
        }
#if DDHUB_ASYNC_PROCESS_ENABLE
        size_t before = notifications;
        assert(ddhub_send(TEST_HUB_A,
                          1U,
                          NULL,
                          0U,
                          NULL) == DDHUB_QUEUE_FULL);
        assert(notifications == before);
        assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
#endif
        drain();
        assert(record_count == 3U);
        for (size_t i = 0U; i < 3U; ++i) {
            assert((records[i].tag == 9) && (records[i].value == values[i]));
        }
    }
    assert(done == 15);
    drain();
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
}

/**
 * @brief Remove and re-register nodes while preserving the current message boundary.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void change_subscriptions(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                                 void *const arg3)
{
    capture(event,
            data,
            len,
            arg1,
            arg2,
            arg3);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_BUSY);
    if (dynamic_once++ == 0) {
        assert(ddhub_unregister(&nodes[1]) == DDHUB_OK);
        /* The same address gets a new generation and must skip this message. */
        subscribe(&nodes[1],
                  TEST_HUB_A,
                  &tags[1],
                  15U);
        subscribe(&nodes[2],
                  TEST_HUB_A,
                  &tags[2],
                  0U);
        assert(ddhub_unregister(removed_tail) == DDHUB_OK);
        free(removed_tail);
        removed_tail = NULL;
    }
}

/**
 * @brief Test dynamic subscriptions.
 */
static void test_dynamic_subscriptions(void)
{
    dynamic_once = 0;
    record_count = 0U;
    removed_tail = calloc(1U, sizeof(*removed_tail));
    assert(removed_tail != NULL);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          10U,
                          change_subscriptions,
                          &tags[0],
                          NULL,
                          NULL) == DDHUB_OK);
    subscribe(&nodes[1],
              TEST_HUB_A,
              &tags[1],
              20U);
    subscribe(&nodes[3],
              TEST_HUB_A,
              &tags[3],
              30U);
    subscribe(removed_tail,
              TEST_HUB_A,
              &tags[4],
              40U);
    assert(ddhub_set_process_done(TEST_HUB_A, complete) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    drain();
    const int expected[] = {
        1,
        4,
        9,
        3,
        1,
        2,
        4,
        9
    };
    assert(record_count == (sizeof(expected) / sizeof(expected[0])));
    for (size_t i = 0U; i < record_count; ++i) {
        assert(records[i].tag == expected[i]);
    }
    for (size_t i = 0U; i < 4U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
}

static int sent_chain;
static int chain_value = 1;
static int cross_value = 100;

/**
 * @brief Send follow-up messages and validate mode-specific reentry behavior.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void chain(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    capture(event,
            data,
            len,
            arg1,
            arg2,
            arg3);
    if (sent_chain++ == 0) {
#if DDHUB_ASYNC_PROCESS_ENABLE
        assert(ddhub_process() == DDHUB_BUSY);
        assert(ddhub_send(TEST_HUB_A,
                          1U,
                          &chain_value,
                          sizeof(chain_value),
                          NULL) == DDHUB_OK);
#else
        assert(ddhub_send(TEST_HUB_A,
                          1U,
                          &chain_value,
                          sizeof(chain_value),
                          NULL) == DDHUB_BUSY);
#endif
        assert(ddhub_send(TEST_HUB_B,
                          1U,
                          &cross_value,
                          sizeof(cross_value),
                          NULL) == DDHUB_OK);
    }
}

/**
 * @brief Test drain and cross hubs.
 */
static void test_drain_and_cross_hubs(void)
{
    int initial = 0;
    int third = 200;
    sent_chain = 0;
    record_count = 0U;
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          chain,
                          &tags[0],
                          NULL,
                          NULL) == DDHUB_OK);
    subscribe(&nodes[1],
              TEST_HUB_B,
              &tags[1],
              0U);
    subscribe(&nodes[2],
              TEST_HUB_C,
              &tags[2],
              0U);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      &initial,
                      sizeof(initial),
                      NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      &third,
                      sizeof(third),
                      NULL) == DDHUB_OK);
    drain();
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(record_count == 4U);
    assert((records[0].value == 0) && (records[1].value == 200));
    assert((records[2].value == 1) && (records[3].value == 100));
#else
    assert(record_count == 3U);
    assert((records[0].value == 0) && (records[1].value == 100) && (records[2].value == 200));
#endif
    for (size_t i = 0U; i < 3U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
}

/**
 * @brief Release producer storage at the final consumption boundary.
 * @param hub_id Actual consuming hub.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg Caller-owned context retained until completion.
 */
static void release_payload(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg)
{
    ddhub_test_assert_unlocked();
    (void)hub_id;
    (void)event;
    assert(len == sizeof(int));
    ++*(int *)arg;
    free(data);
}

/**
 * @brief Test payload release.
 */
static void test_payload_release(void)
{
    int  done = 0;
    int *value = malloc(sizeof(*value));
    assert(value != NULL);
    *value = 123;
    assert(ddhub_set_process_done(TEST_HUB_A, release_payload) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      value,
                      sizeof(*value),
                      &done) == DDHUB_OK);
    drain();
    assert(done == 1);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
}

/**
 * @brief Execute this suite and fail on any violated dispatch contract.
 * @return Zero after all assertions pass.
 */
int main(void)
{
    test_boundaries();
    test_order_contexts();
    test_fifo_completion();
    test_dynamic_subscriptions();
    test_drain_and_cross_hubs();
    test_payload_release();
    puts("ddhub behavior passed");
    return 0;
}
