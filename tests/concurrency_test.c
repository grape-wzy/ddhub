/**
 * @file concurrency_test.c
 * @brief Verify concurrent ddhub publication and processing.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Use real threads and controlled pauses to verify live node safety, drain handoff, and retained wakeups.
 * @version 0.2.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-14： Add concurrency admission and a 2000-message multi-producer test.
 * - Codex 2026-10-15： Document file responsibilities, development dates, and modification history.
 * - Codex 2026-10-15： Inject a pthread adapter to test bool lock contention and processing cleanup without atomics.
 * - Codex 2026-10-15： Test parallel registration, shared-node contention, and ownership transfer after unregistration.
 * - Codex 2026-10-15： Test busy-hub skipping, independent cleanup, and continued ALL dispatch after contention.
 * - Codex 2026-10-15： Use list reservations to test contention and concurrent enqueue against ALL snapshots.
 * - Codex 2026-10-15： Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Test live nodes, writer interleavings, drain handoff, and binary wakeups.
 * - Codex 2026-10-16：Release version 0.2.0.
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
#include <stdlib.h>

struct gate {
    pthread_mutex_t mutex;
    pthread_cond_t  condition;
    bool            entered;
    bool            released;
    unsigned int    calls;
};
static struct gate gate = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    false,
    false,
    0U
};
static ddhub_subscription_t nodes[5];
static unsigned int         captures;

struct request {
    ddhub_subscription_t *node;
    ddhub_id_t            hub;
    ddhub_status_t        status;
    bool                  pause;
};

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
    (void)event;
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    ++captures;
}

/**
 * @brief Hold the active callback until the coordinating thread releases it.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void block(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)event;
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    ++gate.calls;
    gate.entered = true;
    assert(pthread_cond_broadcast(&gate.condition) == 0);
    while (!gate.released) {
        assert(pthread_cond_wait(&gate.condition, &gate.mutex) == 0);
    }
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
}

/**
 * @brief Wait until the consumer has pinned its blocking subscription.
 */
static void await_gate(void)
{
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    while (!gate.entered) {
        assert(pthread_cond_wait(&gate.condition, &gate.mutex) == 0);
    }
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
}

/**
 * @brief Allow the pinned callback to return.
 */
static void release_gate(void)
{
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    gate.released = true;
    assert(pthread_cond_broadcast(&gate.condition) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
}

/**
 * @brief Reset a gate after all previous users have joined.
 */
static void reset_gate(void)
{
    gate.entered = false;
    gate.released = false;
    gate.calls = 0U;
}

/**
 * @brief Dispatch one request from a separate consumer thread.
 * @param arg Caller-owned context retained until completion.
 * @return NULL after the worker completes.
 */
static void *process_thread(void *arg)
{
    struct request *request = arg;
    if (request->pause) {
        ddhub_test_pause_next_unlock();
    }
#if DDHUB_ASYNC_PROCESS_ENABLE
    request->status = ddhub_process();
#else
    request->status = ddhub_send(TEST_HUB_A,
                                 1U,
                                 NULL,
                                 0U,
                                 NULL);
#endif
    return NULL;
}

/**
 * @brief Attempt removal after an optional controlled writer pause.
 * @param arg Caller-owned context retained until completion.
 * @return NULL after the worker completes.
 */
static void *unregister_thread(void *arg)
{
    struct request *request = arg;
    if (request->pause) {
        ddhub_test_pause_next_unlock();
    }
    request->status = ddhub_unregister(request->node);
    return NULL;
}

/**
 * @brief Pause after claiming ownership and then publish a new subscription.
 * @param arg Caller-owned context retained until completion.
 * @return NULL after the worker completes.
 */
static void *register_thread(void *arg)
{
    struct request *request = arg;
    ddhub_test_pause_next_unlock();
    request->status = ddhub_register(request->hub,
                                     request->node,
                                     1U,
                                     10U,
                                     capture,
                                     NULL,
                                     NULL,
                                     NULL);
    return NULL;
}

/**
 * @brief Queue the first asynchronous message before starting the consumer.
 */
static void send_initial(void)
{
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#endif
}

/**
 * @brief Test live nodes.
 */
static void test_live_nodes(void)
{
    reset_gate();
    captures = 0U;
    ddhub_subscription_t *tail = calloc(1U, sizeof(*tail));
    assert(tail != NULL);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          block,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          tail,
                          1U,
                          20U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    send_initial();
    struct request process = {0};
    pthread_t      thread;
    assert(pthread_create(&thread,
                          NULL,
                          process_thread,
                          &process) == 0);
    await_gate();
    assert(ddhub_unregister(&nodes[0]) == DDHUB_BUSY);
    assert(ddhub_unregister(tail) == DDHUB_OK);
    free(tail);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[1],
                          1U,
                          10U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_BUSY);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#else
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_BUSY);
#endif
    assert(ddhub_set_process_done(TEST_HUB_A, NULL) == DDHUB_BUSY);
    release_gate();
    assert(pthread_join(thread, NULL) == 0);
    assert(process.status == DDHUB_OK);
