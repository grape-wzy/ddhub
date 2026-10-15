/**
 * @file concurrency_test.c
 * @brief Verify concurrent ddhub publication and processing.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Use real threads and controlled pauses to test exclusion, busy-hub skipping, snapshots, and message
 * integrity.
 * @version 0.1.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-14: Add concurrency admission and a 2000-message multi-producer test.
 * - Codex 2026-10-15: Document file responsibilities, development dates, and modification history.
 * - Codex 2026-10-15: Inject a pthread adapter to test bool lock contention and processing cleanup without atomics.
 * - Codex 2026-10-15: Test parallel registration, shared-node contention, and ownership transfer after unregistration.
 * - Codex 2026-10-15: Test busy-hub skipping, independent cleanup, and continued ALL dispatch after contention.
 * - Codex 2026-10-15: Use list reservations to test contention and concurrent enqueue against ALL snapshots.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#include "ddhub.h"
#include "ddhub_test_config.h"
#include "test_lock.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct callback_gate {
    pthread_mutex_t mutex;
    pthread_cond_t  condition;
    bool            entered;
    bool            released;
    unsigned int    calls;
};

struct process_request {
    ddhub_id_t     hub_id;
    ddhub_status_t status;
    bool           all;
};

static struct callback_gate gate_a = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};
static struct callback_gate gate_b = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};
static ddhub_subscription_t nodes[5];
static unsigned int         hub_c_calls;
static unsigned int         done_calls[DDHUB_ID_COUNT];

/**
 * @brief Block inside a user callback so another thread can inspect processing exclusion.
 * @param event Matched event mask.
 * @param data Optional payload.
 * @param len Payload size.
 * @param arg1 Callback gate.
 * @param arg2 Unused context.
 * @param arg3 Unused context.
 */
static void blocking_callback(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                              void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg2;
    (void)arg3;
    struct callback_gate *gate = arg1;
    assert(pthread_mutex_lock(&gate->mutex) == 0);
    ++gate->calls;
    gate->entered = true;
    assert(pthread_cond_broadcast(&gate->condition) == 0);
    while (!gate->released) {
        assert(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

/**
 * @brief Count third-hub processing on the coordinating thread.
 * @param event Matched mask.
 * @param data Optional payload.
 * @param len Payload size.
 * @param arg1 Unused context.
 * @param arg2 Unused context.
 * @param arg3 Unused context.
 */
static void count_c(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    ++hub_c_calls;
}

/**
 * @brief Count completions independently for each hub during controlled processing.
 * @param hub_id Consuming hub, whose processing right serializes its own counter.
 * @param event Original event mask.
 * @param data Optional payload.
 * @param len Payload size.
 * @param arg Unused producer context.
 */
static void count_done(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg;
    ++done_calls[hub_id];
}

/**
 * @brief Execute one processing request in a real host thread.
 * @param arg Request storage, retained until thread join.
 * @return NULL after saving the processing result.
 */
static void *process_thread(void *arg)
{
    struct process_request *request = arg;
    request->status = request->all ? ddhub_process_all() : ddhub_process(request->hub_id);
    return NULL;
}

/**
 * @brief Wait until a consumer has entered its blocking callback.
 * @param gate Gate used by the consumer.
 */
static void await_gate(struct callback_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mutex) == 0);
    while (!gate->entered) {
        assert(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

/**
 * @brief Permit a blocked callback to finish.
 * @param gate Gate used by the consumer.
 */
static void release_gate(struct callback_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mutex) == 0);
    gate->released = true;
    assert(pthread_cond_broadcast(&gate->condition) == 0);
    assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

/**
 * @brief Reset a gate after its previous processing thread has been joined.
 * @param gate Inactive gate.
 */
static void reset_gate(struct callback_gate *gate)
{
    gate->entered = false;
    gate->released = false;
    gate->calls = 0U;
}

/** @brief Prove parallel single-hub processing and ALL consumption of the remaining idle hub. */
static void test_parallel_hubs(void)
{
    hub_c_calls = 0U;
    static struct process_request request_a = {.hub_id = TEST_HUB_A};
    static struct process_request request_b = {.hub_id = TEST_HUB_B};
    pthread_t                     thread_a;
    pthread_t                     thread_b;
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_a,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_b,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[2],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
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
    assert(pthread_create(&thread_a,
                          NULL,
                          process_thread,
                          &request_a) == 0);
    await_gate(&gate_a);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_BUSY);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_BUSY);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[3],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_BUSY);
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);

    assert(pthread_create(&thread_b,
                          NULL,
                          process_thread,
                          &request_b) == 0);
    await_gate(&gate_b);
    assert(ddhub_process(TEST_HUB_B) == DDHUB_BUSY);
    /* Another idle hub's registry and queue remain usable while A and B are blocked. */
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[3],
                          1U,
                          1U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[3]) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_process_all() == DDHUB_BUSY);
    assert(hub_c_calls == 1U);

    release_gate(&gate_b);
    assert(pthread_join(thread_b, NULL) == 0);
    release_gate(&gate_a);
    assert(pthread_join(thread_a, NULL) == 0);
    assert((request_a.status == DDHUB_OK) && (request_b.status == DDHUB_OK));
    assert((gate_a.calls == 1U) && (gate_b.calls == 1U));
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert(gate_a.calls == 2U);
    for (size_t i = 0U; i < 3U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
}

