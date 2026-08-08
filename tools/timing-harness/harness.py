#!/usr/bin/env python3
"""
FPVRaceOne timing harness — drives the RX5808 emulator and measures the timer.

WHY THIS EXISTS
---------------
Real-world stimulus cannot resolve millisecond questions: a servo swinging past
an antenna has millisecond variance of its own, so you characterise the servo.
This harness pairs with tools/rx5808-emulator/ to make the stimulus
deterministic, then answers two questions with numbers:

    Is the timer LATE?          -> bias, and absolute detection latency
    Is the timer INCONSISTENT?  -> standard deviation of lap-time error

THE KEY IDEA: NO CLOCK SYNC IS NEEDED
-------------------------------------
A lap time is a DIFFERENCE measured entirely by FPVRaceOne's own clock. The
emulator generates pulses exactly T apart; the device reports lap time L; the
error is L - T. No relationship between the two devices' clocks is involved,
so USB latency and jitter simply do not enter the measurement.

Absolute latency is handled the same way: the emulator timestamps both its own
stimulus and FPVRaceOne's marker edge on one hardware timer.

USAGE
-----
    python harness.py --list
    python harness.py --emu COM7 --dut COM5 interval --count 50 --interval 3000
    python harness.py --emu COM7 --dut COM5 sweep --widths 100,80,60,40,30,20,15,10
    python harness.py --emu COM7 --dut COM5 level          # run this FIRST

Requires: pyserial  (present in the PlatformIO venv; otherwise `pip install pyserial`)
"""

import argparse
import http.client
import json
import math
import random
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not found. Install with:  pip install pyserial")


# ── Serial plumbing ─────────────────────────────────────────────────────────

class JsonLink:
    """Line-oriented JSON link to one device.

    FPVRaceOne shares its USB CDC port between DEBUG() output and the JSON
    protocol, so non-JSON lines are routine and must be skipped rather than
    treated as errors. The emulator is JSON-only but is handled identically.
    """

    def __init__(self, port, baud=115200, name="dev", keep_log=False):
        self.name = name
        self.ser = serial.Serial(port, baud, timeout=0.05)
        # Give USB CDC a moment; some boards reset when the port opens.
        time.sleep(2.0)
        self.ser.reset_input_buffer()
        self._buf = ""
        # Non-JSON lines are the firmware's own DEBUG output, which carries
        # the detection trace ("PEAK CAPTURED", "LAP DETECTED", ceiling-drift
        # resets).  Normally noise to be skipped, but indispensable when
        # detection is failing and the JSON stream simply shows nothing.
        self.keep_log = keep_log
        self.log = []

    def send(self, obj):
        self.ser.write((json.dumps(obj) + "\n").encode())
        self.ser.flush()

    def poll(self):
        """Return a list of parsed JSON objects received since the last call."""
        out = []
        try:
            data = self.ser.read(4096).decode("utf-8", errors="replace")
        except Exception:
            return out
        if not data:
            return out
        self._buf += data
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            line = line.strip()
            if not line or not line.startswith("{"):
                if self.keep_log and line:
                    self.log.append(line)
                    # Generous cap: a 45 s run at ~1 kHz emits a lot of DEBUG,
                    # and attributing a Core-0 stall means correlating it
                    # against broadcast lines that may be 40 s older. At 400
                    # lines the race-START trace was being evicted before the
                    # run finished, which is exactly the evidence needed.
                    if len(self.log) > 4000:
                        del self.log[:1000]
                continue          # DEBUG output — expected, not an error
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue          # truncated or interleaved line
        return out

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


# ── Measurement ─────────────────────────────────────────────────────────────

@dataclass
class RunResult:
    width_ms: int
    interval_ms: int
    expected: int
    laps: list = field(default_factory=list)       # reported lap times (ms)
    latencies: list = field(default_factory=list)  # marker latencies (us)
    dropped: int = 0                               # marker edges the rig lost

    @property
    def detected(self):
        return len(self.laps)

    @property
    def errors(self):
        """Lap-time error in ms. The first lap is dropped deliberately.

        FPVRaceOne's first crossing is Gate 1, which starts the lap rather
        than completing one, so its reported time is not an inter-pulse
        interval and would poison the statistics.
        """
        return [l - self.interval_ms for l in self.laps[1:]]


def await_event(link, event, timeout_s=6.0, collect=None):
    """Wait for a named event from a device, optionally keeping others.

    Opening a serial port asserts DTR/RTS, which hardware-resets an ESP32 with
    the usual auto-reset circuit.  A fixed sleep afterwards is a guess: if the
    board is slow to boot, the commands that follow are swallowed by the
    bootloader and the run proceeds against a device that never armed.  That is
    silent — the emulator sits at baseline and every pass is 'missed'.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for m in link.poll():
            if m.get("event") == event:
                return m
            if collect is not None:
                collect.append(m)
        time.sleep(0.02)
    return None


def emulator_handshake(emu, timeout_s=8.0):
    """Confirm the emulator has booted and is answering before trusting it."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        emu.send({"cmd": "ping"})
        got = await_event(emu, "ready", timeout_s=1.0)
        if got:
            return got
    return None


