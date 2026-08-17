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
#include <esp_system.h>
#include <soc/dac_channel.h>
#include <hal/dac_ll.h>

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

// ── Liveness counters ───────────────────────────────────────────────────────
// The ONLY additions inside the 10 kHz ISR are two stores: an increment and a
// byte copy.  That is a handful of cycles against a 100 us budget, so envelope
// timing is unaffected — which is the whole point of the emulator existing.
//
// Everything expensive (formatting, Serial) happens in loop(), which cannot
// disturb the ISR: the timer interrupt preempts it.
static volatile uint32_t envTicks    = 0;   // ++ per timer tick; proves the ISR runs
static volatile uint8_t  lastDacVal  = 0;   // last value actually written to the DAC

// Min/max commanded DAC since the last heartbeat.
//
// A single instantaneous sample is nearly useless for confirming the envelope
// is playing: a 40 ms pass every 3000 ms is a 1.3% duty cycle, so a once-per-
// second sample sits at baseline ~99% of the time and dacCmd looks frozen at
// 40 even while passes are firing correctly.  The RANGE over the window shows
// the swing, so baseline-to-peak is visible in every heartbeat during a run.
//
// Two compares and two conditional stores in the ISR — negligible at 10 kHz.
static volatile uint8_t  dacWinMin   = 255;
static volatile uint8_t  dacWinMax   = 0;

// Highest DAC value actually commanded during the CURRENT pass, latched when
// the pass ends.  This is what makes the per-pass report meaningful: it proves
// the envelope really swung to peak for that specific pass, rather than the
// profile merely claiming it should have.
static volatile uint8_t  passObsMax     = 0;
static volatile uint8_t  lastPassObsMax = 0;

// Every DAC write goes through here so the reported value can never disagree
// with what we asked the hardware for.
//
// CRITICAL: this must NOT call Arduino's dacWrite().  On this core dacWrite()
// is not a register write at all — it does a peripheral-manager lookup
// (perimanGetPinBus), and on the first call for a pin it calls
// dac_oneshot_new_channel(), which ALLOCATES.  All of it is flash-resident and
// takes locks.  Calling that 10,000 times a second from an IRAM ISR is unsafe
// on every count, and in practice it eventually left the peripheral-manager
// state broken: the DAC pin stopped driving while the firmware carried on
// "writing" to it, so the heartbeat still reported the commanded value.  Only
// a hardware reset recovered it.  That is the emulator "stalling" — the output
// went dead while everything else looked healthy.
//
// dac_ll_update_output_value() is always_inline and compiles to two register
// writes.  No locks, no allocation, no flash access — genuinely ISR-safe, and
// what the previous comment here wrongly claimed dacWrite() already was.
//
// The channel must still be created once, from task context, before this is
// used; setup() does that with a single dacWrite() call.
static inline void IRAM_ATTR dacSet(uint8_t v) {
    lastDacVal = v;
    if (v < dacWinMin) dacWinMin = v;
    if (v > dacWinMax) dacWinMax = v;
    dac_ll_update_output_value(DAC_CHAN_0, v);   // DAC_CHAN_0 == GPIO25
}

// ── Envelope timer ISR ──────────────────────────────────────────────────────
// Kept short and allocation-free. dacWrite() on the original ESP32 is a direct
// register write to the DAC and is safe from an ISR.
static void IRAM_ATTR onEnvTick() {
    envTicks++;                 // counted even when idle, so a stalled timer
                                // is distinguishable from a stopped run
    if (!running) return;

    portENTER_CRITICAL_ISR(&mux);

    if (!inPass) {
        // Between passes: hold the baseline and count down to the next start.
        tickInPass++;
        if (tickInPass >= ticksPerCycle) {
            tickInPass  = 0;
            inPass      = true;
            passObsMax  = 0;          // start a fresh peak observation
            passStartUs = micros();   // t0 for this pass's latency measurement
        }
        portEXIT_CRITICAL_ISR(&mux);
        if (!inPass) dacSet(profile.baseline);
        return;
    }

    const uint32_t t = tickInPass;
    tickInPass++;

    const bool passEnded = (t >= ticksPerPass);
    if (passEnded) {
        inPass = false;
        lastPassObsMax = passObsMax;   // latch for loop() to report
        // tickInPass keeps counting; the gap is (ticksPerCycle - ticksPerPass)
        passesDone++;
        if (passesDone >= profile.count) running = false;
    }

    portEXIT_CRITICAL_ISR(&mux);

    const uint8_t out = passEnded ? profile.baseline
                                  : envelopeAt(t, ticksPerPass);
    if (!passEnded && out > passObsMax) passObsMax = out;
    dacSet(out);
}

