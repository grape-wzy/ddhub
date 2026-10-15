/**
 * @file behavior_test.c
 * @brief Verify deterministic behavior of the public ddhub API.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Cover arguments, stable priorities, event intersections, FIFO wraparound, snapshots, completion, and lock
 * costs.
 * @version 0.1.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-14: Add dispatch behavior and payload lifetime tests.
 * - Codex 2026-10-15: Document file responsibilities, development dates, and modification history.
 * - Codex 2026-10-15: Update the hub count enum and separate ALL processing API.
 * - Codex 2026-10-15: Verify platform binding, unbound states, and callbacks outside exclusion.
 * - Codex 2026-10-15: Verify nested ALL skips its calling hub and preserves that hub's batch quota.
 * - Codex 2026-10-15: Check acquisition counts to verify merged short transactions.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#include "ddhub.h"
#include "ddhub_test_config.h"
#include "test_lock.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct record {
    uint32_t   event;
    void      *data;
    size_t     len;
    void      *arg1;
    void      *arg2;
    void      *arg3;
    int        value;
    int        done;
    ddhub_id_t hub_id;
};

static struct record        records[64];
static size_t               record_count;
static ddhub_subscription_t nodes[6];
static int                  markers[6];
static int                  initial_a;
static int                  initial_c;
static int                  next_a;
static int                  next_b;
static int                  next_c;

/**
 * @brief Capture one consumer invocation and observe the current mutable payload.
 * @param event Matched events.
 * @param data Original payload.
 * @param len Payload length.
 * @param arg1 Subscriber marker.
 * @param arg2 Second subscriber context.
 * @param arg3 Third subscriber context.
 */
static void capture(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    ddhub_test_assert_unlocked();
    assert(record_count < (sizeof(records) / sizeof(records[0])));
    records[record_count++] = (struct record){
        .event = event,
        .data = data,
        .len = len,
        .arg1 = arg1,
        .arg2 = arg2,
        .arg3 = arg3,
        .value = ((data != NULL) && (len == sizeof(int))) ? *(int *)data : 0,
    };
}

/**
 * @brief Record a consumption completion after all subscribers have returned.
 * @param hub_id Actual hub.
 * @param event Original event mask.
 * @param data Original payload.
 * @param len Original size.
 * @param arg Producer context.
 */
static void capture_done(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg)
{
    capture(event,
            data,
            len,
            arg,
            NULL,
            NULL);
    records[record_count - 1U].done = 1;
    records[record_count - 1U].hub_id = hub_id;
}

/**
 * @brief Modify payload and subscriber contexts while preserving pointer identities.
 * @param event Matched events.
 * @param data Integer payload.
 * @param len Integer size.
 * @param arg1 Subscriber marker.
 * @param arg2 Mutable integer context.
 * @param arg3 Mutable integer context.
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
    ++*(int *)arg2;
    ++*(int *)arg3;
}

/** @brief Check invalid inputs, duplicate nodes, optional contexts, and idle-only completion changes. */
static void test_boundaries(void)
{
    assert(ddhub_process((ddhub_id_t)-1) == DDHUB_INVALID);
    assert(ddhub_process((ddhub_id_t)(DDHUB_ID_COUNT + 1)) == DDHUB_INVALID);
    assert(ddhub_process((ddhub_id_t)DDHUB_ID_COUNT) == DDHUB_INVALID);
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
    assert(ddhub_set_process_done((ddhub_id_t)DDHUB_ID_COUNT, NULL) == DDHUB_INVALID);
    assert(ddhub_register((ddhub_id_t)DDHUB_ID_COUNT,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
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
    assert(ddhub_unregister(NULL) == DDHUB_INVALID);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_NOT_REGISTERED);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          UINT8_MAX,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_ALREADY_REGISTERED);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_NOT_REGISTERED);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_set_process_done(TEST_HUB_A, capture_done) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
    record_count = 0U;
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((record_count == 1U) && (records[0].done == 1));
    assert((records[0].data == NULL) && (records[0].len == 0U));
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
    assert(ddhub_process_all() == DDHUB_OK);
}

