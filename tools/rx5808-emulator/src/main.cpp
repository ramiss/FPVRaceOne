// ─────────────────────────────────────────────────────────────────────────────
// RX5808 analog emulator — bench test rig for FPVRaceOne timing measurement
//
// WHAT THIS IS FOR
//
// Real-world stimulus cannot answer "is the lap timer late or inconsistent".
// A servo swinging past an antenna has millisecond-scale variance of its own,
// so you end up characterising the servo. This board replaces the RX5808's
// analog RSSI output with a precisely-timed synthetic envelope, making the
// stimulus deterministic to microseconds.
//
// WHAT IT MEASURES
//
//   1. Lap-time accuracy  — bias between a known pulse interval and the lap
//                           time FPVRaceOne reports
//   2. Lap-time consistency — standard deviation of that error
//   3. Absolute latency   — stimulus-to-detection delay, via the marker pin
//
// WHY NO CLOCK SYNC IS NEEDED (the key idea)
//
// A lap time is a DIFFERENCE measured entirely by FPVRaceOne's own clock. If
// pulses are generated exactly T apart and the device reports L, the error is
// L - T. No relationship between this board's clock and the device's clock is
// involved, so USB jitter is irrelevant. Absolute latency likewise: both the
// stimulus start and the marker edge are timestamped HERE, on one timer.
//
// WHAT IT IS NOT
//
// This reproduces the RSSI ENVELOPE, not RF. No multipath, no antenna nulls,
// no interference. It faithfully exercises the acquisition and detection
// pipeline; it does not predict real-world detection reliability.
//
// WIRING (see tools/timing-harness/README.md)
//
//   GPIO25 (DAC1) --[2.2k]--+-- A2/GPIO2  RSSI input on XIAO C6
//                           |
//                        [1.0k]
//                           |
//   GND --------------------+-- GND       common ground, REQUIRED
//
//   GPIO4 (input) <------------ D3/GPIO21 PIN_TIMING_MARKER on XIAO C6
//
// The real RX5808 must be disconnected from the RSSI line only; the three SPI
// lines may stay wired since this emulator ignores them (analog-only by
// design — verifyFrequency() will log a mismatch, and its return value is
// ignored by the sole caller, so nothing breaks).
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <ArduinoJson.h>

// ── Pins ────────────────────────────────────────────────────────────────────
static const uint8_t PIN_DAC_RSSI = 25;  // DAC1 on WROOM-32 (header pin 9)
static const uint8_t PIN_MARKER   = 4;   // marker input from FPVRaceOne

// ── Envelope playback ───────────────────────────────────────────────────────
//
// A hardware timer ISR steps through the envelope so playback timing is immune
// to anything happening in loop() or in FreeRTOS. 10 kHz gives 100 us steps —
// 200 points across a 20 ms pass, ample shape resolution, and two orders of
// magnitude finer than the millisecond effects being measured.
static const uint32_t TICK_HZ = 10000;
static const uint32_t TICK_US = 1000000UL / TICK_HZ;

// Profile, set over USB before a run.
struct Profile {
    uint8_t  baseline   = 40;    // DAC counts at rest (noise floor)
    uint8_t  peak       = 200;   // DAC counts at pass apex
    uint32_t widthMs    = 40;    // full width of the pass envelope
    uint32_t intervalMs = 5000;  // start-to-start spacing between passes
    uint32_t count      = 10;    // number of passes in a run
};
static Profile profile;

// Run state. Touched by both the ISR and loop(), so anything shared is
// volatile and anything multi-word is read under a critical section.
static volatile bool     running       = false;
static volatile uint32_t passesDone    = 0;
static volatile uint32_t tickInPass    = 0;   // ticks since current pass began
static volatile uint32_t ticksPerPass  = 0;   // envelope duration in ticks
static volatile uint32_t ticksPerCycle = 0;   // pass start to next pass start
static volatile uint32_t passStartUs   = 0;   // micros() at current pass start
static volatile bool     inPass        = false;

// Marker capture — written by the GPIO ISR, drained by loop().
//
// markerStartUs is latched INSIDE the ISR rather than read later in loop().
// Reading passStartUs at drain time was a latent bug: if the next pass began
// between the edge and the drain, the latency would be differenced against
// the wrong t0 and appear as a large negative outlier.  Latching both values
// together makes each measurement self-consistent by construction.
static volatile uint32_t markerUs      = 0;
static volatile uint32_t markerStartUs = 0;
static volatile bool     markerPending = false;
// Edges discarded because the previous one had not been drained.  Silently
// dropping these hid missing measurements; counting them means the harness
// can say so instead of quietly reporting fewer samples than passes.
static volatile uint32_t markerDropped = 0;

static hw_timer_t* envTimer = nullptr;
static portMUX_TYPE mux     = portMUX_INITIALIZER_UNLOCKED;

