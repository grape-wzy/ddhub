/**
 * @file main.c
 * @brief Run the two-hub logging example on Linux.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Bind a static pthread mutex adapter and print the shared four-event demonstration to the console.
 * @version 0.2.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-15：Add the Linux lock adapter and example entry point.
 * - Codex 2026-10-15：Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#include "ddhub.h"
#include "ddhub_example.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t platform_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Enter the Linux thread exclusion region.
 * @return Zero token; a pthread mutex does not require interrupt-state restoration.
 */
static ddhub_lock_key_t linux_lock(void)
{
    if (pthread_mutex_lock(&platform_mutex) != 0) {
        abort();
    }
    return 0U;
}

/**
 * @brief Leave the Linux thread exclusion region.
 * @param key Unused token returned by linux_lock().
 */
static void linux_unlock(ddhub_lock_key_t key)
{
    (void)key;
    if (pthread_mutex_unlock(&platform_mutex) != 0) {
        abort();
    }
}

/**
 * @brief Bind the Linux adapter and run the four-message demonstration once.
 * @return EXIT_SUCCESS on success, otherwise EXIT_FAILURE after printing the ddhub status.
 */
int main(void)
{
    ddhub_status_t status = ddhub_set_lock_ops(linux_lock, linux_unlock);
    if (status == DDHUB_OK) {
        status = ddhub_example_run("Linux");
    }
    if (status != DDHUB_OK) {
        fprintf(stderr, "Linux ddhub example failed: status=%d\n", (int)status);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
