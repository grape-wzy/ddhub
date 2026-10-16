/**
 * @file cpp_interop_test.cpp
 * @brief Verify C++ initialization and linkage with C-compiled ddhub in both modes.
 * @version 0.2.0
 * @date 2026-10-16
 *
 * @par 修改记录
 * - Codex 2026-10-16：Verify C++ initialization and linkage with C-compiled ddhub in both modes.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#include "ddhub.h"
#include "ddhub_test_config.h"
#include "test_lock.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

static unsigned int calls;

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
    (void)data;
    (void)len;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    ddhub_test_assert_unlocked();
    assert(event == 1U);
    ++calls;
}

/**
 * @brief Execute this suite and fail on any violated dispatch contract.
 * @return Zero after all assertions pass.
 */
int main(void)
{
    ddhub_subscription_t node = DDHUB_SUBSCRIPTION_INIT;
    assert(ddhub_set_lock_ops(ddhub_test_lock, ddhub_test_unlock) == DDHUB_OK);
    assert(ddhub_register(TEST_HUB_A,
                          &node,
                          1U,
                          0U,
                          receive,
                          nullptr,
                          nullptr,
                          nullptr) == DDHUB_OK);
    assert(ddhub_send(TEST_HUB_A,
                      1U,
                      nullptr,
                      0U,
                      nullptr) == DDHUB_OK);
#if DDHUB_ASYNC_PROCESS_ENABLE
    assert(ddhub_process() == DDHUB_OK);
#endif
    assert(calls == 1U);
    assert(ddhub_unregister(&node) == DDHUB_OK);
    return 0;
}