/** @brief Verify stable priorities, event intersections including bit 31, and shared mutable pointers. */
static void test_dispatch(void)
{
    static int     payload = 10;
    static int     arg2;
    static int     arg3;
    static int     producer;
    const uint32_t high_bit = UINT32_C(1) << 31;
    const uint32_t published = high_bit | 3U;
    assert(ddhub_set_process_done(TEST_HUB_A, capture_done) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          3U,
                          50U,
                          capture,
                          &markers[0],
                          &arg2,
                          &arg3) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[1],
                          5U,
                          10U,
                          mutate,
                          &markers[1],
                          &arg2,
                          &arg3) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[2],
                          1U,
                          10U,
                          capture,
                          &markers[2],
                          &arg2,
                          &arg3) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[3],
                          high_bit,
                          20U,
                          capture,
                          &markers[3],
                          &arg2,
                          &arg3) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[4],
                          128U,
                          0U,
                          capture,
                          &markers[4],
                          NULL,
                          NULL) == DDHUB_OK);
    record_count = 0U;
    assert(ddhub_send(TEST_HUB_A,
                      published,
                      &payload,
                      sizeof(payload),
                      &producer) == DDHUB_OK);
    assert(record_count == 0U);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert(record_count == 5U);
    assert((records[0].arg1 == &markers[1]) && (records[0].event == 1U));
    assert((records[1].arg1 == &markers[2]) && (records[1].event == 1U));
    assert((records[2].arg1 == &markers[3]) && (records[2].event == high_bit));
    assert((records[3].arg1 == &markers[0]) && (records[3].event == 3U));
    assert((records[0].value == 10) && (records[1].value == 11));
    assert((arg2 == 1) && (arg3 == 1));
    for (size_t i = 0U; i < 4U; ++i) {
        assert((records[i].data == &payload) && (records[i].len == sizeof(payload)));
        assert((records[i].arg2 == &arg2) && (records[i].arg3 == &arg3));
    }
    assert((records[4].done == 1) && (records[4].event == published));
    assert((records[4].arg1 == &producer) && (records[4].hub_id == TEST_HUB_A));
    for (size_t i = 0U; i < 5U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);

    /* Re-registration moves a node behind existing subscribers of the same priority. */
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          &markers[0],
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[1],
                          1U,
                          0U,
                          capture,
                          &markers[1],
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          &markers[0],
                          NULL,
                          NULL) == DDHUB_OK);
    record_count = 0U;
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((record_count == 2U) && (records[0].arg1 == &markers[1]) && (records[1].arg1 == &markers[0]));
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[1]) == DDHUB_OK);
}

/** @brief Exercise non-power-of-two FIFO wraparound, overflow, and independent hub capacity. */
static void test_fifo(void)
{
    static int values[9];
    record_count = 0U;
    assert(ddhub_set_process_done(TEST_HUB_A, capture_done) == DDHUB_OK);
    for (size_t round = 0U; round < 3U; ++round) {
        for (size_t i = 0U; i < DDHUB_FIFO_DEPTH; ++i) {
            size_t index = (round * DDHUB_FIFO_DEPTH) + i;
            values[index] = (int)index;
            assert(ddhub_send(TEST_HUB_A,
                              1U,
                              &values[index],
                              sizeof(int),
                              &values[index]) == DDHUB_OK);
        }
        assert(ddhub_send(TEST_HUB_A,
                          1U,
                          NULL,
                          0U,
                          NULL) == DDHUB_QUEUE_FULL);
        assert(ddhub_send(TEST_HUB_B,
                          1U,
                          NULL,
                          0U,
                          NULL) == DDHUB_OK);
        assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
        assert(ddhub_process(TEST_HUB_B) == DDHUB_OK);
    }
    assert(record_count == 9U);
    for (size_t i = 0U; i < record_count; ++i) {
        assert((records[i].done == 1) && (records[i].data == &values[i]) && (records[i].value == (int)i));
        assert(records[i].arg1 == &values[i]);
    }
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
}

