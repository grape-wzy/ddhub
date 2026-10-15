/**
 * @file ddhub_test_config.h
 * @brief Configure hub IDs and FIFO capacity for host tests.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Provide three independent hubs and a three-message FIFO to test isolation and non-power-of-two wraparound.
 * @version 0.1.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-14: Provide hub configuration independent of the firmware.
 * - Codex 2026-10-15: Add file documentation and C++ linkage guards.
 * - Codex 2026-10-15: Update the hub count enum name and separate ALL processing API.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#ifndef DDHUB_TEST_CONFIG_H_
#define DDHUB_TEST_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define DDHUB_FIFO_DEPTH                                    3U

typedef enum {
    TEST_HUB_A = 0,
    TEST_HUB_B,
    TEST_HUB_C,
    DDHUB_ID_COUNT,
} ddhub_id_t;

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_TEST_CONFIG_H_ */