def _await_ack(dut, want_id, timeout_s=8.0):
    """Wait for the device to acknowledge a specific command id.

    USBTransport::update() runs inside parallelTask, the same task that does
    the blocking multi-node fanout.  Under load that task can be stalled for
    over a second at a time, so a fixed sleep after 'timer/start' is not a
    guarantee the race actually started — and a run against a stopped timer
    samples happily while detecting nothing, which is indistinguishable in the
    output from a genuine detection failure.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for m in dut.poll():
            if m.get("id") == want_id and m.get("status") == "OK":
                return True
        time.sleep(0.02)
    return False


def run_once(emu, dut, width_ms, interval_ms, count, peak, baseline,
             settle_s=1.0, verbose=False, on_armed=None):
    """Run one profile and collect laps + latencies.

    on_armed, if given, is called once the race is CONFIRMED started.  The GUI
    uses it to start multi-node load only after arming, so heavy load cannot
    delay the very command that arms the run.
    """
    result = RunResult(width_ms=width_ms, interval_ms=interval_ms,
                       expected=count)

    emu.send({"cmd": "stop"})
    time.sleep(0.2)
    emu.poll()
    dut.poll()

    emu.send({"cmd": "profile", "baseline": baseline, "peak": peak,
              "widthMs": width_ms, "intervalMs": interval_ms, "count": count})
    ps = await_event(emu, "profileSet", timeout_s=3.0)
    if ps is None:
        raise RuntimeError(
            "Emulator did not acknowledge 'profile'.\n"
            "  Opening the serial port resets the board (DTR/RTS auto-reset), so\n"
            "  this usually means it had not finished booting. Re-run; if it\n"
            "  persists, check the emulator's USB cable and port selection."
        )
    # The emulator applies fields individually and ignores any it cannot parse,
    # so a silently-dropped field would run a DIFFERENT profile than requested
    # and quietly invalidate the numbers.
    for key, want in (("baseline", baseline), ("peak", peak),
                      ("widthMs", width_ms), ("intervalMs", interval_ms),
                      ("count", count)):
        if key in ps and int(ps[key]) != int(want):
            raise RuntimeError(
                f"Emulator rejected profile field '{key}': asked for {want}, "
                f"it reports {ps[key]}. The run would not match the request."
            )
    emu.poll()

    # Clear any stale lap data on the device, then start its race so laps
    # are timed rather than ignored.
    dut.send({"cmd": "timer/stop", "id": 1})
    time.sleep(0.2)
    dut.send({"cmd": "timer/start", "id": 2})
    if not _await_ack(dut, 2):
        raise RuntimeError(
            "Device never acknowledged 'timer/start'.\n"
            "  USB commands are handled by parallelTask, which is also where the\n"
            "  blocking multi-node fanout runs.  If the master is under load the\n"
            "  ack can be delayed past the timeout.  Start the run first and let\n"
            "  load begin afterwards (the GUI does this automatically)."
        )
    time.sleep(settle_s)
    dut.poll()

    # Race is confirmed armed — safe to apply load now.
    if on_armed:
        on_armed()

    emu.send({"cmd": "run"})
    if await_event(emu, "runStarted", timeout_s=3.0) is None:
        raise RuntimeError(
            "Emulator did not acknowledge 'run' — no passes will be generated.\n"
            "  The DAC stays at baseline and every pass reads as 'missed', which\n"
            "  looks identical to a device-side detection failure."
        )

    # Total run time plus generous margin for the final pass to be processed.
    budget = (count * interval_ms / 1000.0) + 5.0
    deadline = time.time() + budget
    done = False

    while time.time() < deadline:
        for msg in emu.poll():
            ev = msg.get("event")
            if ev == "pass" and "latencyUs" in msg:
                result.latencies.append(msg["latencyUs"])
                if verbose:
                    print(f"    pass {msg.get('n')}: latency {msg['latencyUs']} us")
            elif ev == "done":
                result.dropped = int(msg.get("dropped", 0))
                done = True
        for msg in dut.poll():
            if msg.get("event") == "lap":
                result.laps.append(int(msg["data"]))
                if verbose:
                    print(f"    lap: {msg['data']} ms")
        if done:
            # Let the last lap event arrive before closing out.
            time.sleep(1.0)
            for msg in dut.poll():
                if msg.get("event") == "lap":
                    result.laps.append(int(msg["data"]))
            break
        time.sleep(0.01)

    dut.send({"cmd": "timer/stop", "id": 3})
    emu.send({"cmd": "stop"})
    time.sleep(0.2)
    return result


# ── Analog path validation ──────────────────────────────────────────────────
#
# WHAT ACTUALLY MATTERS HERE
#
# An earlier version of this check asserted a theoretical gain derived from
# nominal DAC full-scale, divider ratio and ADC_6db range.  That model did not
# survive contact with hardware — a correctly built rig measured 0.40 against a
# predicted 1.006 — because the real DAC full-scale and the C6's effective ADC
# range are not the textbook values.  Asserting the theory produced a confident
# and wrong "your wiring is broken" verdict.
#
# So the criteria are now empirical:
#
#   LINEARITY  is the health check.  A clean resistive path is linear; loading,
#              clipping and bad connections all show up as curvature.  This is
#              what tells you the rig is sound.
#
#   HEADROOM   is the usability check.  Absolute gain is irrelevant on its own;
#              what matters is whether the DAC can drive RSSI far enough above
#              the device's Enter threshold to trigger detection with margin.
#
# The divider ratio itself is then just a scaling choice, reported so it can be
# tuned if headroom is thin.
NOMINAL_SERIES_K = 2.2
NOMINAL_SHUNT_K  = 1.0

# Kept only for display, never as a pass/fail criterion.
EXPECTED_GAIN = 1.006


def read_device_config(dut, timeout_s=2.0):
    """Fetch the device's live config over USB. Returns a dict, or None."""
    dut.send({"cmd": "config/get", "id": 80})
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for m in dut.poll():
            if isinstance(m.get("data"), dict) and "minLap" in m["data"]:
                return m["data"]
        time.sleep(0.05)
    return None


def check_interval_vs_minlap(cfg, interval_ms):
    """Guard against the trap that produced '30 passes, 1 lap'.

    Lap detection is gated by minLap: any crossing sooner than that after the
    previous one is DISCARDED by design. Gate 1 is exempt, so a run with too
    short an interval reports exactly one lap and looks like a total detection
    failure when the firmware is in fact behaving correctly.
    """
    if not cfg or "minLap" not in cfg:
        return None
    min_lap_ms = int(cfg["minLap"]) * 100
    if interval_ms > min_lap_ms:
        return None
    return (
        f"INTERVAL TOO SHORT: the device's Min Lap is {min_lap_ms} ms but the "
        f"pass interval is {interval_ms} ms. Every crossing after the first "
        f"will be discarded by the minLap guard, and the run will report "
        f"exactly ONE lap (Gate 1) regardless of how well detection is "
        f"working.\n"
        f"    Fix either way:\n"
        f"      - raise the interval above {min_lap_ms} ms, or\n"
        f"      - set Min Lap to 0 in Settings (fastest runs), or tick\n"
        f"        'Force Min Lap = 0' to have the harness do it for you."
    )