/** @brief Prove ALL excludes processors of its selected hubs and snapshots before callbacks start. */
static void test_all_exclusion(void)
{
    static struct process_request request_all = {.all = true};
    pthread_t                     thread_all;
    reset_gate(&gate_a);
    reset_gate(&gate_b);
    hub_c_calls = 0U;
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_a,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_b,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[2],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
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
    assert(pthread_create(&thread_all,
                          NULL,
                          process_thread,
                          &request_all) == 0);
    await_gate(&gate_a);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_BUSY);
    assert(ddhub_process(TEST_HUB_B) == DDHUB_BUSY);
    assert(ddhub_process(TEST_HUB_C) == DDHUB_BUSY);
    assert(ddhub_process_all() == DDHUB_BUSY);
    assert(ddhub_unregister(&nodes[1]) == DDHUB_BUSY);
    assert(ddhub_set_process_done(TEST_HUB_C, NULL) == DDHUB_BUSY);
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    release_gate(&gate_a);
    await_gate(&gate_b);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_BUSY);
    release_gate(&gate_b);
    assert(pthread_join(thread_all, NULL) == 0);
    assert(request_all.status == DDHUB_OK);
    assert(hub_c_calls == 0U);
    assert(ddhub_process(TEST_HUB_C) == DDHUB_OK);
    assert(hub_c_calls == 1U);
    for (size_t i = 0U; i < 3U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
}

struct registration_request {
    ddhub_id_t            hub_id;
    ddhub_subscription_t *node;
    ddhub_status_t        status;
};

/**
 * @brief Hold a hub's metadata lock after claiming a node, before traversing its list.
 * @param arg Registration request retained until thread join.
 * @return NULL after registration finishes.
 */
static void *paused_register_thread(void *arg)
{
    struct registration_request *request = arg;
    ddhub_test_pause_next_unlock();
    request->status = ddhub_register(request->hub_id,
                                     request->node,
                                     1U,
                                     0U,
                                     count_c,
                                     NULL,
                                     NULL,
                                     NULL);
    return NULL;
}

/**
 * @brief Pause after the complete enqueue transaction has released both locks.
 * @param arg Storage for the send result, retained until thread join.
 * @return NULL after the controlled send finishes.
 */
static void *paused_send_thread(void *arg)
{
    ddhub_test_pause_next_unlock();
    *(ddhub_status_t *)arg = ddhub_send(TEST_HUB_A,
                                        1U,
                                        NULL,
                                        0U,
                                        NULL);
    return NULL;
}

/** @brief Verify same-hub contention while unrelated hub operations continue successfully. */
static void test_bool_lock_contention(void)
{
    pthread_t                   worker;
    struct registration_request request = {.hub_id = TEST_HUB_A, .node = &nodes[3]};
    hub_c_calls = 0U;
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[4],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(pthread_create(&worker,
                          NULL,
                          paused_register_thread,
                          &request) == 0);
    ddhub_test_wait_paused();
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_BUSY);
    assert(ddhub_process(TEST_HUB_A) == DDHUB_BUSY);
    assert(ddhub_process_all() == DDHUB_BUSY);
    assert(hub_c_calls == 1U);
    assert(ddhub_set_process_done(TEST_HUB_B, NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_B,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_process(TEST_HUB_B) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[4]) == DDHUB_OK);
    ddhub_test_resume();
    assert(pthread_join(worker, NULL) == 0);
    assert(request.status == DDHUB_OK);
    assert(ddhub_unregister(&nodes[3]) == DDHUB_OK);
    assert(ddhub_process_all() == DDHUB_OK);
}

