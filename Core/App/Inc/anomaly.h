/*
 * anomaly.h
 *
 * Phase B: lightweight CAN anomaly detection for the i_CAN_hack logger.
 *
 * Four detectors:
 *   - FLOOD       : frames/sec exceeds ANOMALY_FLOOD_FPS (bus flood / DoS).
 *   - UNKNOWN_ID  : an ID not in the learned allow-list appears after learning.
 *   - TIMING      : a known ID's inter-arrival gap drifts beyond tolerance from
 *                   its learned mean period (injection / replay / dropout).
 *   - PAYLOAD     : a known ID's DLC changes, or a byte that was constant during
 *                   learning ("frozen" byte) suddenly changes.
 *
 * ID learning (per-ID profile table):
 *   - Auto-learn for ANOMALY_LEARN_MS after init. During learning each ID's
 *     mean period, DLC, and per-byte "frozen" mask + baseline are recorded.
 *   - CLI re-learn: Anomaly_StartLearn() restarts the learn window on demand.
 *
 * Threading model:
 *   - All functions are called from CanTask (task context). NOT ISR-safe.
 *   - On detection, the module calls OLED_RaiseAlert() directly.
 */

#ifndef APP_INC_ANOMALY_H_
#define APP_INC_ANOMALY_H_

#include <stdint.h>
#include "app.h"

/* ---- Tunables ---------------------------------------------------------- */
#ifndef ANOMALY_MAX_IDS
#define ANOMALY_MAX_IDS       64      /* allow-list capacity */
#endif

#ifndef ANOMALY_LEARN_MS
#define ANOMALY_LEARN_MS      5000u   /* auto-learn window after init/relearn */
#endif

#ifndef ANOMALY_FLOOD_FPS
#define ANOMALY_FLOOD_FPS     500u    /* frames/sec above this = FLOOD alert */
#endif

#ifndef ANOMALY_ALERT_HOLDOFF_MS
#define ANOMALY_ALERT_HOLDOFF_MS 2000u /* min gap between repeat alerts (per type) */
#endif

#ifndef ANOMALY_TIMING_TOL_PCT
#define ANOMALY_TIMING_TOL_PCT   50u   /* gap may drift +/- this % of learned period */
#endif

#ifndef ANOMALY_TIMING_MIN_PERIOD_MS
#define ANOMALY_TIMING_MIN_PERIOD_MS 5u /* ignore timing checks below this period */
#endif

#ifndef ANOMALY_MIN_LEARN_SAMPLES
#define ANOMALY_MIN_LEARN_SAMPLES 3u   /* min frames to trust a learned period */
#endif

/* Initialize state. Starts the auto-learn window. Call once before scheduler
 * (or at task start) with the current tick. */
void Anomaly_Init(uint32_t now_ms);

/* (Re)start the learn window: clears the allow-list and relearns for
 * ANOMALY_LEARN_MS. Safe to call from CliTask. */
void Anomaly_StartLearn(uint32_t now_ms);

/* Process one received frame (CanTask). During learning, records the ID;
 * after learning, raises UNKNOWN_ID if the ID is not in the allow-list. */
void Anomaly_ProcessFrame(uint32_t id, uint8_t dlc, const uint8_t *data,
                          uint32_t now_ms);

/* Periodic tick (CanTask, ~every 100 ms). Evaluates the flood detector from
 * the rolling 1-second frame count. */
void Anomaly_Tick(uint32_t now_ms);

/* 1 while the learn window is open (for UI/debug). */
uint8_t Anomaly_IsLearning(void);

/* Number of IDs currently learned. */
uint16_t Anomaly_KnownCount(void);

#endif /* APP_INC_ANOMALY_H_ */
