/**
 * @file test_lock.h
 * @brief Declare the host-test platform lock adapter.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Expose pthread exclusion, thread-local validation, pause controls, and acquisition counters for ddhub tests.
 * @version 0.2.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-15：Declare host validation interfaces for injected platform locks.
 * - Codex 2026-10-15：Allow pauses after a chosen unlock count to test cross-hub node transfer.
 * - Codex 2026-10-15：Expose platform acquisition counts to verify merged critical sections.
 * - Codex 2026-10-15：Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#ifndef DDHUB_TEST_LOCK_H_
#define DDHUB_TEST_LOCK_H_

#include "ddhub.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Acquire the test mutex and mark the current thread inside platform exclusion.
 * @return Unique acquisition token for ddhub_test_unlock().
 */
ddhub_lock_key_t ddhub_test_lock(void);

/**
 * @brief Validate the token and leave platform exclusion.
 * @param key Token returned by the corresponding ddhub_test_lock() call.
 */
void ddhub_test_unlock(ddhub_lock_key_t key);

/** @brief Assert that the current callback is not running inside platform exclusion. */
void ddhub_test_assert_unlocked(void);

/**
 * @brief Read the completed acquisition count without adding a platform lock call.
 * @return Total adapter lock calls; compare deltas only while other workers are inactive.
 */
ddhub_lock_key_t ddhub_test_lock_count(void);

/** @brief Arm a pause after this thread's next platform unlock, outside the mutex. */
void ddhub_test_pause_next_unlock(void);

/**
 * @brief Pause this thread after a selected number of future platform unlocks.
 * @param count Positive unlock count; only one controlled worker is armed at a time.
 */
void ddhub_test_pause_after_unlocks(size_t count);

/** @brief Wait until the armed worker has left the platform mutex and reached its pause. */
void ddhub_test_wait_paused(void);

/** @brief Release the worker paused after its platform unlock. */
void ddhub_test_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_TEST_LOCK_H_ */