#if !DDHUB_ASYNC_PROCESS_ENABLE
    assert(captures == 0U);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#endif
    assert((captures == 1U) && (gate.calls == 2U));
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[1]) == DDHUB_OK);
}

/**
 * @brief Test unregister commit recheck.
 */
static void test_unregister_commit_recheck(void)
{
    reset_gate();
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          block,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    send_initial();
    struct request removal = {
        &nodes[0],
        TEST_HUB_A,
        DDHUB_INVALID,
        true
    };
    pthread_t remover;
    assert(pthread_create(&remover,
                          NULL,
                          unregister_thread,
                          &removal) == 0);
    ddhub_test_wait_paused();
    struct request process = {0};
    pthread_t      consumer;
    assert(pthread_create(&consumer,
                          NULL,
                          process_thread,
                          &process) == 0);
    await_gate();
    ddhub_test_resume();
    assert(pthread_join(remover, NULL) == 0);
    assert(removal.status == DDHUB_BUSY);
    release_gate();
    assert(pthread_join(consumer, NULL) == 0);
    assert(process.status == DDHUB_OK);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
}

/**
 * @brief Test writer does not block dispatch.
 */
static void test_writer_does_not_block_dispatch(void)
{
    captures = 0U;
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    struct request registration = {
        &nodes[1],
        TEST_HUB_A,
        DDHUB_INVALID,
        false
    };
    pthread_t writer;
    assert(pthread_create(&writer,
                          NULL,
                          register_thread,
                          &registration) == 0);
    ddhub_test_wait_paused();
    assert(ddhub_register(TEST_HUB_B,
                          &nodes[1],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_ALREADY_REGISTERED);
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[2],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_BUSY);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_OK);
#endif
    assert(captures == 1U);
    ddhub_test_resume();
    assert(pthread_join(writer, NULL) == 0);
    assert(registration.status == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_OK);
#endif
    assert(captures == 3U);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
    assert(ddhub_unregister(&nodes[1]) == DDHUB_OK);
}

#if DDHUB_ASYNC_PROCESS_ENABLE
#define PRODUCERS                                           4U
#define PER_PRODUCER                                        500U
struct message {
    size_t producer;
    size_t sequence;
    size_t checksum;
};
static struct message  messages[PRODUCERS][PER_PRODUCER];
static size_t          received[PRODUCERS];
static size_t          completed;
static unsigned int    seen[PRODUCERS][PER_PRODUCER];
static pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake_condition = PTHREAD_COND_INITIALIZER;
static bool            notified;

/**
 * @brief Retain one pending wakeup outside platform exclusion.
 */
static void notify(void)
{
    ddhub_test_assert_unlocked();
    assert(pthread_mutex_lock(&wake_mutex) == 0);
    notified = true;
    assert(pthread_cond_signal(&wake_condition) == 0);
    assert(pthread_mutex_unlock(&wake_mutex) == 0);
}

/**
 * @brief Validate the received message and its producer sequence.
 * @param event Matched application event mask.
 * @param data Borrowed message payload.
 * @param len Payload size in bytes.
 * @param arg1 First subscriber test context.
 * @param arg2 Second subscriber test context.
 * @param arg3 Third subscriber test context.
 */
static void receive(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    ddhub_test_assert_unlocked();
    (void)arg1;
    (void)arg2;
    (void)arg3;
    assert(event == 1U);
    assert(len == sizeof(struct message));
    const struct message *message = data;
    assert(message->producer < PRODUCERS);
    assert(message->sequence == received[message->producer]++);
    assert(message->checksum == ((message->producer * 10000U) + message->sequence));
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
    const struct message *message = data;
    assert((event == 1U) && (len == sizeof(*message)) && (arg == data));
    assert(hub_id == (ddhub_id_t)(message->producer % DDHUB_ID_COUNT));
    assert(seen[message->producer][message->sequence]++ == 0U);
    ++completed;
}

/**
 * @brief Publish one producer stream, retrying only a full waiting FIFO.
 * @param arg Caller-owned context retained until completion.
 * @return NULL after the worker completes.
 */
