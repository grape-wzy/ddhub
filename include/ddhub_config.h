/**
 * @file ddhub_config.h
 * @brief Configure dispatch hub IDs and static FIFO capacity.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Let applications define consecutive hub IDs and queue depth in a C/C++ compatible configuration header.
 * @version 0.1.0
 * @date 2026-10-14 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-14: Provide a default hub and FIFO capacity.
 * - Codex 2026-10-15: Add file documentation and C++ linkage guards.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#ifndef DDHUB_CONFIG_H_
#define DDHUB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of waiting messages stored independently by each hub. */
#ifndef DDHUB_FIFO_DEPTH
#define DDHUB_FIFO_DEPTH                                    8U
#endif

/** @brief User-owned consecutive hub IDs; add application IDs before DDHUB_ID_COUNT. */
typedef enum {
    DDHUB_DEFAULT = 0,
    DDHUB_ID_COUNT,
} ddhub_id_t;

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_CONFIG_H_ */