// ── Marker ISR ──────────────────────────────────────────────────────────────
// FPVRaceOne raises PIN_TIMING_MARKER the instant it confirms a lap, before
// any of its DEBUG() serial writes. Timestamping here on the same timer that
// generated the stimulus is what makes absolute latency measurable without
// any cross-device clock sync.
// Ignore edges arriving closer together than any real detection could be.  The
// device raises the marker once per lap, and lap intervals are >= MIN_LAP on the
// product side — so anything in the microsecond range is electrical noise, not a
// detection.  Rejecting it keeps a stuck or floating line from monopolising the
// CPU even if the pulldown above is ever defeated by external wiring.
#define MARKER_MIN_GAP_US 1000UL

static void IRAM_ATTR onMarkerEdge() {
    const uint32_t now = micros();
    static volatile uint32_t lastEdgeUs = 0;
    if ((uint32_t)(now - lastEdgeUs) < MARKER_MIN_GAP_US) {
        return;                  // noise / ringing — not a detection
    }
    lastEdgeUs = now;

    if (markerPending) {         // previous edge not yet drained
        markerDropped++;
        return;
    }
    markerUs      = now;
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

// Translate the ROM/IDF reset cause into something readable.  This is the
// single most useful line for "why did the emulator stop": a panic, a watchdog
// and a brownout look identical from outside, and need completely different
// fixes.
static const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "POWERON";     // normal cold boot
        case ESP_RST_EXT:      return "EXT_RESET";   // the reset button
        case ESP_RST_SW:       return "SW_RESTART";  // esp_restart()
        case ESP_RST_PANIC:    return "PANIC";       // crash — check backtrace
        case ESP_RST_INT_WDT:  return "INT_WDT";     // interrupt watchdog
        case ESP_RST_TASK_WDT: return "TASK_WDT";    // task starved the WDT
        case ESP_RST_WDT:      return "OTHER_WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT";    // power rail sagged
        case ESP_RST_SDIO:     return "SDIO";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        // C6-specific reasons.  Without these a USB/DTR reset — i.e. the normal
        // consequence of opening the serial port — reports as "UNKNOWN", which
        // is the one answer that helps least.  CPU_LOCKUP and PWR_GLITCH matter
        // too: this emulator's "stalls" turned out to be the C6 rebooting.
        case ESP_RST_USB:      return "USB_RESET";
        case ESP_RST_JTAG:     return "JTAG_RESET";
        case ESP_RST_EFUSE:    return "EFUSE_ERROR";
        case ESP_RST_PWR_GLITCH:return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP";
        case ESP_RST_UNKNOWN:  return "UNKNOWN";
        default:               return "UNRECOGNISED";
    }
}

static void emitReady() {
    JsonDocument d;
    d["event"] = "ready";
    d["fw"]    = "rx5808-emulator/1";
    d["rst"]   = resetReasonStr(esp_reset_reason());
    d["heap"]  = (uint32_t)ESP.getFreeHeap();
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
        dacSet(profile.baseline);
        JsonDocument d;
        d["event"] = "stopped";
        emit(d);

    } else if (strcmp(cmd, "level") == 0) {
        // Manual DC level — for validating the divider and the device's
        // RSSI reading before trusting any timing number.
        running = false;
        const uint8_t v = doc["value"] | profile.baseline;
        dacSet(v);
        JsonDocument d;
        d["event"] = "level";
        d["value"] = v;
        emit(d);
    }
}