/** @brief Keep skipped A independent while ALL owns B/C, including a new A processor. */
static void test_all_skips_processing_hub(void)
{
    static struct process_request request_a = {.hub_id = TEST_HUB_A};
    static struct process_request request_all = {.all = true};
    pthread_t                     thread_a;
    pthread_t                     thread_all;
    reset_gate(&gate_a);
    reset_gate(&gate_b);
    hub_c_calls = 0U;
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        done_calls[i] = 0U;
        assert(ddhub_set_process_done((ddhub_id_t)i, count_done) == DDHUB_OK);
    }
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_a,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_b,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[2],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(pthread_create(&thread_a,
                          NULL,
                          process_thread,
                          &request_a) == 0);
    await_gate(&gate_a);
    /* This A message is outside the original single-hub batch, and ALL must skip it too. */
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
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(pthread_create(&thread_all,
                          NULL,
                          process_thread,
                          &request_all) == 0);
    await_gate(&gate_b);
    assert(ddhub_process_all() == DDHUB_BUSY);
    assert(ddhub_process(TEST_HUB_B) == DDHUB_BUSY);
    assert(ddhub_process(TEST_HUB_C) == DDHUB_BUSY);
    assert(ddhub_unregister(&nodes[2]) == DDHUB_BUSY);
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);

    release_gate(&gate_a);
    assert(pthread_join(thread_a, NULL) == 0);
    assert((request_a.status == DDHUB_OK) && (gate_a.calls == 1U) && (done_calls[TEST_HUB_A] == 1U));
    /* ALL did not reserve A, so A's registry and a new processing call remain independent. */
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[3],
                          1U,
                          1U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[3]) == DDHUB_OK);
    reset_gate(&gate_a);
    assert(pthread_create(&thread_a,
                          NULL,
                          process_thread,
                          &request_a) == 0);
    await_gate(&gate_a);
    release_gate(&gate_b);
    assert(pthread_join(thread_all, NULL) == 0);
    assert(request_all.status == DDHUB_BUSY);
    assert((hub_c_calls == 1U) && (done_calls[TEST_HUB_B] == 1U) && (done_calls[TEST_HUB_C] == 1U));
    /* ALL cleanup must not clear the processing right held by A's replacement call. */
    assert(ddhub_process(TEST_HUB_A) == DDHUB_BUSY);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_BUSY);
    assert(ddhub_process_all() == DDHUB_BUSY);
    assert((hub_c_calls == 2U) && (done_calls[TEST_HUB_C] == 2U));
    release_gate(&gate_a);
    assert(pthread_join(thread_a, NULL) == 0);
    assert((request_a.status == DDHUB_OK) && (gate_a.calls == 1U) && (done_calls[TEST_HUB_A] == 2U));
    assert(ddhub_process_all() == DDHUB_OK);
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
        assert(ddhub_set_process_done((ddhub_id_t)i, NULL) == DDHUB_OK);
    }
}

/** @brief Finish A/B/C while a concurrent enqueue returns, deferring the new message to the next batch. */
static void test_all_snapshot_during_send(void)
{
    static struct process_request request_all = {.all = true};
    pthread_t                     thread_all;
    pthread_t                     sender;
    ddhub_status_t                send_status = DDHUB_INVALID;
    reset_gate(&gate_a);
    hub_c_calls = 0U;
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        done_calls[i] = 0U;
        assert(ddhub_set_process_done((ddhub_id_t)i, count_done) == DDHUB_OK);
    }
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          blocking_callback,
                          &gate_a,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[2],
                          1U,
                          0U,
                          count_c,
                          NULL,
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
    assert(ddhub_send(TEST_HUB_C,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(pthread_create(&thread_all,
                          NULL,
                          process_thread,
                          &request_all) == 0);
    await_gate(&gate_a);
    assert(pthread_create(&sender,
                          NULL,
                          paused_send_thread,
                          &send_status) == 0);
    ddhub_test_wait_paused();
    release_gate(&gate_a);
    assert(pthread_join(thread_all, NULL) == 0);
    assert(request_all.status == DDHUB_OK);
    assert((gate_a.calls == 2U) && (hub_c_calls == 2U));
    assert((done_calls[TEST_HUB_A] == 2U) && (done_calls[TEST_HUB_B] == 1U) && (done_calls[TEST_HUB_C] == 1U));
    ddhub_test_resume();
    assert(pthread_join(sender, NULL) == 0);
    assert(send_status == DDHUB_OK);
    assert(ddhub_process_all() == DDHUB_OK);
    assert((gate_a.calls == 3U) && (done_calls[TEST_HUB_A] == 3U) && (hub_c_calls == 2U));
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
        assert(ddhub_set_process_done((ddhub_id_t)i, NULL) == DDHUB_OK);
    }
}

