/*
 * sd_logger.c
 *
 * SD-card CSV logging sink. See sd_logger.h.
 *
 * CSV columns:
 *   type,timestamp_ms,id,dlc,d0,d1,d2,d3,d4,d5,d6,d7,alert_type,severity
 * Frame rows fill id/dlc/data and leave alert columns blank; alert rows fill
 * id/alert_type/severity and leave dlc/data blank. Human-readable, opens in
 * Excel / pandas directly.
 */

#include "sd_logger.h"
#include "fatfs.h"        /* USERPath, USERFatFS, FATFS_LinkDriver glue */
#include "ff.h"
#include <stdio.h>
#include <string.h>

QueueHandle_t sdQueue;

/* ---- Tunables ---------------------------------------------------------- */
#define SD_LOG_FILE        "canlog.csv"
#define SD_SYNC_EVERY      64u      /* f_sync after this many appended rows */
#define SD_SYNC_MS         2000u    /* ...or at least this often */
#define SD_WRITE_CHUNK     8u       /* rows pulled per task wake before yield */

/* ---- State ------------------------------------------------------------- */
static volatile uint8_t s_active;      /* logging requested on */
static volatile uint8_t s_error;       /* last start request failed (mount/open/write) */
static uint8_t          s_mounted;     /* FS mounted */
static uint8_t          s_fileOpen;    /* canlog.csv open */
static FATFS           *s_fs = &USERFatFS;
static FIL              s_file;
static uint32_t         s_rowsSinceSync;
static uint32_t         s_lastSyncTick;
static volatile uint32_t s_overflow;   /* records dropped (queue full) */
static volatile uint32_t s_framesLogged; /* CAN frames enqueued to SD while active */

static const char *AlertName(uint8_t t)
{
    switch (t)
    {
        case ALERT_UNKNOWN_ID: return "UNKNOWN_ID";
        case ALERT_FLOOD:      return "FLOOD";
        case ALERT_TIMING:     return "TIMING";
        case ALERT_PAYLOAD:    return "PAYLOAD";
        default:               return "NONE";
    }
}

/* ---- Producer API ------------------------------------------------------ */

void SD_Log_Frame(uint32_t id, uint8_t dlc, const uint8_t *data, uint32_t ts)
{
    if (!s_active || sdQueue == NULL) return;
    SdRecord_t r;
    r.kind = SD_REC_FRAME;
    r.timestamp = ts;
    r.id = id;
    r.dlc = (dlc > 8) ? 8 : dlc;
    memcpy(r.data, data, 8);
    r.alertType = ALERT_NONE;
    r.severity = 0;
    if (xQueueSend(sdQueue, &r, 0) != pdTRUE) s_overflow++;
    else                                      s_framesLogged++;
}

void SD_Log_Alert(AlertType_t type, uint32_t id, uint8_t severity, uint32_t ts)
{
    if (!s_active || sdQueue == NULL) return;
    SdRecord_t r;
    r.kind = SD_REC_ALERT;
    r.timestamp = ts;
    r.id = id;
    r.dlc = 0;
    memset(r.data, 0, 8);
    r.alertType = (uint8_t)type;
    r.severity = severity;
    if (xQueueSend(sdQueue, &r, 0) != pdTRUE) s_overflow++;
}

/* ---- Control ----------------------------------------------------------- */

void SD_Logger_Init(void)
{
    s_active = 0;
    s_error = 0;
    s_mounted = 0;
    s_fileOpen = 0;
    s_rowsSinceSync = 0;
    s_overflow = 0;
    s_framesLogged = 0;
}

void SD_Logger_Start(void)
{
    s_error = 0;    /* clear any prior failure on a fresh request */
    s_active = 1;   /* task opens the file lazily on the next wake */
}

void SD_Logger_Stop(void)
{
    s_active = 0;   /* task flushes + closes on the next wake */
}

uint8_t SD_Logger_IsActive(void)
{
    return s_active;
}

uint8_t SD_Logger_HasError(void)
{
    return s_error;
}

uint32_t SD_Logger_FramesLogged(void)
{
    return s_framesLogged;
}

/* ---- Internal: mount + open + header ----------------------------------- */