def check_maxlaps_vs_count(cfg, count):
    """Guard against a race that ARMS correctly but finishes early.

    With maxLaps set, the race ends itself once that many laps are recorded.
    Every remaining generated pass then goes undetected, and the run reports a
    partial count that reads exactly like a marginal-detection result — the
    statistics are computed over a truncated sample and quietly understate the
    detection rate.
    """
    if not cfg or "maxLaps" not in cfg:
        return None
    max_laps = int(cfg["maxLaps"])
    if max_laps <= 0 or max_laps >= count:
        return None
    return (
        f"MAX LAPS TOO LOW: the device's Max Laps is {max_laps} but this run "
        f"generates {count} passes. The race will END ITSELF after lap "
        f"{max_laps}, so the remaining {count - max_laps} passes cannot be "
        f"detected and the run will look like {max_laps}/{count} detection.\n"
        f"    Fix: set Max Laps to 0 (unlimited) in Settings, or lower the "
        f"pass count to {max_laps} or fewer."
    )


def geometric_detect_ms(width_ms, base_rssi, peak_rssi, exit_rssi):
    """The EARLIEST a lap can possibly be confirmed, from geometry alone.

    A lap is confirmed on gate EXIT — the filtered RSSI falling back below the
    exit threshold — not at the peak.  So for a pass of a given width, there is
    a hard floor on detection latency set purely by when the envelope falls
    through the exit threshold.  Nothing in the firmware can beat it.

    For the raised-cosine envelope the emulator generates:

        w(x) = 0.5 * (1 - cos(2*pi*x)),  x = 0..1 across the pass

    solving w = exit_frac on the FALLING edge gives the crossing point.

    Comparing measured latency against this floor separates "the pass had to
    finish first" from "the pipeline added lag" — which is the only part the
    firmware controls, and the only part worth judging.
    """
    swing = peak_rssi - base_rssi
    if swing <= 0 or width_ms <= 0:
        return None
    frac = (exit_rssi - base_rssi) / swing
    frac = max(0.001, min(0.999, frac))
    # Falling-edge solution of 0.5*(1-cos(2*pi*x)) = frac
    x = 1.0 - math.acos(max(-1.0, min(1.0, 1.0 - 2.0 * frac))) / (2.0 * math.pi)
    return width_ms * x


def dac_for_rssi(slope, intercept, target_rssi):
    """Invert the measured transfer: what DAC value yields this RSSI?

    WHY THIS MATTERS FOR THE A/B
    The polled and DMA paths do NOT share a gain — measured 0.888 vs 1.70
    RSSI-per-DAC on the same hardware and divider, a factor of ~1.92.  Driving
    both modes with the same DAC values therefore presents the device with a
    DIFFERENT RSSI envelope in each mode, which is exactly the kind of hidden
    second variable that makes a comparison meaningless (and, at the top of
    the range, clips).

    Specifying the envelope in RSSI and back-solving the DAC per mode removes
    that: both modes then see the same signal as the detector sees it, and the
    only thing differing is the acquisition path itself.
    """
    if slope <= 0:
        return None
    dac = (target_rssi - intercept) / slope
    return max(0, min(255, int(round(dac))))


def plan_envelope(slope, intercept, target_base_rssi, target_peak_rssi):
    """Return (baseline_dac, peak_dac, achievable_peak_rssi, clipped).

    clipped is True when the requested peak cannot be reached because DAC 255
    is not enough — the caller should say so rather than silently testing a
    smaller envelope than asked for.
    """
    b = dac_for_rssi(slope, intercept, target_base_rssi)
    p = dac_for_rssi(slope, intercept, target_peak_rssi)
    if b is None or p is None:
        return None, None, None, False
    achievable = slope * p + intercept
    return b, p, achievable, (achievable < target_peak_rssi - 2)


def compute_thresholds(slope, intercept, baseline_dac, peak_dac,
                       enter_frac=0.60, exit_frac=0.40):
    """Derive Enter/Exit from the MEASURED transfer, not from a calibration run.

    The wizard exists to infer thresholds from an unknown real-world signal.
    Here the signal is synthetic and the transfer has just been measured, so
    the thresholds can be computed exactly — which is both repeatable between
    runs and independent of how well a hand-timed wizard recording went.

    enter_frac / exit_frac are positions within the baseline-to-peak swing.
    0.60/0.40 gives useful hysteresis while leaving the enter threshold high
    enough that the pass-width sweep still finds a real detection limit: set
    Enter too low and every width detects, making the sweep meaningless.
    """
    base_rssi = slope * baseline_dac + intercept
    peak_rssi = slope * peak_dac + intercept
    swing = peak_rssi - base_rssi
    enter = int(round(base_rssi + swing * enter_frac))
    exit_ = int(round(base_rssi + swing * exit_frac))
    return (max(0, min(255, enter)), max(0, min(255, exit_)),
            base_rssi, peak_rssi)


def set_thresholds(dut, enter, exit_, timeout_s=2.0):
    """Write Enter/Exit over USB. Returns True on ack."""
    dut.send({"cmd": "config/set", "id": 82,
              "data": {"enterRssi": int(enter), "exitRssi": int(exit_)}})
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for m in dut.poll():
            if m.get("id") == 82 and m.get("status") == "OK":
                return True
        time.sleep(0.05)
    return False


def set_voice_enabled(dut, enabled, timeout_s=2.0):
    """Mute/unmute the device's announcements.

    Announcements are rendered in the BROWSER, not on the device, so muting
    them cannot affect the measurement — it only spares the operator a long
    TTS backlog after a 30-lap run.  Note an already-open browser tab caches
    this at load time and needs a refresh to pick up the change.
    """
    dut.send({"cmd": "config/set", "id": 83,
              "data": {"voiceEnabled": 1 if enabled else 0}})
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for m in dut.poll():
            if m.get("id") == 83 and m.get("status") == "OK":
                return True
        time.sleep(0.05)
    return False