/**
 * @brief Enqueue during ALL processing and verify frozen lists and reentry exclusion.
 * @param event Matched event mask.
 * @param data Current payload.
 * @param len Current payload size.
 * @param arg1 First subscriber context.
 * @param arg2 Second subscriber context.
 * @param arg3 Third subscriber context.
 */
static void snapshot_callback(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                              void *const arg3)
{
    capture(event,
            data,
            len,
            arg1,
            arg2,
            arg3);
    if (data == &initial_a) {
        assert(ddhub_send(TEST_HUB_A,
                          1U,
                          &next_a,
                          sizeof(next_a),
                          NULL) == DDHUB_OK);
        assert(ddhub_send(TEST_HUB_B,
                          1U,
                          &next_b,
                          sizeof(next_b),
                          NULL) == DDHUB_OK);
        assert(ddhub_send(TEST_HUB_C,
                          1U,
                          &next_c,
                          sizeof(next_c),
                          NULL) == DDHUB_OK);
        assert(ddhub_process(TEST_HUB_A) == DDHUB_BUSY);
        assert(ddhub_process(TEST_HUB_B) == DDHUB_BUSY);
        assert(ddhub_process_all() == DDHUB_BUSY);
        assert(ddhub_unregister(&nodes[0]) == DDHUB_BUSY);
        assert(ddhub_register(TEST_HUB_B,
                              &nodes[3],
                              1U,
                              0U,
                              capture,
                              NULL,
                              NULL,
                              NULL) == DDHUB_BUSY);
        assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
    }
}

/** @brief Verify a whole-call ALL snapshot, enum order, and deferred self/cross-hub sends. */
static void test_snapshot(void)
{
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          snapshot_callback,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[2],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      &initial_c,
                      sizeof(initial_c),
                      NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      &initial_a,
                      sizeof(initial_a),
                      NULL) == DDHUB_OK);
    record_count = 0U;
    assert(ddhub_process_all() == DDHUB_OK);
    assert((record_count == 2U) && (records[0].data == &initial_a) && (records[1].data == &initial_c));
    assert(ddhub_process_all() == DDHUB_OK);
    assert(record_count == 5U);
    assert((records[2].data == &next_a) && (records[3].data == &next_b) && (records[4].data == &next_c));
    for (size_t i = 0U; i < 3U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
}

/**
 * @brief Process other hubs from a single-hub callback without disturbing its own batch.
 * @param event Matched event mask.
 * @param data Current payload.
 * @param len Current payload size.
 * @param arg1 Subscriber marker.
 * @param arg2 Second subscriber context.
 * @param arg3 Third subscriber context.
 */
static void process_others_callback(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                                    void *const arg3)
{
    capture(event,
            data,
            len,
            arg1,
            arg2,
            arg3);
    assert(ddhub_process_all() == DDHUB_BUSY);
}

/** @brief Verify nested ALL skips its calling hub and preserves that hub's remaining messages. */
static void test_reentrant_all(void)
{
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          process_others_callback,
                          &markers[0],
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          capture,
                          &markers[1],
                          NULL,
                          NULL) == DDHUB_OK);
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
    assert(ddhub_send(TEST_HUB_B,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    record_count = 0U;
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert(record_count == 3U);
    assert((records[0].arg1 == &markers[0]) && (records[1].arg1 == &markers[1]) && (records[2].arg1 == &markers[0]));
    assert(ddhub_process_all() == DDHUB_OK);
    assert(record_count == 3U);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[1]) == DDHUB_OK);
}

