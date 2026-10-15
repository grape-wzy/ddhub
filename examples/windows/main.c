/**
 * @file main.c
 * @brief Run the two-hub logging example on Windows.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Use a static SRWLOCK in exclusive mode and print the shared four-event demonstration to the console.
 * @version 0.1.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-15: Add the Windows lock adapter and example entry point.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT                                        0x0600
#endif
#define WIN32_LEAN_AND_MEAN

#include "ddhub.h"
#include "ddhub_example.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static SRWLOCK platform_lock = SRWLOCK_INIT;

/**
 * @brief Acquire the Windows exclusion lock in exclusive mode.
 * @return Zero token; the SRW lock requires no interrupt-state restoration.
 */
static ddhub_lock_key_t windows_lock(void)
{
    AcquireSRWLockExclusive(&platform_lock);
    return 0U;
}

/**
 * @brief Release the Windows exclusion lock.
 * @param key Unused token returned by windows_lock().
 */
static void windows_unlock(ddhub_lock_key_t key)
{
    (void)key;
    ReleaseSRWLockExclusive(&platform_lock);
}

/**
 * @brief Bind the Windows adapter and run the four-message demonstration once.
 * @return EXIT_SUCCESS on success, otherwise EXIT_FAILURE after printing the ddhub status.
 */
int main(void)
{
    ddhub_status_t status = ddhub_set_lock_ops(windows_lock, windows_unlock);
    if (status == DDHUB_OK) {
        status = ddhub_example_run("Windows");
    }
    if (status != DDHUB_OK) {
        fprintf(stderr, "Windows ddhub example failed: status=%d\n", (int)status);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
