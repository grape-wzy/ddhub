/**
 * @file ddhub_stm32_example.c
 * @brief Provide a single-core STM32 bare-metal lock adapter and logging example.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Save and restore PRIMASK with CMSIS barriers; board code supplies printf output and hardware initialization.
 * @version 0.2.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-15：Add the STM32 example adapter without RTOS dependencies.
 * - Codex 2026-10-15：Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#include "ddhub_stm32_example.h"
#include "ddhub.h"
#include "ddhub_example.h"

#ifndef DDHUB_STM32_DEVICE_HEADER
#define DDHUB_STM32_DEVICE_HEADER                           "stm32h723xx.h"
#endif
#include DDHUB_STM32_DEVICE_HEADER

#include <cmsis_compiler.h>
#include <stdint.h>

/**
 * @brief Save interrupt state and protect the single-core platform critical section.
 * @return Previous PRIMASK value, restored by the matching stm32_unlock().
 */
static ddhub_lock_key_t stm32_lock(void)
{
    uint32_t key = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return (ddhub_lock_key_t)key;
}

/**
 * @brief Publish shared writes and restore the saved interrupt mask.
 * @param key Previous PRIMASK value returned by the matching stm32_lock().
 */
static void stm32_unlock(ddhub_lock_key_t key)
{
    __DMB();
    __set_PRIMASK((uint32_t)key);
}

ddhub_status_t ddhub_stm32_example_run(void)
{
    ddhub_status_t status = ddhub_set_lock_ops(stm32_lock, stm32_unlock);
    if (status != DDHUB_OK) {
        return status;
    }
    return ddhub_example_run("STM32 baremetal");
}
