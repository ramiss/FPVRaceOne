#pragma once

// ── Per-task CPU load monitor ───────────────────────────────────────────────
//
// WHAT THIS PROVES, AND WHAT IT DOES NOT
//
// This reports HEADROOM — how much CPU each FreeRTOS task consumes and how
// much is left idle.  It is the answer to "can this chip host the web
// interface, AP, receiver polling and race logic at the same time", broken
// down per subsystem so the cost of each is visible rather than argued about.
//
// It is NOT proof of timing punctuality, and must not be presented as such:
//
//   * A task blocked on socket I/O consumes ZERO cycles while it waits.  A
//     700 ms HTTP stall therefore shows up as LOW CPU, not high.  CPU load
//     cannot see that class of delay at all.
//   * Averages hide tails.  A system can sit at 30% mean load and still
//     contain a single 300 ms scheduling gap — and it is the gap that loses
//     a lap, not the average.
//
// The direct evidence for punctuality is the worst RSSI sample gap, reported
// separately by TIMING_STATS_ENABLED in lib/LAPTIMER.  Use the two together:
// low CPU + small worst-gap is a strong result; low CPU + large worst-gap
// means blocking I/O, which the CPU figure alone would have hidden.
//
// IMPLEMENTATION
//
// Uses uxTaskGetSystemState(), which requires the following (all confirmed
// present in the pioarduino ESP32-C6 package sdkconfig — no custom build):
//
//   CONFIG_FREERTOS_USE_TRACE_FACILITY=y
//   CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
//   CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32=y
//
// Load is a DELTA between two snapshots — the absolute counters are
// cumulative since boot and would only ever report a lifetime average.
// The U32 run-time counter wraps roughly every 71 minutes at 1 MHz; plain
// unsigned subtraction handles that correctly, so no wrap check is needed.

#include <Arduino.h>
#include "config.h"

#if CPU_MONITOR_ENABLED

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef CPUMON_MAX_TASKS
#define CPUMON_MAX_TASKS 24
#endif

#ifndef CPUMON_TOP_TASKS
#define CPUMON_TOP_TASKS 6
#endif

class CpuMonitor {
   public:
    struct TaskLoad {
        char     name[configMAX_TASK_NAME_LEN];
        uint8_t  percent;      // 0-100, integer
        uint32_t deltaRuntime;
    };

    // Last completed measurement window.  Published whole, so a reader never
    // sees a half-filled window and never has to block to get a result.
    struct Snapshot {
        TaskLoad top[CPUMON_TOP_TASKS];
        uint8_t  taskCount   = 0;
        uint8_t  idlePercent = 0;
        uint8_t  busyPercent = 0;
        bool     valid       = false;
    };

    // Singleton — mirrors the DebugLogger pattern already used in lib/DEBUG.
    // The window is driven from parallelTask (see main.cpp) and read from the
    // Diagnostics handler, so a single shared instance is what we want.
    static CpuMonitor& getInstance() {
        static CpuMonitor instance;
        return instance;
    }

    // Drive this from a periodic task.  It opens the first window on the
    // initial call and closes/republishes one every CPU_MONITOR_WINDOW_MS.
    //
    // Deliberately NOT called from the web handler: computing load requires a
    // measurement window, and blocking an AsyncWebServer handler for a couple
    // hundred ms risks the TCP slot exhaustion this project is sensitive to.
    void tick(uint32_t nowMs) {
        if (!_havePrev) {
            _prevCount    = _snapshot(_prev, &_prevTotal);
            _havePrev     = (_prevCount > 0);
            _windowStart  = nowMs;
            return;
        }
        if (nowMs - _windowStart < CPU_MONITOR_WINDOW_MS) return;
        _windowStart = nowMs;

        Snapshot s;
        s.taskCount = sample(s.top, CPUMON_TOP_TASKS, &s.idlePercent);
        if (s.taskCount > 0) {
            s.busyPercent = busyPercent(s.idlePercent);
            s.valid       = true;
            _published    = s;
        }
    }

    const Snapshot& getLast() const { return _published; }

    // Take the first snapshot.  Call once, then call sample() later — the
    // gap between the two calls is the measurement window.  A window of a
    // few seconds gives stable numbers; very short windows are noisy.
    void begin() {
        _prevCount = _snapshot(_prev, &_prevTotal);
        _havePrev  = (_prevCount > 0);
    }

