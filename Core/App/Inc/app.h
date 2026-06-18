/*
 * app.h
 *
 * Shared types, queues, and prototypes for the CAN logger / anomaly detector.
 */

#ifndef APP_INC_APP_H_
#define APP_INC_APP_H_

#include <stdint.h>
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "stream_buffer.h"
#include "semphr.h"

extern CAN_HandleTypeDef hcan1;
extern UART_HandleTypeDef huart2;

/* Canonical CAN frame: CAN ISR -> CanTask via canRxQueue. */
typedef struct
{
    uint32_t id;
    uint32_t timestamp;
    uint8_t  dlc;
    uint8_t  data[8];
} CanFrame_t;

extern QueueHandle_t canRxQueue;          /* CAN ISR -> CanTask */
extern StreamBufferHandle_t uartRxStream; /* UART ISR -> CliTask */

/* Formatted log line: any task -> LoggerTask (sole UART TX owner). */
typedef struct
{
    char     text[120];
    uint16_t len;
} LogMsg_t;

extern QueueHandle_t logQueue;

/* Enqueue a string for LoggerTask to transmit. Safe from any task. */
void Log_Send(const char *text);

/* Running total of received frames (StatsTask reports it). */
extern volatile uint32_t statTotalFrames;

void CAN1_FilterConfig(void);
void PrintMenu(void);
void ProcessTxCommand(char *cmd);
uint8_t Logger_ShouldLog(uint32_t id);

/* Logger operating mode. */
typedef enum
{
    MODE_READ_ALL = 0,
    MODE_READ_FILTERED,
    MODE_SPEEDTEST,
    MODE_IDLE,
    MODE_WRITE
} LoggerMode_t;

/* Console input state machine (CliTask). */
typedef enum {
    UART_STATE_IDLE = 0,
    UART_STATE_MENU,
    UART_STATE_WAIT_FILTER_ID,
    UART_STATE_WAIT_TX_FRAME
} UART_State_t;


/* ==========================================================================
 * OLED page-based dashboard (CAN Logger / Anomaly Detection)
 * ========================================================================== */

/* The selectable dashboard pages. ALERT is an overlay (not in the 'p' cycle). */
typedef enum {
    OLED_PAGE_MAIN = 0,
    OLED_PAGE_FILTER,
    OLED_PAGE_SPEED,
    OLED_PAGE_STATS,
    OLED_PAGE_ALERT,
    OLED_PAGE_COUNT
} OLED_Page_t;

/* Anomaly classes carried by an alert. */
typedef enum {
    ALERT_NONE = 0,
    ALERT_UNKNOWN_ID,
    ALERT_FLOOD,
    ALERT_TIMING,
    ALERT_PAYLOAD
} AlertType_t;

/* One alert event, pushed onto oledAlertQueue. */
typedef struct {
    AlertType_t type;
    uint32_t    id;
    uint8_t     severity;   /* 1..3 */
} OledAlert_t;

/* Single source of truth for everything the OLED displays. Producers publish a
 * snapshot under g_oledMutex; the OLED task reads a local copy under the same
 * mutex. NOT written from ISRs. */
typedef struct {
    LoggerMode_t mode;
    uint32_t last_id;
    uint8_t  last_dlc;
    uint8_t  last_data[8];
    uint8_t  have_frame;
    uint32_t fps;          /* frames/sec, computed by the OLED task */
    uint32_t peak_fps;     /* highest fps seen, latched by the OLED task */
    uint32_t total;        /* all frames received */
    uint32_t logged;       /* frames logged to UART (READ_ALL or matched FILTERED) */
    uint32_t sd_logged;    /* frames written to the SD card (frozen when SD off) */
    uint32_t filtered;     /* frames dropped by the filter (non-match) */
    uint32_t dropped;      /* frames dropped due to overflow */
    uint32_t filter_id;    /* active filter ID */
    uint8_t  filter_set;   /* 1 if a filter is configured */
    uint8_t  alert_active; /* latched: an alert is currently being shown */
} OledTelemetry_t;

/* Shared OLED state (defined in main.c) */
extern OledTelemetry_t   g_oledTel;
extern SemaphoreHandle_t g_oledMutex;
extern QueueHandle_t     oledAlertQueue;
/* Page navigation requests from CliTask -> OledTask.
 *   g_pageRequest      : -1 = none, else an OLED_Page_t index
 *   g_pageRequestIsSnap: 1 = mode snap (clears manual lock), 0 = manual 'p' cycle */
extern volatile int32_t  g_pageRequest;
extern volatile int32_t  g_pageRequestIsSnap;

/* Raise an alert (stub hook for future anomaly detectors). Task context only. */
void OLED_RaiseAlert(AlertType_t type, uint32_t id, uint8_t severity);

/* Page renderers - operate on a local telemetry snapshot, no locking. */
void OLED_DrawMainPage(const OledTelemetry_t *t, uint8_t spin);
void OLED_DrawFilterPage(const OledTelemetry_t *t, uint8_t spin);
void OLED_DrawSpeedPage(const OledTelemetry_t *t, uint8_t spin);
void OLED_DrawStatsPage(const OledTelemetry_t *t, uint8_t spin);
void OLED_DrawAlertPage(const OledAlert_t *a, uint8_t spin);
void OLED_DrawPage(OLED_Page_t page, const OledTelemetry_t *t,
                   const OledAlert_t *a, uint8_t spin);


#endif /* APP_INC_APP_H_ */