// Raised-cosine envelope: 0 at the edges, 1 at the centre, smooth throughout.
// Approximates the RSSI rise and fall of a gate pass far better than a square
// pulse, and its smoothness is what makes the median filter behave the way it
// would on real hardware.
// PRECOMPUTED, because the ISR must not touch floating point.
//
// This originally evaluated 0.5*(1-cos(2*pi*x)) inline in the timer ISR.  That
// is unsafe on ESP32: FreeRTOS does not save or restore FPU state across an
// interrupt, so float arithmetic in an ISR yields intermittently wrong results
// and can corrupt the FPU registers of whatever task was interrupted.  cosf()
// is also a flash-resident libm call being made from an IRAM handler.
//
// The symptom was subtle and easy to misread: most passes were generated
// correctly, but occasional ones came out with the wrong amplitude and simply
// failed to cross the detector's enter threshold — presenting as a lap-timer
// detection failure when the fault was in the stimulus.
//
// The shape is now built once in task context (where float is fine) into an
// integer table of DAC values; the ISR does a bounds-checked index and nothing
// else.
#define ENV_TABLE_SIZE 512
static uint8_t envTable[ENV_TABLE_SIZE];

// Build the raised-cosine envelope for the current baseline/peak.  Called from
// startRun(), never from an ISR.
static void buildEnvelopeTable(uint8_t baseline, uint8_t peak) {
    const float span = (float)peak - (float)baseline;
    for (uint32_t i = 0; i < ENV_TABLE_SIZE; i++) {
        const float x = (float)i / (float)(ENV_TABLE_SIZE - 1);   // 0..1
        const float w = 0.5f * (1.0f - cosf(2.0f * PI * x));      // 0..1..0
        envTable[i] = (uint8_t)(baseline + span * w + 0.5f);
    }
}

// Integer-only lookup, safe to call from the ISR.
static inline uint8_t IRAM_ATTR envelopeAt(uint32_t tick, uint32_t total) {
    if (total == 0) return envTable[0];
    uint32_t idx = (tick * (ENV_TABLE_SIZE - 1)) / total;
    if (idx >= ENV_TABLE_SIZE) idx = ENV_TABLE_SIZE - 1;
    return envTable[idx];
}

// ── Envelope timer ISR ──────────────────────────────────────────────────────
// Kept short and allocation-free. dacWrite() on the original ESP32 is a direct
// register write to the DAC and is safe from an ISR.
static void IRAM_ATTR onEnvTick() {
    if (!running) return;

    portENTER_CRITICAL_ISR(&mux);

    if (!inPass) {
        // Between passes: hold the baseline and count down to the next start.
        tickInPass++;
        if (tickInPass >= ticksPerCycle) {
            tickInPass  = 0;
            inPass      = true;
            passStartUs = micros();   // t0 for this pass's latency measurement
        }
        portEXIT_CRITICAL_ISR(&mux);
        if (!inPass) dacWrite(PIN_DAC_RSSI, profile.baseline);
        return;
    }

    const uint32_t t = tickInPass;
    tickInPass++;

    const bool passEnded = (t >= ticksPerPass);
    if (passEnded) {
        inPass = false;
        // tickInPass keeps counting; the gap is (ticksPerCycle - ticksPerPass)
        passesDone++;
        if (passesDone >= profile.count) running = false;
    }

    portEXIT_CRITICAL_ISR(&mux);

    dacWrite(PIN_DAC_RSSI,
             passEnded ? profile.baseline : envelopeAt(t, ticksPerPass));
}

// ── Marker ISR ──────────────────────────────────────────────────────────────
// FPVRaceOne raises PIN_TIMING_MARKER the instant it confirms a lap, before
// any of its DEBUG() serial writes. Timestamping here on the same timer that
// generated the stimulus is what makes absolute latency measurable without
// any cross-device clock sync.
static void IRAM_ATTR onMarkerEdge() {
    if (markerPending) {         // previous edge not yet drained
        markerDropped++;
        return;
    }
    markerUs      = micros();
    markerStartUs = passStartUs; // latch t0 WITH the edge, not at drain time
    markerPending = true;
}

// ── USB protocol ────────────────────────────────────────────────────────────
// JSON lines, matching the shape lib/USB/usb.cpp already uses on the product
// side so the harness parses both devices the same way.
static void emit(const JsonDocument& doc) {
    serializeJson(doc, Serial);
    Serial.println();
}

static void emitReady() {
    JsonDocument d;
    d["event"] = "ready";
    d["fw"]    = "rx5808-emulator/1";
    emit(d);
}