    // Close the window and compute per-task load.  Returns the number of
    // entries written to out[], most-loaded first.  idlePercent receives the
    // combined IDLE task share.  Safe to call repeatedly; each call starts a
    // fresh window from the current instant.
    uint8_t sample(TaskLoad* out, uint8_t maxOut, uint8_t* idlePercent) {
        if (idlePercent) *idlePercent = 0;
        if (!_havePrev || !out || maxOut == 0) return 0;

        // static, not stack: TaskStatus_t[24] is ~1 KB, and this runs from
        // parallelTask whose stack is shared with the rest of the Core-0
        // work.  Single-caller by construction (tick() only), so there is no
        // reentrancy concern.
        static TaskStatus_t cur[CPUMON_MAX_TASKS];
        uint32_t curTotal = 0;
        const UBaseType_t curCount = _snapshot(cur, &curTotal);
        if (curCount == 0) return 0;

        // Wrap-safe: unsigned subtraction is correct across a U32 rollover.
        const uint32_t totalDelta = curTotal - _prevTotal;
        if (totalDelta == 0) return 0;   // window too short to resolve

        uint8_t written = 0;
        uint32_t idleDelta = 0;

        for (UBaseType_t i = 0; i < curCount; i++) {
            // Match by task handle, not name — names are not unique (both
            // idle tasks are "IDLE") and handles are stable for a task's life.
            uint32_t prevRuntime = 0;
            bool     found       = false;
            for (UBaseType_t j = 0; j < _prevCount; j++) {
                if (_prev[j].xHandle == cur[i].xHandle) {
                    prevRuntime = _prev[j].ulRunTimeCounter;
                    found = true;
                    break;
                }
            }
            // A task created mid-window has no baseline; counting its full
            // cumulative runtime would wildly overstate its share, so skip it.
            if (!found) continue;

            const uint32_t d = cur[i].ulRunTimeCounter - prevRuntime;

            if (strncmp(cur[i].pcTaskName, "IDLE", 4) == 0) {
                idleDelta += d;
                continue;
            }
            if (written < maxOut) {
                strncpy(out[written].name, cur[i].pcTaskName, configMAX_TASK_NAME_LEN - 1);
                out[written].name[configMAX_TASK_NAME_LEN - 1] = '\0';
                out[written].deltaRuntime = d;
                out[written].percent      = (uint8_t)((uint64_t)d * 100ULL / totalDelta);
                written++;
            }
        }

        if (idlePercent) {
            *idlePercent = (uint8_t)((uint64_t)idleDelta * 100ULL / totalDelta);
        }

        // Descending by load — insertion sort, tiny N, keeps the report
        // readable without pulling in <algorithm>.
        for (uint8_t i = 1; i < written; i++) {
            TaskLoad key = out[i];
            int8_t j = (int8_t)i - 1;
            while (j >= 0 && out[j].deltaRuntime < key.deltaRuntime) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = key;
        }

        // Roll the window forward so the next call measures from here.
        memcpy(_prev, cur, sizeof(TaskStatus_t) * curCount);
        _prevCount = curCount;
        _prevTotal = curTotal;
        return written;
    }

    // Convenience: total non-idle load for the window just closed.
    static uint8_t busyPercent(uint8_t idlePercent) {
        return (idlePercent >= 100) ? 0 : (uint8_t)(100 - idlePercent);
    }

   private:
    static UBaseType_t _snapshot(TaskStatus_t* buf, uint32_t* totalRunTime) {
        const UBaseType_t n = uxTaskGetNumberOfTasks();
        if (n == 0 || n > CPUMON_MAX_TASKS) {
            // Buffer would overflow — report nothing rather than truncate to
            // a partial task set, which would skew every percentage.
            if (totalRunTime) *totalRunTime = 0;
            return 0;
        }
        return uxTaskGetSystemState(buf, CPUMON_MAX_TASKS, totalRunTime);
    }

    TaskStatus_t _prev[CPUMON_MAX_TASKS];
    UBaseType_t  _prevCount   = 0;
    uint32_t     _prevTotal   = 0;
    bool         _havePrev    = false;
    uint32_t     _windowStart = 0;
    Snapshot     _published;
};

#endif  // CPU_MONITOR_ENABLED
