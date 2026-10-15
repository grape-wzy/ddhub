/**
 * @file ddhub_example.c
 * @brief Demonstrate two callbacks per hub with four distinct event subscriptions.
 * @details ddhub stands for Data Dispatch Hub, an event-driven data distribution center.
 *          Use static nodes and messages to print the platform, source, content, destination hub, callback, and event
 * mask.
 * @version 0.1.0
 * @date 2026-10-15 (development date)
 *
 * @par Release notes
 * 0.1.0: Initial version; first release.
 *
 * @par Modification history
 * - Codex 2026-10-15: Add shared registration, publication, and dispatch for three platform examples.
 * - Codex 2026-10-15: Prepare version 0.1.0 with English source documentation.
 */

#include "ddhub_example.h"
#include "ddhub.h"
#include "ddhub_example_config.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define EVENT_TEMPERATURE                                   (UINT32_C(1) << 0)
#define EVENT_HUMIDITY                                      (UINT32_C(1) << 1)
#define EVENT_COMMAND                                       (UINT32_C(1) << 2)
#define EVENT_ALARM                                         (UINT32_C(1) << 3)

struct example_message {
    const char *platform;
    const char *source;
    const char *content;
};

static ddhub_subscription_t   subscriptions[4];
static struct example_message messages[] = {
    {.source = "temperature_sensor", .content = "temperature=25 C"},
    {.source = "humidity_sensor", .content = "humidity=60 percent"},
    {.source = "operator", .content = "command=start"},
    {.source = "alarm_monitor", .content = "alarm=clear"},
};

/**
 * @brief Print the source, content, and callback routing of a validated message descriptor.
 * @param hub Human-readable destination hub.
 * @param callback Name of the callback that is processing the message.
 * @param event Matched event mask.
 * @param data Borrowed example message.
 * @param len Size of the example message structure.
 */
static void print_message(const char *hub, const char *callback, uint32_t event, void *const data, size_t len)
{
    if ((data == NULL) || (len != sizeof(struct example_message))) {
        printf("hub=%s callback=%s invalid message\n", hub, callback);
        return;
    }
    const struct example_message *message = data;
    printf("[%s] source=%s message=\"%s\" hub=%s callback=%s event=0x%08" PRIx32 "\n",
           message->platform,
           message->source,
           message->content,
           hub,
           callback,
           event);
}

/**
 * @brief Print the sensor hub's temperature event.
 * @param event Matched temperature event.
 * @param data Borrowed message structure.
 * @param len Message structure size.
 * @param arg1 Unused subscriber context.
 * @param arg2 Unused subscriber context.
 * @param arg3 Unused subscriber context.
 */
static void on_temperature(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                           void *const arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    print_message("SENSOR",
                  "on_temperature",
                  event,
                  data,
                  len);
}

/**
 * @brief Print the sensor hub's humidity event.
 * @param event Matched humidity event.
 * @param data Borrowed message structure.
 * @param len Message structure size.
 * @param arg1 Unused subscriber context.
 * @param arg2 Unused subscriber context.
 * @param arg3 Unused subscriber context.
 */
static void on_humidity(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                        void *const arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    print_message("SENSOR",
                  "on_humidity",
                  event,
                  data,
                  len);
}

/**
 * @brief Print the control hub's command event.
 * @param event Matched command event.
 * @param data Borrowed message structure.
 * @param len Message structure size.
 * @param arg1 Unused subscriber context.
 * @param arg2 Unused subscriber context.
 * @param arg3 Unused subscriber context.
 */
static void on_command(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2,
                       void *const arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    print_message("CONTROL",
                  "on_command",
                  event,
                  data,
                  len);
}

/**
 * @brief Print the control hub's alarm event.
 * @param event Matched alarm event.
 * @param data Borrowed message structure.
 * @param len Message structure size.
 * @param arg1 Unused subscriber context.
 * @param arg2 Unused subscriber context.
 * @param arg3 Unused subscriber context.
 */
static void on_alarm(uint32_t event, void *const data, size_t len, void *const arg1, void *const arg2, void *const arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    print_message("CONTROL",
                  "on_alarm",
                  event,
                  data,
                  len);
}

ddhub_status_t ddhub_example_run(const char *platform)
{
    static const struct {
        ddhub_id_t       hub_id;
        uint32_t         event;
        ddhub_callback_t callback;
    } routes[] = {
        {EXAMPLE_HUB_SENSOR, EVENT_TEMPERATURE, on_temperature},
        {EXAMPLE_HUB_SENSOR, EVENT_HUMIDITY, on_humidity},
        {EXAMPLE_HUB_CONTROL, EVENT_COMMAND, on_command},
        {EXAMPLE_HUB_CONTROL, EVENT_ALARM, on_alarm},
    };

    if (platform == NULL) {
        return DDHUB_INVALID;
    }
    for (size_t i = 0U; i < (sizeof(routes) / sizeof(routes[0])); ++i) {
        ddhub_status_t status = ddhub_register(routes[i].hub_id,
                                               &subscriptions[i],
                                               routes[i].event,
                                               0U,
                                               routes[i].callback,
                                               NULL,
                                               NULL,
                                               NULL);
        if (status != DDHUB_OK) {
            return status;
        }
    }
    for (size_t i = 0U; i < (sizeof(routes) / sizeof(routes[0])); ++i) {
        messages[i].platform = platform;
        ddhub_status_t status = ddhub_send(routes[i].hub_id,
                                           routes[i].event,
                                           &messages[i],
                                           sizeof(messages[i]),
                                           NULL);
        if (status != DDHUB_OK) {
            return status;
        }
    }

    /* An explicit hub call consumes SENSOR only; ALL then consumes CONTROL. */
    ddhub_status_t status = ddhub_process(EXAMPLE_HUB_SENSOR);
    if (status != DDHUB_OK) {
        return status;
    }
    status = ddhub_process_all();
    if (status != DDHUB_OK) {
        return status;
    }
    for (size_t i = 0U; i < (sizeof(subscriptions) / sizeof(subscriptions[0])); ++i) {
        status = ddhub_unregister(&subscriptions[i]);
        if (status != DDHUB_OK) {
            return status;
        }
    }
    return DDHUB_OK;
}
