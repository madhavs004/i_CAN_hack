/*
 * anomaly.c
 *
 * Phase B anomaly detection: flood, unknown-ID, timing, payload.
 * See anomaly.h. Called only from task context (CanTask / CliTask).
 */

#include "anomaly.h"

/* ---- Per-ID profile table ---------------------------------------------- */
typedef struct {
    uint32_t id;
    uint8_t  used;

    /* timing */
    uint32_t lastSeen;     /* tick of the last frame for this ID */
    uint32_t period;       /* learned mean inter-arrival period (ms) */
    uint32_t samples;      /* number of gaps accumulated during learning */
    uint32_t periodSum;    /* sum of gaps during learning (for the mean) */

    /* payload */
    uint8_t  dlc;          /* learned DLC */
    uint8_t  baseline[8];  /* first payload seen (frozen-byte reference) */
    uint8_t  frozenMask;   /* bit i = 1 -> byte i never changed during learning */
    uint8_t  seenOnce;     /* baseline captured */
} IdProfile_t;

static IdProfile_t s_profiles[ANOMALY_MAX_IDS];
static uint16_t    s_knownCount;

/* ---- Learn window ------------------------------------------------------ */
static uint8_t  s_learning;
static uint8_t  s_learnArmed;   /* 0 = window not started yet (waiting for 1st frame) */
static uint32_t s_learnStart;

/* ---- Flood detector (rolling 1 s count) -------------------------------- */
static uint32_t s_winStart;
static uint32_t s_winCount;

/* ---- Per-type alert holdoff -------------------------------------------- */
static uint32_t s_lastFloodAlert;
static uint32_t s_lastUnknownAlert;
static uint32_t s_lastTimingAlert;
static uint32_t s_lastPayloadAlert;

/* Elapsed helper that is safe across tick wraparound. */
static uint8_t Elapsed(uint32_t now, uint32_t since, uint32_t span)
{
    return (uint8_t)((now - since) >= span);
}

static IdProfile_t *FindProfile(uint32_t id)
{
    for (uint16_t i = 0; i < s_knownCount; i++)
    {
        if (s_profiles[i].used && s_profiles[i].id == id)
            return &s_profiles[i];
    }
    return NULL;
}

static IdProfile_t *AddProfile(uint32_t id)
{
    if (s_knownCount >= ANOMALY_MAX_IDS)
        return NULL;                  /* table full */
    IdProfile_t *p = &s_profiles[s_knownCount++];
    p->id = id;
    p->used = 1;
    p->lastSeen = 0;
    p->period = 0;
    p->samples = 0;
    p->periodSum = 0;
    p->dlc = 0;
    p->frozenMask = 0xFF;             /* assume all frozen until proven otherwise */
    p->seenOnce = 0;
    return p;
}

void Anomaly_Init(uint32_t now_ms)
{
    for (uint16_t i = 0; i < ANOMALY_MAX_IDS; i++)
        s_profiles[i].used = 0;
    s_knownCount = 0;
    s_learning = 1;
    s_learnArmed = 0;            /* window starts on the first received frame */
    s_learnStart = now_ms;
    s_winStart = now_ms;
    s_winCount = 0;
    s_lastFloodAlert   = now_ms - ANOMALY_ALERT_HOLDOFF_MS;
    s_lastUnknownAlert = now_ms - ANOMALY_ALERT_HOLDOFF_MS;
    s_lastTimingAlert  = now_ms - ANOMALY_ALERT_HOLDOFF_MS;
    s_lastPayloadAlert = now_ms - ANOMALY_ALERT_HOLDOFF_MS;
}

void Anomaly_StartLearn(uint32_t now_ms)
{
    for (uint16_t i = 0; i < ANOMALY_MAX_IDS; i++)
        s_profiles[i].used = 0;
    s_knownCount = 0;
    s_learning = 1;
    s_learnArmed = 0;            /* window starts on the first received frame */
    s_learnStart = now_ms;
}

/* During learning: record period samples, DLC, and the frozen-byte mask. */
static void LearnFrame(IdProfile_t *p, uint8_t dlc, const uint8_t *data,
                       uint32_t now_ms)
{
    if (!p->seenOnce)
    {
        /* First sighting: capture the payload baseline + DLC. */
        p->seenOnce = 1;
        p->dlc = dlc;
        for (uint8_t i = 0; i < 8; i++)
            p->baseline[i] = (i < dlc) ? data[i] : 0;
    }
    else
    {
        /* Accumulate the inter-arrival gap for the mean period. */
        uint32_t gap = now_ms - p->lastSeen;
        p->periodSum += gap;
        p->samples++;
        p->period = p->periodSum / p->samples;

        /* Any byte that differs from the baseline is NOT frozen. */
        for (uint8_t i = 0; i < dlc && i < 8; i++)
        {
            if (data[i] != p->baseline[i])
                p->frozenMask &= (uint8_t)~(1u << i);
        }
    }
    p->lastSeen = now_ms;
}