void setup() {
    Serial.begin(115200);

    // INPUT_PULLDOWN, not INPUT.  This line is driven by FPVRaceOne's
    // PIN_TIMING_MARKER, and an ESP32's GPIOs go high-impedance while it is in
    // reset or rebooting.  With a bare INPUT the line then FLOATS, and a
    // floating pin under a RISING-edge interrupt picks up noise as a continuous
    // edge storm.  The ISR is short, but at noise frequencies it starves the
    // main loop and the envelope timer, so the DAC stops playing and the
    // emulator stops answering serial — indistinguishable from a hang, and it
    // needed a physical reset to recover.
    //
    // That is why "the emulator stalled" kept coinciding with the C6 rebooting:
    // the reboot was the CAUSE, via this pin.  The pulldown holds the line low
    // whenever nothing drives it; the marker pulse still reads normally.
    pinMode(PIN_MARKER, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_MARKER), onMarkerEdge, RISING);

    // Populate the envelope before the timer ISR can ever index it.
    buildEnvelopeTable(profile.baseline, profile.peak);
    // Create the DAC oneshot channel ONCE, here in task context, where
    // allocation and locking are fine.  Every later write goes through
    // dacSet(), which touches only registers and is ISR-safe.
    dacWrite(PIN_DAC_RSSI, profile.baseline);
    dacSet(profile.baseline);

    // Arduino-ESP32 v3.x timer API: timerBegin takes a frequency directly.
    // 1 MHz base so the alarm value is simply the tick period in microseconds.
    envTimer = timerBegin(1000000);
    timerAttachInterrupt(envTimer, &onEnvTick);
    timerAlarm(envTimer, TICK_US, true, 0);   // period, auto-reload, unlimited

    delay(200);
    emitReady();
}

// Once a second, publish proof of life.  The decisive field is tickHz: the
// timer ISR should run at TICK_HZ regardless of whether a profile is playing,
// so tickHz==0 means the DAC has stopped being driven — which is exactly the
// "emulator stalled" state that previously could only be inferred from the
// device failing to detect anything.
static void emitHeartbeat(uint32_t nowMs) {
    static uint32_t lastMs    = 0;
    static uint32_t lastTicks = 0;
    if (nowMs - lastMs < 1000) return;

    const uint32_t ticks = envTicks;
    const uint32_t dt    = nowMs - lastMs;
    const uint32_t hz    = dt ? ((ticks - lastTicks) * 1000UL) / dt : 0;
    lastMs    = nowMs;
    lastTicks = ticks;

    JsonDocument d;
    d["event"]   = "hb";
    d["tickHz"]  = hz;                      // ~TICK_HZ when healthy, 0 if dead
    // "commanded", not measured: this is what the firmware last asked the
    // DAC for.  There is no ADC on this board to read the pin back, so a
    // dead output can still report a healthy value here.  Confirm the pin
    // itself via the device's own RSSI reading.
    d["dacCmd"]  = (uint8_t)lastDacVal;
    // Range over the last second.  During a run these straddle
    // baseline..peak; if dacMax never rises above baseline while running is
    // true, the envelope is NOT being played even though the ISR ticks.
    d["dacMin"]  = (uint8_t)dacWinMin;
    d["dacMax"]  = (uint8_t)dacWinMax;
    dacWinMin    = 255;          // reset the window for the next second
    dacWinMax    = 0;
    d["running"] = (bool)running;
    d["passes"]  = (uint32_t)passesDone;
    d["dropped"] = (uint32_t)markerDropped;
    d["heap"]    = (uint32_t)ESP.getFreeHeap();
    d["upMs"]    = nowMs;
    emit(d);
}

// One line per generated pass, emitted from loop() AFTER the pass has already
// finished playing.  Deliberately not in the ISR and not real-time: the point
// is confirmation that the stimulus went out, and nothing here may perturb the
// envelope timing the whole rig exists to keep clean.
//
// This is separate from the "pass" event, which only appears when FPVRaceOne
// raises its marker — i.e. when it DETECTED the pass.  Comparing the two tells
// you whether a missing lap was never generated or generated but not seen.
static void emitPassSent() {
    static uint32_t lastReported = 0;
    const uint32_t done = passesDone;
    if (done == lastReported) return;
    lastReported = done;

    JsonDocument d;
    d["event"]    = "passSent";
    d["n"]        = done;
    d["peak"]     = profile.peak;              // commanded
    d["peakObs"]  = (uint8_t)lastPassObsMax;   // actually reached
    d["baseline"] = profile.baseline;
    d["widthMs"]  = profile.widthMs;
    emit(d);
}

void loop() {
    emitHeartbeat(millis());
    emitPassSent();
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