static void *producer_thread(void *arg)
{
    size_t producer = *(size_t *)arg;
    for (size_t i = 0U; i < PER_PRODUCER; ++i) {
        struct message *message = &messages[producer][i];
        *message = (struct message){producer, i, (producer * 10000U) + i};
        ddhub_status_t status;
        do {
            status = ddhub_send((ddhub_id_t)(producer % DDHUB_ID_COUNT),
                                1U,
                                message,
                                sizeof(*message),
                                message);
            if (status == DDHUB_QUEUE_FULL) {
                sched_yield();
            }
        } while (status == DDHUB_QUEUE_FULL);
        assert(status == DDHUB_OK);
    }
    return NULL;
}

/**
 * @brief Consume a retained binary wakeup and drain on the unique worker.
 * @param arg Caller-owned context retained until completion.
 * @return NULL after the worker completes.
 */
static void *consumer_thread(void *arg)
{
    (void)arg;
    while (completed < (PRODUCERS * PER_PRODUCER)) {
        assert(pthread_mutex_lock(&wake_mutex) == 0);
        while (!notified) {
            assert(pthread_cond_wait(&wake_condition, &wake_mutex) == 0);
        }
        notified = false;
        assert(pthread_mutex_unlock(&wake_mutex) == 0);
        assert(ddhub_process() == DDHUB_OK);
    }
    return NULL;
}

/**
 * @brief Test multi producer binary wakeup.
 */
static void test_multi_producer_binary_wakeup(void)
{
    assert(ddhub_set_process_notify(notify) == DDHUB_OK);
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        assert(ddhub_register((ddhub_id_t)i,
                              &nodes[i],
                              1U,
                              0U,
                              receive,
                              NULL,
                              NULL,
                              NULL) == DDHUB_OK);
        assert(ddhub_set_process_done((ddhub_id_t)i, complete) == DDHUB_OK);
    }
    pthread_t consumer;
    pthread_t producers[PRODUCERS];
    size_t    ids[PRODUCERS];
    assert(pthread_create(&consumer,
                          NULL,
                          consumer_thread,
                          NULL) == 0);
    for (size_t i = 0U; i < PRODUCERS; ++i) {
        ids[i] = i;
        assert(pthread_create(&producers[i],
                              NULL,
                              producer_thread,
                              &ids[i]) == 0);
    }
    for (size_t i = 0U; i < PRODUCERS; ++i) {
        assert(pthread_join(producers[i], NULL) == 0);
    }
    assert(pthread_join(consumer, NULL) == 0);
    assert(completed == (PRODUCERS * PER_PRODUCER));
    for (size_t i = 0U; i < PRODUCERS; ++i) {
        assert(received[i] == PER_PRODUCER);
    }
    for (size_t i = 0U; i < DDHUB_ID_COUNT; ++i) {
        assert(ddhub_unregister(&nodes[i]) == DDHUB_OK);
        assert(ddhub_set_process_done((ddhub_id_t)i, NULL) == DDHUB_OK);
    }
}

/** @brief Verify a send after the final empty check schedules new work before the old call returns. */
static void test_final_check_handoff(void)
{
    assert(pthread_mutex_lock(&wake_mutex) == 0);
    notified = false;
    assert(pthread_mutex_unlock(&wake_mutex) == 0);
    captures = 0U;
    assert(ddhub_register(TEST_HUB_A,
                          &nodes[0],
                          1U,
                          0U,
                          capture,
                          NULL,
                          NULL,
                          NULL) == DDHUB_OK);
    struct request process = {
        NULL,
        TEST_HUB_A,
        DDHUB_INVALID,
        true
    };
    pthread_t consumer;
    assert(pthread_create(&consumer,
                          NULL,
                          process_thread,
                          &process) == 0);
    /* An empty process unlocks only after its final check and active-state release. */
    ddhub_test_wait_paused();
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      NULL,
                      0U,
                      NULL) == DDHUB_OK);
    assert(pthread_mutex_lock(&wake_mutex) == 0);
    assert(notified);
    assert(pthread_mutex_unlock(&wake_mutex) == 0);
    assert(ddhub_process() == DDHUB_OK);
    assert(captures == 1U);
    ddhub_test_resume();
    assert(pthread_join(consumer, NULL) == 0);
    assert(process.status == DDHUB_OK);
    assert(ddhub_unregister(&nodes[0]) == DDHUB_OK);
}
#endif

/**
 * @brief Execute this suite and fail on any violated dispatch contract.
 * @return Zero after all assertions pass.
 */
int main(void)
{
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_OK);
    test_live_nodes();
    test_unregister_commit_recheck();
    test_writer_does_not_block_dispatch();
#if DDHUB_ASYNC_PROCESS_ENABLE
    test_multi_producer_binary_wakeup();
    test_final_check_handoff();
#endif
    puts("ddhub concurrency passed");
    return 0;
}