def set_min_lap(dut, value, timeout_s=2.0):
    """Set the device's minLap (units of 0.1 s). Returns True on ack."""
    dut.send({"cmd": "config/set", "id": 81, "data": {"minLap": int(value)}})
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for m in dut.poll():
            if m.get("id") == 81 and m.get("status") == "OK":
                return True
        time.sleep(0.05)
    return False


def measure_transfer(emu, dut, levels=(0, 32, 64, 96, 128, 160, 200, 255),
                     dwell_s=0.8, progress=None):
    """Hold each DAC level and record what the device reports.

    Returns [(dac, rssi_or_None), ...].  Requires the device's USB RSSI
    stream, which is OFF by default and must be started explicitly — that
    omission is what made the first version of this check report nothing.
    """
    dut.send({"cmd": "rssi/start", "id": 90})
    time.sleep(0.3)
    dut.poll()

    points = []
    try:
        for d in levels:
            emu.send({"cmd": "level", "value": int(d)})
            # Discard anything already queued BEFORE reading.  The device
            # streams RSSI every 200 ms and its serial can back up under load,
            # so without this drain the readings for one level include stale
            # events from the previous one.  Smeared like that the fitted slope
            # collapses toward zero, and Set Thresholds then computes a swing
            # of ~0 and writes Enter == Exit — which is how a perfectly good
            # rig produced "Enter 34, Exit 34".
            time.sleep(0.25)          # let the level settle and old events land
            dut.poll()                # drop them
            time.sleep(dwell_s)       # now sample only this level
            seen = [m["data"] for m in dut.poll()
                    if m.get("event") == "rssi" and isinstance(m.get("data"), int)]
            val = statistics.median(seen) if seen else None
            points.append((d, val))
            if progress:
                progress(d, val)
    finally:
        dut.send({"cmd": "rssi/stop", "id": 91})
        emu.send({"cmd": "stop"})
    return points


def _fit_line(points):
    """Least-squares fit. Returns (slope, intercept, max_abs_residual)."""
    n = len(points)
    sx = sum(d for d, _ in points)
    sy = sum(v for _, v in points)
    sxy = sum(d * v for d, v in points)
    sxx = sum(d * d for d, _ in points)
    den = n * sxx - sx * sx
    if den == 0:
        return 0.0, 0.0, 0.0
    slope = (n * sxy - sx * sy) / den
    intercept = (sy - slope * sx) / n
    resid = max(abs(v - (slope * d + intercept)) for d, v in points)
    return slope, intercept, resid


