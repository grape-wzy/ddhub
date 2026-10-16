/**
 * @file ddhub_stm32_example.h
 * @brief Declare the STM32 bare-metal example entry point.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Let board code run the example after initializing clocks and printf output, without RTOS or HAL
 * synchronization.
 * @version 0.2.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.2.0: Dispatch modes, global notification, draining, and live subscriptions.
 * 0.1.0: Initial version; first release.
 *
 * @par 修改记录
 * - Codex 2026-10-15：Declare the bare-metal integration entry point.
 * - Codex 2026-10-15：Prepare version 0.1.0 with English source documentation.
 * - Codex 2026-10-16：Release version 0.2.0.
 */

#ifndef DDHUB_STM32_EXAMPLE_H_
#define DDHUB_STM32_EXAMPLE_H_

#include "ddhub.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind the STM32 interrupt adapter and print the four-message example once.
 * @return DDHUB_OK, or the first failed platform binding or example operation's status.
 *
 * Call from the main loop after printf retargeting, before any other ddhub user starts.
 * This adapter targets one Cortex-M core; NMI and HardFault must not access ddhub.
 */
ddhub_status_t ddhub_stm32_example_run(void);

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_STM32_EXAMPLE_H_ */