static int OpenLog(void)
{
    FRESULT fr;

    if (!s_mounted)
    {
        fr = f_mount(s_fs, USERPath, 1);   /* mount now (opt=1) */
        if (fr != FR_OK)
        {
            Log_Send("SD: mount failed\r\n");
            return 0;
        }
        s_mounted = 1;
    }

    /* Truncate to a fresh file each session: each logging session starts a
     * clean canlog.csv (overwrites any previous session's data). */
    fr = f_open(&s_file, SD_LOG_FILE, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        Log_Send("SD: open failed\r\n");
        return 0;
    }

    /* Always (re)write the CSV header - the file was just truncated. */
    {
        UINT bw;
        const char *hdr =
            "type,timestamp_ms,id,dlc,d0,d1,d2,d3,d4,d5,d6,d7,alert_type,severity\r\n";
        f_write(&s_file, hdr, (UINT)strlen(hdr), &bw);
        f_sync(&s_file);
    }

    s_fileOpen = 1;
    s_rowsSinceSync = 0;
    s_lastSyncTick = HAL_GetTick();
    Log_Send("SD: logging to canlog.csv\r\n");
    return 1;
}

static void CloseLog(void)
{
    if (s_fileOpen)
    {
        f_sync(&s_file);
        f_close(&s_file);
        s_fileOpen = 0;
        Log_Send("SD: log closed\r\n");
    }
}

/* Format one record into the caller's buffer; returns length. */
static int FormatRow(const SdRecord_t *r, char *buf, int cap)
{
    if (r->kind == SD_REC_FRAME)
    {
        int n = snprintf(buf, cap, "FRAME,%lu,%lX,%u,",
                         (unsigned long)r->timestamp,
                         (unsigned long)r->id, (unsigned)r->dlc);
        for (uint8_t i = 0; i < 8; i++)
        {
            if (i < r->dlc)
                n += snprintf(buf + n, cap - n, "%02X", r->data[i]);
            if (i < 7)
                n += snprintf(buf + n, cap - n, ",");
        }
        /* trailing two alert columns empty */
        n += snprintf(buf + n, cap - n, ",,\r\n");
        return n;
    }
    else
    {
        /* ALERT row: id filled, dlc/data blank, alert_type + severity set. */
        return snprintf(buf, cap,
                        "ALERT,%lu,%lX,,,,,,,,,,%s,%u\r\n",
                        (unsigned long)r->timestamp,
                        (unsigned long)r->id,
                        AlertName(r->alertType), (unsigned)r->severity);
    }
}

/* ---- SdTask ------------------------------------------------------------ */

void StartSdTask(void *argument)
{
    SdRecord_t r;
    char line[80];

    for (;;)
    {
        /* If logging was requested but the file isn't open, open it. */
        if (s_active && !s_fileOpen)
        {
            if (!OpenLog())
            {
                s_active = 0;            /* give up until the user retries */
                s_error = 1;             /* requested but mount/open failed */
            }
        }

        /* If logging was stopped, flush + close. */
        if (!s_active && s_fileOpen)
        {
            CloseLog();
        }

        /* Drain a batch of records (bounded so we periodically yield/sync). */
        uint8_t pulled = 0;
        while (s_fileOpen && pulled < SD_WRITE_CHUNK &&
               xQueueReceive(sdQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            int len = FormatRow(&r, line, sizeof(line));
            if (len > 0)
            {
                UINT bw;
                if (f_write(&s_file, line, (UINT)len, &bw) != FR_OK || bw != (UINT)len)
                {
                    Log_Send("SD: write error\r\n");
                    CloseLog();
                    s_active = 0;
                    s_error = 1;             /* write failure -> error state */
                    break;
                }
                s_rowsSinceSync++;
            }
            pulled++;
        }

        /* If nothing was open to receive into, sleep so we don't spin. */
        if (!s_fileOpen)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        /* Periodic flush: every N rows or every SD_SYNC_MS. */
        if (s_fileOpen)
        {
            uint32_t now = HAL_GetTick();
            if (s_rowsSinceSync >= SD_SYNC_EVERY ||
                (now - s_lastSyncTick) >= SD_SYNC_MS)
            {
                f_sync(&s_file);
                s_rowsSinceSync = 0;
                s_lastSyncTick = now;
            }
        }
    }
}