def diagnose_transfer(points, enter_rssi=None, adc_mode=None):
    """Judge the analog path on linearity and headroom, not on absolute gain.

    adc_mode: 0 = polled, 1 = DMA, None = unknown.  The two acquisition paths
    have materially different ADC gain (~1.9x measured), so saturation at the
    top of the DAC sweep is EXPECTED in DMA mode and is not a wiring fault.
    Reporting it as one sends people to check resistors that are already right.

    Returns (verdicts, gain).
    """
    all_pts = [(d, v) for d, v in points if v is not None]
    out = []

    if not all_pts:
        out.append(("bad",
            "NO RSSI RECEIVED. The device never reported a reading. Check the "
            "FPVRaceOne port, and that the firmware accepts 'rssi/start'."))
        return out, None

    if len(all_pts) < 3:
        out.append(("warn", "Too few readings to judge the path."))
        return out, None

    top_dac    = max(d for d, _ in all_pts)
    clipped_at = next((d for d, v in sorted(all_pts) if v >= 254), None)

    # Characterise the LINEAR REGION only.  Including saturated points would
    # drag the fit and understate the true gain, hiding the very thing that
    # caused the saturation.
    usable = [(d, v) for d, v in all_pts if v < 254]
    if len(usable) < 3:
        out.append(("bad",
            f"SATURATED ALMOST IMMEDIATELY — only {len(usable)} unclipped "
            f"point(s). Far too much signal reaching the device; check the "
            f"divider is present and the right way round (series resistor from "
            f"the DAC, shunt to ground)."))
        return out, None

    slope, intercept, resid = _fit_line(usable)
    max_rssi = max(v for _, v in usable)

    span = max_rssi - min(v for _, v in usable)
    resid_pct = (resid / span * 100.0) if span else 100.0

    if clipped_at is not None and clipped_at <= top_dac:
        mode_txt = {0: "polled", 1: "DMA"}.get(adc_mode)
        out.append(("bad",
            f"CLIPPING{' in ' + mode_txt + ' mode' if mode_txt else ''} — RSSI "
            f"pinned at 255 from DAC {clipped_at} upward, with DAC range to "
            f"spare. Too much signal reaching the detector."))
        if adc_mode == 1:
            # DMA used to clip here for a firmware reason (RSSI_SCALE_MAX was
            # tuned for the polled path only).  That is fixed, so clipping in
            # DMA mode is no longer expected and must not be waved through.
            out.append(("dim",
                f"Measured slope in this mode is {slope:.3f}. After the DMA scale "
                f"fix both modes should read within a few percent of each other. "
                f"If polled measures near half this, the firmware's "
                f"RSSI_DMA_GAIN_NUM constant does not match this chip — re-derive "
                f"it as (DMA slope / polled slope) x 1000 in lib/RX5808/RX5808.cpp."))
        else:
            out.append(("dim",
                "Check the divider is present and the right way round: series "
                "resistor from the DAC, shunt from the node to ground."))
        return out, slope

    if resid_pct > 5.0:
        out.append(("bad",
            f"NON-LINEAR — points deviate up to {resid:.1f} RSSI counts "
            f"({resid_pct:.0f}% of span) from a straight line. A clean resistive "
            f"path is linear, so this suggests something is loading the node: a "
            f"still-connected RX5808 RSSI pin, a bad joint, or a missing ground."))
        return out, slope

    region = "" if clipped_at is None else " across the unclipped region"
    out.append(("good",
        f"LINEARITY GOOD — worst deviation {resid:.1f} RSSI counts "
        f"({resid_pct:.1f}% of span){region} from a straight-line fit. The analog "
        f"path is clean; wiring and resistors are behaving correctly."))

    mode_txt = {0: "polled", 1: "DMA"}.get(adc_mode, "unknown mode")
    out.append(("dim",
        f"Measured transfer ({mode_txt}): rssi = {slope:.3f} x dac + "
        f"{intercept:.1f}. Highest usable reading RSSI {max_rssi}. Absolute gain "
        f"depends on the real DAC full-scale and the C6's ADC range, so it is "
        f"calibrated here rather than assumed."))

    # Cross-mode consistency.  The firmware scales the DMA path so both
    # acquisition modes report the same RSSI for the same voltage; that
    # correction uses a constant measured on one board.  Surfacing the slope
    # here makes a mismatch on a different chip visible instead of silent —
    # it would otherwise show up much later as calibration that does not
    # survive an adcMode change.
    if adc_mode is not None:
        out.append(("dim",
            f"Run this in the OTHER mode too: the slopes should now agree within "
            f"a few percent. A ~2x difference means the firmware's "
            f"RSSI_DMA_GAIN_NUM does not suit this chip — re-derive it as "
            f"(DMA slope / polled slope) x 1000 in lib/RX5808/RX5808.cpp."))

    # 2. HEADROOM — the usability check, against the real Enter threshold.
    if enter_rssi is None:
        out.append(("warn",
            f"Could not read the device's Enter threshold, so headroom is "
            f"unchecked. Peak available is RSSI {max_rssi}; make sure Enter is "
            f"comfortably below that."))
        return out, slope

    margin = max_rssi - enter_rssi
    if margin >= 60:
        out.append(("good",
            f"HEADROOM GOOD — peak RSSI {max_rssi} against an Enter threshold of "
            f"{enter_rssi}, {margin} counts of margin. Plenty to trigger "
            f"detection reliably."))
    elif margin >= 25:
        out.append(("warn",
            f"HEADROOM WORKABLE — peak RSSI {max_rssi} vs Enter {enter_rssi}, "
            f"{margin} counts of margin. Usable, but short passes will be "
            f"marginal because the median filter needs several samples above "
            f"the threshold. Consider more range (below)."))
    else:
        out.append(("bad",
            f"HEADROOM TOO THIN — peak RSSI {max_rssi} vs Enter {enter_rssi}, "
            f"only {margin} counts. Detection will be unreliable regardless of "
            f"timing performance."))

    if margin < 60:
        want = 2.2 / 3.2 / (1.0 / 3.2)   # swap ratio improvement = 2.2x
        out.append(("dim",
            f"To gain range, SWAP THE TWO RESISTORS: put the 1.0k in series from "
            f"the DAC and the 2.2k from the node to ground. That raises the "
            f"divider ratio from 0.31 to 0.69 ({want:.1f}x), taking peak RSSI "
            f"from about {max_rssi} to roughly {min(255, int(max_rssi * want))}. "
            f"Same parts, no rewiring beyond the swap."))

    return out, slope


def _split_host_port(addr, default_port=80):
    """Accept '192.168.4.1' or '192.168.4.1:8080'.

    http.client ignores a port embedded in the host string when an explicit
    port argument is also supplied, which silently sends traffic to the wrong
    place — so parse it here rather than relying on that behaviour.
    """
    if ":" in addr:
        host, _, p = addr.rpartition(":")
        try:
            return host, int(p)
        except ValueError:
            return addr, default_port
    return addr, default_port