/** @brief Verify two threads can register distinct nodes on different hubs. */
static void test_parallel_registration(void)
{
    struct registration_request request = {.hub_id = TEST_HUB_A, .node = &nodes[3]};
    pthread_t                   worker;
    assert(pthread_create(&worker,
                          NULL,
                          paused_register_thread,
                          &request) == 0);
    ddhub_test_wait_paused();
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[4],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[4]) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[4],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_BUSY);
    ddhub_test_resume();
    assert(pthread_join(worker, NULL) == 0);
    assert(request.status == DDHUB_OK);
    assert(ddhub_unregister(&nodes[3]) == DDHUB_OK);
}

/** @brief Verify competing cross-hub registrations cannot duplicate one intrusive node. */
static void test_shared_node_registration(void)
{
    struct registration_request request = {.hub_id = TEST_HUB_A, .node = &nodes[3]};
    pthread_t                   worker;
    hub_c_calls = 0U;
    assert(pthread_create(&worker,
                          NULL,
                          paused_register_thread,
                          &request) == 0);
    ddhub_test_wait_paused();
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[3],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_ALREADY_REGISTERED);
    ddhub_test_resume();
    assert(pthread_join(worker, NULL) == 0);
    assert(request.status == DDHUB_OK);
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
    assert(ddhub_process(TEST_HUB_A) == DDHUB_OK);
    assert(hub_c_calls == 1U);
    assert(ddhub_process(TEST_HUB_B) == DDHUB_OK);
    assert(hub_c_calls == 1U);
    assert(ddhub_unregister(&nodes[3]) == DDHUB_OK);
}

/**
 * @brief Pause after publishing node availability and releasing its old hub, before returning.
 * @param arg Storage for the unregister result, retained until thread join.
 * @return NULL after unregistration finishes.
 */
static void *paused_unregister_thread(void *arg)
{
    ddhub_test_pause_after_unlocks(2U);
    *(ddhub_status_t *)arg = ddhub_unregister(&nodes[3]);
    return NULL;
}

/** @brief Verify a node can move to a new hub while its old unregister call finishes. */
static void test_node_owner_transfer(void)
{
    pthread_t      worker;
    ddhub_status_t result = DDHUB_INVALID;
    hub_c_calls = 0U;
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[3],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(pthread_create(&worker,
                          NULL,
                          paused_unregister_thread,
                          &result) == 0);
    ddhub_test_wait_paused();
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[3],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_B,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(ddhub_process(TEST_HUB_B) == DDHUB_OK);
    assert(hub_c_calls == 1U);
    ddhub_test_resume();
    assert(pthread_join(worker, NULL) == 0);
    assert(result == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[4],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[4]) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_C,
                          &nodes[3],
                          1U,
                          0U,
                          count_c,
                          NULL,
                          NULL,
                          NULL) == DDHUB_ALREADY_REGISTERED);
    assert(ddhub_unregister(&nodes[3]) == DDHUB_OK);
}

enum {
    PRODUCER_COUNT = 4,
    MESSAGES_PER_PRODUCER = 500,
};

struct stress_message {
    unsigned int producer;
    unsigned int sequence;
    unsigned int visits;
    bool         completed;
};

static struct stress_message messages[PRODUCER_COUNT][MESSAGES_PER_PRODUCER];
static unsigned int          expected_sequence[PRODUCER_COUNT];
static unsigned int          producer_ids[PRODUCER_COUNT];
static unsigned int          completed_count;

/**
 * @brief Validate and mutate a message in priority order during concurrent production.
 * @param event Matched mask.
 * @param data Stress message.
 * @param len Stress message size.
 * @param arg1 Expected preceding callback count.
 * @param arg2 Unused context.
 * @param arg3 Unused context.
 */
static void stress_consume(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                           void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)arg2;
    (void)arg3;
    struct stress_message *message = data;
    assert((event == 1U) && (len == sizeof(*message)));
    assert(message->visits == *(unsigned int *)arg1);
    assert(!message->completed);
    ++message->visits;
}

