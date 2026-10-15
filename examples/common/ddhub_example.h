/**
 * @file ddhub_example.h
 * @brief Declare the shared two-hub logging example.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Run shared registration, publication, and dispatch after the platform has bound its own lock adapter.
 * @version 0.1.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-15: Declare the shared platform example interface.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#ifndef DDHUB_EXAMPLE_H_
#define DDHUB_EXAMPLE_H_

#include "ddhub.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register two callbacks per hub, publish messages, and print their routing.
 * @param platform Platform label retained through this call; a string literal is suitable.
 * @return DDHUB_OK, or the first failed ddhub operation's status.
 *
 * Call once after binding platform lock operations and initializing printf output.
 * Payloads and subscription nodes use static storage. Successful processing unregisters
 * all four nodes. A failure keeps any accepted static payloads valid.
 */
ddhub_status_t ddhub_example_run(const char *platform);

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_EXAMPLE_H_ */