class MultiNodeLoad:
    """Generates master-side multi-node load by POSTing laps over WiFi.

    WHAT THIS REPRODUCES
    Every client lap hits the master's most expensive path:

        handleLap() -> events.send("multiNodeLap") -> pushMultiNodeState()
                                                       `-> BLOCKING HTTP fanout
                                                           to every registered client

    handleLap() validates only that nodeId matches a registered node — it does
    not care which socket the request arrived on.  So a POST from the PC costs
    the master exactly what a POST from a client costs, and triggers the same
    fanout.  That fanout is the mechanism capable of starving loop(), which is
    where RSSI sampling happens, so it is the thing worth loading.

    WHAT IT DOES NOT REPRODUCE
    N clients each opening a TCP connection at the same instant, contending for
    the medium.  Real clients are still on the air here (heartbeats,
    re-registration, and ACKing every fanout POST), so most radio traffic is
    genuine — but simultaneous client-initiated lap bursts are not.  Splitting
    across two source addresses (see source_ips) recovers some of that.

    Registered clients are still REQUIRED: handleLap rejects unknown nodeIds,
    and the fanout needs somewhere to go.  They can otherwise sit idle.
    """

    def __init__(self, master_ip, node_ids, laps_per_sec=2.0,
                 source_ips=None, timeout=2.0,
                 mode="burst", burst_interval_s=3.0, burst_spread_ms=50):
        """Injects lap POSTs into the master to simulate client traffic.

        mode="burst" (default) models what actually happens in a race: a pack
        crosses the gate together, so every node reports a lap within a few
        milliseconds and the master must absorb N concurrent POSTs and then fan
        state out to N clients off the back of them.

        mode="spread" is the original behaviour — one POST every 1/laps_per_sec,
        strictly sequential.  It never places two arrivals concurrently, so it
        cannot exercise the contention that a real pack creates. Kept for
        comparison against earlier results.

        burst_spread_ms is the window the pack arrives within; 0 is a perfectly
        simultaneous (unphysical) crossing, and ~50 ms is a tight real pack.
        """
        self.master_ip = master_ip
        self.node_ids = list(node_ids)
        self.period = 1.0 / max(0.01, laps_per_sec)
        self.mode = mode
        self.burst_interval_s = max(0.2, float(burst_interval_s))
        self.burst_spread_ms = max(0, int(burst_spread_ms))
        # Worst observed time for one burst to fully clear, and how many bursts
        # overran their interval — the signal that the master stopped keeping up.
        self.worst_burst_ms = 0
        self.bursts = 0
        self.bursts_overrun = 0
        # Optional local addresses to bind outgoing sockets to.  With two WiFi
        # adapters on the same subnet the OS picks one route by metric, so
        # genuine dual-station behaviour needs explicit source binding.
        self.source_ips = list(source_ips) if source_ips else [None]
        self.timeout = timeout

        self._stop = threading.Event()
        self._thread = None
        # Burst mode posts from one thread per node, so the result counters are
        # touched concurrently. int += is not atomic under free-threaded Python
        # and would silently undercount.
        self._counts = threading.Lock()
        self.sent = 0
        self.failed = 0
        self.last_error = None

    def _post_one(self, node_id, lap_num, src_ip):
        body = json.dumps({
            "nodeId": int(node_id),
            "lapTimeMs": 3000 + (lap_num * 7) % 400,   # plausible, varying
            "lapNumber": int(lap_num) & 0xFF,
        })
        conn = None
        try:
            kwargs = {"timeout": self.timeout}
            if src_ip:
                kwargs["source_address"] = (src_ip, 0)
            host, port = _split_host_port(self.master_ip)
            conn = http.client.HTTPConnection(host, port, **kwargs)
            conn.request("POST", "/api/multinode/lap", body,
                         {"Content-Type": "application/json"})
            resp = conn.getresponse()
            resp.read()
            if resp.status == 200:
                with self._counts:
                    self.sent += 1
            else:
                with self._counts:
                    self.failed += 1
                # 404 means the nodeId is not registered — the usual cause of a
                # load run that appears to do nothing.
                self.last_error = f"HTTP {resp.status} for node {node_id}"
        except Exception as e:
            with self._counts:
                self.failed += 1
                self.last_error = f"{type(e).__name__}: {e}"
        finally:
            if conn:
                try:
                    conn.close()
                except Exception:
                    pass

    def _run(self):
        if self.mode == "burst":
            self._run_burst()
        else:
            self._run_spread()

    def _run_spread(self):
        lap_num = 1
        i = 0
        while not self._stop.is_set():
            node = self.node_ids[i % len(self.node_ids)]
            src = self.source_ips[i % len(self.source_ips)]
            self._post_one(node, lap_num, src)
            i += 1
            if i % len(self.node_ids) == 0:
                lap_num += 1
            self._stop.wait(self.period)

    def _run_burst(self):
        """Fire every node concurrently, once per burst interval.

        One thread per node, because _post_one() blocks on the socket — posting
        them from a single thread would serialise the very concurrency this is
        meant to create.
        """
        lap_num = 1
        while not self._stop.is_set():
            t0 = time.time()
            threads = []
            for idx, node in enumerate(self.node_ids):
                src = self.source_ips[idx % len(self.source_ips)]
                # Stagger arrivals across the spread window so the pack is
                # realistically ragged rather than perfectly aligned.
                delay = (random.uniform(0, self.burst_spread_ms / 1000.0)
                         if self.burst_spread_ms else 0.0)
                t = threading.Thread(target=self._burst_one,
                                     args=(node, lap_num, src, delay),
                                     daemon=True)
                t.start()
                threads.append(t)

            for t in threads:
                t.join(timeout=self.timeout + 1.0)

            burst_ms = (time.time() - t0) * 1000.0
            self.worst_burst_ms = max(self.worst_burst_ms, burst_ms)
            self.bursts += 1
            lap_num += 1

            remaining = self.burst_interval_s - (time.time() - t0)
            if remaining <= 0:
                # The burst took longer than the interval that gates it — the
                # master is no longer absorbing a pack within one lap.
                self.bursts_overrun += 1
            else:
                self._stop.wait(remaining)

    def _burst_one(self, node_id, lap_num, src_ip, delay):
        if delay:
            time.sleep(delay)
        if not self._stop.is_set():
            self._post_one(node_id, lap_num, src_ip)

    @property
    def started(self):
        """True once start() has actually launched the poster thread.  Load is
        now armed from inside run_once(), so it may never start if the run
        aborts before the race is confirmed."""
        return self._thread is not None

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self.sent = self.failed = 0
        self.last_error = None
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3.0)


def probe_master(master_ip, timeout=2.0):
    """Return the master's node list, or None. Confirms reachability and tells
    us which nodeIds are actually registered — POSTing to an unregistered id
    is silently rejected with 404 and would produce a load run that does
    nothing."""
    conn = None
    try:
        host, port = _split_host_port(master_ip)
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.request("GET", "/api/multinode/nodes")
        resp = conn.getresponse()
        if resp.status != 200:
            return None
        return json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return None
    finally:
        if conn:
            try:
                conn.close()
            except Exception:
                pass