static void startRun() {
    // Build the envelope here, in task context — the ISR must not do float.
    buildEnvelopeTable(profile.baseline, profile.peak);

    // Convert milliseconds to ticks once, up front — the ISR must not divide.
    const uint32_t tpp = (profile.widthMs    * TICK_HZ) / 1000UL;
    const uint32_t tpc = (profile.intervalMs * TICK_HZ) / 1000UL;

    portENTER_CRITICAL(&mux);
    ticksPerPass  = tpp ? tpp : 1;
    ticksPerCycle = (tpc > ticksPerPass) ? tpc : (ticksPerPass + 1);
    passesDone    = 0;
    tickInPass    = 0;
    inPass        = false;
    markerPending = false;
    markerDropped = 0;
    running       = true;
    portEXIT_CRITICAL(&mux);

    JsonDocument d;
    d["event"]      = "runStarted";
    d["count"]      = profile.count;
    d["widthMs"]    = profile.widthMs;
    d["intervalMs"] = profile.intervalMs;
    emit(d);
}

static void processCommand(const char* line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;      // not JSON — ignore quietly
    if (!doc["cmd"].is<const char*>()) return;

    const char* cmd = doc["cmd"];

    if (strcmp(cmd, "ping") == 0) {
        emitReady();

    } else if (strcmp(cmd, "profile") == 0) {
        if (doc["baseline"].is<uint8_t>())    profile.baseline   = doc["baseline"];
        if (doc["peak"].is<uint8_t>())        profile.peak       = doc["peak"];
        if (doc["widthMs"].is<uint32_t>())    profile.widthMs    = doc["widthMs"];
        if (doc["intervalMs"].is<uint32_t>()) profile.intervalMs = doc["intervalMs"];
        if (doc["count"].is<uint32_t>())      profile.count      = doc["count"];

        JsonDocument d;
        d["event"]      = "profileSet";
        d["baseline"]   = profile.baseline;
        d["peak"]       = profile.peak;
        d["widthMs"]    = profile.widthMs;
        d["intervalMs"] = profile.intervalMs;
        d["count"]      = profile.count;
        emit(d);

    } else if (strcmp(cmd, "run") == 0) {
        startRun();

    } else if (strcmp(cmd, "stop") == 0) {
        running = false;
        dacWrite(PIN_DAC_RSSI, profile.baseline);
        JsonDocument d;
        d["event"] = "stopped";
        emit(d);

    } else if (strcmp(cmd, "level") == 0) {
        // Manual DC level — for validating the divider and the device's
        // RSSI reading before trusting any timing number.
        running = false;
        const uint8_t v = doc["value"] | profile.baseline;
        dacWrite(PIN_DAC_RSSI, v);
        JsonDocument d;
        d["event"] = "level";
        d["value"] = v;
        emit(d);
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_MARKER, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_MARKER), onMarkerEdge, RISING);

    // Populate the envelope before the timer ISR can ever index it.
    buildEnvelopeTable(profile.baseline, profile.peak);
    dacWrite(PIN_DAC_RSSI, profile.baseline);

    // Arduino-ESP32 v3.x timer API: timerBegin takes a frequency directly.
    // 1 MHz base so the alarm value is simply the tick period in microseconds.
    envTimer = timerBegin(1000000);
    timerAttachInterrupt(envTimer, &onEnvTick);
    timerAlarm(envTimer, TICK_US, true, 0);   // period, auto-reload, unlimited

    delay(200);
    emitReady();
}

void loop() {
    // Drain a captured marker edge and report its latency relative to the
    // pass that produced it.
    if (markerPending) {
        uint32_t mUs, sUs, n, dropped;
        portENTER_CRITICAL(&mux);
        mUs     = markerUs;
        sUs     = markerStartUs;   // latched with the edge, not read now
        n       = passesDone;
        dropped = markerDropped;
        markerPending = false;
        portEXIT_CRITICAL(&mux);

        JsonDocument d;
        d["event"]     = "pass";
        d["n"]         = n;
        d["tStartUs"]  = sUs;
        d["latencyUs"] = (uint32_t)(mUs - sUs);   // wrap-safe unsigned subtract
        if (dropped) d["dropped"] = dropped;
        emit(d);
    }

    // Report run completion once.
    static bool wasRunning = false;
    const bool isRunning = running;
    if (wasRunning && !isRunning) {
        JsonDocument d;
        d["event"]   = "done";
        d["count"]   = passesDone;
        d["dropped"] = markerDropped;   // measurements lost, not passes missed
        emit(d);
    }
    wasRunning = isRunning;

    // Line-buffered command input.
    static char buf[192];
    static size_t pos = 0;
    while (Serial.available() > 0) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                buf[pos] = '\0';
                processCommand(buf);
                pos = 0;
            }
        } else if (pos < sizeof(buf) - 1) {
            buf[pos++] = c;
        } else {
            pos = 0;   // overlong line — discard
        }
    }
}