/** @brief Confirm that subscription changes after send affect later processing. */
static void test_subscription_timing(void)
{
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          &markers[0],
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[1],
                          1U,
                          0U,
                          capture,
                          &markers[1],
                          NULL,
                          NULL) == DDHUB_OK);
    record_count = 0U;
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((record_count == 1U) && (records[0].arg1 == &markers[1]));
    assert(ddhub_unregister(&nodes[1]) == DDHUB_OK);
}

/**
 * @brief Reuse a completed payload and enqueue a successor from its completion notification.
 * @param hub_id Completed hub.
 * @param event Original mask.
 * @param data Completed mutable payload.
 * @param len Payload size.
 * @param arg Producer marker.
 */
static void reuse_done(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg)
{
    capture_done(hub_id,
                 event,
                 data,
                 len,
                 arg);
    assert(ddhub_process(hub_id) == DDHUB_BUSY);
    assert(ddhub_set_process_done(hub_id, NULL) == DDHUB_BUSY);
    if (arg == &markers[0]) {
        *(int *)data = 77;
        assert(ddhub_send(hub_id,
                          event,
                          data,
                          len,
                          &markers[1]) == DDHUB_OK);
    }
}

/** @brief Check one completion per message and safe payload reuse with no subscribers. */
static void test_completion(void)
{
    static int payload = 12;
    assert(ddhub_set_process_done(TEST_HUB_A, reuse_done) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      &payload,
                      sizeof(payload),
                      &markers[0]) == DDHUB_OK);
    record_count = 0U;
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((record_count == 1U) && (records[0].value == 12) && (payload == 77));
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((record_count == 2U) && (records[1].value == 77));
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert(record_count == 2U);
}

/** @brief Verify platform lock costs for short transactions and batches with callbacks outside locks. */
static void test_critical_section_count(void)
{
    ddhub_lock_key_t before = ddhub_test_lock_count();
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 2U);

    before = ddhub_test_lock_count();
    assert(ddhub_set_process_done(TEST_HUB_A, capture_done) == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 1U);
    for (size_t i = 0U; i < 2U; ++i) {
        before = ddhub_test_lock_count();
        assert(ddhub_send(TEST_HUB_A,
                          1U,
                          NULL,
                          0U,
                          NULL) == DDHUB_OK);
        assert((ddhub_test_lock_count() - before) == 1U);
    }
    before = ddhub_test_lock_count();
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
    assert((ddhub_test_lock_count() - before) == 1U);

    record_count = 0U;
    before = ddhub_test_lock_count();
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 4U);
    assert(record_count == 4U);
    assert((records[0].done == 0) && (records[1].done == 1) && (records[2].done == 0) && (records[3].done == 1));
    before = ddhub_test_lock_count();
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 2U);

    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_B,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    before = ddhub_test_lock_count();
    assert(ddhub_process_all() == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 4U);
    assert(record_count == 6U);
    before = ddhub_test_lock_count();
    assert(ddhub_process_all() == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 2U);

    before = ddhub_test_lock_count();
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert((ddhub_test_lock_count() - before) == 2U);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
}

/**
 * @brief Run deterministic public-API behavior checks without accessing ddhub internals.
 * @return Zero if every assertion passes.
 */
int main(void)
{
    assert(ddhub_set_lock_ops(NULL, NULL) == DDHUB_INVALID);
    assert(ddhub_set_lock_ops(ddhub_test_lock, NULL) == DDHUB_INVALID);
    assert(ddhub_set_lock_ops(NULL, ddhub_test_unlock) == DDHUB_INVALID);
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
                          NULL,
                          NULL,
                          NULL) == DDHUB_NOT_READY);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_NOT_READY);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_NOT_READY);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_NOT_READY);
    assert(ddhub_process_all() == DDHUB_NOT_READY);
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_OK);
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_BUSY);
    test_boundaries();
    test_dispatch();
    test_fifo();
    test_snapshot();
    test_reentrant_all();
    test_subscription_timing();
    test_completion();
    test_critical_section_count();
    ddhub_test_assert_unlocked();
    puts("ddhub behavior: all checks passed");
    return 0;
}
