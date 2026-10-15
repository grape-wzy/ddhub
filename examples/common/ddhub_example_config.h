/**
 * @file ddhub_example_config.h
 * @brief Define two hubs shared by the platform examples.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Configure separate sensor and control hubs with four static queue slots each, independently of firmware
 * settings.
 * @version 0.1.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-15: Provide the two-hub example configuration.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#ifndef DDHUB_EXAMPLE_CONFIG_H_
#define DDHUB_EXAMPLE_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define DDHUB_FIFO_DEPTH                                    4U

typedef enum {
    EXAMPLE_HUB_SENSOR = 0,
    EXAMPLE_HUB_CONTROL,
    DDHUB_ID_COUNT,
} ddhub_id_t;

#ifdef __cplusplus
}
#endif

#endif /* DDHUB_EXAMPLE_CONFIG_H_ */
