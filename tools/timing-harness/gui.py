#!/usr/bin/env python3
"""
FPVRaceOne Timing Rig — GUI

Point-and-click front end for harness.py.  Runs the same measurements and adds
plain-language interpretation, because the raw numbers are easy to misread:
bias and jitter mean very different things, and only one of them actually costs
you a lap.

Tkinter deliberately: it ships with Python, so this runs on a clean Windows box
with nothing installed but pyserial (already needed for the harness itself).

    python gui.py

Measurement logic is imported from harness.py rather than duplicated, so the
CLI and GUI can never drift apart.
"""

import json
import os
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, scrolledtext

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not found. Install with:  pip install pyserial")

from harness import (JsonLink, run_once, summarize, open_port_no_reset,
                     measure_transfer, diagnose_transfer, EXPECTED_GAIN,
                     read_device_config, check_interval_vs_minlap, set_min_lap,
                     check_maxlaps_vs_count, emulator_handshake,
                     compute_thresholds, set_thresholds, _fit_line,
                     geometric_detect_ms, plan_envelope,
                     find_min_detectable_width, estimate_search_runs,
                     MultiNodeLoad, probe_master, set_voice_enabled)


# ── Interpretation ──────────────────────────────────────────────────────────
#
# Thresholds are judgement calls, chosen so a "GOOD" verdict means the effect
# is well below anything that could matter in a race, not merely measurable.

# Mirrored from firmware so measured stalls can be expressed as a duty cycle.
# Keep in step with lib/MULTINODE/multinode.h and multinode.cpp.
FANOUT_THROTTLE_MS       = 2000.0   # MIN_DIRECTOR_BROADCAST_INTERVAL_MS
FANOUT_CLIENT_TIMEOUT_MS = 300      # http.setTimeout() in _broadcastDirectorState
FANOUT_RACE_TIMEOUT_MS   = 500      # http.setTimeout() in _broadcastRaceStart/Stop

JITTER_GOOD_MS = 5.0     # stdev below this is inaudible in a lap time
JITTER_WARN_MS = 15.0
BIAS_NOTE_MS   = 20.0    # bias worth mentioning for cross-device comparison
LAT_GOOD_MS    = 30.0


def interpret_interval(res, st, interval_ms, geo_floor_ms=None):
    """Turn interval-test statistics into sentences that say what they mean.

    geo_floor_ms is the earliest a lap could possibly be confirmed for this
    pass width, from envelope geometry alone. Passing it lets the latency
    verdict judge pipeline lag rather than raw latency.
    """
    out = []

    # Before blaming detection, check the RIG actually produced the stimulus.
    # The emulator can reset or lose USB mid-run (a vanishing COM port is the
    # tell), and a stalled rig looks exactly like a device detecting nothing.
    if res.generated >= 0 and res.generated < res.expected:
        out.append(
            f"RIG FAULT — NOT A DEVICE PROBLEM: the emulator reports generating "
            f"only {res.generated} of {res.expected} passes. The device cannot "
            f"detect passes that were never played, so the miss count below is "
            f"the rig's, not the timer's. Check the emulator's USB connection "
            f"(a changing serial-port count is the tell) and re-run."
        )
    elif res.generated < 0:
        out.append(
            f"RIG WARNING: the emulator never sent its 'done' event, so the "
            f"number of passes actually played is unknown. Treat a high miss "
            f"count with suspicion until a clean run confirms the rig."
        )

    missed = res.expected - res.detected
    if missed > 0:
        out.append(
            f"MISSED PASSES: {missed} of {res.expected} generated passes produced "
            f"no lap. That is a detection failure, not a timing one — check the "
            f"enter/exit thresholds and the peak level before reading anything "
            f"below."
        )

    if not st:
        out.append("Not enough laps to compute statistics.")
        return out

    j = st["stdev_ms"]
    if j <= JITTER_GOOD_MS:
        out.append(
            f"CONSISTENCY: GOOD — {j:.2f} ms standard deviation. This is the "
            f"number that actually costs you a lap: lap-time error is the "
            f"difference in jitter between the two crossings, so low jitter "
            f"means repeatable lap times."
        )
    elif j <= JITTER_WARN_MS:
        out.append(
            f"CONSISTENCY: MARGINAL — {j:.2f} ms standard deviation. Visible in "
            f"a lap time but small against typical lap lengths. Worth checking "
            f"whether it grows under multi-node load."
        )
    else:
        out.append(
            f"CONSISTENCY: POOR — {j:.2f} ms standard deviation. Lap times will "
            f"vary noticeably run to run. Check Diagnostics -> RSSI Sample "
            f"Timing for a large worst-gap, which would point at a scheduling "
            f"stall."
        )

    b = st["bias_ms"]
    if abs(b) <= BIAS_NOTE_MS:
        out.append(
            f"BIAS: {b:+.2f} ms. Largely harmless on its own — the same delay "
            f"applies to both crossings of a lap and subtracts out. It matters "
            f"only when comparing two DIFFERENT devices head to head."
        )
    else:
        out.append(
            f"BIAS: {b:+.2f} ms — large. Still mostly cancels within a single "
            f"device's lap times, but two units with different bias would "
            f"disagree by roughly the difference in head-to-head racing."
        )

    if "lat_mean_us" in st:
        lm = st["lat_mean_us"] / 1000.0
        lj = st["lat_stdev_us"] / 1000.0

        # Judge the PIPELINE contribution, not raw latency.  A lap is confirmed
        # on gate exit, so most of the measured figure is simply "the pass had
        # to finish first" — a floor no firmware can beat.  An earlier version
        # compared raw latency against a flat 30 ms and called a perfectly
        # healthy 42 ms "HIGH", which was wrong and alarming.
        floor = geo_floor_ms
        if floor is not None:
            pipe = lm - floor
            if pipe <= 15.0:
                tag = "LATENCY GOOD"
            elif pipe <= 30.0:
                tag = "LATENCY MARGINAL"
            else:
                tag = "LATENCY HIGH"
            out.append(
                f"{tag}: {lm:.2f} ms total, of which {floor:.1f} ms is the pass "
                f"itself and only {pipe:.1f} ms is pipeline lag. A lap is "
                f"confirmed on gate EXIT, not at the peak, so a {res.width_ms} ms "
                f"pass cannot be detected sooner than {floor:.1f} ms no matter "
                f"what the firmware does. The {pipe:.1f} ms is median-filter "
                f"group delay plus the enter debounce."
            )
        else:
            out.append(
                f"LATENCY: {lm:.2f} ms mean, {lj:.2f} ms stdev, stimulus to "
                f"detection. Most of this is the pass width — a lap is confirmed "
                f"on gate exit, not at the peak."
            )

        # Separate real spread from outliers before drawing conclusions.
        if "lat_iqr_us" in st:
            iqr = st["lat_iqr_us"] / 1000.0
            med = st["lat_median_us"] / 1000.0
            outl = st.get("lat_outliers", 0)
            if lj > 2.0 * max(iqr, 0.01) and outl:
                out.append(
                    f"NOTE ON LATENCY SPREAD: stdev is {lj:.2f} ms but the "
                    f"inter-quartile range is only {iqr:.2f} ms, with {outl} "
                    f"outlier(s). The typical pass is tight — the stdev is being "
                    f"inflated by a few stray samples, not by genuine jitter. "
                    f"Median latency is {med:.2f} ms."
                )
            else:
                out.append(
                    f"LATENCY SPREAD: stdev {lj:.2f} ms, IQR {iqr:.2f} ms, "
                    f"median {med:.2f} ms. The two agree, so this is real "
                    f"spread rather than a few outliers."
                )

        if res.dropped:
            out.append(
                f"RIG NOTE: the emulator dropped {res.dropped} marker edge(s) "
                f"because a previous one had not been read yet. Those are lost "
                f"MEASUREMENTS, not missed detections — the device found the "
                f"laps. It only reduces the latency sample count."
            )

        out.append(
            f"IMPORTANTLY, THIS DOES NOT AFFECT LAP TIMES. Lap timing is "
            f"anchored to the recorded PEAK timestamp, not to when detection "
            f"finished, which is why bias is {st['bias_ms']:+.2f} ms despite "
            f"{lm:.0f} ms of detection latency. Latency jitter is {lj:.2f} ms "
            f"yet lap-time jitter is only {st['stdev_ms']:.2f} ms — the "
            f"peak-anchoring is measurably doing its job."
        )
    else:
        out.append(
            "ABSOLUTE LATENCY: not measured — no marker edges seen. Set "
            "TIMING_MARKER_ENABLED to 1 in lib/CONFIG/config.h, reflash, and "
            "confirm D3/GPIO21 is wired to the emulator's GPIO4."
        )

    return out


def interpret_sweep(rows):
    """rows: list of (width_ms, rate, stats-or-None)."""
    out = []
    reliable = [w for w, r, _ in rows if r >= 0.99]
    partial  = [w for w, r, _ in rows if 0.0 < r < 0.99]

    if reliable:
        best = min(reliable)
        out.append(
            f"MINIMUM RELIABLE PASS: {best} ms at >=99% detection. Shorter "
            f"passes than this start to be missed."
        )
        out.append(
            f"For context, a pass whose RSSI envelope is {best} ms wide is a "
            f"very fast gate transit. The narrower this number, the faster the "
            f"drone the pipeline can catch."
        )
    else:
        out.append(
            "NO WIDTH REACHED 99% DETECTION. Either the peak level is too close "
            "to the enter threshold, or calibration does not match the emulated "
            "envelope. Re-run the calibration wizard with the emulator "
            "connected, then retry."
        )

    if partial:
        out.append(
            f"PARTIAL DETECTION between {min(partial)} and {max(partial)} ms — "
            f"this is the roll-off band where the filter starts losing peaks."
        )

    out.append(
        "Compare this figure between polled and DMA modes. If they land in the "
        "same place, DMA peak-hold is not earning its complexity and polled is "
        "the simpler choice."
    )
    return out


# ── GUI ─────────────────────────────────────────────────────────────────────