/**
 * @brief Verify producer routing, per-producer FIFO order, and exactly-once completion.
 * @param hub_id Destination hub.
 * @param event Original mask.
 * @param data Stress message.
 * @param len Stress message size.
 * @param arg Same message address supplied as producer context.
 */
static void stress_done(ddhub_id_t hub_id, uint32_t event, void *const data, size_t len, void *const arg)
{
    ddhub_test_assert_unlocked();
    struct stress_message *message = data;
    assert((data == arg) && (len == sizeof(*message)) && (event == 1U));
    assert((unsigned int)hub_id == (message->producer % 2U));
    assert(message->visits == 2U);
    assert(message->sequence == expected_sequence[message->producer]++);
    assert(!message->completed);
    message->completed = true;
    ++completed_count;
}

/**
 * @brief Submit persistent unique buffers while retrying only documented transient failures.
 * @param arg Producer index.
 * @return NULL after all messages have been accepted.
 */
static void *producer_thread(void *arg)
{
    unsigned int producer = *(unsigned int *)arg;
    for (unsigned int i = 0U; i < MESSAGES_PER_PRODUCER; ++i) {
        struct stress_message *message = &messages[producer][i];
        message->producer = producer;
        message->sequence = i;
        ddhub_status_t status;
        do {
            status = ddhub_send((ddhub_id_t)(producer % 2U),
                                1U,
                                message,
                                sizeof(*message),
                                message);
            assert((status == DDHUB_OK) || (status == DDHUB_BUSY) || (status == DDHUB_QUEUE_FULL));
            if (status != DDHUB_OK) {
                sched_yield();
            }
        } while (status != DDHUB_OK);
    }
    return NULL;
}

/** @brief Stress borrowed pointer publication and notification with four concurrent producers. */
static void test_multi_producer(void)
{
    static unsigned int first_visit = 0U;
    static unsigned int second_visit = 1U;
    pthread_t           producers[PRODUCER_COUNT];
    for (size_t i = 0U; i < 2U; ++i) {
        assert(ddhub_set_process_done((ddhub_id_t)i, stress_done) == DDHUB_OK);
        assert(ddhub_register((ddhub_id_t)i,
                              &nodes[i * 2U],
                              1U,
                              0U,
                              stress_consume,
                              &first_visit,
                              NULL,
                              NULL) == DDHUB_OK);
        assert(ddhub_register((ddhub_id_t)i,
                              &nodes[(i * 2U) + 1U],
                              1U,
                              10U,
                              stress_consume,
                              &second_visit,
                              NULL,
                              NULL) == DDHUB_OK);
    }
    for (unsigned int i = 0U; i < PRODUCER_COUNT; ++i) {
        producer_ids[i] = i;
        assert(pthread_create(&producers[i],
                              NULL,
                              producer_thread,
                              &producer_ids[i]) == 0);
    }
    /* Completion fields are accessed only by this single processing thread. */
    while (completed_count != (PRODUCER_COUNT * MESSAGES_PER_PRODUCER)) {
        ddhub_status_t status = ddhub_process_all();
        assert((status == DDHUB_OK) || (status == DDHUB_BUSY));
        sched_yield();
    }
    for (size_t i = 0U; i < PRODUCER_COUNT; ++i) {
        assert(pthread_join(producers[i], NULL) == 0);
        assert(expected_sequence[i] == MESSAGES_PER_PRODUCER);
        for (size_t j = 0U; j < MESSAGES_PER_PRODUCER; ++j) {
            assert(messages[i][j].completed);
        }
    }
    for (size_t i = 0U; i < 4U; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
    }
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_OK);
    assert(ddhub_set_process_done(TEST_HUB_B, NULL) == DDHUB_OK);
}

/**
 * @brief Run real-thread exclusion and message-publication tests with bounded CTest timeouts.
 * @return Zero if every assertion passes.
 */
int main(void)
{
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_OK);
    test_bool_lock_contention();
    test_parallel_registration();
    test_shared_node_registration();
    test_node_owner_transfer();
    test_parallel_hubs();
    test_all_exclusion();
    test_all_skips_processing_hub();
    test_all_snapshot_during_send();
    test_multi_producer();
    ddhub_test_assert_unlocked();
    puts("ddhub concurrency: parallel hubs, ALL busy-hub skipping, 2000 messages passed");
    return 0;
}