/* Post-learn: timing + payload checks for a known ID. */
static void CheckFrame(IdProfile_t *p, uint8_t dlc, const uint8_t *data,
                       uint32_t now_ms)
{
    /* ---- Timing: gap drift beyond tolerance of the learned period ---- */
    if (p->samples >= ANOMALY_MIN_LEARN_SAMPLES &&
        p->period >= ANOMALY_TIMING_MIN_PERIOD_MS)
    {
        uint32_t gap = now_ms - p->lastSeen;
        uint32_t tol = (p->period * ANOMALY_TIMING_TOL_PCT) / 100u;
        uint32_t lo  = (p->period > tol) ? (p->period - tol) : 0;
        uint32_t hi  = p->period + tol;

        if (gap < lo || gap > hi)
        {
            if (Elapsed(now_ms, s_lastTimingAlert, ANOMALY_ALERT_HOLDOFF_MS))
            {
                s_lastTimingAlert = now_ms;
                /* Big deviation -> higher severity. */
                uint8_t sev = (gap > (hi * 2u) || (lo > 0 && gap < (lo / 2u))) ? 3 : 2;
                OLED_RaiseAlert(ALERT_TIMING, p->id, sev);
            }
        }
    }
    p->lastSeen = now_ms;

    /* ---- Payload: DLC change OR a frozen byte changed ---- */
    uint8_t payloadBad = 0;
    if (dlc != p->dlc)
    {
        payloadBad = 1;
    }
    else
    {
        for (uint8_t i = 0; i < dlc && i < 8; i++)
        {
            if ((p->frozenMask & (1u << i)) && data[i] != p->baseline[i])
            {
                payloadBad = 1;
                break;
            }
        }
    }

    if (payloadBad)
    {
        if (Elapsed(now_ms, s_lastPayloadAlert, ANOMALY_ALERT_HOLDOFF_MS))
        {
            s_lastPayloadAlert = now_ms;
            OLED_RaiseAlert(ALERT_PAYLOAD, p->id, 2);
        }
    }
}

void Anomaly_ProcessFrame(uint32_t id, uint8_t dlc, const uint8_t *data,
                          uint32_t now_ms)
{
    /* Count toward the flood window. */
    s_winCount++;

    /* The learn window begins on the FIRST received frame, not at boot - so a
     * slow-to-power sender (or a quiet bus) can't expire the window before any
     * traffic is seen, which would otherwise flag the first real frame as an
     * UNKNOWN_ID anomaly. */
    if (s_learning && !s_learnArmed)
    {
        s_learnArmed = 1;
        s_learnStart = now_ms;
    }

    /* Close the learn window once it expires (measured from the first frame). */
    if (s_learning && s_learnArmed && Elapsed(now_ms, s_learnStart, ANOMALY_LEARN_MS))
    {
        s_learning = 0;
    }

    if (s_learning)
    {
        IdProfile_t *p = FindProfile(id);
        if (p == NULL)
            p = AddProfile(id);
        if (p != NULL)
            LearnFrame(p, dlc, data, now_ms);
        return;                       /* no alerts while learning */
    }

    /* Post-learn. */
    IdProfile_t *p = FindProfile(id);
    if (p == NULL)
    {
        /* Unknown-ID detection. */
        if (Elapsed(now_ms, s_lastUnknownAlert, ANOMALY_ALERT_HOLDOFF_MS))
        {
            s_lastUnknownAlert = now_ms;
            OLED_RaiseAlert(ALERT_UNKNOWN_ID, id, 2);
        }
        return;
    }

    CheckFrame(p, dlc, data, now_ms);
}

void Anomaly_Tick(uint32_t now_ms)
{
    /* Evaluate the rolling 1 s frame rate. */
    if (Elapsed(now_ms, s_winStart, 1000u))
    {
        uint32_t fps = s_winCount;
        s_winStart = now_ms;
        s_winCount = 0;

        if (!s_learning && fps > ANOMALY_FLOOD_FPS)
        {
            if (Elapsed(now_ms, s_lastFloodAlert, ANOMALY_ALERT_HOLDOFF_MS))
            {
                s_lastFloodAlert = now_ms;
                uint8_t sev = (fps > (ANOMALY_FLOOD_FPS * 2u)) ? 3 : 2;
                OLED_RaiseAlert(ALERT_FLOOD, 0, sev);
            }
        }
    }
}

uint8_t Anomaly_IsLearning(void)
{
    return s_learning;
}

uint16_t Anomaly_KnownCount(void)
{
    return s_knownCount;
}
