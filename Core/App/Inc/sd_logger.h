/*
 * sd_logger.h
 *
 * SD-card CSV logging sink for the CAN logger / anomaly detector.
 *
 * Producers (CanTask, anomaly module) push SdRecord_t items onto sdQueue via the
 * non-blocking SD_Log_Frame() / SD_Log_Alert() helpers. A dedicated SdTask
 * drains the queue, batches writes, appends to "canlog.csv", and periodically
 * f_sync()s so a power loss costs little data.
 *
 * Frame logging respects the current logger mode (the producer only enqueues
 * frames it would also log to UART). Alerts are always enqueued.
 */

#ifndef APP_INC_SD_LOGGER_H_
#define APP_INC_SD_LOGGER_H_

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "app.h"      /* AlertType_t */

/* Record kinds carried on sdQueue. */
typedef enum {
    SD_REC_FRAME = 0,
    SD_REC_ALERT
} SdRecKind_t;

/* One log record (frame or alert). Kept small for the queue. */
typedef struct {
    SdRecKind_t kind;
    uint32_t    timestamp;   /* HAL_GetTick() at capture */
    uint32_t    id;          /* CAN id (frame) or offending id (alert) */
    uint8_t     dlc;         /* frame DLC */
    uint8_t     data[8];     /* frame payload */
    uint8_t     alertType;   /* AlertType_t (alert records) */
    uint8_t     severity;    /* alert severity 1..3 */
} SdRecord_t;

/* Shared queue (created in main.c before the scheduler). */
extern QueueHandle_t sdQueue;

/* Initialize logger state. Call once before the scheduler. */
void SD_Logger_Init(void);

/* SdTask body (FreeRTOS task). */
void StartSdTask(void *argument);

/* Producer helpers - non-blocking, safe from task context (NOT ISR).
 * No-ops when logging is stopped or the queue is full (overflow counted). */
void SD_Log_Frame(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t ts);
void SD_Log_Alert(AlertType_t type, uint32_t id, uint8_t severity, uint32_t ts);

/* Start/stop logging (CLI). Start mounts the card + opens the file lazily in
 * the task; stop flushes and closes. Returns nothing - status via UART log. */
void SD_Logger_Start(void);
void SD_Logger_Stop(void);

/* 1 if logging is currently active. */
uint8_t SD_Logger_IsActive(void);

/* 1 if the last start request failed (mount/open/write). Cleared on next Start. */
uint8_t SD_Logger_HasError(void);

/* Cumulative count of CAN frames enqueued to SD while logging was active.
 * Only advances when logging is on; frozen while stopped. Reset by Init. */
uint32_t SD_Logger_FramesLogged(void);

#endif /* APP_INC_SD_LOGGER_H_ */