class TimingRigGUI:
    def __init__(self, root):
        self.root = root
        root.title("FPVRaceOne Timing Rig")
        root.geometry("880x720")

        self.q = queue.Queue()
        self.worker = None
        self.cancel = threading.Event()

        # Live-monitor state.  Buffers are bounded: this runs for hours between
        # tests and must not grow without limit.
        self._mon_thread = None
        self._mon_stop = threading.Event()
        self._mon_resume_after_test = False
        self._mon_lines = {"DUT": [], "EMU": []}
        self._MON_MAX_LINES = 20000
        # Raw serial goes to its own window rather than the results pane, so a
        # long-running monitor cannot bury the test output that gets shared.
        self._log_win = None
        self._log_text = {}
        self._log_follow = {}

        self._build_ports(root)
        self._build_settings(root)
        self._build_buttons(root)
        self._build_output(root)

        # Capture the as-built values BEFORE restoring, so "Reset to Defaults"
        # has something to go back to without re-reading the source.
        self._defaults = {k: v.get() for k, v in self._setting_vars().items()}
        self.restore_settings()

        self.refresh_ports()
        # Listen from launch.  The faults worth catching (the master rebooting
        # itself, the emulator panicking, clients cycling) happen while nobody
        # is running a test, so a monitor that only starts on demand misses
        # exactly the events it exists for.  Deferred slightly so the port
        # comboboxes and the drain loop are live first.
        if self.monitor_on.get():
            self.root.after(300, self._autostart_monitor)
        self._update_sweep_estimate()   # traces only fire on edit, so seed it
        self.root.after(100, self._drain)

    # -- widgets ------------------------------------------------------------

    def _build_ports(self, root):
        f = ttk.LabelFrame(root, text="Serial ports", padding=8)
        f.pack(fill="x", padx=10, pady=(10, 4))

        ttk.Label(f, text="Emulator (WROOM-32):").grid(row=0, column=0, sticky="w")
        self.emu_cb = ttk.Combobox(f, width=46, state="readonly")
        self.emu_cb.grid(row=0, column=1, padx=6, pady=2)

        ttk.Label(f, text="FPVRaceOne (XIAO C6):").grid(row=1, column=0, sticky="w")
        self.dut_cb = ttk.Combobox(f, width=46, state="readonly")
        self.dut_cb.grid(row=1, column=1, padx=6, pady=2)

        ttk.Button(f, text="Refresh", command=self.refresh_ports)\
            .grid(row=0, column=2, padx=6)
        ttk.Button(f, text="Remember Ports", command=self.remember_ports)\
            .grid(row=1, column=2, padx=6)
        ttk.Button(f, text="Save Settings", command=self.save_settings)\
            .grid(row=0, column=3, padx=6)
        ttk.Button(f, text="Reset to Defaults", command=self.reset_settings)\
            .grid(row=1, column=3, padx=6)

        # Live monitor.  Several faults this session were only visible in raw
        # serial output that no test was capturing at the time — a floating
        # marker pin hanging the emulator, the master self-rebooting on low
        # heap, clients cycling between timeout and re-registration.  Watching
        # continuously, outside a test run, is how those get seen at all.
        self.monitor_on = tk.BooleanVar(value=True)
        ttk.Checkbutton(f, text="Live monitor", variable=self.monitor_on,
                        command=self._toggle_monitor)\
            .grid(row=0, column=4, padx=(12, 2), sticky="w")
        ttk.Button(f, text="Save Logs", command=self.save_logs)\
            .grid(row=1, column=4, padx=(12, 2), sticky="w")
        ttk.Button(f, text="Log Panel \u2197", command=self.open_log_panel)\
            .grid(row=0, column=5, rowspan=2, padx=(6, 2))

    def _build_settings(self, root):
        # Settings are grouped by WHICH TEST USES THEM.  A single flat panel
        # put "Width" and "Sweep widths" side by side with no indication that
        # each is read by a different button and ignored by the other, which
        # is a fair way to confuse someone.
        self.vars = {}

        # ── Shared: the envelope every test drives the emulator with ────────
        f = ttk.LabelFrame(root, text="Signal — used by all tests", padding=8)
        f.pack(fill="x", padx=10, pady=(4, 2))

        def spin(parent, col, label, key, default, lo, hi, tip="", row=0):
            ttk.Label(parent, text=label).grid(row=row, column=col * 2,
                                               sticky="e", padx=(8, 2))
            v = tk.StringVar(value=str(default))
            ttk.Spinbox(parent, from_=lo, to=hi, textvariable=v, width=8)\
                .grid(row=row, column=col * 2 + 1, sticky="w")
            self.vars[key] = v
            if tip:
                ttk.Label(parent, text=tip, foreground="#666", font=("", 8))\
                    .grid(row=row + 1, column=col * 2, columnspan=2,
                          sticky="w", padx=(8, 2))

        spin(f, 0, "Baseline:", "baseline", 40, 0, 255, "at rest")
        spin(f, 1, "Peak:", "peak", 200, 0, 255, "at apex")
        spin(f, 2, "Interval (ms):", "interval", 3000, 100, 60000, "pass to pass")
        spin(f, 3, "Count:", "count", 30, 2, 500, "passes per run")

        # Min Lap discards any crossing sooner than its threshold (default
        # 5000 ms).  Zeroing it lets the rig use short intervals, which turns a
        # 16-minute sweep into about 4.  Restored afterwards.
        self.zero_minlap = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Force device Min Lap = 0 during run (restored after)",
                        variable=self.zero_minlap)\
            .grid(row=2, column=0, columnspan=8, sticky="w", padx=(8, 0), pady=(6, 0))

        # Announcements are rendered in the browser, never on the device, so
        # muting them cannot affect the measurement — it just spares a long
        # TTS backlog after a 30-lap run.
        self.mute_voice = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Mute device announcements during run "
                                "(restored after; refresh the browser tab to apply)",
                        variable=self.mute_voice)\
            .grid(row=3, column=0, columnspan=8, sticky="w", padx=(8, 0), pady=(2, 0))

        # ── Test 3 only ────────────────────────────────────────────────────
        f3 = ttk.LabelFrame(root, text="Test 3 only — Accuracy + Consistency",
                            padding=8)
        f3.pack(fill="x", padx=10, pady=2)
        spin(f3, 0, "Pass width (ms):", "width", 40, 1, 1000,
             "one fixed width; measures jitter at that width")

        # ── Test 4 only ────────────────────────────────────────────────────
        # Binary search rather than a hand-written list of widths: a list
        # spends most of its runs far from the boundary while still only
        # resolving to whatever spacing was typed.  The search halves until
        # detection fails, then bisects — same answer, fewer runs, and to a
        # stated precision.
        f4 = ttk.LabelFrame(root, text="Test 4 only — Min Pass Width "
                                       "(automatic search)", padding=8)
        f4.pack(fill="x", padx=10, pady=2)

        spin(f4, 0, "Start width (ms):", "sweep_start", 100, 5, 1000,
             "must detect reliably")
        spin(f4, 1, "Resolution (ms):", "sweep_res", 2, 1, 50,
             "how precisely to find it")
        spin(f4, 2, "Passes per step:", "sweep_count", 10, 3, 100,
             "more = slower, surer")
        spin(f4, 3, "Require (%):", "sweep_require", 90, 50, 100,
             "counts as 'detects'")

        # ── Multi-node load (optional, applies to tests 3 and 4) ───────────
        # Drives the master's expensive path — inbound lap -> blocking
        # directorState fanout to every registered client — at a known rate,
        # while the timing test runs underneath.  Real clients must be
        # registered: handleLap rejects unknown nodeIds and the fanout needs
        # somewhere to go.
        fL = ttk.LabelFrame(root, text="Multi-node load (optional) — applies to "
                                       "tests 3 and 4", padding=8)
        fL.pack(fill="x", padx=10, pady=2)

        self.load_on = tk.BooleanVar(value=False)
        ttk.Checkbutton(fL, text="Generate master load during the run",
                        variable=self.load_on)\
            .grid(row=0, column=0, columnspan=6, sticky="w", padx=(8, 0))

        # A master serves its AP on 192.168.5.1 (MULTINODE_MASTER_IP); a
        # single/standalone unit uses 192.168.4.1.  Default to the master
        # address, since load testing only applies in multi-node mode.
        ttk.Label(fL, text="Master IP:").grid(row=1, column=0, sticky="e", padx=(8, 2))
        self.master_ip = tk.StringVar(value="192.168.5.1")
        ttk.Entry(fL, textvariable=self.master_ip, width=16)\
            .grid(row=1, column=1, sticky="w")

        ttk.Label(fL, text="Node IDs:").grid(row=1, column=2, sticky="e", padx=(8, 2))
        self.load_nodes = tk.StringVar(value="1,2")
        ttk.Entry(fL, textvariable=self.load_nodes, width=12)\
            .grid(row=1, column=3, sticky="w")

        ttk.Label(fL, text="Pattern:").grid(row=1, column=4, sticky="e", padx=(8, 2))
        self.load_mode = tk.StringVar(value="burst")
        ttk.Combobox(fL, textvariable=self.load_mode, width=7, state="readonly",
                     values=("burst", "spread")).grid(row=1, column=5, sticky="w")

        ttk.Label(fL, text="Lap (s):").grid(row=1, column=6, sticky="e", padx=(8, 2))
        self.burst_interval = tk.StringVar(value="3.0")
        ttk.Entry(fL, textvariable=self.burst_interval, width=6)\
            .grid(row=1, column=7, sticky="w")

        ttk.Label(fL, text="Quiet before stop (s):").grid(row=2, column=4, sticky="e", padx=(8, 2))
        self.stop_delay = tk.StringVar(value="0")
        ttk.Entry(fL, textvariable=self.stop_delay, width=6)            .grid(row=2, column=5, sticky="w")
        ttk.Label(fL, text="Pack (ms):").grid(row=1, column=8, sticky="e", padx=(8, 2))
        self.burst_spread = tk.StringVar(value="10")
        ttk.Entry(fL, textvariable=self.burst_spread, width=6)\
            .grid(row=1, column=9, sticky="w")

        # Kept for 'spread' mode and for comparison against earlier results.
        self.load_rate = tk.StringVar(value="2.0")

        ttk.Label(fL, text="Source IPs:").grid(row=2, column=0, sticky="e", padx=(8, 2))
        self.load_srcs = tk.StringVar(value="")
        ttk.Entry(fL, textvariable=self.load_srcs, width=30)\
            .grid(row=2, column=1, columnspan=3, sticky="w", pady=(2, 0))
        ttk.Label(fL, text="blank = let the OS choose. Two comma-separated local "
                           "IPs splits the load across both WiFi adapters as "
                           "independent stations.",
                  foreground="#666", font=("", 8))\
            .grid(row=3, column=0, columnspan=6, sticky="w", padx=(8, 2))

        ttk.Button(fL, text="Check master", command=self.check_master)\
            .grid(row=1, column=6, padx=(10, 0))

        self.sweep_est = ttk.Label(f4, text="", foreground="#666", font=("", 8))
        self.sweep_est.grid(row=2, column=0, columnspan=8, sticky="w",
                            padx=(8, 2), pady=(6, 0))
        for k in ("sweep_start", "sweep_res", "sweep_count", "interval"):
            self.vars[k].trace_add("write", lambda *_: self._update_sweep_estimate())

        # Was needed when the polled and DMA paths had ~1.9x different ADC
        # gain: identical DAC values gave each mode a different RSSI envelope.
        # The firmware now scales the DMA path to match, so raw DAC values are
        # directly comparable again and this is OFF by default — leaving it on
        # would actually MASK a residual gain mismatch by silently correcting
        # for it, which is the opposite of what you want while verifying.
        #
        # Turn it on when the envelope must be pinned regardless of gain, e.g.
        # on a chip whose RSSI_DMA_GAIN_NUM has not been re-derived.
        self.target_rssi = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Baseline/Peak are RSSI targets (off = raw DAC; "
                                "on = pin the envelope regardless of gain)",
                        variable=self.target_rssi)\
            .grid(row=4, column=0, columnspan=8, sticky="w", padx=(8, 0), pady=(2, 0))

    def _build_buttons(self, root):
        f = ttk.Frame(root, padding=(10, 4))
        f.pack(fill="x")

        self.btn_level = ttk.Button(f, text="1. Check Analog Path",
                                    command=self.run_level)
        self.btn_level.pack(side="left", padx=(0, 6))

        self.btn_thresh = ttk.Button(f, text="2. Set Thresholds",
                                     command=self.run_thresholds)
        self.btn_thresh.pack(side="left", padx=6)

        self.btn_interval = ttk.Button(f, text="3. Accuracy + Consistency",
                                       command=self.run_interval)
        self.btn_interval.pack(side="left", padx=6)

        self.btn_sweep = ttk.Button(f, text="4. Min Pass Width (Sweep)",
                                    command=self.run_sweep)
        self.btn_sweep.pack(side="left", padx=6)

        self.btn_stop = ttk.Button(f, text="Stop", command=self.stop_run,
                                   state="disabled")
        self.btn_stop.pack(side="left", padx=6)

        ttk.Button(f, text="Clear", command=self.clear).pack(side="right")

    def _build_output(self, root):
        f = ttk.LabelFrame(root, text="Results", padding=6)
        f.pack(fill="both", expand=True, padx=10, pady=(4, 10))

        self.out = scrolledtext.ScrolledText(f, wrap="word", height=24,
                                             font=("Consolas", 9))
        self.out.pack(fill="both", expand=True)
        self.out.tag_config("head", font=("Consolas", 10, "bold"))
        self.out.tag_config("good", foreground="#0a7d33")
        self.out.tag_config("warn", foreground="#b06f00")
        self.out.tag_config("bad", foreground="#b00020")
        self.out.tag_config("dim", foreground="#666666")

        self.status = ttk.Label(root, text="Ready", relief="sunken", anchor="w")
        self.status.pack(fill="x", side="bottom")

    # -- helpers ------------------------------------------------------------

    # Where the chosen ports are remembered between sessions.  Keyed by USB
    # serial number, not COM port: COM numbers move when devices are re-plugged,
    # while the serial number is stable.
    _PREFS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "rig_settings.json")

    def _load_prefs(self):
        try:
            with open(self._PREFS_PATH, "r", encoding="utf-8") as fh:
                return json.load(fh)
        except Exception:
            return {}

    def _write_prefs(self, prefs):
        try:
            with open(self._PREFS_PATH, "w", encoding="utf-8") as fh:
                json.dump(prefs, fh, indent=2, sort_keys=True)
            return True
        except Exception as e:
            self.log(f"Could not save settings: {e}\n", "warn")
            return False

    def _load_port_prefs(self):
        return self._load_prefs().get("ports", {})

    def _save_port_prefs(self, ports):
        prefs = self._load_prefs()
        prefs["ports"] = ports
        self._write_prefs(prefs)

    def _setting_vars(self):
        """Every tk variable that represents a user setting.

        Discovered by type rather than listed by hand, so a setting added later
        is saved automatically instead of being silently forgotten — the usual
        way these files rot.
        """
        found = {}
        for name, val in vars(self).items():
            if name.startswith("_"):
                continue
            if isinstance(val, (tk.StringVar, tk.BooleanVar,
                                tk.IntVar, tk.DoubleVar)):
                found[name] = val
        for key, val in getattr(self, "vars", {}).items():
            found[f"vars.{key}"] = val        # the Signal spinboxes
        return found

    def save_settings(self):
        prefs = self._load_prefs()
        prefs["settings"] = {k: v.get() for k, v in self._setting_vars().items()}
        if self._write_prefs(prefs):
            self.log(f"Saved {len(prefs['settings'])} setting(s). They will be "
                     f"restored next time the GUI starts.\n", "good")

    def reset_settings(self):
        """Back to the values the GUI ships with, and forget the saved ones.

        Without this, one bad saved value (a 60000 ms interval, load pointed at
        a stale IP) persists across every future launch with no obvious way out
        short of deleting the file by hand.
        """
        for name, var in self._setting_vars().items():
            if name in getattr(self, "_defaults", {}):
                try:
                    var.set(self._defaults[name])
                except Exception:
                    pass
        prefs = self._load_prefs()
        prefs.pop("settings", None)          # ports are kept deliberately
        self._write_prefs(prefs)
        self.log("Settings reset to defaults and the saved copy cleared. "
                 "Remembered ports are kept.\n", "good")

    def restore_settings(self, quiet=False):
        saved = self._load_prefs().get("settings", {})
        if not saved:
            return
        applied = 0
        for name, var in self._setting_vars().items():
            if name not in saved:
                continue
            try:
                var.set(saved[name])
                applied += 1
            except Exception:
                pass      # a stale or renamed setting must not stop the rest
        if applied and not quiet:
            self.log(f"Restored {applied} saved setting(s).\n", "dim")

    @staticmethod
    def _serial_of(label):
        """Pull the USB serial number back out of a combobox label."""
        parts = label.split("  ")
        return parts[2].strip() if len(parts) >= 3 else ""

    def remember_ports(self):
        """Pin the current selections by USB serial number.

        Stored as serial numbers rather than COM ports because COM numbers are
        reassigned on re-plug, while the serial number (the device MAC on ESP32
        native USB) identifies one specific unit forever.
        """
        prefs = self._load_port_prefs()
        dut_sn = self._serial_of(self.dut_cb.get())
        emu_sn = self._serial_of(self.emu_cb.get())
        if dut_sn and dut_sn != "no-serial":
            prefs["dut"] = dut_sn
        if emu_sn and emu_sn != "no-serial":
            prefs["emu"] = emu_sn
        self._save_port_prefs(prefs)
        self.log(f"Remembered: FPVRaceOne={prefs.get('dut','-')}  "
                 f"emulator={prefs.get('emu','-')}\n"
                 f"  These are matched by serial number from now on, whatever "
                 f"COM port they land on.\n", "good")

    def refresh_ports(self):
        ports = list(list_ports.comports())
        prefs = self._load_port_prefs()
        labels, emu_guess, dut_guess = [], None, None
        pref_emu_label, pref_dut_label = None, None
        for p in ports:
            vid = f"{p.vid:04X}" if p.vid else "????"
            # On ESP32 native USB the serial number IS the device MAC, which is
            # the only reliable way to tell the master from its client units —
            # they all enumerate as 303A:1001 with identical descriptions.
            sn = p.serial_number or "no-serial"
            label = f"{p.device}  [{vid}]  {sn}  {p.description[:28]}"
            labels.append(label)
            if sn and sn == prefs.get("dut"):
                pref_dut_label = label
            if sn and sn == prefs.get("emu"):
                pref_emu_label = label
            # Same discriminator the upload task uses: Espressif native USB
            # (0x303A) is the XIAO C6; a bridge chip is the WROOM-32 devkit.
            if p.vid == 0x303A and dut_guess is None:
                dut_guess = label
            elif p.vid in (0x10C4, 0x1A86, 0x0403) and emu_guess is None:
                emu_guess = label

        self.emu_cb["values"] = labels
        self.dut_cb["values"] = labels

        # Auto-pick by USB VID, but never override a choice already made and
        # still present.  Every client unit is also a 0x303A ESP32-C6, so
        # "first native-USB port found" is not reliably the master — once the
        # operator has picked the right one, a refresh must not silently move
        # the test to a different device.
        # Priority: remembered serial number > current selection > VID guess.
        def _pick(cb, remembered, guess):
            if remembered:
                cb.set(remembered)
                return remembered, True
            cur = cb.get()
            if cur and cur in labels:
                return cur, False        # still connected — leave it alone
            if guess:
                cb.set(guess)
                return guess, False
            return None, False

        _pick(self.emu_cb, pref_emu_label, emu_guess)
        dut_sel, dut_remembered = _pick(self.dut_cb, pref_dut_label, dut_guess)

        self.log(f"Found {len(ports)} serial port(s).\n", "dim")
        if dut_remembered:
            self.log(f"  FPVRaceOne restored from saved serial "
                     f"{self._serial_of(dut_sel)} on {dut_sel.split()[0]}.\n", "good")
        else:
            n303a = sum(1 for p in ports if p.vid == 0x303A)
            if n303a > 1 and dut_sel:
                self.log(f"  {n303a} devices share VID 0x303A (client units are "
                         f"ESP32-C6 too). Pick the master, then press Remember "
                         f"Ports\n  so it is matched by serial number "
                         f"(= its MAC) from now on.\n", "warn")

    @staticmethod
    def _port_of(label):
        return label.split()[0] if label else None

    # ── Live serial monitor ────────────────────────────────────────────
    def _autostart_monitor(self):
        """Deferred launch start — re-check the toggle at FIRE time.

        Checking only at schedule time was a race: switching the monitor off
        within the first 300 ms still let it grab the ports afterwards, so the
        checkbox and reality disagreed.
        """
        if self.monitor_on.get():
            self._start_monitor()

    def _toggle_monitor(self):
        if self.monitor_on.get():
            self._start_monitor()
        else:
            self._stop_monitor()

    def _start_monitor(self):
        if self._mon_thread and self._mon_thread.is_alive():
            return
        if self.worker and self.worker.is_alive():
            self.log("Cannot monitor while a test is running.\n", "warn")
            self.monitor_on.set(False)
            return
        ports = self._links()
        if not ports:
            self.monitor_on.set(False)
            return
        emu_p, dut_p = ports
        self._mon_stop.clear()
        self._mon_thread = threading.Thread(
            target=self._monitor_worker, args=(emu_p, dut_p), daemon=True)
        self._mon_thread.start()
        self.log(f"Live monitor ON — FPVRaceOne {dut_p}, emulator {emu_p}. "
                 f"Raw output, nothing filtered.\n"
                 f"  Open 'Log Panel' to view it. The ports are held while "
                 f"monitoring, so untick Live monitor before flashing from "
                 f"PlatformIO.\n", "good")

    def _stop_monitor(self):
        self._mon_stop.set()
        t = self._mon_thread
        if t:
            t.join(timeout=2.0)
        self._mon_thread = None
        self.monitor_on.set(False)

    def _monitor_worker(self, emu_p, dut_p):
        """Read both ports raw and stream them to the log panel.

        Deliberately does NOT parse or filter.  Every diagnostic gap this
        session came from a filter dropping the one line that mattered — the
        panic, the [HEAP] FATAL, the [CORE0] stall.  Raw means raw.

        Each port reconnects independently.  Both boards reset in normal use —
        the master on its own low-heap watchdog, the emulator when its port is
        opened — and a monitor that gave up on the first read error would go
        silent precisely when something interesting had just happened.
        """
        targets = {"DUT": dut_p, "EMU": emu_p}
        handles = {}          # name -> serial handle (absent = needs opening)
        buffers = {"DUT": "", "EMU": ""}
        next_try = {"DUT": 0.0, "EMU": 0.0}
        announced = set()

        def _open(name):
            now = time.time()
            if now < next_try[name]:
                return
            next_try[name] = now + 2.0        # retry cadence while unplugged
            try:
                handles[name] = open_port_no_reset(targets[name])
                if name in announced:
                    self.emit_mon_status(name, f"  Monitor: {name} reconnected "
                                               f"on {targets[name]}.\n", "good")
                    announced.discard(name)
            except Exception as e:
                if name not in announced:
                    hint = ""
                    if "denied" in str(e).lower() or "access" in str(e).lower():
                        hint = ("\n     Another program holds that port — a "
                                "PlatformIO/VSCode serial monitor, PuTTY, or a "
                                "second copy\n     of this GUI. Close it and "
                                "the monitor will reconnect on its own.")
                    self.emit_mon_status(name,
                        f"  Monitor: cannot open {name} on {targets[name]} — "
                        f"{e}{hint}\n", "bad")
                    announced.add(name)

        try:
            while not self._mon_stop.is_set():
                for name in ("DUT", "EMU"):
                    if name not in handles:
                        _open(name)
                        continue
                    try:
                        data = handles[name].read(4096).decode("utf-8",
                                                               errors="replace")
                    except Exception as e:
                        # Port vanished (board reset / cable). Drop it and let
                        # the reconnect loop pick it up again.
                        try:
                            handles[name].close()
                        except Exception:
                            pass
                        handles.pop(name, None)
                        self.emit_mon_status(name, f"  Monitor: {name} "
                                                   f"disconnected — {e}\n", "bad")
                        announced.add(name)
                        continue
                    if not data:
                        continue
                    buffers[name] += data
                    while "\n" in buffers[name]:
                        line, buffers[name] = buffers[name].split("\n", 1)
                        line = line.rstrip("\r")
                        if not line:
                            continue
                        stamp = time.strftime("%H:%M:%S")
                        rec = f"{stamp} [{name}] {line}"
                        lines = self._mon_lines[name]
                        lines.append(rec)
                        if len(lines) > self._MON_MAX_LINES:
                            del lines[:len(lines) - self._MON_MAX_LINES]
                        tag = self._mon_tag(line)
                        # The emulator heartbeat carries tickHz — the timer ISR
                        # rate.  It should sit at TICK_HZ whether or not a run
                        # is playing, so zero means the DAC has stopped being
                        # driven.  Call that out rather than leave an operator
                        # to notice one number among thousands of lines.
                        if name == "EMU" and '"event":"hb"' in line.replace(" ", ""):
                            m = re.search(r'"tickHz"\s*:\s*(\d+)', line)
                            if m:
                                hz = int(m.group(1))
                                if hz == 0:
                                    tag = "bad"
                                    self.emit_mon(name,
                                        "--- EMULATOR DAC STOPPED: tickHz=0, the "
                                        "envelope timer ISR is not running ---\n",
                                        "bad")
                                elif hz < 9000 or hz > 11000:
                                    tag = "warn"
                        if name == "EMU" and '"rst"' in line and "POWERON" not in line \
                                and "EXT_RESET" not in line:
                            self.emit_mon(name,
                                "--- EMULATOR RESTARTED for a non-normal reason "
                                "(see rst= above) ---\n", "bad")
                        self.emit_mon(name, rec + "\n", tag)
                time.sleep(0.02)
        finally:
            for sp in handles.values():
                try:
                    sp.close()
                except Exception:
                    pass
            self.emit("  Live monitor OFF.\n", "dim")

    def save_logs(self):
        """Write the monitor buffers and the on-screen results to text files."""
        out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
        try:
            os.makedirs(out_dir, exist_ok=True)
        except Exception as e:
            self.log(f"Could not create {out_dir}: {e}\n", "bad")
            return
        stamp = time.strftime("%Y%m%d-%H%M%S")
        written = []
        for name in ("DUT", "EMU"):
            lines = self._mon_lines.get(name, [])
            if not lines:
                continue
            path = os.path.join(out_dir, f"{stamp}-{name.lower()}-serial.txt")
            try:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(lines) + "\n")
                written.append((path, len(lines)))
            except Exception as e:
                self.log(f"Could not write {path}: {e}\n", "bad")
        # The results pane too — that is what actually gets shared.
        try:
            path = os.path.join(out_dir, f"{stamp}-results.txt")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(self.out.get("1.0", "end"))
            written.append((path, None))
        except Exception as e:
            self.log(f"Could not write results: {e}\n", "bad")

        if not written:
            self.log("Nothing to save yet — turn the live monitor on, or run a "
                     "test first.\n", "warn")
            return
        self.log(f"Saved {len(written)} file(s) to {out_dir}\n", "good")
        for path, n in written:
            self.log(f"   {os.path.basename(path)}"
                     f"{f'  ({n} lines)' if n else ''}\n", "dim")

    def log(self, text, tag=None):
        self.out.insert("end", text, tag or "")
        self.out.see("end")

    def clear(self):
        self.out.delete("1.0", "end")

    def _busy(self, busy):
        state = "disabled" if busy else "normal"
        for b in (self.btn_level, self.btn_thresh, self.btn_interval, self.btn_sweep):
            b.config(state=state)
        self.btn_stop.config(state="normal" if busy else "disabled")
        self.status.config(text="Running…" if busy else "Ready")

    def _ints(self):
        try:
            return {k: int(v.get()) for k, v in self.vars.items()}
        except ValueError:
            self.log("Settings must be whole numbers.\n", "bad")
            return None

    def _links(self):
        emu_p = self._port_of(self.emu_cb.get())
        dut_p = self._port_of(self.dut_cb.get())
        if not emu_p or not dut_p:
            self.log("Select both serial ports first.\n", "bad")
            return None
        if emu_p == dut_p:
            self.log("Emulator and FPVRaceOne cannot be the same port.\n", "bad")
            return None
        return emu_p, dut_p

    def _open_emu(self, emu_p):
        """Open the emulator port and CONFIRM it booted before using it.

        Opening the port asserts DTR/RTS, which hardware-resets the WROOM-32.
        The fixed settle in JsonLink is a guess; if the board is slow the
        commands that follow land in the bootloader and vanish.  The emulator
        then sits at baseline and every pass reads as missed — indistinguishable
        from a device-side detection failure, which is exactly how a run of 30
        passes reported 0 laps.
        """
        # keep_log=True: the emulator is an ESP32 and prints a panic message,
        # backtrace and ROM boot banner to the same serial port it speaks JSON
        # on.  Without capture those lines were discarded, so a crashing rig was
        # invisible and its runs looked like device detection failures.
        emu = JsonLink(emu_p, name="emu", keep_log=True,
                       on_line=self._mon_sink("EMU"))
        if emulator_handshake(emu) is None:
            emu.close()
            self.emit(
                "  Emulator did not respond to 'ping' after opening the port.\n"
                "  It resets when the port opens; if it is slow to boot the run\n"
                "  would silently generate nothing. Re-run, or check the cable\n"
                "  and port selection.\n", "bad")
            return None
        return emu

    def stop_run(self):
        self.cancel.set()
        self.status.config(text="Stopping…")

    def _start(self, fn, *a):
        if self.worker and self.worker.is_alive():
            return
        # A test needs exclusive access to both ports, so the monitor must
        # release them first.  Remember to bring it back afterwards so the
        # operator does not have to notice and re-tick it every run.
        if self._mon_thread and self._mon_thread.is_alive():
            self.log("Live monitor paused for the test; it will resume after.\n",
                     "dim")
            self._mon_resume_after_test = True
            self._stop_monitor()
        self.cancel.clear()
        self._busy(True)
        self.worker = threading.Thread(target=fn, args=a, daemon=True)
        self.worker.start()

    def _drain(self):
        """Pump worker messages onto the Tk thread."""
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "log":
                    text, tag = payload
                    self.log(text, tag)
                elif kind == "mon":
                    name, text, tag = payload
                    self._log_write(name, text, tag)
                elif kind == "done":
                    self._busy(False)
                    if self._mon_resume_after_test:
                        self._mon_resume_after_test = False
                        self.monitor_on.set(True)
                        self._start_monitor()
        except queue.Empty:
            pass
        self.root.after(100, self._drain)

    # ── Raw-serial side panel ──────────────────────────────────────────
    @staticmethod
    def _mon_tag(line):
        """Colour for a raw serial line. Shared so the live monitor and the
        during-test sink highlight the same things."""
        if any(k in line for k in ("FATAL", "Guru", "panic", "rst:",
                                   "assert", "Backtrace")):
            return "bad"
        if any(k in line for k in ("timed out", "WARN", "blocked")):
            return "warn"
        return "dim"

    def _mon_sink(self, name):
        """Callback that pushes one device's serial lines into the log panel.

        Given to JsonLink during tests so the panels keep filling while the
        monitor is paused.  Everything lands in the same _mon_lines buffers, so
        Save Logs produces one continuous record of the session rather than
        only the gaps between tests.
        """
        def sink(line):
            stamp = time.strftime("%H:%M:%S")
            rec = f"{stamp} [{name}] {line}"
            buf = self._mon_lines.setdefault(name, [])
            buf.append(rec)
            if len(buf) > self._MON_MAX_LINES:
                del buf[:len(buf) - self._MON_MAX_LINES]
            self.emit_mon(name, rec + "\n", self._mon_tag(line))
        return sink

    def emit_mon_status(self, name, text, tag=None):
        """Monitor connection status — shown in the MAIN pane as well.

        Status was previously sent only to the log panel, so "port unavailable"
        was invisible whenever that window happened to be closed: the operator
        saw an empty pane and no reason for it.  The most important message
        must not live in the window you might not have open.
        """
        self.emit(text, tag)
        self.q.put(("mon", (name, text, tag)))

    def emit_mon(self, name, text, tag=None):
        """Queue a raw serial line for the log panel (thread-safe)."""
        self.q.put(("mon", (name, text, tag)))

    def open_log_panel(self):
        """Two stacked raw-serial panes in a separate window.

        Separate from the results pane because the monitor runs for hours and
        would otherwise bury the test output — which is the part that gets
        saved and shared.  Stacked rather than side-by-side so long firmware
        lines stay readable without wrapping.
        """
        if self._log_win is not None and tk.Toplevel.winfo_exists(self._log_win):
            self._log_win.lift()
            self._log_win.focus_force()
            return

        win = tk.Toplevel(self.root)
        win.title("Raw serial — FPVRaceOne / emulator")
        win.geometry("980x760")
        self._log_win = win
        self._log_text = {}
        self._log_follow = {}

        for name, title in (("DUT", "FPVRaceOne (device under test)"),
                            ("EMU", "RX5808 emulator")):
            frame = ttk.LabelFrame(win, text=title, padding=4)
            frame.pack(fill="both", expand=True, padx=6, pady=(6, 3))

            bar = ttk.Frame(frame)
            bar.pack(fill="x")
            follow = tk.BooleanVar(value=True)
            self._log_follow[name] = follow
            ttk.Checkbutton(bar, text="Follow", variable=follow).pack(side="left")
            ttk.Button(bar, text="Clear",
                       command=lambda n=name: self._log_clear(n)).pack(side="left", padx=4)
            ttk.Label(bar, text="(raw, unfiltered)", foreground="#666",
                      font=("", 8)).pack(side="left", padx=6)

            txt = scrolledtext.ScrolledText(frame, wrap="none", height=18,
                                            font=("Consolas", 9))
            txt.pack(fill="both", expand=True)
            txt.tag_config("bad", foreground="#b00020")
            txt.tag_config("warn", foreground="#b06f00")
            txt.tag_config("dim", foreground="#333333")
            self._log_text[name] = txt

        # Backfill whatever the monitor already captured, so opening the panel
        # mid-session shows history instead of starting blank.
        for name in ("DUT", "EMU"):
            for line in self._mon_lines.get(name, [])[-2000:]:
                self._log_write(name, line + "\n", None, backfill=True)

        win.protocol("WM_DELETE_WINDOW", self._close_log_panel)

    def _close_log_panel(self):
        # Buffers survive in _mon_lines, so closing loses nothing and Save Logs
        # still works.
        if self._log_win is not None:
            try:
                self._log_win.destroy()
            except Exception:
                pass
        self._log_win = None
        self._log_text = {}
        self._log_follow = {}

    def _log_clear(self, name):
        txt = self._log_text.get(name)
        if txt:
            txt.delete("1.0", "end")

    def _log_write(self, name, text, tag, backfill=False):
        txt = self._log_text.get(name)
        if txt is None:
            return                    # panel closed; _mon_lines still has it
        txt.insert("end", text, tag or "")
        # Bound the widget independently of the memory buffer — Tk slows badly
        # once a text widget holds tens of thousands of lines.
        if not backfill and int(txt.index("end-1c").split(".")[0]) > 5000:
            txt.delete("1.0", "1000.0")
        follow = self._log_follow.get(name)
        if follow is None or follow.get():
            txt.see("end")

    def emit(self, text, tag=None):
        self.q.put(("log", (text, tag)))

    def check_master(self):
        """Confirm the master is reachable and report which nodes are
        registered — POSTing to an unregistered nodeId is rejected with 404,
        which would produce a load run that silently does nothing."""
        ip = self.master_ip.get().strip()
        self.log(f"\nProbing master at {ip} ...\n", "dim")
        data = probe_master(ip)
        if not data:
            self.log(f"  No response. Is this PC associated with the master's "
                     f"AP, and is {ip} correct?\n", "bad")
            return
        nodes = data.get("nodes", []) if isinstance(data, dict) else []
        clients = [n for n in nodes if not n.get("isMaster")]
        online = [n for n in clients if n.get("online")]
        self.log(f"  Master reachable. {len(clients)} client(s) registered, "
                 f"{len(online)} online.\n", "good" if online else "warn")
        for n in clients:
            self.log(f"    node {n.get('nodeId')}  "
                     f"{'online ' if n.get('online') else 'OFFLINE'}  "
                     f"{n.get('pilotName','')}\n", "dim")
        if online:
            ids = ",".join(str(n.get("nodeId")) for n in online)
            self.load_nodes.set(ids)
            self.log(f"  Node IDs set to: {ids}\n", "dim")
        else:
            self.log("  No online clients — load POSTs would be rejected (404) "
                     "and the fanout would have no targets.\n", "bad")

    def _make_load(self):
        """Build a MultiNodeLoad from the UI, or None if disabled/invalid."""
        if not self.load_on.get():
            return None
        try:
            ids = [int(x) for x in self.load_nodes.get().split(",") if x.strip()]
            rate = float(self.load_rate.get())
        except ValueError:
            self.emit("  Load settings invalid — node IDs must be numbers and "
                      "laps/sec a number. Running WITHOUT load.\n", "bad")
            return None
        if not ids:
            self.emit("  No node IDs given — running WITHOUT load.\n", "warn")
            return None
        srcs = [s.strip() for s in self.load_srcs.get().split(",") if s.strip()]
        try:
            interval = float(self.burst_interval.get())
            spread   = int(float(self.burst_spread.get()))
        except ValueError:
            self.emit("  Burst settings invalid — using 3.0 s lap, 50 ms pack.\n",
                      "warn")
            interval, spread = 3.0, 50
        return MultiNodeLoad(self.master_ip.get().strip(), ids, rate,
                             source_ips=srcs or None,
                             mode=self.load_mode.get().strip() or "burst",
                             burst_interval_s=interval,
                             burst_spread_ms=spread)

    def _attribute_peak_stall(self, dut, peak_ms):
        """Say WHICH broadcast caused the largest Core-0 stall, not guess.

        `CORE0_TIME("multinode", ...)` wraps the whole of process(), so a stall
        may be the recurring director fanout, a one-shot race start/stop
        broadcast, or recruitment.  The ratio to the median hints at which, but
        the firmware timestamps every line, so the answer is available exactly:
        the stall line is printed AFTER the block, so the block spans
        [t - duration, t], and any broadcast logged in that window was inside it.
        """
        stall_t = None
        for l in dut.log:
            m = re.search(r"\[(\d+)\]\s+\[CORE0\] multinode blocked for (\d+) ms", l)
            if m and int(m.group(2)) == peak_ms:
                stall_t = int(m.group(1))
                break
        if stall_t is None:
            return
        lo, hi = stall_t - peak_ms, stall_t

        kinds = {}
        for l in dut.log:
            m = re.search(r"\[(\d+)\]\s+\[(?:MULTINODE|RECRUIT)\]\s+(.*)", l)
            if not m:
                continue
            t, text = int(m.group(1)), m.group(2)
            if not (lo <= t <= hi):
                continue
            low = text.lower()
            if   "race start"   in low: kinds["race START broadcast"]   = kinds.get("race START broadcast", 0) + 1
            elif "race stop"    in low: kinds["race STOP broadcast"]    = kinds.get("race STOP broadcast", 0) + 1
            elif "pre-arm"      in low: kinds["race PRE-ARM broadcast"] = kinds.get("race PRE-ARM broadcast", 0) + 1
            elif "[RECRUIT]" in l or "recruit" in low:
                kinds["RECRUIT job"] = kinds.get("RECRUIT job", 0) + 1

        if kinds:
            what = ", ".join(f"{n}x {k}" for k, n in
                             sorted(kinds.items(), key=lambda kv: -kv[1]))
            self.emit(f"      ATTRIBUTED: the {peak_ms} ms stall spans "
                      f"[{lo}..{hi}] ms, which contains {what}.\n"
                      f"      That is a ONE-SHOT event, not the recurring "
                      f"director fanout — exclude it when judging fanout "
                      f"headroom.\n", "good")
        else:
            self.emit(f"      NOT ATTRIBUTED: no broadcast lines fall inside "
                      f"[{lo}..{hi}] ms. Either the trace was evicted or this "
                      f"stall really is\n      the director fanout — worth a "
                      f"second run before dismissing it.\n", "warn")

    def _emit_burst_stats(self, load):
        """How well the master absorbed simultaneous pack arrivals.

        The POST tallies alone hide this: every POST can succeed while each one
        takes far longer than it should, which is what a saturated master
        actually looks like from outside.
        """
        if getattr(load, "mode", "") != "burst" or not getattr(load, "bursts", 0):
            return
        worst = load.worst_burst_ms
        budget = load.burst_interval_s * 1000.0
        pct = 100.0 * worst / budget if budget else 0.0
        tag = "bad" if load.bursts_overrun else ("warn" if pct >= 50 else "good")
        self.emit(f"  BURST ABSORPTION: {load.bursts} pack(s), worst took "
                  f"{worst:.0f} ms of the {budget:.0f} ms lap ({pct:.0f}%).\n", tag)
        if load.bursts_overrun:
            self.emit(f"    {load.bursts_overrun} pack(s) took LONGER than one "
                      f"lap to clear — the master could not absorb a "
                      f"simultaneous crossing\n    within the time before the "
                      f"next one. This is the multi-node limit, not a timing "
                      f"fault.\n", "bad")

    def _load_desc(self):
        """One-line description of the load actually being applied."""
        if self.load_mode.get() == "burst":
            n = len([x for x in self.load_nodes.get().split(",") if x.strip()])
            return (f"BURST: all {n} node(s) together within "
                    f"{self.burst_spread.get()} ms, every "
                    f"{self.burst_interval.get()} s")
        return f"SPREAD: {self.load_rate.get()} laps/sec, sequential"

    def _update_sweep_estimate(self):
        """Show how long the search will take, since it varies with settings."""
        try:
            start = int(self.vars["sweep_start"].get())
            res = int(self.vars["sweep_res"].get())
            cnt = int(self.vars["sweep_count"].get())
            itv = int(self.vars["interval"].get())
        except (ValueError, KeyError):
            return
        runs = estimate_search_runs(start, 2, res)
        secs = runs * cnt * itv / 1000.0
        self.sweep_est.config(
            text=f"~{runs} runs, about {secs/60.0:.1f} min. "
                 f"Tick 'Force Min Lap = 0' and drop the interval to shorten it.")

    def _resolve_envelope(self, emu, dut, cfg):
        """Turn the baseline/peak settings into DAC values for THIS mode.

        With 'RSSI targets' on, the fields are the envelope the DEVICE should
        see, and the DAC values needed to produce it are back-solved from a
        freshly measured transfer. That keeps the two acquisition modes
        comparable despite their ~1.9x gain difference, and avoids clipping.

        Returns (baseline_dac, peak_dac) or (None, None) on failure.
        """
        if not self.target_rssi.get():
            return cfg["baseline"], cfg["peak"]

        points = measure_transfer(emu, dut, levels=(0, 64, 128, 192, 255),
                                  dwell_s=0.6)
        usable = [(d, v) for d, v in points if v is not None]
        if len(usable) < 3:
            self.emit("  Could not measure the transfer to resolve the RSSI "
                      "targets — run Check Analog Path first.\n", "bad")
            return None, None

        sl, ic, _ = _fit_line(usable)
        b, p, achievable, clipped = plan_envelope(sl, ic, cfg["baseline"], cfg["peak"])
        if b is None:
            self.emit("  Transfer looks degenerate — cannot resolve RSSI targets.\n",
                      "bad")
            return None, None

        self.emit(f"  Envelope: RSSI {cfg['baseline']} to {cfg['peak']} "
                  f"-> DAC {b} to {p}  (transfer {sl:.3f} x dac + {ic:.1f})\n", "dim")
        if clipped:
            self.emit(f"  WARNING: peak RSSI {cfg['peak']} is not reachable — DAC 255 "
                      f"only gets to {achievable:.0f}. Lower the target, or increase "
                      f"the divider ratio.\n", "bad")
        return b, p

    def _report_emulator_health(self, emu, res=None):
        """Say whether the RIG misbehaved, using its own serial output.

        The emulator shares one port between JSON and panic/boot output, so its
        crashes are visible — they were simply not being captured.  A rig that
        resets or hangs mid-run produces exactly the same harness result as a
        device that detected nothing, and that ambiguity has cost several runs.
        """
        log = getattr(emu, "log", None)
        if not log:
            return

        resets  = [l for l in log if ("rst:" in l and "boot:" in l) or "ESP-ROM:" in l]
        panics  = [l for l in log if ("Guru Meditation" in l or "panic" in l.lower()
                                      or "abort()" in l or "Backtrace:" in l
                                      or "assert failed" in l)]
        wdt     = [l for l in log if "watchdog" in l.lower() or "WDT" in l]

        # One reset is EXPECTED: opening the port asserts DTR/RTS, which
        # auto-resets a bridge-chip board.  Only speak up for a genuine fault,
        # or this fires on every healthy run and becomes noise to skip past.
        if not (panics or wdt or len(resets) > 2):
            return

        self.emit("\n  RIG (emulator) DIAGNOSTICS\n", "head")
        if panics:
            self.emit("    EMULATOR CRASHED — its own output says so:\n", "bad")
            for l in panics[:6]:
                self.emit(f"      {l.strip()}\n", "bad")
        if wdt:
            self.emit(f"    Watchdog mentioned {len(wdt)}x — consistent with an "
                      f"ISR or loop that stopped yielding:\n", "bad")
            for l in wdt[:3]:
                self.emit(f"      {l.strip()}\n", "dim")
        if resets:
            # One reset is normal: opening the port asserts DTR/RTS, which
            # auto-resets a bridge-chip board.  More than one means it restarted
            # while the test was running.
            extra = len(resets) - 1
            if extra > 0:
                self.emit(f"    EMULATOR RESET {extra} time(s) DURING the run "
                          f"(one reset at port-open is normal).\n", "bad")
            else:
                self.emit("    One reset seen — that is the expected port-open "
                          "auto-reset, not a fault.\n", "dim")
        if res is not None and getattr(res, "generated", -1) >= 0:
            self.emit(f"    Emulator reported generating {res.generated} of "
                      f"{res.expected} passes.\n",
                      "bad" if res.generated < res.expected else "dim")

    def _check_for_reboot(self, dut):
        """Detect a mid-run restart and say so LOUDLY.

        The firmware timestamps every DEBUG line with millis().  A line whose
        timestamp is far BELOW one already seen means the device restarted and
        its clock went back to zero.  Without this the run is reported as a
        detection failure ("check the enter/exit thresholds"), which sends the
        operator hunting a timing problem that does not exist — the device
        simply was not running for part of the test.

        Returns True if a restart was detected.
        """
        last, reboot_at = None, None
        for l in dut.log:
            m = re.match(r"\s*\[(\d+)\]", l)
            if not m:
                continue
            t = int(m.group(1))
            if last is not None and t < last - 10000:   # clock went backwards
                reboot_at = (last, t)
                break
            last = t
        if not reboot_at:
            return False

        before, after = reboot_at
        self.emit(f"\n  *** DEVICE REBOOTED MID-RUN ***\n", "bad")
        self.emit(f"    Log timestamps jump from {before} ms back to {after} ms. "
                  f"The device restarted\n    during this test, so any missed "
                  f"passes are explained by it not running — NOT by\n    "
                  f"thresholds, sampling or detection. Treat every number below "
                  f"as void.\n", "bad")

        # The ROM prints the reset cause before the app starts; it is the single
        # most useful line for telling a crash from a brownout from a watchdog.
        for l in dut.log:
            if "rst:" in l and "boot:" in l:
                self.emit(f"    Reset cause: {l.strip()}\n", "bad")
                break
        else:
            self.emit(f"    No 'rst:0x' line captured — reconnect the port and "
                      f"reboot to see the reset cause.\n", "warn")

        # Dump the RAW lines spanning the restart, unfiltered.  A panic message,
        # "Guru Meditation", an assert or a backtrace appears here and nowhere
        # else — the normal device-log view filters for PEAK/LAP/CORE0/etc and
        # would drop precisely the lines that name the fault.
        # The firmware reboots ITSELF when free heap or the largest contiguous
        # block stays low for 10 s (webserver.cpp HEAP_REBOOT_AFTER).  That is a
        # deliberate restart, not a crash — and it is indistinguishable from a
        # crash by reset code alone, since both report SW_CPU.  The [HEAP] trail
        # is what separates them, so report it whenever a restart is seen.
        heaps = [l for l in dut.log if "[HEAP]" in l]
        fatal = [l for l in heaps if "FATAL" in l]
        warns = [l for l in heaps if "WARN" in l]
        if fatal:
            self.emit(f"    CAUSE FOUND — the firmware rebooted ITSELF on low "
                      f"heap, this is not a crash:\n      {fatal[-1].strip()}\n",
                      "bad")
            # WHICH threshold fired matters more than the fact one did.  Free
            # heap low means too much is allocated; maxBlk low with free heap
            # healthy means FRAGMENTATION — the memory exists but not in one
            # piece.  They need completely different fixes, and the message
            # above does not distinguish them.
            fm = re.search(r"free=(\d+) maxBlk=(\d+)", fatal[-1])
            if fm:
                f_free, f_blk = int(fm.group(1)), int(fm.group(2))
                if f_blk < 8000 and f_free >= 20000:
                    self.emit(f"      FRAGMENTATION, not exhaustion: free heap "
                              f"was {f_free} (healthy, floor is 20000) but the "
                              f"largest\n      contiguous block was only "
                              f"{f_blk} (floor 8000). The memory was there — "
                              f"just not in one piece.\n      Fix repeated "
                              f"large alloc/free churn, not total usage.\n",
                              "bad")
                elif f_free < 20000:
                    self.emit(f"      EXHAUSTION: free heap {f_free} fell below "
                              f"the 20000 floor. Something is holding too much, "
                              f"not merely\n      fragmenting it.\n", "bad")
        elif warns:
            self.emit(f"    Heap warnings seen before the restart "
                      f"({len(warns)}x). Low heap is the likely cause:\n"
                      f"      {warns[-1].strip()}\n", "bad")
        if heaps:
            # Keep only the LAST boot's samples.  A log spanning restarts holds
            # several epochs, and simply taking the tail interleaves them —
            # producing a "trail" whose timestamps go backwards and whose trend
            # is meaningless.  Walk back from the end until the clock stops
            # increasing; that is the boot the reboot actually happened in.
            epoch, prev = [], None
            for l in reversed(heaps):
                m = re.match(r"\s*\[(\d+)\]", l)
                t = int(m.group(1)) if m else None
                if prev is not None and t is not None and t > prev:
                    break                      # crossed into an earlier boot
                epoch.append(l)
                prev = t
            epoch.reverse()
            self.emit(f"    Heap trail — LAST BOOT ONLY ({len(epoch)} sample(s); "
                      f"reboot triggers below 20000 free OR 8000 maxBlk):\n", "dim")
            for l in epoch[-6:]:
                self.emit(f"      {l.strip()}\n", "dim")

        idx = None
        for i, l in enumerate(dut.log):
            m = re.match(r"\s*\[(\d+)\]", l)
            if m and int(m.group(1)) == after:
                idx = i
                break
        if idx is not None:
            # Wide window: the [HEAP] FATAL line that names the cause can sit a
            # dozen lines above the AP-teardown burst, and a 14-line window
            # missed it entirely on the first capture.
            lo_i = max(0, idx - 40)
            self.emit("    Raw trace across the restart (unfiltered — a panic, "
                      "backtrace or [HEAP] FATAL would be here):\n", "bad")
            for l in dut.log[lo_i:idx + 3]:
                self.emit(f"      {l}\n", "dim")
        return True

    def _emit_load_evidence(self, dut, loaded, missed=None, detected=None,
                            run_s=None):
        """Report Core-0 stalls and measured sample rate for the run.

        Two independent facts, and it matters that they are reported together:
        parallelTask stalling is NOT the same as the sampler stalling.  The
        fanout blocks on socket I/O, and a task blocked on a socket yields, so
        loop() keeps sampling straight through a 1 s multinode stall.  Reading
        a CORE0 stall as a sampling failure is the mistake this block exists to
        prevent.
        """
        blocked = [l for l in dut.log if "CORE0" in l and "blocked" in l]
        timing = [l for l in dut.log if "TIMING" in l and "Hz" in l]
        drops  = [l for l in dut.log if "timed out" in l]
        worst_gap_ms = 0.0
        late_total = 0
        window_s = 10.0   # TIMING_STATS_WINDOW_MS
        if not (blocked or timing):
            return

        self.emit("\n  Load evidence\n", "head")

        if blocked:
            worst = 0
            for l in blocked:
                m = re.search(r"blocked for (\d+) ms", l)
                if m:
                    worst = max(worst, int(m.group(1)))
            self.emit(f"    {len(blocked)} Core-0 stall(s), worst {worst} ms — "
                      f"parallelTask blocked by the multi-node fanout.\n", "warn")

            # Fanout DUTY CYCLE.  The director broadcast is gated by
            # MIN_DIRECTOR_BROADCAST_INTERVAL_MS, so a fanout that takes longer
            # than that interval runs effectively back-to-back and never lets
            # parallelTask do anything else.  Duty is the headroom figure that
            # actually matters, and it is measurable straight from the stalls.
            times = [int(m.group(1)) for m in
                     (re.search(r"blocked for (\d+) ms", l) for l in blocked) if m]
            if times:
                # Judge on the MEDIAN, not the peak.  CORE0_TIME wraps the whole
                # of multiNodeManager::process(), so these stalls include
                # one-shot race start/stop broadcasts (7 clients x 500 ms) as
                # well as the repeating director fanout.  A single race-stop
                # outlier would otherwise dominate the verdict and hide the fact
                # that the recurring cost is fine.
                times_sorted = sorted(times)
                med  = times_sorted[len(times_sorted) // 2]
                peak = times_sorted[-1]
                md_duty = 100.0 * med / FANOUT_THROTTLE_MS
                pk_duty = 100.0 * peak / FANOUT_THROTTLE_MS
                tag = ("bad" if md_duty >= 100 else
                       "warn" if md_duty >= 75 else "good")
                self.emit(f"    MULTINODE DUTY: median {md_duty:.0f}% "
                          f"({med} ms), peak {pk_duty:.0f}% ({peak} ms) against "
                          f"a {FANOUT_THROTTLE_MS:.0f} ms throttle.\n", tag)
                if md_duty >= 100:
                    self.emit(f"      AT/OVER CEILING: the recurring fanout "
                              f"takes longer than the interval that gates it, "
                              f"so it runs back-to-back.\n", "bad")
                elif md_duty >= 75:
                    self.emit(f"      Headroom is thin. Every client that stops "
                              f"answering costs the full "
                              f"{FANOUT_CLIENT_TIMEOUT_MS} ms timeout instead of "
                              f"its normal reply time.\n", "warn")
                # A peak far above the median is the signature of a one-shot
                # broadcast rather than a fanout that is struggling.  Rather
                # than infer it from the ratio, ATTRIBUTE it: both the stall
                # line and the broadcast lines carry the firmware's millisecond
                # timestamp, so the broadcasts that happened INSIDE the stall
                # window can be identified exactly.
                if peak > 3 * max(med, 1):
                    self._attribute_peak_stall(dut, peak)
        elif loaded:
            self.emit("    No Core-0 stalls logged. The load was NOT stressing "
                      "the master — treat a clean result here as untested, not "
                      "as proof of robustness.\n", "warn")

        for l in timing[-1:]:
            self.emit(f"    {l.strip()}\n", "dim")
            m = re.search(r"late\(>10ms\)=(\d+) of (\d+)", l)
            mx = re.search(r"min/mean/max = \d+/\d+/(\d+) us", l)
            if m and mx:
                late, total = int(m.group(1)), int(m.group(2))
                worst_ms = int(mx.group(1)) / 1000.0
                worst_gap_ms = max(worst_gap_ms, worst_ms)
                late_total = max(late_total, late)
                if late == 0 and blocked:
                    self.emit(f"    SAMPLING HEALTHY: worst gap {worst_ms:.1f} ms, "
                              f"0 late samples of {total}. The Core-0 stalls above "
                              f"did NOT starve the sampler — the fanout blocks on "
                              f"socket I/O, which yields to loop().\n", "good")
                elif late == 0:
                    self.emit(f"    SAMPLING HEALTHY: worst gap {worst_ms:.1f} ms, "
                              f"0 late samples of {total}.\n", "good")
                else:
                    # Judge against what actually happened, not the raw count.
                    # A handful of late samples with every pass still detected
                    # is a note; calling it a threat cries wolf and buries the
                    # case where it genuinely matters.
                    pct = 100.0 * late / max(1, total)
                    if missed:
                        self.emit(f"    SAMPLING DEGRADED: {late} of {total} "
                                  f"({pct:.2f}%) samples arrived >10 ms late, "
                                  f"worst gap {worst_ms:.1f} ms — and {missed} "
                                  f"pass(es) went undetected. These are very "
                                  f"likely the same event.\n", "bad")
                    else:
                        self.emit(f"    SAMPLING GAPS: {late} of {total} "
                                  f"({pct:.2f}%) samples arrived >10 ms late, "
                                  f"worst gap {worst_ms:.1f} ms. Every pass was "
                                  f"still detected, so this cost nothing at this "
                                  f"pass width — but a gap of {worst_ms:.0f} ms "
                                  f"drops ~{worst_ms / 7.0:.0f} samples from a "
                                  f"7-sample median window, which would matter "
                                  f"on a much narrower pass.\n", "warn")

        # Name the sub-call responsible.  The firmware already attributes each
        # window's worst blocker; without surfacing it the operator is left
        # guessing which subsystem caused a gap.
        worst_call, worst_call_ms = None, 0
        for l in dut.log:
            m = re.search(r"longest sub-call (\w+)=(\d+) ms", l)
            if m and int(m.group(2)) > worst_call_ms:
                worst_call, worst_call_ms = m.group(1), int(m.group(2))
        if worst_call:
            note = ""
            if worst_call == "eeprom":
                note = ("  This is a config WRITE — if the harness muted "
                        "announcements\n      for you, that write is the "
                        "harness's own doing.")
            self.emit(f"    Worst blocker attributed by firmware: "
                      f"{worst_call}={worst_call_ms} ms.{note}\n", "dim")

            # If the sample gap is far larger than anything parallelTask was
            # doing, the cause is NOT parallelTask.  CORE0_TIME only wraps that
            # task's sub-calls, while sampling runs in loop() — so an
            # unexplained gap means something at HIGHER priority than loop()
            # preempted it, and the current instrumentation cannot see it.
            # Only worth raising when samples were ACTUALLY late.  A worst gap
            # of ~6 ms with zero late samples is normal scheduling and firing
            # "UNATTRIBUTED" on it contradicts the SAMPLING HEALTHY verdict
            # printed two lines above.
            if late_total and worst_gap_ms > worst_call_ms + 3:
                self.emit(
                    f"    UNATTRIBUTED: the {worst_gap_ms:.0f} ms sample gap is "
                    f"much larger than the {worst_call_ms} ms worst sub-call, so "
                    f"parallelTask\n      did NOT cause it — CORE0_TIME only "
                    f"wraps that task, while sampling runs in loop().\n", "warn")

                # Measured 2026-08-07: late-sample count tracks the number of
                # DETECTIONS, not elapsed time.  A 45 s run detecting 30 passes
                # logged 6 late samples; an identical 45 s run detecting none
                # logged zero.  DEBUG() ends in a synchronous Serial.printf and
                # is called from handleLapTimerUpdate() — i.e. from loop(), the
                # very function that samples.
        # The DEBUG correlation is reported INDEPENDENTLY of the UNATTRIBUTED
        # check above.  Nesting it there meant it went silent as soon as a large
        # parallelTask sub-call (e.g. multinode=935 ms) became the biggest
        # attributed blocker — but parallelTask blocks on socket I/O and yields,
        # so it cannot be causing sample gaps at all.  Suppressing the true
        # explanation because a louder-but-irrelevant one appeared is exactly
        # the wrong behaviour.
        #
        # 'late' is counted over one TIMING window; 'detected' is over the whole
        # run.  Scale to the same basis before comparing, or the ratio means
        # nothing.
        per_window = None
        if detected and run_s and run_s > 0:
            per_window = detected * (window_s / run_s)
        if per_window and late_total and 0.5 <= late_total / per_window <= 2.0:
            self.emit(
                f"    SAMPLE GAPS EXPLAINED: {late_total} late sample(s) per "
                f"{window_s:.0f}s window against ~{per_window:.1f} "
                f"detection(s) — near 1:1.\n"
                f"      DEBUG() ends in a synchronous Serial.printf called from "
                f"loop() on every lap. HWCDC only BLOCKS when a USB host is\n"
                f"      attached (HWCDC.cpp:432 no-ops via flushTXBuffer when "
                f"disconnected), so this is THE HARNESS PERTURBING ITS OWN\n"
                f"      MEASUREMENT — it does not happen in the field and needs "
                f"no firmware fix.\n", "dim")

        # Heap telemetry on EVERY run, not only when a reboot already happened.
        # The firmware self-reboots below 20000 free / 8000 maxBlk, so watching
        # the trend across consecutive runs distinguishes a slow LEAK (declines
        # with uptime, never recovers) from BURST PRESSURE (dips during packs,
        # recovers between them).  Those need different fixes, and waiting for a
        # crash to find out wastes a run each time.
        heaps = []
        for l in dut.log:
            m = re.search(r"\[HEAP\] free=(\d+) min=(\d+) maxBlk=(\d+)"
                          r"(?: sta=(\d+))?(?: sse=(\d+))?(?: sseQ=(\d+))?", l)
            if m:
                heaps.append(tuple(int(g) if g else 0 for g in m.groups()))
        if heaps:
            free, mn, blk, sta, sse, sseq = heaps[-1]
            headroom = min(free / 20000.0, blk / 8000.0)
            tag = ("bad" if headroom < 1.0 else
                   "warn" if headroom < 2.0 else "good")
            self.emit(f"    HEAP: free={free} min={mn} maxBlk={blk} "
                      f"sta={sta} sse={sse} sseQ={sseq}  "
                      f"({headroom:.1f}x above the self-reboot floor)\n", tag)
            if len(heaps) >= 2:
                d_free = heaps[-1][0] - heaps[0][0]
                d_blk  = heaps[-1][2] - heaps[0][2]
                if d_free < -3000 or d_blk < -3000:
                    self.emit(f"      DECLINING within this run: free "
                              f"{heaps[0][0]} -> {free} ({d_free:+}), maxBlk "
                              f"{heaps[0][2]} -> {blk} ({d_blk:+}).\n"
                              f"      Note the value at the START of the next "
                              f"run: if it does not recover, it is a LEAK.\n",
                              "bad")
            if sse > 1:
                self.emit(f"      sse={sse} — more than one SSE client is "
                          f"registered. Zombie SSE connections leak heap; this "
                          f"is the known suspect.\n", "bad")

            # sseQ is the deciding evidence for whether a heap decline is the
            # SSE backlog or something else.  Each queued message retains a copy
            # of the multi-kilobyte director-state payload it was sent with.
            q_first = heaps[0][5] if len(heaps) >= 2 else sseq
            if sseq >= 3 or (sseq > q_first and sseq >= 2):
                self.emit(f"      SSE BACKLOG CONFIRMED: sseQ={sseq} "
                          f"(started {q_first}). The browser is not draining as "
                          f"fast as the master pushes,\n      and every queued "
                          f"message holds its own copy of the payload. THIS is "
                          f"the leak.\n", "bad")
            elif sseq <= 1 and len(heaps) >= 2 and heaps[-1][0] < heaps[0][0] - 3000:
                self.emit(f"      NOT the SSE backlog: sseQ={sseq} stayed low "
                          f"while heap fell. The leak is somewhere else — look "
                          f"at the\n      multi-node fanout payload copy or "
                          f"per-node lap storage.\n", "warn")

        if drops:
            self.emit(f"    {len(drops)} client timeout(s) logged — the fanout is "
                      f"losing clients even though timing held.\n", "bad")

    def _preflight(self, dut, interval_ms, count=None):
        """Check device settings that silently corrupt a run, before running.

        Two traps, both of which produce output that reads as a detection
        result rather than a misconfiguration:
          - Min Lap above the pass interval  -> exactly one lap reported
          - Max Laps below the pass count    -> race ends itself part-way

        Returns (ok, restore_minlap_value_or_None).
        """
        # Decide whether the run can proceed BEFORE changing anything on the
        # device.  Muting first would leave announcements off when the Min Lap
        # check aborts the run, since the caller returns early and never
        # reaches the restore.
        cfg = read_device_config(dut)
        restore = None

        if cfg is None:
            self.emit("  Could not read device config — continuing without the "
                      "Min Lap check.\n", "warn")
        elif self.zero_minlap.get():
            original = int(cfg.get("minLap", 50))
            if original != 0:
                if set_min_lap(dut, 0):
                    self.emit(f"  Min Lap temporarily set to 0 "
                              f"(was {original * 100} ms); will restore after.\n", "dim")
                    restore = original
                else:
                    self.emit("  Could not set Min Lap to 0 — falling back to "
                              "the interval check.\n", "warn")
                    warn = check_interval_vs_minlap(cfg, interval_ms)
                    if warn:
                        self.emit(f"  {warn}\n\n", "bad")
                        return False, None
        else:
            warn = check_interval_vs_minlap(cfg, interval_ms)
            if warn:
                self.emit(f"  {warn}\n\n", "bad")
                return False, None

        if count:
            warn = check_maxlaps_vs_count(cfg, count)
            if warn:
                self.emit(f"  {warn}\n\n", "bad")
                self._restore_minlap(dut, restore)
                return False, None

        # Committed to running — safe to mute now.
        if self.mute_voice.get():
            if set_voice_enabled(dut, False):
                self._voice_was_muted = True
                self.emit("  Announcements muted for this run.\n", "dim")

        self.emit("\n")
        return True, restore

    def _restore_minlap(self, dut, original):
        """Undo anything the preflight changed on the device."""
        if original is not None:
            if set_min_lap(dut, original):
                self.emit(f"\n  Min Lap restored to {original * 100} ms.\n", "dim")
            else:
                self.emit(f"\n  WARNING: could not restore Min Lap — set it back "
                          f"to {original * 100} ms manually in Settings.\n", "bad")

        if getattr(self, "_voice_was_muted", False):
            self._voice_was_muted = False
            if set_voice_enabled(dut, True):
                self.emit("  Announcements re-enabled.\n", "dim")
            else:
                self.emit("  WARNING: could not re-enable announcements — turn "
                          "Voice back on in Settings.\n", "bad")

    # -- test runners (worker thread) ---------------------------------------

    def run_level(self):
        cfg = self._ints()
        links = self._links()
        if not cfg or not links:
            return
        self._start(self._level_worker, links, cfg)

    def _level_worker(self, links, cfg):
        emu_p, dut_p = links
        emu = dut = None
        try:
            self.emit("\n" + "=" * 66 + "\n", "dim")
            self.emit("ANALOG PATH CHECK\n", "head")
            self.emit("Measures the DAC-to-RSSI transfer, then judges it on\n"
                      "LINEARITY (is the path clean?) and HEADROOM (can it drive\n"
                      "the signal far enough above the Enter threshold?).\n"
                      "Run this FIRST — low amplitude presents as a detection\n"
                      "failure that looks like a timing bug.\n\n", "dim")

            emu = self._open_emu(emu_p)
            if emu is None:
                return
            dut = JsonLink(dut_p, name="dut", on_line=self._mon_sink("DUT"))

            dcfg = read_device_config(dut)
            enter = dcfg.get("enterRssi") if dcfg else None
            amode = None
            if dcfg is not None:
                amode = int(dcfg.get("adcActive", dcfg.get("adcMode", 0)))
            if enter is not None:
                self.emit(f"  Device Enter threshold: {enter}"
                          f"{'   Mode: ' + ('DMA' if amode == 1 else 'polled') if amode is not None else ''}\n\n",
                          "dim")

            self.emit(f"  {'DAC':>5} {'measured':>10}\n")
            self.emit(f"  {'-'*5} {'-'*10}\n")

            def show(d, v):
                if v is None:
                    self.emit(f"  {d:>5} {'--':>10}\n", "warn")
                else:
                    self.emit(f"  {d:>5} {int(v):>10}\n")

            points = measure_transfer(emu, dut, progress=show)
            verdicts, _gain = diagnose_transfer(points, enter_rssi=enter,
                                                adc_mode=amode)

            self.emit("\n  What this means\n", "head")
            for tag, line in verdicts:
                self.emit(f"  {line}\n\n", tag)
        except Exception as e:
            self.emit(f"\nERROR: {e}\n", "bad")
        finally:
            if emu:
                emu.close()
            if dut:
                dut.close()
            self.q.put(("done", None))

    def run_thresholds(self):
        cfg = self._ints()
        links = self._links()
        if not cfg or not links:
            return
        self._start(self._thresholds_worker, links, cfg)

    def _thresholds_worker(self, links, cfg):
        """Compute Enter/Exit from the measured transfer and apply them.

        This replaces running the Calibration wizard for rig work. The wizard
        exists to infer thresholds from an unknown real signal by finding peaks
        in a hand-timed recording; here the signal is synthetic and the transfer
        has just been measured, so the thresholds can be derived exactly. That
        is repeatable run to run, and avoids having to coordinate a browser
        recording window against a pass run.
        """
        emu_p, dut_p = links
        emu = dut = None
        try:
            self.emit("\n" + "=" * 66 + "\n", "dim")
            self.emit("SET THRESHOLDS FROM MEASURED TRANSFER\n", "head")
            self.emit("Re-measures the DAC-to-RSSI transfer, then computes Enter/Exit\n"
                      "for the current baseline/peak and writes them to the device.\n"
                      "Deterministic — no Calibration wizard needed for rig work.\n\n",
                      "dim")

            emu = self._open_emu(emu_p)
            if emu is None:
                return
            dut = JsonLink(dut_p, name="dut", on_line=self._mon_sink("DUT"))

            points = measure_transfer(emu, dut)
            usable = [(d, v) for d, v in points if v is not None]
            if len(usable) < 3:
                self.emit("  Could not measure the transfer — run Check Analog "
                          "Path first.\n", "bad")
                return

            slope, intercept, _ = _fit_line(usable)
            # Thresholds must be derived from the SAME envelope the run will
            # use, so resolve RSSI targets to DAC values first when that mode
            # is active.
            if self.target_rssi.get():
                b_dac, p_dac, _ach, _clip = plan_envelope(
                    slope, intercept, cfg["baseline"], cfg["peak"])
                if b_dac is None:
                    b_dac, p_dac = cfg["baseline"], cfg["peak"]
            else:
                b_dac, p_dac = cfg["baseline"], cfg["peak"]

            en, ex, base_r, peak_r = compute_thresholds(
                slope, intercept, b_dac, p_dac)

            # Sanity-gate before writing anything to the device.  A degenerate
            # transfer (flat, or barely sloping) yields Enter == Exit, which
            # would destroy a working calibration and produce a device that
            # detects nothing.  Refuse rather than apply.
            swing = peak_r - base_r
            if slope < 0.1 or swing < 40 or (en - ex) < 5:
                self.emit(f"  Transfer      : rssi = {slope:.3f} x dac + "
                          f"{intercept:.1f}\n")
                self.emit(f"  Envelope      : RSSI {base_r:.0f} to {peak_r:.0f} "
                          f"(swing {swing:.0f})\n")
                self.emit(f"  Computed      : Enter {en}, Exit {ex}\n\n")
                self.emit("  REFUSED TO APPLY — this transfer is degenerate.\n", "bad")
                self.emit("  A healthy sweep on this hardware slopes about 0.89 "
                          "with a swing near 140. A flat or shallow one means the "
                          "RSSI stream was not tracking the DAC, which happens "
                          "when the device is too busy to service USB promptly.\n\n",
                          "dim")
                self.emit("  Set thresholds with the device IDLE (no multi-node "
                          "load, clients disconnected if need be), then re-enable "
                          "load for the timing runs. Thresholds are a property of "
                          "the analog path, not of the load, so measuring them "
                          "unloaded is correct.\n\n", "dim")
                return

            self.emit(f"  Transfer      : rssi = {slope:.3f} x dac + {intercept:.1f}\n")
            self.emit(f"  Envelope      : baseline DAC {cfg['baseline']} -> RSSI "
                      f"{base_r:.0f}, peak DAC {cfg['peak']} -> RSSI {peak_r:.0f}\n")
            self.emit(f"  Computed      : Enter {en}, Exit {ex}\n\n")

            if set_thresholds(dut, en, ex):
                self.emit(f"  Applied — Enter {en}, Exit {ex} written to the device.\n",
                          "good")
            else:
                self.emit("  FAILED to apply — set them manually in Settings.\n", "bad")

            self.emit("\n  Why these values\n", "head")
            self.emit("  - Enter sits at 60% of the baseline-to-peak swing, Exit at "
                      "40%, giving hysteresis so the gate cleanly enters and exits.\n\n")
            self.emit("  - Enter is deliberately NOT set low. A low threshold makes "
                      "every pass width detect, which would make the sweep test "
                      "meaningless — the point of that test is to find where "
                      "detection actually breaks down.\n\n")
            self.emit("  - Exit stays above the baseline RSSI so the signal reliably "
                      "drops out of the gate between passes.\n\n")
        except Exception as e:
            self.emit(f"\nERROR: {e}\n", "bad")
        finally:
            if emu:
                emu.close()
            if dut:
                dut.close()
            self.q.put(("done", None))

    def run_interval(self):
        cfg = self._ints()
        links = self._links()
        if not cfg or not links:
            return
        self._start(self._interval_worker, links, cfg)

    def _interval_worker(self, links, cfg):
        emu_p, dut_p = links
        emu = dut = None
        restore = None          # bound before the try so finally can always use it
        try:
            self.emit("\n" + "=" * 66 + "\n", "dim")
            self.emit("ACCURACY + CONSISTENCY\n", "head")
            self.emit(f"{cfg['count']} passes, {cfg['interval']} ms apart, "
                      f"{cfg['width']} ms wide.\n"
                      f"Estimated run time: "
                      f"{cfg['count'] * cfg['interval'] / 1000.0:.0f} s\n\n", "dim")

            emu = self._open_emu(emu_p)
            if emu is None:
                return
            # Capture the device's DEBUG output for this run.  When detection
            # under-performs, the JSON stream shows only an absence of laps —
            # the firmware's own trace is what says WHY.
            dut = JsonLink(dut_p, name="dut", keep_log=True,
                             on_line=self._mon_sink("DUT"))

            ok, restore = self._preflight(dut, cfg["interval"], cfg.get("count"))
            if not ok:
                return

            # Record the conditions this run happened under.  Comparing two
            # runs is only meaningful if adcMode is the ONLY thing that
            # differed — a config reset or an un-re-run Set Thresholds will
            # otherwise silently confound the comparison.
            rcfg = read_device_config(dut)
            if rcfg:
                req = int(rcfg.get("adcMode", 0))
                act = int(rcfg.get("adcActive", req))
                mode = "DMA" if act == 1 else "polled"
                self.emit(f"  Conditions: adcMode={mode}, "
                          f"Enter={rcfg.get('enterRssi','?')}, "
                          f"Exit={rcfg.get('exitRssi','?')}, "
                          f"MinLap={int(rcfg.get('minLap', 0)) * 100}ms, "
                          f"baseline={cfg['baseline']}, peak={cfg['peak']}\n",
                          "dim")
                if req != act:
                    self.emit("  WARNING: DMA was requested but the device is "
                              "running POLLED — adc_continuous init failed and "
                              "fell back. Any A/B against this run is invalid.\n",
                              "bad")
                self.emit("  Compare runs ONLY when every field above matches "
                          "except adcMode.\n\n", "dim")

            base_dac, peak_dac = self._resolve_envelope(emu, dut, cfg)
            if base_dac is None:
                return

            # Load starts only once run_once() has CONFIRMED the race is armed.
            # Starting it earlier delays the 'timer/start' ack itself, because
            # USB commands and the multi-node fanout share parallelTask — the
            # run then samples happily while detecting nothing.
            load = self._make_load()

            def _arm_load():
                if load:
                    load.start()
                    self.emit(f"  Load ACTIVE — {self._load_desc()}\n"
                              f"  to node(s) {self.load_nodes.get()} on "
                              f"{self.master_ip.get()}\n\n", "warn")

            try:
                try:
                    stop_gap = float(self.stop_delay.get())
                except ValueError:
                    stop_gap = 0.0
                if stop_gap > 0:
                    self.emit(
                        f"  Quiet gap of {stop_gap:.0f}s between the last lap and "
                        f"race stop, to separate\n  the stop broadcast from "
                        f"trailing lap traffic.\n", "dim")
                res = run_once(emu, dut, cfg["width"], cfg["interval"],
                               cfg["count"], peak_dac, base_dac,
                               on_armed=_arm_load, stop_delay_s=stop_gap)
            finally:
                if load and load.started:
                    load.stop()
                    self.emit(f"\n  Load stopped: {load.sent} POST(s) accepted, "
                              f"{load.failed} failed"
                              f"{' — ' + load.last_error if load.last_error else ''}\n",
                              "bad" if load.failed > load.sent * 0.1 else "dim")
                    self._emit_burst_stats(load)
                    if load.sent == 0:
                        self.emit("  NO LOAD WAS APPLIED — results are "
                                  "equivalent to an unloaded run. Use 'Check "
                                  "master' to confirm reachability and node "
                                  "IDs.\n", "bad")

            st = summarize(res)

            self.emit(f"  Passes generated : {res.expected}\n")
            self.emit(f"  Laps reported    : {res.detected}\n")
            if st:
                self.emit(f"\n  Lap-time error (reported - {cfg['interval']} ms), "
                          f"n={st['n']}\n")
                self.emit(f"    bias   : {st['bias_ms']:+.2f} ms\n")
                self.emit(f"    stdev  : {st['stdev_ms']:.2f} ms\n")
                self.emit(f"    range  : {st['min_ms']:+d} .. {st['max_ms']:+d} ms\n")
                if "lat_mean_us" in st:
                    self.emit(f"\n  Detection latency, n={len(res.latencies)}\n")
                    self.emit(f"    mean   : {st['lat_mean_us']/1000.0:.2f} ms\n")
                    self.emit(f"    stdev  : {st['lat_stdev_us']/1000.0:.2f} ms\n")
                    self.emit(f"    range  : {st['lat_min_us']/1000.0:.2f} .. "
                              f"{st['lat_max_us']/1000.0:.2f} ms\n")

            # Work out the geometric floor so the latency verdict can separate
            # "the pass had to finish" from "the pipeline added lag".
            geo = None
            try:
                dcfg = read_device_config(dut)
                if dcfg and "exitRssi" in dcfg:
                    pts = measure_transfer(emu, dut, levels=(0, 128, 255),
                                           dwell_s=0.6)
                    up = [(d, v) for d, v in pts if v is not None]
                    if len(up) >= 2:
                        sl, ic, _ = _fit_line(up)
                        geo = geometric_detect_ms(
                            cfg["width"],
                            sl * cfg["baseline"] + ic,
                            sl * cfg["peak"] + ic,
                            float(dcfg["exitRssi"]))
            except Exception:
                geo = None

            # Dump the device trace when a meaningful share of passes went
            # undetected — the interesting lines are PEAK CAPTURED (enter
            # threshold reached) and LAP DETECTED (exit reached).  Seeing
            # neither means the median never reached Enter; seeing PEAK
            # without LAP means it never dropped below Exit.
            if res.detected < res.expected * 0.8 and dut.log:
                self.emit("\n  Device log (why detection failed)\n", "head")
                # [CORE0] lines are the important ones when detection fails
                # under load: they name the sub-call that blocked Core 0 and
                # for how long, which is exactly the mechanism that starves
                # loop() and stops RSSI sampling.  An earlier version of this
                # filter omitted them, which hid the very evidence needed.
                keep = [l for l in dut.log
                        if any(k in l for k in ("PEAK", "LAP", "Ceiling", "RSSI",
                                                "Lap ", "CORE0", "TIMING",
                                                "MULTINODE"))]
                for line in (keep or dut.log)[-40:]:
                    tag = "bad" if ("CORE0" in line and "blocked" in line) else "dim"
                    self.emit(f"    {line}\n", tag)
                if not keep:
                    self.emit("    (no trace — the firmware's DEBUG output may "
                              "be disabled)\n", "dim")

            # Load evidence, printed whether or not detection succeeded.  A
            # clean loaded run is only meaningful if the load was actually
            # biting; without this the operator cannot tell a robust result
            # from a run where nothing happened to be stressing the master.
            # A restart invalidates everything else, so check it FIRST.  When
            # one happened, the log holds TWO clock epochs and mixing them
            # produces nonsense — e.g. reporting the post-reboot boot-time
            # webUpdate stall as this run's worst sampling gap.
            self._report_emulator_health(emu, res)
            rebooted = self._check_for_reboot(dut)
            if not rebooted:
                self._emit_load_evidence(
                    dut, loaded=bool(load and load.started),
                    missed=res.expected - res.detected,
                    detected=res.detected,
                    run_s=cfg["count"] * cfg["interval"] / 1000.0)
            else:
                self.emit(
                    "    Load evidence SUPPRESSED — the log spans a restart, so\n"
                    "    its Core-0 and sampling figures would mix two different\n"
                    "    boots. Re-run once the device stays up.\n", "warn")

            self.emit("\n  What this means\n", "head")
            for line in interpret_interval(res, st, cfg["interval"], geo):
                tag = ("good" if line.startswith(("CONSISTENCY: GOOD",
                                                  "LATENCY GOOD"))
                       else "bad" if line.startswith(("MISSED", "CONSISTENCY: POOR",
                                                      "LATENCY HIGH"))
                       else "warn" if line.startswith(("CONSISTENCY: MARGINAL",
                                                       "LATENCY MARGINAL"))
                       else None)
                self.emit(f"  - {line}\n\n", tag)
        except Exception as e:
            self.emit(f"\nERROR: {e}\n", "bad")
        finally:
            # Restore device state HERE, not in the try body: an exception
            # mid-run would otherwise leave Min Lap at 0 and announcements
            # muted, for the operator to discover and undo by hand.
            try:
                if dut is not None:
                    self._restore_minlap(dut, restore)
            except Exception:
                pass
            if emu:
                emu.close()
            if dut:
                dut.close()
            self.q.put(("done", None))

    def run_sweep(self):
        cfg = self._ints()
        links = self._links()
        if not cfg or not links:
            return
        self._start(self._sweep_worker, links, cfg)

    def _sweep_worker(self, links, cfg):
        emu_p, dut_p = links
        emu = dut = None
        restore = None          # bound before the try so finally can always use it
        try:
            start = cfg["sweep_start"]
            resol = cfg["sweep_res"]
            cnt   = cfg["sweep_count"]
            req   = cfg["sweep_require"] / 100.0
            runs  = estimate_search_runs(start, 2, resol)

            self.emit("\n" + "=" * 66 + "\n", "dim")
            self.emit("MINIMUM DETECTABLE PASS WIDTH — automatic search\n", "head")
            self.emit(f"Halves the pass width until detection fails, then bisects\n"
                      f"to +/-{resol} ms. Starting at {start} ms, {cnt} passes per\n"
                      f"step, counting >={cfg['sweep_require']}% as detected.\n"
                      f"About {runs} runs, roughly "
                      f"{runs * cnt * cfg['interval'] / 60000.0:.1f} min.\n\n", "dim")

            emu = self._open_emu(emu_p)
            if emu is None:
                return
            dut = JsonLink(dut_p, name="dut", keep_log=True,
                             on_line=self._mon_sink("DUT"))

            ok, restore = self._preflight(dut, cfg["interval"], cfg.get("count"))
            if not ok:
                return

            base_dac, peak_dac = self._resolve_envelope(emu, dut, cfg)
            if base_dac is None:
                return

            self.emit(f"  {'width':>8} {'detected':>10} {'rate':>7}   verdict\n")
            self.emit(f"  {'-'*8} {'-'*10} {'-'*7}   {'-'*7}\n")

            def progress(width, detected, expected, passed):
                rate = (detected / expected * 100.0) if expected else 0.0
                self.emit(f"  {width:>6}ms {detected:>4}/{expected:<5} "
                          f"{rate:>6.0f}%   {'detects' if passed else 'FAILS'}\n",
                          "good" if passed else "warn")

            # The sweep calls run_once() many times; load arms on the first
            # confirmed race start and stays up for the rest (start() is
            # idempotent once the poster thread is alive).
            load = self._make_load()

            def _arm_load():
                if load and not load.started:
                    load.start()
                    self.emit(f"  Load ACTIVE — {self._load_desc()}\n"
                              f"  to node(s) {self.load_nodes.get()}\n\n", "warn")

            try:
                result = find_min_detectable_width(
                    emu, dut,
                    interval_ms=cfg["interval"], peak=peak_dac, baseline=base_dac,
                    start_ms=start, resolution_ms=resol, count=cnt, require=req,
                    progress=progress, cancelled=self.cancel.is_set,
                    on_armed=_arm_load)
            finally:
                if load and load.started:
                    load.stop()
                    self.emit(f"\n  Load stopped: {load.sent} accepted, "
                              f"{load.failed} failed\n",
                              "bad" if load.sent == 0 else "dim")
                    self._emit_burst_stats(load)

            self.emit("\n  What this means\n", "head")
            limit, reason = result["limit_ms"], result["reason"]

            if reason == "start_failed":
                self.emit(f"  - The starting width of {start} ms did not detect "
                          f"reliably, so there is nothing to search downward for. "
                          f"Fix that first: raise the start width, check Enter/Exit "
                          f"via Set Thresholds, or confirm the envelope reaches "
                          f"well above Enter.\n\n", "bad")
            elif reason == "cancelled":
                self.emit(f"  - Search stopped early. Best confirmed width so far: "
                          f"{limit} ms.\n\n", "warn")
            elif reason == "below_floor":
                self.emit(f"  - Detects reliably even at {limit} ms, the shortest "
                          f"width tested. The real limit is below that — lower the "
                          f"floor if you need to find it.\n\n", "good")
            else:
                self.emit(f"  - MINIMUM RELIABLE PASS: {limit} ms "
                          f"(+/-{resol} ms), at >={cfg['sweep_require']}% detection.\n\n",
                          "good")
                self.emit(f"  - Shorter passes than this start being missed. The "
                          f"narrower this number, the faster the gate transit the "
                          f"pipeline can catch.\n\n")

            self.emit(f"  - Record this alongside adcMode. Comparing the figure "
                      f"between polled and DMA is the real test of whether "
                      f"peak-hold buys capability rather than just precision — "
                      f"if they land in the same place, DMA is not earning its "
                      f"complexity.\n\n")
        except Exception as e:
            self.emit(f"\nERROR: {e}\n", "bad")
        finally:
            # Same reasoning as the interval worker — never leave the device
            # with Min Lap 0 or announcements muted after a failure.
            try:
                if dut is not None:
                    self._restore_minlap(dut, restore)
            except Exception:
                pass
            if emu:
                emu.close()
            if dut:
                dut.close()
            self.q.put(("done", None))


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")   # native-ish on Windows
    except tk.TclError:
        pass
    TimingRigGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