def find_min_detectable_width(emu, dut, interval_ms, peak, baseline,
                              start_ms=100, floor_ms=2, resolution_ms=2,
                              count=10, require=0.9, progress=None,
                              cancelled=None, on_armed=None):
    """Binary-search the shortest pass width the device still detects.

    WHY A SEARCH RATHER THAN A LIST
    A fixed list of widths spends most of its runs far from the boundary —
    widths that obviously work and widths that obviously don't — while still
    only resolving the answer to whatever spacing the list happened to use.
    A search spends its runs where the information is.

    Two phases:
      1. BRACKET  — halve the width until detection fails, giving a
                    known-good / known-bad pair around the limit.
      2. BISECT   — narrow that pair until it is within resolution_ms.

    'Detects' means the detection rate meets `require` (default 90%), not
    merely non-zero: near the limit detection is probabilistic, and a single
    lucky pass is not a capability.

    Returns dict with: limit_ms (smallest reliable width, or None),
    tested (list of (width, detected, expected)), and reason.
    """
    tested = []

    def _try(width):
        if cancelled is not None and cancelled():
            return None
        res = run_once(emu, dut, width, interval_ms, count, peak, baseline,
                       on_armed=on_armed)
        rate = (res.detected / res.expected) if res.expected else 0.0
        ok = rate >= require
        tested.append((width, res.detected, res.expected))
        if progress:
            progress(width, res.detected, res.expected, ok)
        return ok

    # ── Phase 1: bracket ────────────────────────────────────────────────
    good = _try(start_ms)
    if good is None:
        return {"limit_ms": None, "tested": tested, "reason": "cancelled"}
    if not good:
        # Cannot even detect the starting width — searching downward is
        # pointless, and the real problem is elsewhere (thresholds, envelope).
        return {"limit_ms": None, "tested": tested,
                "reason": "start_failed"}

    hi = start_ms          # known good
    lo = None              # known bad
    w = start_ms
    while w > floor_ms:
        w = max(floor_ms, w // 2)
        ok = _try(w)
        if ok is None:
            return {"limit_ms": hi, "tested": tested, "reason": "cancelled"}
        if ok:
            hi = w
            if w == floor_ms:
                # Detects even at the floor — the limit is below what we test.
                return {"limit_ms": hi, "tested": tested, "reason": "below_floor"}
        else:
            lo = w
            break

    if lo is None:
        return {"limit_ms": hi, "tested": tested, "reason": "below_floor"}

    # ── Phase 2: bisect ─────────────────────────────────────────────────
    while hi - lo > resolution_ms:
        mid = (hi + lo) // 2
        if mid == lo or mid == hi:
            break
        ok = _try(mid)
        if ok is None:
            return {"limit_ms": hi, "tested": tested, "reason": "cancelled"}
        if ok:
            hi = mid
        else:
            lo = mid

    return {"limit_ms": hi, "tested": tested, "reason": "converged"}


def estimate_search_runs(start_ms, floor_ms, resolution_ms):
    """Roughly how many runs the search will need — for a time estimate."""
    import math
    bracket = max(1, int(math.log2(max(1, start_ms / max(1, floor_ms))))) + 1
    bisect = max(1, int(math.log2(max(1, start_ms / max(1, resolution_ms)))))
    return bracket + bisect


def summarize(res):
    """Return a dict of statistics, or None if there is nothing to report."""
    errs = res.errors
    if not errs:
        return None
    s = {
        "n": len(errs),
        "bias_ms": statistics.mean(errs),
        "min_ms": min(errs),
        "max_ms": max(errs),
        "stdev_ms": statistics.stdev(errs) if len(errs) > 1 else 0.0,
    }
    if res.latencies:
        lat = sorted(res.latencies)
        s["lat_mean_us"] = statistics.mean(lat)
        s["lat_min_us"] = min(lat)
        s["lat_max_us"] = max(lat)
        s["lat_stdev_us"] = statistics.stdev(lat) if len(lat) > 1 else 0.0
        # Robust statistics alongside mean/stdev.  A single bad sample — a
        # dropped or mis-associated marker edge — can inflate stdev enough to
        # look like real jitter.  Median and inter-quartile range are immune to
        # that, so a large gap between stdev and IQR is itself the signal that
        # outliers, not spread, are driving the number.
        s["lat_median_us"] = statistics.median(lat)
        if len(lat) >= 4:
            mid = len(lat) // 2
            lo = statistics.median(lat[:mid])
            hi = statistics.median(lat[-mid:])
            s["lat_iqr_us"] = hi - lo
            # Count points more than 1.5 IQR outside the quartiles (Tukey).
            fence = 1.5 * (hi - lo)
            s["lat_outliers"] = sum(1 for v in lat if v < lo - fence or v > hi + fence)
    return s


# ── Commands ────────────────────────────────────────────────────────────────

def cmd_interval(args, emu, dut):
    cfg = read_device_config(dut)
    warn = check_interval_vs_minlap(cfg, args.interval)
    if warn:
        print(f"\n  {warn}\n")
        if not args.force:
            print("  Aborting. Re-run with --force to proceed anyway.\n")
            return 1

    print(f"\nInterval test — {args.count} passes, {args.interval} ms apart, "
          f"{args.width} ms wide\n")
    res = run_once(emu, dut, args.width, args.interval, args.count,
                   args.peak, args.baseline, verbose=args.verbose)
    st = summarize(res)

    print(f"  Passes generated : {res.expected}")
    print(f"  Laps reported    : {res.detected}")
    if res.detected < res.expected:
        print(f"  MISSED           : {res.expected - res.detected}")

    if not st:
        print("\n  Not enough laps to compute statistics.")
        return 1

    print(f"\n  Lap-time error (reported - {args.interval} ms), n={st['n']}")
    print(f"    bias   : {st['bias_ms']:+.2f} ms   <- LATE?")
    print(f"    stdev  : {st['stdev_ms']:.2f} ms    <- INCONSISTENT?")
    print(f"    range  : {st['min_ms']:+d} .. {st['max_ms']:+d} ms")

    if "lat_mean_us" in st:
        print(f"\n  Absolute detection latency, n={len(res.latencies)}")
        print(f"    mean   : {st['lat_mean_us']/1000.0:.2f} ms")
        print(f"    range  : {st['lat_min_us']/1000.0:.2f} .. "
              f"{st['lat_max_us']/1000.0:.2f} ms")
        print(f"    stdev  : {st['lat_stdev_us']/1000.0:.2f} ms")
    else:
        print("\n  No marker edges seen — is TIMING_MARKER_ENABLED set to 1 "
              "and D3/GPIO21 wired to the emulator's GPIO4?")
    print()
    return 0


def cmd_sweep(args, emu, dut):
    cfg = read_device_config(dut)
    warn = check_interval_vs_minlap(cfg, args.interval)
    if warn:
        print(f"\n  {warn}\n")
        if not args.force:
            print("  Aborting. Re-run with --force to proceed anyway.\n")
            return 1

    runs = estimate_search_runs(args.start, 2, args.resolution)
    print(f"\nMinimum pass width — automatic search")
    print(f"Halves from {args.start} ms until detection fails, then bisects to "
          f"+/-{args.resolution} ms.")
    print(f"{args.count} passes per step, >={args.require}% counts as detected.")
    print(f"About {runs} runs, roughly "
          f"{runs * args.count * args.interval / 60000.0:.1f} min.\n")
    print(f"  {'width':>8} {'detected':>10} {'rate':>7}   verdict")
    print(f"  {'-'*8} {'-'*10} {'-'*7}   {'-'*7}")

    def progress(width, detected, expected, passed):
        rate = (detected / expected * 100.0) if expected else 0.0
        print(f"  {width:>6}ms {detected:>4}/{expected:<5} {rate:>6.0f}%   "
              f"{'detects' if passed else 'FAILS'}")

    result = find_min_detectable_width(
        emu, dut, interval_ms=args.interval, peak=args.peak,
        baseline=args.baseline, start_ms=args.start,
        resolution_ms=args.resolution, count=args.count,
        require=args.require / 100.0, progress=progress)

    limit, reason = result["limit_ms"], result["reason"]
    print()
    if reason == "start_failed":
        print(f"  Starting width {args.start} ms did not detect reliably — "
              f"nothing to search for. Raise it, or re-check Enter/Exit.")
        return 1
    if reason == "below_floor":
        print(f"  Detects reliably even at {limit} ms, the shortest tested.")
    else:
        print(f"  MINIMUM RELIABLE PASS: {limit} ms (+/-{args.resolution} ms) "
              f"at >={args.require}% detection")
    print()
    return 0


def cmd_level(args, emu, dut):
    """Sweep the DAC and check the device sees what it should.

    Run this FIRST. Timing numbers from a mis-wired analog path are worthless,
    and a low-amplitude signal presents as a detection failure that looks like
    a timing bug.
    """
    cfg = read_device_config(dut)
    enter = cfg.get("enterRssi") if cfg else None
    amode = int(cfg.get("adcActive", cfg.get("adcMode", 0))) if cfg else None

    print("\nANALOG PATH CHECK")
    print("Sweeping the DAC to measure the transfer, then judging it on")
    print("linearity (is the path clean?) and headroom (can it trigger?).\n")
    if enter is not None:
        print(f"  Device Enter threshold: {enter}\n")
    print(f"  {'DAC':>5} {'measured':>10}")
    print(f"  {'-'*5} {'-'*10}")

    def show(d, v):
        print(f"  {d:>5} {('--' if v is None else int(v)):>10}")

    points = measure_transfer(emu, dut, progress=show)
    verdicts, gain = diagnose_transfer(points, enter_rssi=enter, adc_mode=amode)

    print()
    for _tag, line in verdicts:
        print(f"  {line}\n")

    usable = [(d, v) for d, v in points if v is not None]
    if len(usable) >= 3 and not any(t == "bad" for t, _ in verdicts):
        slope, intercept, _ = _fit_line(usable)
        en, ex, base_r, peak_r = compute_thresholds(
            slope, intercept, args.baseline, args.peak)
        print(f"  Suggested thresholds for baseline={args.baseline} "
              f"peak={args.peak} (RSSI {base_r:.0f} to {peak_r:.0f}):")
        print(f"    Enter {en}, Exit {ex}")
        if args.set_thresholds:
            ok = set_thresholds(dut, en, ex)
            print(f"    {'Applied.' if ok else 'FAILED to apply.'}")
        else:
            print("    Re-run with --set-thresholds to apply them.")
        print()

    return 0 if not any(t == "bad" for t, _ in verdicts) else 1


# ── Entry point ─────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    p.add_argument("--emu", help="emulator serial port (ESP32 WROOM-32)")
    p.add_argument("--dut", help="FPVRaceOne serial port (XIAO C6)")
    p.add_argument("--baseline", type=int, default=40, help="DAC counts at rest")
    p.add_argument("--peak", type=int, default=200, help="DAC counts at apex")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="proceed even if the interval is below the device's Min Lap")

    sub = p.add_subparsers(dest="mode")

    # Default interval must exceed the firmware's default Min Lap of 5000 ms,
    # or every crossing after Gate 1 is discarded and the run reports one lap.
    pi = sub.add_parser("interval", help="accuracy + consistency at one width")
    pi.add_argument("--count", type=int, default=30)
    pi.add_argument("--interval", type=int, default=6000, help="ms between passes")
    pi.add_argument("--width", type=int, default=40, help="pass width in ms")

    ps = sub.add_parser("sweep", help="binary-search the minimum detectable width")
    ps.add_argument("--start", type=int, default=100,
                    help="starting width; must detect reliably")
    ps.add_argument("--resolution", type=int, default=2,
                    help="how precisely to locate the limit")
    ps.add_argument("--count", type=int, default=10, help="passes per step")
    ps.add_argument("--require", type=int, default=90,
                    help="percent detection that counts as 'detects'")
    ps.add_argument("--interval", type=int, default=6000)

    pl = sub.add_parser("level", help="sweep the DAC and validate the analog path")
    pl.add_argument("--set-thresholds", action="store_true",
                    help="apply the computed Enter/Exit to the device")

    args = p.parse_args()

    if args.list:
        for prt in list_ports.comports():
            print(f"  {prt.device:12s} {prt.description}")
        return 0

    if not args.mode:
        p.print_help()
        return 1
    if not args.emu or not args.dut:
        sys.exit("Both --emu and --dut are required. Use --list to find ports.")

    emu = JsonLink(args.emu, name="emu")
    dut = JsonLink(args.dut, name="dut")
    try:
        emu.send({"cmd": "ping"})
        time.sleep(0.3)
        if not any(m.get("event") == "ready" for m in emu.poll()):
            print("Warning: emulator did not answer ping — wrong port?",
                  file=sys.stderr)

        return {"interval": cmd_interval,
                "sweep": cmd_sweep,
                "level": cmd_level}[args.mode](args, emu, dut)
    finally:
        emu.close()
        dut.close()


if __name__ == "__main__":
    sys.exit(main())
