console.log('[FPV] js');
// Transport manager for WiFi/USB connectivity
let transportManager = null;
let currentConnectionMode = 'wifi'; // 'auto', 'wifi', 'usb'  — USB/Electron disabled for now
let usbConnected = false;
let eventSource = null;
let eventSourceReconnectTimer = null;
let eventSourceReconnectAttempts = 0;
const MAX_RECONNECT_ATTEMPTS = 10;
const RECONNECT_DELAY_MS = 2000;
const RECONNECT_DELAY_FIRST_MS = 500;
let connectionStatusUpdateInterval = null;
let stagedConfig = {};      
let stagedDirty = false;    
let settingsLoading = false;         // true while we are populating UI from device config
let baselineConfig = {};             // last config loaded from device (for "same value" comparisons)
let scannerPaused = false;
const SCANNER_BUFFER_WHILE_PAUSED = false;
let mnClientSkipEnabled   = false; // mirrors config.mnSkipMasterStart — set at page load and on toggle change
let mnClientRaceAudio     = false; // mirrors config.mnClientRaceAudio — client mode race-start audio toggle
let _raceCountdownAborted = false; // set by stopRace/mnStopRace to cancel in-progress countdown
// Multi-tab sync (single/standalone mode): when two browsers view the same
// unit, only the tab that pressed Start ("initiator") announces laps/race
// events.  Spectator tabs mirror the display via SSE raceState events but
// suppress TTS to avoid a stereo announcer.  Defaults true so a lone tab
// still announces normally; flips to false when a raceState=prearming or
// =started arrives that we didn't originate.  Resets to true on =stopped.

let _iAmRaceInitiator = true;
// Wall-clock timestamp of the last local startRace() call.  Used by the
// raceState=started SSE branch to self-heal when the prearming event was
// missed — e.g. a spectator that connected mid-countdown, or the onConnect
// replay of "started" for a tab that joined mid-race.  If `started` arrives
// and we haven't pressed Start locally in the last 30 s, we're a spectator
// and must run startRaceDisplayOnly() to sync the timer + flash animation.
let _localRaceStartTs = 0;

// --- Wizard loop control (prevents stale timers/fetches blocking restart) ---
let wizardRecordingTimerId = null;
let wizardAbortController = null;

// Number of RSSI points per page when fetching calibration data.
//
// Briefly lowered to 200 on the theory that the firmware's ~14 KB per-page
// String was exhausting the heap.  It was not: the failure surfaced as fetch's
// "Failed to fetch", a connection-level rejection, where a truncated body would
// have been a JSON SyntaxError.  The pressure is on the connection, not the
// heap, so keep the page count DOWN — 500 means 10 requests for a full
// recording instead of 25.  Pacing and retries live in
// _fetchCalibrationPageRetrying(); the firmware clamps limit to 1000.
const CALIBRATION_PAGE_SIZE = 500;

// --- Calibration overview mode (draw full wizard dataset on the live scanner canvas) ---
let calibOverviewMode = false;     // true when we're showing the full recorded dataset on the live chart canvas
let calibOverviewData = null;      // [{ rssi: number }, ...] downsampled for display
const CALIB_OVERVIEW_MAX_POINTS = 900; // cap so very large logs still render fast
let pausedScannerFrame = null;      // ImageData snapshot of the live scanner when pause is pressed
let pausedScannerFrameW = 0, pausedScannerFrameH = 0;
let pausedEnterStart = null;               
let pausedExitStart = null;           
let _rssiCanvasCtx = null;

const bcf = document.getElementById("bandChannelFreq");
const bandSelect = document.getElementById("bandSelect");
const channelSelect = document.getElementById("channelSelect");
const freqOutput = document.getElementById("freqOutput");
const announcerSelect = document.getElementById("announcerSelect");
const announcerRateInput = document.getElementById("rate");
const enterRssiInput = document.getElementById("enter");
const exitRssiInput = document.getElementById("exit");
const enterRssiSpan = document.getElementById("enterSpan");
const exitRssiSpan = document.getElementById("exitSpan");
const pilotNameInput = document.getElementById("pname");
const ssidInput = document.getElementById("ssid");
const pwdInput = document.getElementById("pwd");
const minLapInput = document.getElementById("minLap");
const alarmThreshold = document.getElementById("alarmThreshold");
const maxLapsInput = document.getElementById("maxLaps");

// --- Wake Lock: keep screen/CPU alive while the page is open ---
let _wakeLock = null;

async function acquireWakeLock() {
  if (!('wakeLock' in navigator)) return; // not supported
  try {
    _wakeLock = await navigator.wakeLock.request('screen');
  } catch (e) {
    // User denied or OS refused (e.g. low battery) — fail silently
  }
}

// Re-acquire after the page becomes visible again (browser may release on hide)
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') acquireWakeLock();
});

acquireWakeLock();

const freqLookup = [
  [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725], // A
  [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866], // B
  [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945], // E
  [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880], // F
  [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917], // R (RaceBand)
  [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621], // L (LowBand)
  [5660, 5695, 5735, 5770, 5805, 5878, 5914, 5839], // DJIv1-25
  [5735, 5770, 5805, 0, 0, 0, 0, 5839],             // DJIv1-25CE
  [5695, 5770, 5878, 0, 0, 0, 0, 5839],             // DJIv1_50
  [5669, 5705, 5768, 5804, 5839, 5876, 5912, 0],    // DJI03/04-20
  [5768, 5804, 5839, 0, 0, 0, 0, 0],                // DJI03/04-20CE
  [5677, 5794, 5902, 0, 0, 0, 0, 0],                // DJI03/04-40
  [5794, 0, 0, 0, 0, 0, 0, 0],                      // DJI03/04-40CE
  [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917], // DJI04-R
  [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917], // HDZero-R
  [5707, 0, 0, 0, 0, 0, 0, 0],                      // HDZero-E
  [5740, 5760, 0, 5800, 0, 0, 0, 0],                // HDZero-F
  [5732, 5769, 5806, 5843, 0, 0, 0, 0],             // HDZero-CE
  [5658, 5659, 5732, 5769, 5806, 5843, 5880, 5917], // WLKSnail-R
  [5660, 5695, 5735, 5770, 5805, 5878, 5914, 5839], // WLKSnail-25
  [5735, 5770, 5805, 0, 0, 0, 0, 5839],             // WLKSnail-25CE
  [5695, 5770, 5878, 0, 0, 0, 0, 5839],             // WLKSnail-50
];

const config = document.getElementById("config");
const race = document.getElementById("race");
const calib = document.getElementById("calib");
const ota = document.getElementById("ota");

var enterRssi = 120,
  exitRssi = 100;

// Last-persisted enter / exit values — kept separate from `enterRssi` /
// `exitRssi` (the live slider values) so the Save RSSI Thresholds button
// can light up "dirty" the moment a slider moves and clear the instant
// the values are persisted.  The Settings modal's saveConfigBtn tracks
// dirty via stagedConfig, but enter / exit have multiple save paths
// (Calibration tab's Save RSSI, the wizard's Apply, /api/multinode/editPilot
// self-edit) and stagedConfig isn't kept in sync across all of them.  A
// dedicated baseline avoids piggy-backing on that flaky state.
let _rssiSavedEnter = null;
let _rssiSavedExit  = null;

function _rssiHasUnsavedChanges() {
  return (_rssiSavedEnter !== null && enterRssi !== _rssiSavedEnter)
      || (_rssiSavedExit  !== null && exitRssi  !== _rssiSavedExit);
}

function updateRssiSaveButton() {
  const btn = document.getElementById('saveRssiBtn');
  if (!btn) return;
  btn.classList.toggle('dirty', _rssiHasUnsavedChanges());
}

function _markRssiSaved(enter, exit) {
  if (Number.isFinite(enter)) _rssiSavedEnter = enter;
  if (Number.isFinite(exit))  _rssiSavedExit  = exit;
  updateRssiSaveButton();
}
var frequency = 0;
var announcerRate = 1.0;

var lapNo = -1;
var lapTimes = [];

// Lap numbers the user has excluded from the summary statistics (Fastest,
// Fastest 3 Consecutive, Median, Best 3).  Single-mode feature.
//
// Keyed by LAP NUMBER, not by array index.  The two coincide today, but lap
// numbers are what the user sees on the badge, what the export carries, and
// what survives a firmware lap-ring wraparound where the array no longer starts
// at lap 0 — indices would silently re-point at different laps after a reload.
//
// Lap 0 is never a member: it is the first crossing, not a timed lap, and is
// already excluded from every statistic by construction.
//
// An excluded lap is still recorded, still displayed, and still counts toward
// the lap count and total time.  It is omitted from the summary only — this is
// for discarding a lap spoiled by a crash or a cut course, not for deleting it.
var excludedLaps = new Set();

function isLapExcluded(n) {
  return excludedLaps.has(n);
}

// Flip one lap's excluded state and refresh everything that depends on it.
function toggleLapExcluded(n) {
  if (!Number.isFinite(n) || n <= 0) return;
  if (excludedLaps.has(n)) excludedLaps.delete(n);
  else excludedLaps.add(n);
  applyExcludedLapUI();
  highlightFastestLap();
  updateAnalysisView();
}

// Repaint the per-row exclude buttons and the struck-through styling from
// `excludedLaps`.  Called after any change to the set or the table.
function applyExcludedLapUI() {
  const table = document.getElementById('lapTable');
  if (!table) return;
  for (let i = 1; i < table.rows.length; i++) {
    const row = table.rows[i];
    const n = parseInt(row.getAttribute('data-lap-number'), 10);
    if (!Number.isFinite(n) || n <= 0) continue;
    const off = isLapExcluded(n);
    row.classList.toggle('lap-excluded', off);
    const btn = row.querySelector('.lap-exclude-btn');
    if (btn) {
      btn.textContent = off ? 'Include' : 'Exclude';
      btn.title = off
        ? `Lap ${n} is excluded from the summary — click to put it back`
        : `Exclude lap ${n} from Fastest / Median / Best 3`;
      btn.setAttribute('aria-pressed', off ? 'true' : 'false');
    }
  }
}

// The laps eligible for summary statistics: everything after the first
// crossing, minus anything the user excluded.  Returns {time, lapNo} so a
// statistic can still report the real lap number after filtering.
function eligibleLapsForStats() {
  const out = [];
  for (let i = 1; i < lapTimes.length; i++) {
    if (isLapExcluded(i)) continue;
    out.push({ time: lapTimes[i], lapNo: i });
  }
  return out;
}
var maxLaps = 0;

// ── Splash screen ────────────────────────────────────────────────────────────
let _splashHidden = false;
function setSplashStatus(text) {
  const el = document.getElementById('splash-status');
  if (el) el.textContent = text;
}
function hideSplash() {
  if (_splashHidden) return;
  _splashHidden = true;
  const el = document.getElementById('splash');
  if (!el) return;
  setSplashStatus('Connected');
  el.classList.add('out');
  setTimeout(() => el.remove(), 500);
}
// Fallback — remove splash after 6 s even if every fetch fails
setTimeout(hideSplash, 6000);
// ─────────────────────────────────────────────────────────────────────────────

// Dynamically inject a <script> tag and return a Promise that resolves when loaded.
// Subsequent calls for the same URL resolve immediately (idempotent).
const _loadedScripts = {};
function loadScript(src) {
  if (_loadedScripts[src]) return _loadedScripts[src];
  _loadedScripts[src] = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload  = resolve;
    s.onerror = () => reject(new Error('Failed to load ' + src));
    document.head.appendChild(s);
  });
  return _loadedScripts[src];
}

// Fetch with automatic retry — returns the Response or throws after maxAttempts failures.
async function fetchWithRetry(url, options, maxAttempts = 3, delayMs = 1000) {
  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    try {
      const r = await fetch(url, options);
      if (r.ok) return r;
    } catch (_) {}
    if (attempt < maxAttempts) {
      setSplashStatus(`Retrying\u2026 (${attempt}/${maxAttempts})`);
      await new Promise(res => setTimeout(res, delayMs));
    }
  }
  throw new Error(`fetchWithRetry: ${url} failed after ${maxAttempts} attempts`);
}

var timerInterval;
var lapTimerStartMs = 0;            // Start time for current lap timer
// Race-relative time of the last gate crossing, and the Date.now() reference
// for race zero.  Together these let the current-lap clock be DERIVED rather
// than stamped, which is what makes it survive being recomputed.
//
// startRaceDisplayOnly() is called more than once on a page load — once by the
// /api/mode restore and again by the SSE onConnect replay of "started:<ms>" —
// so anything it stamps directly gets clobbered by whichever call lands last.
// That ordering race is why the lap clock first read 00:00 on refresh and then,
// once it was anchored to race start instead, read the whole race.
var lastCrossingRaceMs = 0;         // race-relative ms of the last lap crossing
var raceDisplayStartMs = 0;         // Date.now() corresponding to race elapsed 0
var raceReanchorTimer  = null;      // periodic device re-anchor while racing
// Date.now() reference for THIS device's own race start, published by
// startRaceDisplayOnly() and cleared by stopRaceDisplayOnly().  0 = our own
// timer is not running (e.g. a pilot ignoring the race director).
var rvOwnRaceStartMs = 0;
const timer = document.getElementById("timer");
const lapCounter = document.getElementById("lapCounter");
const startRaceButton = document.getElementById("startRaceButton");
const stopRaceButton = document.getElementById("stopRaceButton");
const addLapButton = document.getElementById("addLapButton");

const batteryVoltageDisplay = document.getElementById("bvolt");

const rssiBuffer = [];
var rssiValue = 0;
var rssiSending = false;

let lastKeepaliveMs = 0;
let keepaliveWatchdogTimer = null;
const KEEPALIVE_TIMEOUT_MS = 12000; // flag stale connection after 2+ missed keepalives (server sends every 5s)
var rssiChart;
var crossing = false;
var rssiSeries = null;          // initialised inside createRssiChart() after smoothie.js loads
var rssiCrossingSeries = null;
var rssiLapMarkerSeries = null; // brief full-height spike at each lap detection
var maxRssiValue = enterRssi + 10;
var minRssiValue = exitRssi - 10;

var audioEnabled = false;
var speakObjsQueue = [];
var lapFormat = 'pilottime'; // 'full', 'pilottime', 'laptime', 'timeonly'
// Decimal places for lap times — SPOKEN AND DISPLAYED.  1 = x.1s, 2 = x.01s
// (default, matches the historical hard-coded toFixed(2)), 3 = x.001s for
// millisecond work, which makes bench timing runs readable and audible.
//
// Everything that renders a lap time routes through lapFracStr() below, so the
// table, statistics, race history, multi-node leaderboard and the announcer all
// agree.  A displayed time that disagreed with the spoken one would be worse
// than either — pilots cross-check the two.
var announcerDecimals = 2;

function lapDecimals() {
  return (announcerDecimals >= 1 && announcerDecimals <= 3) ? announcerDecimals : 2;
}

// Sub-second remainder of a lap time, zero-padded to the configured precision.
// 1234 ms -> "2" (tenths) / "23" (hundredths) / "234" (thousandths).
// Padding matters: 45 ms at 3 dp must render "045", not "45".
function lapFracStr(ms) {
  const d = lapDecimals();
  return Math.floor((ms % 1000) / Math.pow(10, 3 - d)).toString().padStart(d, '0');
}

// Format a lap time in ms for the announcer at the configured precision.
//
// Built from lapFracStr() rather than toFixed() so speech TRUNCATES exactly as
// the display does.  toFixed() rounds, which made 1999 ms show as "1.99" but
// speak as "2.00" — the same lap reported two different ways. Truncation is
// also the right convention for race timing: a lap is never rounded up.
function formatLapForSpeech(ms) {
  if (!ms || ms <= 0) return `0.${'0'.repeat(lapDecimals())}`;
  return `${Math.floor(ms / 1000)}.${lapFracStr(ms)}`;
}
var selectedVoice = 'default';

// Initialize hybrid audio announcer
const audioAnnouncer = new AudioAnnouncer();

// Transport initialization functions
async function initializeTransport() {
  // Check if we have USB transport available (Electron or Web Serial API)
  const hasUSB = (typeof window.electronAPI !== 'undefined') || ('serial' in navigator);
  console.log('[Init] USB available:', hasUSB, 'Mode:', currentConnectionMode);
  console.log('[Init] electronAPI:', typeof window.electronAPI);
  
  // run this to force the banner on the Race tab if no SD Card exists.
  loadRaceHistory();

  if (!hasUSB || currentConnectionMode === 'wifi') {
    // WiFi-only mode — delay SSE so HTML/CSS/JS finish loading first before
    // the persistent SSE connection opens (avoids hitting ESP32 TCP conn limit).
    console.log('[Init] Initializing WiFi-only mode');
    setTimeout(() => { setupWiFiEvents(); updateConnectionStatus('WiFi', true); }, 400);
    return;
  }
  
  // Try USB first in auto/usb mode — lazy-load the USB transport library now
  // (not needed for WiFi-only browsers, saving one script fetch on every page load)
  if (currentConnectionMode === 'auto' || currentConnectionMode === 'usb') {
    try { await loadScript('usb-transport.js'); } catch (_) {
      console.warn('[Init] usb-transport.js failed to load, falling back to WiFi');
      setTimeout(() => { setupWiFiEvents(); updateConnectionStatus('WiFi', true); }, 400);
      return;
    }
    try {
      console.log('[Init] Creating USBTransport...');
      transportManager = new USBTransport();
      
      // List available ports and auto-connect in auto mode
      console.log('[Init] Listing ports...');
      const ports = await transportManager.listPorts();
      console.log('[Init] Found ports:', ports);
      
      if (ports.length > 0) {
        // Populate COM port dropdown
        const comPortSelect = document.getElementById('comPort');
        comPortSelect.innerHTML = '<option value="">Select a port...</option>';
        ports.forEach(port => {
          console.log('[Init] Adding port:', port.path, port.manufacturer);
          const option = document.createElement('option');
          option.value = port.path;
          option.textContent = `${port.path}${port.manufacturer ? ' - ' + port.manufacturer : ''}`;
          comPortSelect.appendChild(option);
        });
        
        // Auto-connect to first device in auto mode
        if (currentConnectionMode === 'auto') {
          // Try to find by manufacturer first
          let fpvraceonePort = ports.find(p =>
            p.manufacturer && (p.manufacturer.includes('Espressif') || p.manufacturer.includes('Silicon Labs'))
          );

          // If not found, look for COM12 specifically (common port on Windows)
          if (!fpvraceonePort) {
            fpvraceonePort = ports.find(p => p.path === 'COM12');
          }

          console.log('[Init] FPVRaceOne port found:', fpvraceonePort);
          if (fpvraceonePort) {
            await connectUSB(fpvraceonePort.path);
            return;
          }
        }
      } else {
        console.log('[Init] No ports found');
      }
    } catch (err) {
      console.error('[Init] USB initialization failed:', err);
    }
  }
  
  // Fall back to WiFi if USB failed and in auto mode
  if (currentConnectionMode === 'auto' && !usbConnected) {
    console.log('USB not available, falling back to WiFi');
    setTimeout(() => { setupWiFiEvents(); updateConnectionStatus('WiFi', true); }, 400);
  } else if (currentConnectionMode === 'usb' && !usbConnected) {
    updateConnectionStatus('USB', false);
  }
}

async function connectUSB(portPath) {
  try {
    await transportManager.connect(portPath);
    usbConnected = true;
    setupUSBEvents();
    updateConnectionStatus('USB', true);
    
    // Update COM port dropdown to show selected port
    const comPortSelect = document.getElementById('comPort');
    comPortSelect.value = portPath;
    
    console.log('Connected to USB:', portPath);
  } catch (err) {
    console.error('Failed to connect USB:', err);
    usbConnected = false;
    updateConnectionStatus('USB', false);
    throw err;
  }
}

function showDisconnectedBanner(message) {
  // During the recruit job the master's AP intentionally drops for ~60s while
  // it scans + configures peers.  Don't pop the generic "WiFi disconnected"
  // banner during that window — recruitNearbyUnits() is already showing a
  // dedicated overlay that explains what's happening.
  if (window.__recruitInProgress) return;
  const banner = document.getElementById('disconnectedBanner');
  const text = document.getElementById('disconnectedBannerText');
  if (!banner) return;
  if (text) text.textContent = message || 'WiFi disconnected — reconnecting...';
  banner.style.display = 'block';
}

function hideDisconnectedBanner() {
  const banner = document.getElementById('disconnectedBanner');
  if (banner) banner.style.display = 'none';
}

// Watchdog: if the SSE connection appears open but no keepalive arrives within
// KEEPALIVE_TIMEOUT_MS (12s ≈ 2+ missed 5s server pings), the connection is stale
// (TCP alive but server silent). Force a reconnect.
function startKeepaliveWatchdog() {
  lastKeepaliveMs = Date.now();
  if (keepaliveWatchdogTimer) clearInterval(keepaliveWatchdogTimer);
  keepaliveWatchdogTimer = setInterval(() => {
    if (!eventSource || eventSource.readyState !== EventSource.OPEN) return;
    if (Date.now() - lastKeepaliveMs > KEEPALIVE_TIMEOUT_MS) {
      console.warn(`[Keepalive] No keepalive in ${KEEPALIVE_TIMEOUT_MS / 1000}s — connection is stale, forcing reconnect`);
      showDisconnectedBanner(mnNodeMode === 2 ? 'Connection to master node stalled — reconnecting...' : 'Connection stalled — reconnecting...');
      setupWiFiEvents();
    }
  }, 10000);
}

// Called every time the SSE connection (re)opens. Restores any server-side state
// that was active before the connection dropped.
async function onWiFiReconnect() {
  hideDisconnectedBanner();

  // If the user had the calibration tab open and RSSI streaming was active,
  // the firmware resets sendRssi=false on each new SSE connection. Re-request it.
  if (rssiSending) {
    rssiSending = false;
    try {
      const resp = await fetch('/timer/rssiStart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' }
      });
      if (resp.ok) {
        rssiSending = true;
        console.log('[Reconnect] RSSI streaming re-started');
      }
    } catch (err) {
      console.error('[Reconnect] Failed to restart RSSI streaming:', err);
    }
  }

  // On initial page load the DOMContentLoaded IIFE already ran the full init. The
  // SSE onopen fires shortly after and would duplicate every fetch. Skip it: polling
  // keeps node/lap state fresh and the clock is already running.
  if (_pageInitDone) return;

  // Re-fetch mode and race state to restore everything after a page reload or reconnect.
  try {
    const r = await fetchWithRetry('/api/mode', {}, 3, 1500);
    const data = await r.json();
    const newMode     = data.nodeMode    || 0;
    const modeChanged = newMode !== mnNodeMode;
    mnNodeMode        = newMode;
    if (typeof updateApplyMultiNodeButtonState === 'function') updateApplyMultiNodeButtonState();
    mnMyNodeId        = data.myNodeId        || 0;
    mnMasterConnected = data.masterConnected  || false;
    mnMasterRaceActive = data.masterRaceActive || false;
    if (data.nodeMode !== 2) mnStatusSSID = data.ssid || '';
    if (data.ssid) mnMyOwnSSID = data.ssid;
    // Both inputs to the Add Lap rules (node mode, master-race state) were just
    // refreshed from the device, so re-apply.  This poll is also what recovers
    // the correct state after a page reload mid-race.
    applyAddLapButtonUI();

    const timerRunning  = data.timerRunning  || false;
    const raceElapsedMs = data.raceElapsedMs || 0;

    // Restore master race clock
    if (newMode === 1 && timerRunning) {
      mnRaceRunning = true;
      if (!mnRaceTimerIntervalId) _mnStartTimer(raceElapsedMs);
    }

    // Restore single / client timer display
    if (timerRunning && (newMode === 0 || newMode === 2)) {
      startRaceDisplayOnly(raceElapsedMs);
    }

    // Always restore lap data regardless of run state — laps persist after race stop.
    if (newMode === 1) {
      try { await mnRefreshNodes(); } catch (_) {}
    } else {
      try {
        const lr = await fetchWithRetry('/api/laps/current', {}, 3, 1500);
        const ld = await lr.json();
        if (ld.laps && ld.laps.length > 0) {
          _restoreInProgressLaps(ld.laps);
          // Record where the last crossing was.  Safe to run before OR after
          // startRaceDisplayOnly() — both derive the lap clock from the same
          // two values rather than stamping it, so the SSE onConnect replay
          // can no longer undo this.
          setLapTimerFromLastLap(ld.laps);
        }
      } catch (_) {}
    }

    // Start mode-specific UI and polling.
    if (modeChanged || newMode === 1) {
      onRaceTabOpen();
      if (newMode === 1) {
        mnStatusSSID = data.ssid || '';
        if (!mnPollingInterval) mnStartPolling();  // one fetch, not four
      }
      if (newMode === 2) {
        // Fetch master SSID from config (only on first connect or mode change)
        if (modeChanged) {
          try {
            const r2 = await fetch('/config');
            if (r2.ok) { const cfg = await r2.json(); mnStatusSSID = cfg.masterSSID || ''; }
          } catch (_) {}
        }
        mnStartClientPoll();
      }
    }

    mnUpdateRaceStatusBar();
    hideSplash();
  } catch (err) {
    console.warn('[Reconnect] /api/mode fetch failed:', err);
    hideSplash();
  }
}

function setupWiFiEvents() {
  // Clear any pending reconnect timer
  if (eventSourceReconnectTimer) {
    clearTimeout(eventSourceReconnectTimer);
    eventSourceReconnectTimer = null;
  }

  // Tear down the previous connection's periodic timers up front. setupWiFiEvents()
  // is re-entered from several paths (reconnect timer, keepalive watchdog,
  // changeConnectionMode, USB fallback). If two calls land before the new SSE 'open'
  // fires, the old watchdog + status interval keep running and a second EventSource
  // can briefly coexist, each re-issuing /timer/rssiStart — compounding TCP-slot
  // pressure on the C6's 16-slot LwIP limit. Clearing them here makes re-entry safe.
  if (keepaliveWatchdogTimer) {
    clearInterval(keepaliveWatchdogTimer);
    keepaliveWatchdogTimer = null;
  }
  if (connectionStatusUpdateInterval) {
    clearInterval(connectionStatusUpdateInterval);
    connectionStatusUpdateInterval = null;
  }

  if (eventSource) {
    eventSource.close();
  }

  if (!window.EventSource) return;

  lastKeepaliveMs = Date.now(); // reset watchdog baseline for this new connection attempt
  console.log('[FPV] sse attempt=' + eventSourceReconnectAttempts);
  document.getElementById('splash-status') && (document.getElementById('splash-status').textContent = 'Connecting\u2026');
  eventSource = new EventSource("/events");

  eventSource.addEventListener("open", function () {
    console.log("WiFi Events Connected");
    eventSourceReconnectAttempts = 0;
    updateConnectionStatus('WiFi', true);
    startKeepaliveWatchdog();
    onWiFiReconnect();

    if (connectionStatusUpdateInterval) clearInterval(connectionStatusUpdateInterval);
    connectionStatusUpdateInterval = setInterval(() => {
      updateConnectionStatus('WiFi', true);
    }, 5000);
  }, false);

  eventSource.addEventListener("error", function (e) {
    if (e.target.readyState !== EventSource.OPEN) {
      console.log("WiFi Events Disconnected - attempting reconnect...");
      updateConnectionStatus('WiFi', false);

      if (eventSourceReconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        eventSourceReconnectAttempts++;
        const msg = `WiFi disconnected — reconnecting (${eventSourceReconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})...`;
        console.log(msg);
        showDisconnectedBanner(msg);

        const delay = eventSourceReconnectAttempts === 1 ? RECONNECT_DELAY_FIRST_MS : RECONNECT_DELAY_MS;
        eventSourceReconnectTimer = setTimeout(() => {
          console.log('Attempting EventSource reconnect...');
          setupWiFiEvents();
        }, delay);
      } else {
        showDisconnectedBanner('Connection lost. Please refresh the page.');
        console.error('Max reconnect attempts reached.');
      }
    }
  }, false);

  // Server sends a keepalive ping every 5s (WEB_SSE_KEEPALIVE_MS). Track it so the
  // watchdog can detect a stale-but-open TCP connection before the browser notices.
  eventSource.addEventListener("keepalive", function () {
    lastKeepaliveMs = Date.now();
  }, false);

  eventSource.addEventListener("rssi", function (e) {
    rssiBuffer.push(e.data);
    if (rssiBuffer.length > 10) rssiBuffer.shift();
  }, false);

  eventSource.addEventListener("lap", function (e) {
    // Format: "lapTimeMs,peakRssi"  (peakRssi may be absent on older firmware)
    const parts   = e.data.split(',');
    const lapMs   = parseFloat(parts[0]);
    const peakRssi = parts.length > 1 ? parseInt(parts[1], 10) : 0;
    var lap = formatLapForSpeech(lapMs);
    addLap(lap);
    console.log("lap:", lap + "s", "peakRssi:", peakRssi);
    // Paint a brief marker spike on the live RSSI chart at the exact peak RSSI
    // value confirmed by the firmware. The spike height reflects the actual
    // detected peak — useful for calibrating the enter/exit thresholds.
    if (rssiLapMarkerSeries && peakRssi > 0) {
      const t = Date.now();
      rssiLapMarkerSeries.append(t - 50,  0);        // sharp leading edge
      rssiLapMarkerSeries.append(t,        peakRssi); // true peak height
      rssiLapMarkerSeries.append(t + 500,  peakRssi); // hold briefly so it's readable
      rssiLapMarkerSeries.append(t + 600,  0);        // drop back to baseline
    }
  }, false);

  // Multi-tab sync (single/standalone mode): server broadcasts
  // prearming / started / stopped / cleared over the raceState SSE channel so
  // every browser viewing this unit mirrors the race lifecycle.  The tab that
  // pressed Start owns the announcer (_iAmRaceInitiator); spectator tabs
  // suppress TTS in addLap() but still update the display.  Also fires when
  // the server auto-stops a race (maxLaps reached) so the local button state
  // resyncs even for the initiator.
  eventSource.addEventListener("raceState", function (e) {
    // Multi-tab sync is a SINGLE/STANDALONE-mode feature only.  In master
    // and client modes, coordinated races use their own event channels
    // (masterRaceState, directorState, multiNodeState) — applying the
    // spectator-sync logic here would spuriously start the Race tab's
    // display timer during coord races and, worse, flip _iAmRaceInitiator
    // to false, which would silence the client pilot's own lap
    // announcements.  In those modes fall back to the original behavior:
    // just update button-disabled state, don't touch the display or the
    // announcer-owner flag.
    if (mnNodeMode !== 0) {
      if (e.data === "started" || e.data.indexOf("started:") === 0) {
        startRaceButton.disabled = true;
        stopRaceButton.disabled = false;
        addLapButton.disabled = false;
        // Re-apply the gate straight after enabling.  This branch serves BOTH
        // master and client and then returns, so without this a client racing
        // under a director was left with a live-looking Add Lap button —
        // addManualLap() would refuse it, but the UI would not say so.
        applyAddLapButtonUI();
        // Master: this is where the host's race clock and the GO cue belong.
        // The fleet start is scheduled ahead so every unit begins on one
        // instant (§8), and the firmware emits this event when that instant
        // arrives — so the host lands with the clients instead of starting
        // when Start All was pressed.
        //
        // It has to live INSIDE this early-return block: master and client
        // modes never reach the multi-tab logic below.
        if (mnNodeMode === 1 && !mnRaceTimerIntervalId) {
          let _off = 0;
          const _c = e.data.indexOf(':');
          if (_c > 0) {
            const _p = parseInt(e.data.substring(_c + 1), 10);
            if (!isNaN(_p) && _p >= 0) _off = _p;
          }
          _mnStartTimer(_off);
          beep(1, 1, "square");
          beep(500, 880, "square");
          if (navigator.vibrate) navigator.vibrate(500);
        }
      } else if (e.data === "stopped") {
        stopRaceButton.disabled = true;
        startRaceButton.disabled = false;
        addLapButton.disabled = true;
      }
      return;
    }
    // A "local origin" event is one that echoes our own recent action —
    // detected by whether startRace() ran in this tab within the last 30 s.
    // Anything else is another tab's action (or the onConnect replay of a
    // race that was already running before we joined) → spectator mode.
    const isLocalOrigin = (_localRaceStartTs > 0) && (Date.now() - _localRaceStartTs < 30000);
    if (e.data === "prearming") {
      if (!isLocalOrigin) {
        _iAmRaceInitiator = false;
        if (startRaceButton) startRaceButton.classList.add('active');
        clearInterval(timerInterval);
        if (timer) timer.innerHTML = '00:00:00s';
        const hdr = lapTable ? lapTable.rows.length : 0;
        for (let i = 1; i < hdr; i++) lapTable.deleteRow(1);
        lapNo = -1; lapTimes = []; excludedLaps.clear();
      lastCrossingRaceMs = 0;   // clean slate: current lap begins at race zero
        updateLapCounter();
      }
    } else if (e.data === "started" || e.data.indexOf("started:") === 0) {
      // The onConnect replay for a mid-race refresh sends "started:<elapsedMs>"
      // so the new tab's display timer picks up at the correct offset instead
      // of restarting from zero.  Normal /timer/start broadcasts send just
      // "started" (offset = 0, race is starting right now).
      let offsetMs = 0;
      const colon = e.data.indexOf(':');
      if (colon > 0) {
        const parsed = parseInt(e.data.substring(colon + 1), 10);
        if (!isNaN(parsed) && parsed >= 0) offsetMs = parsed;
      }
      startRaceButton.disabled = true;
      stopRaceButton.disabled = false;
      addLapButton.disabled = false;
      applyAddLapButtonUI();
      // Self-heal: if we didn't originate this locally (missed prearming,
      // late-join replay, cross-tab), enter spectator mode now.
      if (!isLocalOrigin) {
        _iAmRaceInitiator = false;
      }
      if (!_iAmRaceInitiator) {
        if (startRaceButton) startRaceButton.classList.remove('active');
        startRaceDisplayOnly(offsetMs);
      }
    } else if (e.data === "stopped") {
      stopRaceButton.disabled = true;
      startRaceButton.disabled = false;
      addLapButton.disabled = true;
      if (!_iAmRaceInitiator) {
        if (startRaceButton) startRaceButton.classList.remove('active');
        stopRaceDisplayOnly();
      }
      // Race ended — default back to announcer role for the next race, and
      // clear the local-start timestamp so origin detection for the next
      // race is based on a fresh startRace() call, not this stale one.
      _iAmRaceInitiator = true;
      _localRaceStartTs = 0;
    } else if (e.data === "cleared") {
      // Another tab cleared laps — mirror the table wipe locally.
      const hdr = lapTable ? lapTable.rows.length : 0;
      for (let i = 1; i < hdr; i++) lapTable.deleteRow(1);
      lapNo = -1; lapTimes = []; excludedLaps.clear();
      lastCrossingRaceMs = 0;   // clean slate: current lap begins at race zero
      updateLapCounter();
      if (typeof updateAnalysisView === 'function') updateAnalysisView();
    }
  }, false);

  // OTA update progress — mirrors the OtaManager state machine onto the UI.
  eventSource.addEventListener("updateProgress", function (e) {
    try {
      const data = JSON.parse(e.data);
      handleUpdateProgress(data);
    } catch (err) {
      console.warn('[OTA] Bad updateProgress payload:', e.data, err);
    }
  }, false);

  // Master pushed updated node list (running state or DNF changed)
  eventSource.addEventListener("multiNodeState", function (e) {
    try {
      const data = JSON.parse(e.data);
      if (Array.isArray(data.nodes)) {
        // MUST go through rvMergeNodes, exactly like the client Race View.
        // Once the §10 capability gate opens the master stops sending per-node
        // `laps` arrays, so mnRenderRaceTab's `found.laps` is undefined and
        // every client row renders 0 laps.  This path used to read the raw
        // nodes and only worked because the legacy arrays were still there.
        const merged = rvMergeNodes(data.nodes,
                                    Array.isArray(data.lapDeltas) ? data.lapDeltas : [],
                                    (data.race || {}).raceId);
        mnRenderNodes(merged);
        mnRenderRaceTab(merged);
      }
    } catch (_) {}
  }, false);

  // Master receives a lap from a client node — refresh race tab and announce
  eventSource.addEventListener("multiNodeLap", function (e) {
    try {
      const data     = JSON.parse(e.data);
      const node     = mnCurrentNodes.find(n => n.nodeId === data.node);
      const callsign = node ? (node.pilotName || ('Node ' + data.node)) : ('Node ' + data.node);
      // Refresh immediately so the new lap appears without waiting for the next poll
      mnRefreshNodes();
      // Announce the lap using the existing announcer
      if (typeof queueSpeak === 'function') {
        if (data.lap === 0) {
          queueSpeak(`<p>${callsign} first crossing</p>`);
        } else {
          const timeStr = formatMsSpeak(data.ms);
          let text;
          switch (lapFormat) {
            case 'pilottime': text = `<p>${callsign} ${timeStr}</p>`; break;
            case 'timeonly':  text = `<p>${timeStr}</p>`;             break;
            default:          text = `<p>${callsign} Lap ${data.lap}, ${timeStr}</p>`;
          }
          queueSpeak(text);
        }
      }
    } catch (_) {}
  }, false);

  // Master pushed full director state (nodes + race) to this client node for the Race View tab
  eventSource.addEventListener("directorState", function (e) {
    try {
      const data = JSON.parse(e.data);
      rvHandleDirectorState(data);
    } catch (_) {}
  }, false);

  // Master pushed race start/stop to this client node
  eventSource.addEventListener("masterRaceState", function (e) {
    if (e.data === "prearming") {
      if (mnClientSkipEnabled) return;
      const btn = document.getElementById('startRaceButton');
      if (btn) btn.classList.add('active');
      // Reset the display immediately so pilot sees a clean slate during countdown.
      // Firmware clears the actual lap data when masterStart fires.
      clearInterval(timerInterval);
      if (timer) timer.innerHTML = '00:00:00s';
      const hdr = lapTable ? lapTable.rows.length : 0;
      for (let i = 1; i < hdr; i++) lapTable.deleteRow(1);
      lapNo = -1; lapTimes = []; excludedLaps.clear();
      lastCrossingRaceMs = 0;   // clean slate: current lap begins at race zero
      updateLapCounter();
    } else if (e.data === "started") {
      if (mnClientSkipEnabled) return;
      mnMasterRaceActive = true;
      const btn = document.getElementById('startRaceButton');
      if (btn) btn.classList.remove('active');
      startRaceDisplayOnly();
    } else if (e.data === "stopped") {
      if (mnClientSkipEnabled) return;
      mnMasterRaceActive = false;
      const btn = document.getElementById('startRaceButton');
      if (btn) btn.classList.remove('active');
      stopRaceDisplayOnly();
      // The director's race ended, so the lockout no longer applies.  This does
      // not re-enable the button — stopRaceDisplayOnly() has just disabled it
      // because no race is running; it only refreshes visibility.
      applyAddLapButtonUI();
    }
  }, false);

  // Master updated this client's pilot name/color — reflect in UI immediately
  eventSource.addEventListener("pilotInfoChanged", function (e) {
    try {
      const d = JSON.parse(e.data);
      if (d.name !== undefined && pilotNameInput) {
        pilotNameInput.value = d.name;
        const pilotNameDisplay = document.getElementById('pilotNameDisplay');
        if (pilotNameDisplay) pilotNameDisplay.textContent = d.name;
      }
      if (d.pilotColor !== undefined) {
        const colorInput = document.getElementById('pilotColor');
        if (colorInput) {
          colorInput.value = '#' + ('000000' + d.pilotColor.toString(16)).slice(-6).toUpperCase();
          updateColorPreview();
        }
      }
    } catch (_) {}
  }, false);

  // Client node's own connection state changed — update status bar immediately
  eventSource.addEventListener("multiNodeClientState", function (e) {
    try {
      const d = JSON.parse(e.data);
      mnMasterConnected = !!d.connected;
      mnMyNodeId        = d.nodeId || 0;
      mnUpdateRaceStatusBar();
      rvUpdateBanner();
    } catch (_) {}
  }, false);
}

function setupUSBEvents() {
  if (!transportManager) return;
  
  transportManager.on('rssi', (data) => {
    rssiBuffer.push(data);
    if (rssiBuffer.length > 10) rssiBuffer.shift();
  });

  transportManager.on('lap', (data) => {
    var lap = formatLapForSpeech(parseFloat(data));
    addLap(lap);
    console.log("USB lap:", lap + "s");
  });
  
  transportManager.on('disconnect', () => {
    console.log('USB disconnected');
    usbConnected = false;
    updateConnectionStatus('USB', false);
    
    // Auto-fallback to WiFi if in auto mode
    if (currentConnectionMode === 'auto') {
      setupWiFiEvents();
      updateConnectionStatus('WiFi', true);
    }
  });
}

/* WiFi signal strength indicator disabled — use OS WiFi indicator instead
function updateWifiSignalBars(rssi, isAP, rttMs, connected) {
  const icon = document.getElementById('wifiIcon');
  const dot  = document.getElementById('wifiDot');
  const b1   = document.getElementById('wifiBar1');
  const b2   = document.getElementById('wifiBar2');
  const b3   = document.getElementById('wifiBar3');
  if (!icon || !dot || !b1 || !b2 || !b3) return;

  const LIT = '#ffffff';
  const DIM = 'rgba(255,255,255,0.4)';

  let litBars = 0;
  if (!connected) {
    litBars = 0;
  } else if (isAP && rttMs != null) {
    if      (rttMs < 30)  litBars = 3;
    else if (rttMs < 80)  litBars = 2;
    else if (rttMs < 200) litBars = 1;
    else                  litBars = 0;
  } else if (rssi != null) {
    if      (rssi >= -50) litBars = 3;
    else if (rssi >= -60) litBars = 2;
    else if (rssi >= -70) litBars = 1;
    else                  litBars = 0;
  } else {
    litBars = 3;
  }

  dot.setAttribute('fill',  connected ? LIT : DIM);
  b1.setAttribute('stroke', litBars >= 1 ? LIT : DIM);
  b2.setAttribute('stroke', litBars >= 2 ? LIT : DIM);
  b3.setAttribute('stroke', litBars >= 3 ? LIT : DIM);

  icon.classList.toggle('disconnected', !connected);
}
*/

async function updateConnectionStatus(mode, connected) {
  const modeEl    = document.querySelector('.connection-mode');
  const detailsEl = document.getElementById('connectionDetails');

  if (modeEl) {
    modeEl.textContent = `${mode}: ${connected ? 'Connected' : 'Disconnected'}`;
  }

  let details = '';

  if (mode === 'WiFi' && connected) {
    try {
      const response = await fetch('/api/wifi');
      if (response.ok) {
        const wifiData = await response.json();
        if (wifiData.mode === 'AP') {
          details = `SSID: ${wifiData.ssid}<br>IP: ${wifiData.ip}<br>Clients: ${wifiData.clients}`;
        } else if (wifiData.mode === 'STA') {
          details = `SSID: ${wifiData.ssid}<br>IP: ${wifiData.ip}`;
        }
      }
    } catch (err) {
      console.error('Failed to fetch WiFi info:', err);
      details = 'Unable to fetch WiFi details';
    }
  } else if (mode === 'USB' && connected) {
    details = 'Direct serial connection';
  } else if (!connected) {
    details = 'Connection lost';
  }

  if (detailsEl) detailsEl.innerHTML = details;

  // updateWifiSignalBars disabled — use OS WiFi indicator instead

  // Update settings panel status (USB only)
  const statusEl = document.getElementById('comPortStatus');
  if (statusEl) {
    statusEl.style.display = 'block';
    statusEl.textContent = `Status: ${connected ? 'Connected' : 'Disconnected'} (${mode})`;
    statusEl.style.color = connected ? 'var(--success-color, #4CAF50)' : 'var(--error-color, #f44336)';
  }
}

async function changeConnectionMode() {
  const modeSelect = document.getElementById('connectionMode');
  currentConnectionMode = modeSelect.value;
  
  const comPortSection = document.getElementById('comPortSection');
  
  // Show COM port selector in USB mode
  if (currentConnectionMode === 'usb') {
    comPortSection.style.display = 'flex';
  } else {
    comPortSection.style.display = 'none';
  }
  
  // Disconnect current connection
  if (eventSource) {
    eventSource.close();
    eventSource = null;
  }
  if (transportManager && usbConnected) {
    await transportManager.disconnect();
    usbConnected = false;
  }
  
  // Reinitialize with new mode
  await initializeTransport();
}

async function selectComPort() {
  const comPortSelect = document.getElementById('comPort');
  const portPath = comPortSelect.value;
  
  if (!portPath) return;
  
  // Disconnect if already connected
  if (transportManager && usbConnected) {
    await transportManager.disconnect();
    usbConnected = false;
  }
  
  // Connect to selected port
  if (!transportManager) {
    transportManager = new USBTransport();
  }
  
  await connectUSB(portPath);
}

async function checkTuningStatusOnStartup() {
  try {
    const r = await fetch('/tuningstatus', { cache: 'no-store' });
    if (!r.ok) {
      console.warn('[Startup] /tuningstatus HTTP', r.status);
      return;
    }

    const data = await r.json();
    console.log('[Startup] tuningstatus:', data.tuningstatus);

    if (data.tuningstatus === 'setting') {
      if (typeof showCalibrationBanner === 'function') {
        showCalibrationBanner();
      } else {
        console.warn('[Startup] showCalibrationBanner() not defined');
      }
    }
  } catch (e) {
    // Fail silently — startup should not break if this endpoint is unavailable
    console.warn('[Startup] tuningstatus check failed:', e);
  }
}


onload = async function (e) {
  // Load dark mode preference
  loadDarkMode();

  loadFirmwareVersion();

  config.style.display = "none";
  // '' not "block" — see the note in openTab(); .tabcontent is a flex column.
  race.style.display = "";
  calib.style.display = "none";

  // Draw the empty lap row on first paint.  Everything else that maintains it
  // hangs off a state change (a lap, a clear, a stop), none of which has
  // happened yet on a cold load.
  updateLapTablePlaceholder();

  attachConfigStagingListeners();

  // Initialize transport (USB/WiFi)
  await initializeTransport(); 

  // IMPORTANT: Load race history immediately so the Race tab banner + History tab label
  // can reflect SD vs RAM-only mode on first landing.
  try {
    // If loadRaceHistory already uses transportFetch internally, this will work in USB + WiFi.
    // If it still uses fetch('/races'), update loadRaceHistory accordingly (we discussed earlier).
    await loadRaceHistory();

    // Some codebases only call applyRaceHistoryModeUI inside renderRaceHistory()
    // so call it here too (harmless if redundant).
    if (typeof applyRaceHistoryModeUI === 'function') {
      applyRaceHistoryModeUI();
    }
  } catch (err) {
    console.error('[Script] Failed to load race history on startup:', err);
  }

  console.log('[Script] Starting Debug Listener...');
  try{
    startDebugListener();
    console.log('[Script] Debug Listener started OK');
  } catch (err) {
      console.error('[Script] Debug Listener failed:', err);
  }

  // Dev mode: click pilot name display on single/client Race view to inject a
  // simulated lap.  This is the OTHER route into addManualLap() — it does not
  // go near the Add Lap button, so hiding or disabling that button never
  // covered it.  addManualLap() carries the real guard against injecting a lap
  // while racing under a director; the check is repeated here so the rule is
  // visible at the call site rather than only at the far end.
  const pilotNameDisplay = document.getElementById('pilotNameDisplay');
  if (pilotNameDisplay) {
    pilotNameDisplay.addEventListener('click', () => {
      if (!mnDevMode) return;
      if (mnNodeMode === 2 && mnMasterRaceActive) return;
      addManualLap();
    });
  }

  // Always-hide banner toggle — default OFF for every new build / page load.
  //
  // The old behavior read the saved value out of localStorage, which persists
  // across firmware reflashes for the same browser+IP combo.  Result: a unit
  // freshly flashed with new firmware kept "always hide" stuck on because the
  // browser remembered an earlier session.  We now wipe the persistent key on
  // every page load so the toggle is always reported OFF at startup.  The
  // session-scoped "Dismiss" button (sessionStorage / hideRaceDownloadReminder)
  // still works for one-session dismissal.
  const alwaysHideBannerToggle = document.getElementById("alwaysHideBannerToggle");
  const alwaysHideBannerLabel  = document.getElementById("alwaysHideBannerLabel");
  if (alwaysHideBannerToggle) {
    localStorage.removeItem("alwaysHideRaceBanner");
    alwaysHideBannerToggle.checked = false;
    if (alwaysHideBannerLabel) alwaysHideBannerLabel.textContent = "Off";
    alwaysHideBannerToggle.addEventListener("change", () => {
      if (alwaysHideBannerLabel) alwaysHideBannerLabel.textContent = alwaysHideBannerToggle.checked ? "On" : "Off";
      // When opting back in to seeing the banner, clear the session-level dismiss too
      if (!alwaysHideBannerToggle.checked) sessionStorage.removeItem("hideRaceDownloadReminder");
    });
  }

  // Fetch config using appropriate transport
  let configData;
  try {
    if (usbConnected && transportManager) {
      configData = await transportManager.sendCommand('config', 'GET');
    } else {
      const response = await fetch("/config");
      configData = await response.json();
    }
    ledConnected = (configData.hasLed !== undefined) ? !!configData.hasLed : false;
    applyRaceHistoryModeUI(); // this will call setLEDSettingsVisible(ledConnected)
  } catch (err) {
    console.error('[Script] Failed to fetch config:', err);
    // Set defaults if config fetch fails
    configData = {};
  }

  // Block autoSaveConfig() from staging stale defaults while we populate the UI
  settingsLoading = true;
  {
    // old reverse freq lookup no longer works due to multiple bands having same channels
    //if (configData.freq !== undefined) setBandChannelIndex(configData.freq);
    if (configData.band !== undefined && configData.chan !== undefined) {
      // Apply band/chan directly (no ambiguity)
      bandSelect.selectedIndex = configData.band;
      updateChannelOptionsForBand(configData.band);

      // channelIndex is 0-based; dropdown value is "1".."8"
      const desiredValue = String((configData.chan | 0) + 1);
      const exists = Array.from(channelSelect.options).some(o => o.value === desiredValue);
      if (exists) channelSelect.value = desiredValue;

      populateFreqOutput();
    } else if (configData.freq !== undefined) {
      // Backward compatible fallback
      setBandChannelIndex(configData.freq);
    }


    if (configData.minLap !== undefined) {
      minLapInput.value = (parseFloat(configData.minLap) / 10).toFixed(1);
      updateMinLap(minLapInput, minLapInput.value);
    }

    if (configData.alarm !== undefined) {
      alarmThreshold.value = (parseFloat(configData.alarm) / 10).toFixed(1);
      updateAlarmThreshold(alarmThreshold, alarmThreshold.value);
    }

    if (configData.anType !== undefined) announcerSelect.selectedIndex = configData.anType;

    if (configData.anRate !== undefined) {
      announcerRateInput.value = (parseFloat(configData.anRate) / 10).toFixed(1);
      updateAnnouncerRate(announcerRateInput, announcerRateInput.value);
    }

    if (configData.enterRssi !== undefined && enterRssiInput) {
      enterRssiInput.value = configData.enterRssi;
      updateEnterRssi(enterRssiInput, enterRssiInput.value);
    }

    if (configData.exitRssi !== undefined && exitRssiInput) {
      exitRssiInput.value = configData.exitRssi;
      updateExitRssi(exitRssiInput, exitRssiInput.value);
    }

    // Seed the RSSI saved-baseline.  updateEnterRssi/updateExitRssi above
    // ran updateRssiSaveButton() against a still-null baseline, so the
    // button looked clean — but only because the comparison short-circuits.
    // Calling _markRssiSaved here pins the freshly-loaded values as the
    // baseline so subsequent slider moves correctly flip the button dirty.
    _markRssiSaved(
      configData.enterRssi !== undefined ? parseInt(configData.enterRssi) : enterRssi,
      configData.exitRssi  !== undefined ? parseInt(configData.exitRssi)  : exitRssi
    );

    if (configData.name !== undefined && pilotNameInput) pilotNameInput.value = configData.name;
    if (configData.ssid !== undefined && ssidInput) ssidInput.value = configData.ssid;
    if (configData.pwd !== undefined && pwdInput) pwdInput.value = configData.pwd;

    maxLapsInput.value = (configData.maxLaps !== undefined) ? configData.maxLaps : 0;
    updateMaxLaps(maxLapsInput, maxLapsInput.value);

    // Load pilot color from device config
    const colorInput = document.getElementById('pilotColor');

    if (configData.name !== undefined) {
      const pilotNameDisplay = document.getElementById('pilotNameDisplay');
      if (pilotNameDisplay) pilotNameDisplay.textContent = configData.name || '';
    }
    if (colorInput && configData.pilotColor !== undefined) {
      const hexColor = '#' + ('000000' + configData.pilotColor.toString(16)).slice(-6).toUpperCase();
      colorInput.value = hexColor;
      updateColorPreview();
    }

    updateChannelOptionsForBand();
    populateFreqOutput();

    // Only reset timer and laps when race is not already running AND no laps have been
    // restored yet. clearLaps() skips row[0] (treating it as a header), so if
    // _restoreInProgressLaps already populated the table it would silently delete all but
    // the first (Gate 1) row. Guard with lapTimes.length === 0 to avoid this.
    if (stopRaceButton.disabled && lapTimes.length === 0) {
      stopRaceButton.disabled = true;
      startRaceButton.disabled = false;
      addLapButton.disabled = true;
      clearInterval(timerInterval);
      timer.innerHTML = "00:00:00s";
      clearLaps();
    }

    // Apply voice enabled state from device config (DO NOT auto-save here)
    audioEnabled = !!Number(configData.voiceEnabled);

    if (audioAnnouncer) {
      if (audioEnabled) {
        audioAnnouncer.enable();
      } else {
        audioAnnouncer.disable();
      }
    }
    updateVoiceButtons();


    // Setup pilot color preview
    const colorSelect = document.getElementById('pilotColor');
    if (colorSelect) {
      colorSelect.addEventListener('change', updateColorPreview);
      updateColorPreview();
    }

    // Load lap format and voice selection from device config
    lapFormat = configData.lapFormat || 'full';
    if (configData.anDecimals !== undefined) {
      announcerDecimals = parseInt(configData.anDecimals, 10) || 2;
    }
    selectedVoice = configData.selectedVoice || 'default';

    const lapFormatSelect = document.getElementById('lapFormatSelect');
    const voiceSelect = document.getElementById('voiceSelect');
    if (lapFormatSelect) lapFormatSelect.value = lapFormat;
    if (voiceSelect) voiceSelect.value = selectedVoice;

    // Load and apply theme from device config.  resolveTheme() maps a legacy
    // slug (every unit shipped before the reskin has "oceanic" in NVS) onto a
    // palette that exists — without it the page renders with no [data-theme]
    // match at all.  The PICKER is set to the resolved value too, so opening
    // Settings shows what is actually on screen rather than a stale name.
    if (configData.theme) {
      const savedTheme = resolveTheme(configData.theme);
      document.documentElement.setAttribute('data-theme', savedTheme);
      // Refresh the first-paint mirror the <head> script reads.  This is the
      // device's authoritative value, already alias-resolved, so the next load
      // paints the right theme immediately — even if /config cannot be reached
      // (e.g. the browser reloads while the device is rebooting after a flash).
      try { localStorage.setItem('theme', savedTheme); } catch (e) { /* storage off */ }
      const themeSelect = document.getElementById('themeSelect');
      if (themeSelect) themeSelect.value = savedTheme;
    }

    // Load LED settings from config (if available)
    const ledPresetSelect = document.getElementById('ledPreset');
    const ledBrightnessInput = document.getElementById('ledBrightness');
    const ledColorInput = document.getElementById('ledColor');
    const ledManualOverrideToggle = document.getElementById('ledManualOverride');
    const customColorSection = document.getElementById('customColorSection');

    // Load ledPreset from backend config
    if (configData.ledPreset !== undefined && ledPresetSelect) {
      ledPresetSelect.value = configData.ledPreset;
    }

    if (configData.ledBrightness !== undefined && ledBrightnessInput) {
      ledBrightnessInput.value = configData.ledBrightness;
      //updateLedBrightness(ledBrightnessInput, configData.ledBrightness);
    }

    if (configData.ledColor !== undefined && ledColorInput) {
      // Convert color integer to hex string
      const hexColor = '#' + ('000000' + configData.ledColor.toString(16)).slice(-6).toUpperCase();
      ledColorInput.value = hexColor;
    }

    // Initialize LED preset UI on page load (UI only, no command sent)
    if (ledPresetSelect) {
      updateLedPresetUI();
    }

    // Load Gate LED settings from config
    const gateLEDsEnabledToggle = document.getElementById('gateLEDsEnabled');
    const webhookRaceStartToggle = document.getElementById('webhookRaceStart');
    const webhookRaceStopToggle = document.getElementById('webhookRaceStop');
    const webhookLapToggle = document.getElementById('webhookLap');
    const gateLEDOptions = document.getElementById('gateLEDOptions');

    if (gateLEDsEnabledToggle && configData.gateLEDsEnabled !== undefined) {
      gateLEDsEnabledToggle.checked = configData.gateLEDsEnabled === 1;
      if (gateLEDOptions) {
        gateLEDOptions.style.display = configData.gateLEDsEnabled === 1 ? 'block' : 'none';
      }
    }

    if (webhookRaceStartToggle && configData.webhookRaceStart !== undefined) {
      webhookRaceStartToggle.checked = configData.webhookRaceStart === 1;
    }

    if (webhookRaceStopToggle && configData.webhookRaceStop !== undefined) {
      webhookRaceStopToggle.checked = configData.webhookRaceStop === 1;
    }

    if (webhookLapToggle && configData.webhookLap !== undefined) {
      webhookLapToggle.checked = configData.webhookLap === 1;
    }

    // Battery monitoring capability (hardware dependent)
    const batterySection = document.getElementById('batteryMonitoringSection');
    const batteryToggle = document.getElementById('batteryMonitorToggle');
    const batteryNote = document.getElementById('batteryMonitoringUnavailableNote');

    // Default to true for older firmware that doesn't provide hasVbat yet
    const hasVbat = (configData.hasVbat !== undefined) ? !!configData.hasVbat : true;

    if (!hasVbat) {
      if (batterySection) batterySection.style.display = 'none';
      if (batteryNote) batteryNote.style.display = 'block';
      if (batteryToggle) {
        batteryToggle.checked = false;
        batteryToggle.disabled = true;
      }
    } else {
      if (batteryNote) batteryNote.style.display = 'none';
      if (batteryToggle) batteryToggle.disabled = false;
      if (batterySection && batteryToggle) {
        batterySection.style.display = batteryToggle.checked ? 'block' : 'none';
      }
    }

    // Load RSSI sensitivity setting
    const rssiSensitivitySelect = document.getElementById('rssiSensitivity');
    if (rssiSensitivitySelect && configData.rssiSens !== undefined) {
      rssiSensitivitySelect.value = configData.rssiSens;
    }

    // Populate nodeModeSelect at startup so autoSaveConfig() never stages the wrong nodeMode.
    // The settings modal also sets this, but the startup config fetch fires first.
    const nodeModeSelectEarly = document.getElementById('nodeModeSelect');
    if (nodeModeSelectEarly && configData.nodeMode !== undefined) {
      nodeModeSelectEarly.value = String(configData.nodeMode);
    }
    const masterSSIDEarly = document.getElementById('masterSSIDInput');
    if (masterSSIDEarly && configData.masterSSID !== undefined) {
      masterSSIDEarly.value = configData.masterSSID;
    }
    if (configData.mnSkipMasterStart !== undefined) {
      mnClientSkipEnabled = !!configData.mnSkipMasterStart;
      const mnSkipToggleEarly = document.getElementById('mnSkipMasterStartToggle');
      if (mnSkipToggleEarly) mnSkipToggleEarly.checked = mnClientSkipEnabled;
    }
    if (configData.mnClientRaceAudio !== undefined) {
      mnClientRaceAudio = !!configData.mnClientRaceAudio;
      const mnAudioToggleEarly = document.getElementById('mnClientRaceAudioToggle');
      if (mnAudioToggleEarly) mnAudioToggleEarly.checked = mnClientRaceAudio;
    }

    // Populate antenna and TX power so buildConfigSnapshotFromUI() gets correct values
    const extAntennaEarly = document.getElementById('externalAntennaToggle');
    const antennaLabelEarly = document.getElementById('antennaLabel');
    if (extAntennaEarly && configData.wifiExtAntenna !== undefined) {
      extAntennaEarly.checked = configData.wifiExtAntenna === 1;
      if (antennaLabelEarly) antennaLabelEarly.textContent = configData.wifiExtAntenna === 1 ? 'External' : 'Internal';
    }
    const txPowerEarly = document.getElementById('wifiTxPowerInput');
    if (txPowerEarly && configData.wifiTxPower !== undefined) {
      txPowerEarly.value = configData.wifiTxPower;
    }
  }

  // Set baseline so stageConfig() can detect actual changes vs device state
  baselineConfig = (configData && Object.keys(configData).length) ? { ...configData } : {};
  settingsLoading = false;

  checkTuningStatusOnStartup();

};

function confirmDiscardUnsavedChanges() {
  // If nothing staged AND no deferred-apply work is pending, do not prompt.
  // A queued apply (e.g. LED preset awaiting Apply & Save) counts as
  // "unsaved" for the user-visible prompt even if no /config key is staged.
  const haveStaged = stagedDirty && stagedConfig && Object.keys(stagedConfig).length > 0;
  const havePending = (typeof _pendingApply !== 'undefined') && _pendingApply.length > 0;
  if (!haveStaged && !havePending) return true;

  return window.confirm(
    'You have unsaved changes.\n\nClose settings without saving?'
  );
}

// POST a single-key /config patch and advance baseline.  Used by handlers
// that are intentionally "auto-apply, auto-persist" (live preview style —
// LED color/brightness/speed/fade/strobe).  Bypasses the staged-config
// snapshot so it does NOT dirty the Save button.
async function saveConfigPatchImmediate(patch) {
  if (!patch || typeof patch !== 'object') return;
  try {
    if (usbConnected && transportManager) {
      await transportManager.sendCommand('config', 'POST', patch);
    } else {
      await fetch('/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
        body: JSON.stringify(patch),
      });
    }
    // Advance baseline so the Save button doesn't light up retroactively
    // for these keys.  Also drop any matching staged entry — same key, now
    // equal to baseline, would otherwise leave Save dirty.
    if (typeof baselineConfig === 'object' && baselineConfig) {
      Object.assign(baselineConfig, patch);
    }
    if (stagedConfig && typeof stagedConfig === 'object') {
      let changed = false;
      for (const k of Object.keys(patch)) {
        if (k in stagedConfig) { delete stagedConfig[k]; changed = true; }
      }
      if (changed) {
        stagedDirty = Object.keys(stagedConfig).length > 0;
        if (typeof updateSaveButton === 'function') updateSaveButton();
      }
    }
  } catch (err) {
    console.error('[Config] Immediate patch save failed:', patch, err);
  }
}

// Deferred-apply registry.  Bucket A handlers (Settings controls that
// have a runtime side effect but are NOT designed to auto-save) push a
// thunk here instead of running their POST /led/* (etc.) call inline.
// saveConfig() drains the registry after a successful /config commit so
// the apply + persist happen as one atomic-feeling Apply & Save click.
// A close-without-save discards both staged config AND pending applies.
//
// The optional `key` argument dedupes: if a handler queues an apply with
// the same key more than once (successive slider drags, repeated theme
// picks), only the latest thunk runs on Save.  Handlers that read the
// live form value inside the thunk automatically pick up the final value
// regardless.
const _pendingApply = [];

function deferApply(fnOrKey, maybeFn) {
  const key = typeof fnOrKey === 'string' ? fnOrKey : null;
  const fn  = typeof fnOrKey === 'function' ? fnOrKey : maybeFn;
  if (typeof fn !== 'function') return;
  if (key) {
    // Drop any earlier entry with the same key; keep only the latest thunk.
    for (let i = _pendingApply.length - 1; i >= 0; i--) {
      if (_pendingApply[i].__key === key) _pendingApply.splice(i, 1);
    }
    fn.__key = key;
  }
  _pendingApply.push(fn);
}

async function _drainPendingApply() {
  while (_pendingApply.length) {
    const fn = _pendingApply.shift();
    try { await Promise.resolve(fn()); }
    catch (err) { console.error('[Apply] deferred handler failed:', err); }
  }
}

function _clearPendingApply() {
  _pendingApply.length = 0;
}

// Bucket A handlers for the two multi-node client toggles.  The former
// inline onchange handlers wrote to mnClientSkipEnabled / mnClientRaceAudio
// directly, so flipping the toggle immediately affected client behaviour
// even if the user then cancelled Settings.  Now the global is set only
// when Apply & Save runs.
function onMnSkipMasterStartChange(checked) {
  if (settingsLoading) {
    mnClientSkipEnabled = !!checked;
    return;
  }
  deferApply('mnSkipMasterStart', async () => {
    const el = document.getElementById('mnSkipMasterStartToggle');
    mnClientSkipEnabled = !!(el && el.checked);
  });
  autoSaveConfig();
}

function onMnClientRaceAudioChange(checked) {
  if (settingsLoading) {
    mnClientRaceAudio = !!checked;
    return;
  }
  deferApply('mnClientRaceAudio', async () => {
    const el = document.getElementById('mnClientRaceAudioToggle');
    mnClientRaceAudio = !!(el && el.checked);
  });
  autoSaveConfig();
}

async function saveVoiceEnabledImmediate(enabled) {
  const patch = { voiceEnabled: enabled ? 1 : 0 };

  if (usbConnected && transportManager) {
    // USB transport path
    const res = await transportManager.sendCommand('config', 'POST', patch);
    console.log('[Config] voiceEnabled saved over USB:', patch, res);
    return res;
  }

  // WiFi fetch path
  const r = await fetch('/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
    body: JSON.stringify(patch),
  });

  if (!r.ok) {
    const t = await r.text().catch(() => '');
    throw new Error(`saveVoiceEnabledImmediate failed: HTTP ${r.status} ${r.statusText} ${t}`);
  }

  const json = await r.json().catch(() => null);
  console.log('[Config] voiceEnabled saved over WiFi:', patch, json);
  return json;
}

function getRssiCanvasCtx() {
  const canvas = document.getElementById('rssiChart');
  if (!canvas) return null;

  // If canvas node changed (replaced), reset cache
  if (_rssiCanvasCtx && _rssiCanvasCtx.canvas !== canvas) {
    _rssiCanvasCtx = null;
  }

  if (!_rssiCanvasCtx) {
    _rssiCanvasCtx = canvas.getContext('2d', { willReadFrequently: true });
  }
  return _rssiCanvasCtx;
}


function setLEDSettingsVisible(visible) {
  const navItems = document.querySelectorAll('.settings-nav-item');

  navItems.forEach(item => {
    const onclick = item.getAttribute('onclick') || '';
    if (onclick.includes("switchSettingsSection('led')")) {
      item.style.display = visible ? '' : 'none';
    }
  });

  // If LED is currently selected and we hide it, switch away
  if (!visible && typeof switchSettingsSection === 'function') {
    switchSettingsSection('system');
  }
}

function stageConfig(key, value) {
  // Do not mark dirty while we are simply populating the UI during modal open.
  if (settingsLoading) return;

  const norm = (v) => (v === true ? 1 : v === false ? 0 : v);

  const hasBaseline = baselineConfig && Object.prototype.hasOwnProperty.call(baselineConfig, key);

  // If matches baseline => UN-stage it
  if (hasBaseline) {
    const base = baselineConfig[key];

    // For arrays (webhookIPs), compare shallow
    const bothArrays = Array.isArray(base) && Array.isArray(value);
    if (bothArrays) {
      const same =
        base.length === value.length &&
        base.every((x, i) => String(x) === String(value[i]));

      if (same) {
        delete stagedConfig[key];
      } else {
        stagedConfig[key] = value;
      }
    } else {
      if (norm(base) === norm(value)) {
        delete stagedConfig[key];
      } else {
        stagedConfig[key] = value;
      }
    }
  } else {
    stagedConfig[key] = value;
  }

  stagedDirty = Object.keys(stagedConfig).length > 0;
  updateSaveButton();
}

function updateSaveButton() {
  const btn = document.getElementById('saveConfigBtn');
  if (!btn) return;
  if (stagedDirty) {
    btn.disabled = false;
    btn.classList.add('dirty');
  } else {
    btn.disabled = true;
    btn.classList.remove('dirty');
  }
}

function clearStagedConfig() {
  stagedConfig = {};
  stagedDirty = false;
  updateSaveButton();
}


async function getBatteryVoltage() {
  // Skip if the battery section is not visible (feature disabled or hardware unavailable)
  const batterySection = document.getElementById('batteryMonitoringSection');
  if (!batterySection || batterySection.style.display === 'none') return;

  try {
    let response;
    if (usbConnected && transportManager) {
      const data = await transportManager.sendCommand('status', 'GET');
      response = JSON.stringify(data);
    } else {
      const resp = await fetch("/status");
      response = await resp.text();
    }

    const batteryVoltageMatch = response.match(/Battery Voltage:\s*([\d.]+v)/);
    const batteryVoltage = batteryVoltageMatch ? batteryVoltageMatch[1] : null;
    if (batteryVoltageDisplay) {
      batteryVoltageDisplay.innerText = batteryVoltage;
    }
  } catch (err) {
    console.error('Failed to get battery voltage:', err);
  }
}

setInterval(getBatteryVoltage, 2000);

// --- Calibration scanner pause (graphics only; incoming samples discarded) ---
let rssiPaused = false;

function setRssiPaused(paused) {
  rssiPaused = !!paused;

  const btn = document.getElementById('pauseCalibBtn');
  if (calibOverviewMode)
  {
    if (btn) btn.textContent = rssiPaused ? 'Exit Wizard' : 'Exit Wizard';
  } else {
    if (btn) btn.textContent = rssiPaused ? 'Resume' : 'Pause';
  }
  

  if (rssiPaused) {
    // Freeze the plot so it doesn’t scroll off screen
    if (rssiChart) rssiChart.stop();

    // No buffering / no catch-up
    if (Array.isArray(rssiBuffer)) rssiBuffer.length = 0;

    // Capture "start" threshold positions so we can draw them as gray reference lines
    pausedEnterStart = enterRssi;
    pausedExitStart = exitRssi;

    // Snapshot the frozen scanner canvas (ONLY for regular paused scanner, not overview mode)
    pausedScannerFrame = null;
    pausedScannerFrameW = 0;
    pausedScannerFrameH = 0;

    if (!calibOverviewMode) {
      const canvas = document.getElementById('rssiChart');
      if (canvas) {
        // IMPORTANT: do NOT touch canvas.width/height here (it clears the frame!)
        const ctx = getRssiCanvasCtx();
        if (ctx) {
          pausedScannerFrameW = canvas.width;
          pausedScannerFrameH = canvas.height;

          try {
            pausedScannerFrame = ctx.getImageData(0, 0, pausedScannerFrameW, pausedScannerFrameH);
          } catch (e) {
            pausedScannerFrame = null;
          }
        }
      }
    }

    // Keep lines visible and adjustable while paused
    if (calibOverviewMode) drawCalibrationOverview();
    else drawPausedOverlayLines();

  } else {
    // Leaving pause: if we were in overview mode, exit it back to live
    if (calibOverviewMode) {
      exitCalibrationOverviewModeByUserAction();
      calibOverviewMode = false;
      calibOverviewData = null;
    }

    // Clear pause snapshot state
    pausedScannerFrame = null;
    pausedScannerFrameW = 0;
    pausedScannerFrameH = 0;
    pausedEnterStart = null;
    pausedExitStart = null;

    if (rssiChart) rssiChart.start();
  }
}

function toggleRssiPaused() {
  // "Exit Wizard" path: in overview mode the pause/resume button's text
  // changes to "Exit Wizard", and unpausing tears down the overview to
  // return to the live scanner.  If the user adjusted Enter / Exit since
  // the last save (manually after the wizard, or before clicking Save
  // RSSI) they're about to leave with unsaved values — confirm first.
  if (calibOverviewMode && rssiPaused && _rssiHasUnsavedChanges()) {
    if (!confirm('You have not saved the new values.\n\nAre you sure you want to exit?')) {
      return;
    }
  }
  setRssiPaused(!rssiPaused);
}

function drawPausedOverlayLines() {
  // Only used when paused AND not in overview mode.
  const canvas = document.getElementById('rssiChart');
  if (!canvas) return;

  const ctx = getRssiCanvasCtx();
  if (!ctx) return;

  // If CSS size changed while paused, rescale the stored snapshot instead of resizing here.
  const dw = canvas.offsetWidth || canvas.width;
  const dh = canvas.offsetHeight || canvas.height;
  if (pausedScannerFrame && (dw !== canvas.width || dh !== canvas.height)) {
    rescalePausedScannerFrameToCanvas(); // will redraw overlays
    return;
  }

  const h = canvas.height;
  const w = canvas.width;

  // Restore frozen frame first so we don't "stack" lines.
  if (!calibOverviewMode && pausedScannerFrame) {
    if (pausedScannerFrameW !== w || pausedScannerFrameH !== h) {
      rescalePausedScannerFrameToCanvas(); // will call drawPausedOverlayLines() again
      return;
    }
    try {
      ctx.putImageData(pausedScannerFrame, 0, 0);
    } catch (e) {
      ctx.clearRect(0, 0, w, h);
    }
  }

  // Use the same value range Smoothie was using
  const minV = (rssiChart && rssiChart.options && typeof rssiChart.options.minValue === 'number')
    ? rssiChart.options.minValue
    : Math.max(0, Math.min(minRssiValue, exitRssi - 10));

  const maxV = (rssiChart && rssiChart.options && typeof rssiChart.options.maxValue === 'number')
    ? rssiChart.options.maxValue
    : Math.max(maxRssiValue, enterRssi + 10);

  if (maxV <= minV) return;

  const yOf = (v) => {
    const t = (v - minV) / (maxV - minV);
    return h - Math.round(t * (h - 1));
  };

  ctx.save();

  // Draw the "starting" reference lines in gray (where the pause began)
  if (pausedEnterStart != null && pausedExitStart != null) {
    ctx.strokeStyle = 'rgba(200,200,200,0.45)';
    ctx.lineWidth = 2;

    ctx.beginPath();
    ctx.moveTo(0, yOf(pausedEnterStart));
    ctx.lineTo(w, yOf(pausedEnterStart));
    ctx.stroke();

    ctx.beginPath();
    ctx.moveTo(0, yOf(pausedExitStart));
    ctx.lineTo(w, yOf(pausedExitStart));
    ctx.stroke();
  }

  // Current Enter line (red)
  ctx.strokeStyle = "hsl(8.2, 86.5%, 53.7%)";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(0, yOf(enterRssi));
  ctx.lineTo(w, yOf(enterRssi));
  ctx.stroke();

  // Current Exit line (orange)
  ctx.strokeStyle = "hsl(25, 85%, 55%)";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(0, yOf(exitRssi));
  ctx.lineTo(w, yOf(exitRssi));
  ctx.stroke();

  ctx.restore();
}

function rescalePausedScannerFrameToCanvas() {
  if (!rssiPaused) return;
  if (calibOverviewMode) return;
  if (!pausedScannerFrame) return;

  const canvas = document.getElementById('rssiChart');
  if (!canvas) return;

  const newW = canvas.offsetWidth || canvas.width;
  const newH = canvas.offsetHeight || canvas.height;

  // If unchanged, nothing to do
  if (newW === pausedScannerFrameW && newH === pausedScannerFrameH) return;

  // Build an offscreen canvas from the old ImageData
  const src = document.createElement('canvas');
  src.width = pausedScannerFrameW;
  src.height = pausedScannerFrameH;

  const sctx = src.getContext('2d', { willReadFrequently: true });
  if (!sctx) return;

  try {
    sctx.putImageData(pausedScannerFrame, 0, 0);
  } catch (e) {
    return;
  }

  // Resize visible canvas to new dimensions (this clears it, which is OK here)
  canvas.width = newW;
  canvas.height = newH;

  const ctx = getRssiCanvasCtx();
  if (!ctx) return;

  // Draw scaled frozen frame
  ctx.clearRect(0, 0, newW, newH);
  ctx.drawImage(src, 0, 0, newW, newH);

  // Re-snapshot scaled image for future redraws
  pausedScannerFrameW = newW;
  pausedScannerFrameH = newH;
  try {
    pausedScannerFrame = ctx.getImageData(0, 0, newW, newH);
  } catch (e) {
    pausedScannerFrame = null;
  }

  // Redraw overlay lines (gray start + current)
  drawPausedOverlayLines();
}

function addRssiPoint() {
  if (!rssiChart) return; // Chart not initialized yet
  
  if (calib.style.display != "none") {

    if (rssiPaused) {
      // discard live data while paused so it doesn’t scroll and doesn’t “catch up”
      if (Array.isArray(rssiBuffer)) rssiBuffer.length = 0;

      // Ensure Smoothie has up-to-date ranges/lines for when we unpause,
      // and redraw overlay lines so slider changes show immediately.
      rssiChart.options.horizontalLines = [
        { color: "hsl(8.2, 86.5%, 53.7%)", lineWidth: 1.7, value: enterRssi }, // red
        { color: "hsl(25, 85%, 55%)", lineWidth: 1.7, value: exitRssi }, // orange
      ];
      rssiChart.options.maxValue = Math.max(maxRssiValue, enterRssi + 10);
      rssiChart.options.minValue = Math.max(0, Math.min(minRssiValue, exitRssi - 10));

      // If we're in overview mode, keep that view (lines included) fresh.
      if (calibOverviewMode) {
        drawCalibrationOverview();
      } else {
        drawPausedOverlayLines();
      }
      return;
    }


    rssiChart.start();
    if (rssiBuffer.length > 0) {
      // Firmware streams RSSI at 10 Hz but addRssiPoint runs at 5 Hz.  Using
      // FIFO shift() here meant the chart drew the oldest queued sample each
      // cycle, accumulating up to ~10 samples (~1 s) of artificial lag in
      // steady state and feeling much more sluggish than the actual signal
      // processing.  Take the newest sample instead and discard the rest —
      // the chart only renders one point per cycle anyway, so older samples
      // would never have been drawn.
      rssiValue = parseInt(rssiBuffer[rssiBuffer.length - 1], 10);
      rssiBuffer.length = 0;
      // A malformed/empty SSE frame yields NaN. Math.max/min(x, NaN) === NaN, which
      // would poison maxRssiValue/minRssiValue (and thus the chart's axis range) for
      // the rest of the session, and the crossing comparisons silently evaluate false.
      // Drop the bad sample instead.
      if (!Number.isFinite(rssiValue)) return;
      if (crossing && rssiValue < exitRssi) {
        crossing = false;
      } else if (!crossing && rssiValue > enterRssi) {
        crossing = true;
      }
      // Y-axis range tracker: monotonically expands to fit observed extremes,
      // never contracts within a session.  Reset on Calibration tab exit
      // (see `else` branch below) so each visit starts fresh.
      maxRssiValue = Math.max(maxRssiValue, rssiValue);
      minRssiValue = Math.min(minRssiValue, rssiValue);
    }

    // update horizontal lines and min max values
    rssiChart.options.horizontalLines = [
      { color: "hsl(8.2, 86.5%, 53.7%)", lineWidth: 1.7, value: enterRssi }, // red
      { color: "hsl(25, 85%, 55%)", lineWidth: 1.7, value: exitRssi }, // orange
    ];

    rssiChart.options.maxValue = Math.max(maxRssiValue, enterRssi + 10);

    rssiChart.options.minValue = Math.max(0, Math.min(minRssiValue, exitRssi - 10));

    var now = Date.now();
    rssiSeries.append(now, rssiValue);
    if (crossing) {
      rssiCrossingSeries.append(now, 256);
    } else {
      rssiCrossingSeries.append(now, -10);
    }
  } else {
    rssiChart.stop();
    maxRssiValue = enterRssi + 10;
    minRssiValue = exitRssi - 10;
  }
}

function setStartWizardEnabled(enabled) {
  const btn = document.getElementById('startWizardButton');
  if (!btn) return;
  btn.disabled = !enabled;
  btn.classList.toggle('disabled', !enabled); // optional, if you style .disabled
}

function setOverviewNoticeVisible(visible) {
  const el = document.getElementById('calibrationOverviewNotice');
  if (!el) return;
  el.style.display = visible ? 'block' : 'none';
}


function downsampleWizardDataForOverview(data, maxPoints) {
  if (!Array.isArray(data) || data.length === 0) return [];
  if (data.length <= maxPoints) return data;

  const step = data.length / maxPoints;
  const out = [];
  for (let i = 0; i < maxPoints; i++) {
    out.push(data[Math.floor(i * step)]);
  }
  return out;
}

function drawCalibrationOverview() {
  if (!calibOverviewMode || !calibOverviewData || calibOverviewData.length === 0) return;

  // Use the SAME canvas as the live scanner uses
  const canvas = document.getElementById('rssiChart');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');

  // Ensure canvas matches its displayed size
  canvas.width = canvas.offsetWidth;
  canvas.height = canvas.offsetHeight;

  const w = canvas.width;
  const h = canvas.height;

  // Background
  ctx.fillStyle = getComputedStyle(document.body).getPropertyValue('--tabcontent-bg').trim() || '#000';
  ctx.fillRect(0, 0, w, h);

  // Compute bounds
  const values = calibOverviewData.map(d => d.rssi ?? 0);
  let minV = Math.min(...values);
  let maxV = Math.max(...values);
  if (maxV <= minV) maxV = minV + 1;

  // Draw polyline
  ctx.strokeStyle = '#3bd16f'; // match your vibe; change if you want
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (let i = 0; i < calibOverviewData.length; i++) {
    const x = (i / (calibOverviewData.length - 1)) * w;
    const v = calibOverviewData[i].rssi ?? 0;
    const y = h - ((v - minV) / (maxV - minV)) * h;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Draw Enter/Exit lines based on current UI values
  // (These IDs match your calibration tab sliders)
  const enterEl = document.getElementById('enter');
  const exitEl  = document.getElementById('exit');
  const enterVal = enterEl ? parseInt(enterEl.value, 10) : null;
  const exitVal  = exitEl  ? parseInt(exitEl.value, 10)  : null;

  function drawHLine(val, color) {
    if (val == null || Number.isNaN(val)) return;
    const y = h - ((val - minV) / (maxV - minV)) * h;
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  // Enter slightly brighter than Exit
  drawHLine(enterVal, '#00ff99');
  drawHLine(exitVal,  '#ffcc00');
}

function enterCalibrationOverviewModeFromWizard() {
  if (!wizardState || !Array.isArray(wizardState.data) || wizardState.data.length < 2) return;

  // Use the recorded wizard dataset, scaled to fit screen
  calibOverviewData = downsampleWizardDataForOverview(wizardState.data, CALIB_OVERVIEW_MAX_POINTS);

  calibOverviewMode = true;

  // Pause live scrolling
  setRssiPaused(true);

  // Draw overview onto chart
  drawCalibrationOverview();

  // Disable Start Wizard while in post-wizard overview mode
  setStartWizardEnabled(false);

  // Show notice
  setOverviewNoticeVisible(true);
}

function exitCalibrationOverviewModeByUserAction() {
  // Only act if we are actually in overview mode
  if (!calibOverviewMode) return;

  // Hide the notice
  setOverviewNoticeVisible(false);

  // Re-enable Start Wizard now that user acknowledged the overview step
  setStartWizardEnabled(true);

  // Exit overview mode back to normal calibration tab (paused or live depends on your existing flow)
  calibOverviewMode = false;
  calibOverviewData = null;

  // If you want the chart to resume live immediately when leaving overview:
  setRssiPaused(false);
  //
  // If you prefer to keep it paused until user explicitly hits Resume, leave it paused.
}


function exitCalibrationOverviewMode() {
  calibOverviewMode = false;
  calibOverviewData = null;

  // Restart the live chart if you stopped it
  try { if (window.rssiChart && typeof window.rssiChart.start === 'function') window.rssiChart.start(); } catch (e) {}
}

// Match the firmware's 100 ms RSSI send rate (WEB_RSSI_SEND_TIMEOUT_MS) so
// every SSE event gets drawn on the next cycle, instead of accumulating in
// the buffer and arriving on the chart up to 200 ms late.
setInterval(addRssiPoint, 100);

async function createRssiChart() {
  await loadScript('smoothie.js');
  rssiSeries         = new TimeSeries();
  rssiCrossingSeries = new TimeSeries();
  rssiLapMarkerSeries = new TimeSeries();
  rssiChart = new SmoothieChart({
    responsive: true,
    millisPerPixel: 50,
    // Accuracy-critical defaults — SmoothieChart's defaults compromise the
    // diagnostic value of this chart and have to be turned off explicitly:
    //   interpolation 'bezier' (default) draws CURVED lines between samples
    //     and rounds off sharp peaks; 'linear' connects actual sample points
    //     with straight segments so peak position and value are preserved.
    //   scaleSmoothing 0.125 (default) animates Y-axis range changes slowly,
    //     so a tall peak briefly clips at the top before the axis "catches
    //     up"; 1.0 snaps the axis instantly so peaks appear at correct height.
    interpolation: 'linear',
    scaleSmoothing: 1.0,
    grid: {
      strokeStyle: "rgba(255,255,255,0.25)",
      sharpLines: true,
      verticalSections: 4,
      // Horizontal lines only.  SmoothieChart defaults millisPerLine to 1000,
      // which ruled the plot with a vertical line every second — a dense picket
      // fence that carried no information here, because this chart has no time
      // axis and nothing is ever read off it horizontally.  0 disables them
      // (smoothie.js guards the draw with `millisPerLine > 0`).  The horizontal
      // lines stay: those ARE read against, for the enter/exit thresholds.
      millisPerLine: 0,
      borderVisible: false,
    },
    labels: {
      precision: 0,
      fillStyle: "rgba(255,255,255,0.85)",
      fontSize: 11,
      showIntermediateLabels: true,
    },
    maxValue: 1,
    minValue: 0,
  });
  rssiChart.addTimeSeries(rssiSeries, {
    lineWidth: 1.7,
    strokeStyle: "hsl(214, 53%, 60%)",
    fillStyle: "hsla(214, 53%, 60%, 0.4)",
  });
  rssiChart.addTimeSeries(rssiCrossingSeries, {
    lineWidth: 1.7,
    strokeStyle: "none",
    fillStyle: "hsla(136, 71%, 70%, 0.3)",
  });
  rssiChart.addTimeSeries(rssiLapMarkerSeries, {
    lineWidth: 0,
    strokeStyle: "none",
    fillStyle: "hsla(45, 100%, 60%, 0.55)",
  });
  // streamTo's second arg is a deliberate render delay used to let the chart
  // smooth-scroll without flicker.  200ms (the original value) was the single
  // largest contributor to perceived live-view lag.  50ms is enough buffer
  // for the 100ms SSE rate while keeping the trace visibly close to real-time.
  rssiChart.streamTo(document.getElementById("rssiChart"), 50);
}

function openTab(evt, tabName) {
  // Declare all variables
  var i, tabcontent, tablinks;

  // Get all elements with class="tabcontent" and hide them
  tabcontent = document.getElementsByClassName("tabcontent");
  for (i = 0; i < tabcontent.length; i++) {
    tabcontent[i].style.display = "none";
  }

  // Get all elements with class="tablinks" and remove the class "active"
  tablinks = document.getElementsByClassName("tablinks");
  for (i = 0; i < tablinks.length; i++) {
    tablinks[i].className = tablinks[i].className.replace(" active", "");
  }

  // Show the current tab, and add an "active" class to the button that opened
  // the tab.  '' rather than 'block': .tabcontent is a flex column that also
  // carries flex:1/min-height:0, which is what lets .race-shell fill the
  // viewport and scroll its right column instead of scrolling the whole page.
  // An inline display:block would beat the class and collapse that chain.
  document.getElementById(tabName).style.display = "";

  // Switch between single-pilot and master race view
  if (tabName === "race")     onRaceTabOpen();
  if (tabName === "history")  updateHistoryTabMode();
  if (tabName === "raceview") rvRender();

  // Hook pause button when entering Calibration tab; create chart on first open
  if (tabName === "calib") {
    const btn = document.getElementById('pauseCalibBtn');
    if (btn && !btn.dataset.bound) {
      btn.dataset.bound = "1";
      btn.addEventListener('click', toggleRssiPaused);
      btn.textContent = rssiPaused ? 'Resume' : 'Pause';
    }
    // Create the chart now that the canvas is visible and has real dimensions.
    // createRssiChart() is async — it lazy-loads smoothie.js on first call.
    if (!rssiChart) {
      createRssiChart();  // intentionally not awaited; addRssiPoint guards with if(!rssiChart)
    }
  }


  evt.currentTarget.className += " active";

  // if event comes from calibration tab, signal to start sending RSSI events
  if (tabName === "calib" && !rssiSending) {
    if (usbConnected && transportManager) {
      transportManager.sendCommand('timer/rssiStart', 'POST')
        .then((response) => {
          rssiSending = true;
          console.log("/timer/rssiStart:", response);
        })
        .catch(err => console.error('Failed to start RSSI:', err));
    } else {
      fetch("/timer/rssiStart", {
        method: "POST",
        headers: {
          Accept: "application/json",
          "Content-Type": "application/json",
        },
      })
        .then((response) => {
          if (response.ok) rssiSending = true;
          return response.json();
        })
        .then((response) => console.log("/timer/rssiStart:" + JSON.stringify(response)));
    }
  } else if (rssiSending) {
    if (usbConnected && transportManager) {
      transportManager.sendCommand('timer/rssiStop', 'POST')
        .then((response) => {
          rssiSending = false;
          console.log("/timer/rssiStop:", response);
        })
        .catch(err => console.error('Failed to stop RSSI:', err));
    } else {
      fetch("/timer/rssiStop", {
        method: "POST",
        headers: {
          Accept: "application/json",
          "Content-Type": "application/json",
        },
      })
        .then((response) => {
          if (response.ok) rssiSending = false;
          return response.json();
        })
        .then((response) => console.log("/timer/rssiStop:" + JSON.stringify(response)));
    }
  }
  
  // Load race history when opening history tab
  if (tabName === 'history') {
    loadRaceHistory();
  }
}

function redrawCalibrationLinesIfPaused() {
  // Only redraw overlays when we're paused on the Calibration tab
  if (!window.rssiPaused) return;

  // If you're in "overview" mode, that draw should include lines
  if (window.calibOverviewMode && typeof drawCalibrationOverview === 'function') {
    drawCalibrationOverview();
    return;
  }

  // Otherwise redraw the paused overlay lines on top of the frozen chart
  if (typeof drawPausedOverlayLines === 'function') {
    drawPausedOverlayLines();
  }
}

function stepRssi(which, delta) {
  if (which === 'enter') {
    const v = Math.min(255, Math.max(50, enterRssi + delta));
    enterRssiInput.value = v;
    updateEnterRssi(enterRssiInput, v);
  } else {
    const v = Math.min(255, Math.max(50, exitRssi + delta));
    exitRssiInput.value = v;
    updateExitRssi(exitRssiInput, v);
  }
}

function updateEnterRssi(obj, value) {
  enterRssi = parseInt(value);
  enterRssiSpan.textContent = enterRssi;

  if (enterRssi <= exitRssi) {
    exitRssi = Math.max(0, enterRssi - 1);
    exitRssiInput.value = exitRssi;
    exitRssiSpan.textContent = exitRssi;
  }

  // Stage both (your existing behavior)
  stageConfig('enterRssi', enterRssi);
  stageConfig('exitRssi', exitRssi);

  // NEW: if paused, redraw overlay so the line moves immediately
  redrawCalibrationLinesIfPaused();
  // Flip the Save RSSI Thresholds button to dirty/clean based on the new
  // values vs. the last-persisted baseline.
  updateRssiSaveButton();
}


function updateExitRssi(obj, value) {
  exitRssi = parseInt(value);
  exitRssiSpan.textContent = exitRssi;

  if (exitRssi >= enterRssi) {
    enterRssi = Math.min(255, exitRssi + 1);
    if (enterRssiInput) enterRssiInput.value = enterRssi;
    enterRssiSpan.textContent = enterRssi;
  }

  // Stage both (your existing behavior)
  stageConfig('exitRssi', exitRssi);
  stageConfig('enterRssi', enterRssi);

  // NEW: if paused, redraw overlay so the line moves immediately
  redrawCalibrationLinesIfPaused();
  updateRssiSaveButton();
}

function stageBandChan() {
  // band index is 0-based
  stageConfig('band', bandSelect.selectedIndex);

  // channelSelect.value is "1".."8" per our current convention
  const chanNum = parseInt(channelSelect.value, 10);
  const chanIndex = Number.isFinite(chanNum) ? (chanNum - 1) : 0;
  stageConfig('chan', chanIndex);
}

function buildConfigSnapshotFromUI() {
  // Pilot settings
  const colorInput = document.getElementById('pilotColor');

  let pilotColorInt = 0x0080FF;
  if (colorInput && colorInput.value) {
    const _parsed = parseInt(colorInput.value.replace('#', ''), 16);
    if (!isNaN(_parsed)) pilotColorInt = _parsed;
  }

  // RSSI sensitivity (0/1)
  const rssiSensitivitySelect = document.getElementById('rssiSensitivity');
  const rssiSens = rssiSensitivitySelect ? parseInt(rssiSensitivitySelect.value, 10) : 1;

  // Theme / voice / lap format
  const themeSelect = document.getElementById('themeSelect');
  const voiceSelect = document.getElementById('voiceSelect');
  const lapFormatSelect = document.getElementById('lapFormatSelect');

  // Core timing
  const minLapInput = document.getElementById('minLap');
  const alarmThreshold = document.getElementById('alarmThreshold');
  const announcerSelect = document.getElementById('announcerSelect');
  const announcerRateInput = document.getElementById('rate');
  const enterRssiInput = document.getElementById('enter');
  const exitRssiInput = document.getElementById('exit');
  const maxLapsInput = document.getElementById('maxLaps');

  // LED settings
  const ledPresetSelect = document.getElementById('ledPreset');
  const ledBrightnessInput = document.getElementById('ledBrightness');
  const ledSpeedInput = document.getElementById('ledSpeed');

  // IMPORTANT: your HTML uses ledSolidColor (not ledColor)
  const ledSolidColorInput = document.getElementById('ledSolidColor');
  const ledFadeColorInput = document.getElementById('ledFadeColor');
  const ledStrobeColorInput = document.getElementById('ledStrobeColor');
  const ledManualOverrideToggle = document.getElementById('ledManualOverride');

  const parseColor = (el, fallbackHex) => {
    const hex = (el && el.value) ? el.value : fallbackHex;
    return parseInt(hex.replace('#', ''), 16);
  };

  const ledColorInt = parseColor(ledSolidColorInput, '#0080FF');
  const ledFadeColorInt = parseColor(ledFadeColorInput, '#00FF00');
  const ledStrobeColorInt = parseColor(ledStrobeColorInput, '#FFFFFF');

  // Webhooks
  const webhooksEnabledToggle = document.getElementById('webhooksEnabled');

  // We maintain this global from displayWebhooks() (see drop-in below)
  const ips = Array.isArray(window.currentWebhookIPs) ? window.currentWebhookIPs : [];

  // Gate LED + race event toggles
  const gateLEDsEnabledToggle = document.getElementById('gateLEDsEnabled');
  const webhookRaceStartToggle = document.getElementById('webhookRaceStart');
  const webhookRaceStopToggle = document.getElementById('webhookRaceStop');
  const webhookLapToggle = document.getElementById('webhookLap');

  // Battery / antenna (only if present)
  const batteryToggle = document.getElementById('batteryMonitorToggle');
  const externalAntennaToggle = document.getElementById('externalAntennaToggle');

  // WiFi credentials
  const ssidInput = document.getElementById('ssid');
  const pwdInput = document.getElementById('pwd');

  // Band/channel
  const bandSelect = document.getElementById('bandSelect');
  const channelSelect = document.getElementById('channelSelect');

  const cfg = {
    band: bandSelect ? bandSelect.selectedIndex : 0,
    chan: (() => {
      const n = parseInt(channelSelect?.value ?? "1", 10);
      if (!Number.isFinite(n)) return 0;
      return Math.max(0, Math.min(7, n - 1));
    })(),

    // Frequency used by backend too
    freq: (typeof frequency !== 'undefined') ? frequency : 0,

    // Units stored x10 (0.1s)
    minLap: parseInt(parseFloat(minLapInput?.value || 0) * 10),
    alarm: parseInt(parseFloat(alarmThreshold?.value || 0) * 10),
    anType: announcerSelect ? announcerSelect.selectedIndex : 0,
    anRate: parseInt(parseFloat(announcerRateInput?.value || 0) * 10),
    enterRssi: parseInt(enterRssiInput?.value || 0),
    exitRssi: parseInt(exitRssiInput?.value || 0),
    maxLaps: parseInt(maxLapsInput?.value || 0),

    // NEW: RSSI sensitivity (must be supported in firmware; see section B)
    rssiSens: Number.isFinite(rssiSens) ? rssiSens : 1,

    // LED config (matches firmware keys)
    ledPreset: ledPresetSelect ? parseInt(ledPresetSelect.value, 10) : 0,
    ledBrightness: ledBrightnessInput ? parseInt(ledBrightnessInput.value, 10) : 128,
    ledSpeed: ledSpeedInput ? parseInt(ledSpeedInput.value, 10) : 10,
    ledColor: ledColorInt,
    ledFadeColor: ledFadeColorInt,
    ledStrobeColor: ledStrobeColorInt,
    ledManualOverride: (ledManualOverrideToggle && ledManualOverrideToggle.checked) ? 1 : 0,

    // Webhooks
    webhooksEnabled: (webhooksEnabledToggle && webhooksEnabledToggle.checked) ? 1 : 0,
    webhookIPs: ips,

    // Gate LED + event webhooks
    gateLEDsEnabled: (gateLEDsEnabledToggle && gateLEDsEnabledToggle.checked) ? 1 : 0,
    webhookRaceStart: (webhookRaceStartToggle && webhookRaceStartToggle.checked) ? 1 : 0,
    webhookRaceStop: (webhookRaceStopToggle && webhookRaceStopToggle.checked) ? 1 : 0,
    webhookLap: (webhookLapToggle && webhookLapToggle.checked) ? 1 : 0,

    // Pilot
    name: (document.getElementById('pname')?.value || ''),
    pilotColor: pilotColorInt,

    // UI prefs
    theme: themeSelect ? themeSelect.value : '',
    selectedVoice: voiceSelect ? voiceSelect.value : '',
    lapFormat: lapFormatSelect ? lapFormatSelect.value : '',

    // WiFi
    ssid: ssidInput ? ssidInput.value : '',
    pwd: pwdInput ? pwdInput.value : '',

    // Optional platform features
    batteryMonitor: (batteryToggle && batteryToggle.checked) ? 1 : 0,
    // wifiExtAntenna is FORCE-PINNED to 1 (External) — this hardware build
    // ships with a fixed external antenna, so Internal is never the right
    // answer.  Pinning here (not reading from the DOM) also corrects any
    // device that was left on Internal before the toggle was hidden.
    //
    // TO REVERT: change back to reading externalAntennaToggle.checked AND
    // unhide the row in data/index.html near id="externalAntennaToggle"
    // (they're linked with matching TO REVERT markers).
    wifiExtAntenna: 1,
    wifiTxPower: (() => {
      const el = document.getElementById('wifiTxPowerInput');
      const v = el ? parseInt(el.value, 10) : 21;
      return Number.isFinite(v) ? Math.min(21, Math.max(2, v)) : 21;
    })(),

    // Signal processing
    gate1Bootstrap: (() => {
      const el = document.getElementById('gate1BootstrapToggle');
      return (el && el.checked) ? 1 : 0;
    })(),
    v1Smoothing: (() => {
      const el = document.getElementById('v1Smoothing');
      const v = el ? parseInt(el.value, 10) : 5;
      return Number.isFinite(v) ? Math.min(10, Math.max(0, v)) : 5;
    })(),

    // Multi-node
    nodeMode: (() => {
      const el = document.getElementById('nodeModeSelect');
      return el ? parseInt(el.value, 10) : 0;
    })(),
    masterSSID: (document.getElementById('masterSSIDInput')?.value || ''),
    mnSkipMasterStart: document.getElementById('mnSkipMasterStartToggle')?.checked ? 1 : 0,
    mnClientRaceAudio: document.getElementById('mnClientRaceAudioToggle')?.checked ? 1 : 0,
    devMode: document.getElementById('devModeToggle')?.checked ? 1 : 0,

    // OTA — channel selector: 0=Published (stable), 1=Pre-releases.  Field
    // name is kept as otaIncludePrereleases for on-disk config compatibility
    // with older firmwares, but the semantics are now "channel select"
    // (either/or) rather than the previous "include in addition" toggle.
    otaIncludePrereleases: parseInt(document.getElementById('otaChannelSelect')?.value || '0', 10) || 0,

    // RSSI acquisition: 0 = polled analogRead, 1 = DMA continuous peak-hold.
    // The selector this read was written for is GONE (removed 2026-09-12 — see
    // the note in index.html).  That turns this line into the migration path
    // rather than a dead read: `?.value` is now always undefined, so the '1'
    // fallback fires and every save writes DMA.  A unit whose stored config
    // still says polled heals itself the first time any setting is saved.
    // Keep the line for exactly that reason; deleting it would leave a stale
    // adcMode=0 in NVS with nothing to correct it.
    adcMode: parseInt(document.getElementById('adcModeSelect')?.value || '1', 10) || 0,

    // Spoken lap-time precision: 1=tenths, 2=hundredths, 3=thousandths.
    anDecimals: parseInt(document.getElementById('announcerDecimalsSelect')?.value || '2', 10) || 2,

  };

  return cfg;
}


function autoSaveConfig() {
  // Stage ONLY: compute snapshot and stage each key vs baseline.
  // Skip keys that require a reboot — those have their own save path
  // (applyMultiNodeSettings) and must not light up the Save button here.
  const REBOOT_ONLY_KEYS = new Set(['nodeMode', 'masterSSID']);
  const snap = buildConfigSnapshotFromUI();
  Object.keys(snap).forEach(k => { if (!REBOOT_ONLY_KEYS.has(k)) stageConfig(k, snap[k]); });
}

function saveRSSIThresholds() {
  if (calibOverviewMode) exitCalibrationOverviewModeByUserAction();
  // Explicitly stage current RSSI values before saving (enter/exit may have changed via the calibration wizard)
  const enterEl = document.getElementById('enter');
  const exitEl  = document.getElementById('exit');
  if (enterEl) stageConfig('enterRssi', parseInt(enterEl.value || 0));
  if (exitEl)  stageConfig('exitRssi',  parseInt(exitEl.value  || 0));
  saveConfig();
  // saveConfig is async fire-and-forget — advance the baseline optimistically
  // so the dirty indicator clears immediately.  If /config actually fails
  // the next slider drag will still re-trigger updateRssiSaveButton; this
  // matches the UX of the other Save paths.
  _markRssiSaved(enterRssi, exitRssi);
}

// THE saveConfig.  There is exactly one.
//
// An earlier full-snapshot implementation, plus a debounced autoSaveConfig()
// that called it on every control change, used to sit commented out a few
// dozen lines below this.  It was removed 2026-09-13: a complete, plausible
// second `async function saveConfig()` in the file is a trap — it matches a
// grep, reads as live to anyone skimming, and cost a debugging session in
// exactly that way.  Git has it.  The reason it was abandoned is worth keeping
// though, and it is the reason for the staging model below: it wrote to flash
// on EVERY change.
async function saveConfig() {
  // Commit staged config to device (single write).  If there's nothing to
  // persist AND nothing in the deferred-apply queue, exit early.  Either
  // alone is reason to run — e.g. ledPreset change stages a key AND queues
  // a /led/preset call.
  if (!stagedDirty && _pendingApply.length === 0) {
    console.log('[Config] No staged changes to save.');
    return;
  }

  // Send only staged (delta) fields — avoids accidentally overwriting fields like voiceEnabled
  // that have their own save path. Falls back to full snapshot only if staged is somehow empty.
  const payload = stagedConfig && Object.keys(stagedConfig).length ? stagedConfig : buildConfigSnapshotFromUI();

  try {
    if (stagedDirty) {
      if (usbConnected && transportManager) {
        const response = await transportManager.sendCommand('config', 'POST', payload);
        console.log('/config (USB):', response);
      } else {
        const resp = await fetch('/config', {
          method: 'POST',
          headers: {
            Accept: 'application/json',
            'Content-Type': 'application/json',
          },
          body: JSON.stringify(payload),
        });
        const json = await resp.json().catch(() => ({}));
        console.log('/config (WiFi):', json);
      }

      // Clear staged/dirty state ONLY after successful commit
      stagedConfig = {};
      stagedDirty = false;
      updateSaveButton();
      // Advance baseline so future stageConfig() calls compare against what was just saved
      if (typeof baselineConfig === 'object' && baselineConfig) Object.assign(baselineConfig, payload);
    }

    // Drain deferred Bucket A applies (e.g. /led/preset, /led/override).
    // Runs even when staged was empty — handlers that defer can register
    // an apply without staging if their persistence is handled elsewhere.
    await _drainPendingApply();
  } catch (err) {
    console.error('[Config] Save failed:', err);
    // Keep stagedDirty=true so user can try saving again.  Pending applies
    // are left in place so the next Save click retries them too.
  }
}

let configStagingListenersAttached = false;

function attachConfigStagingListeners() {
  if (configStagingListenersAttached) return;
  configStagingListenersAttached = true;

  // Helper: attach change/input and stage via autoSaveConfig
  const wire = (id, evt = 'change') => {
    const el = document.getElementById(id);
    if (!el) return;
    el.addEventListener(evt, autoSaveConfig);
  };

  // --- Configuration tab controls (stage everything) ---

  // Theme / Voice / Lap format
  wire('themeSelect', 'change');
  wire('voiceSelect', 'change');
  wire('lapFormatSelect', 'change');

  // LED controls
  wire('ledPreset', 'change');
  wire('ledBrightness', 'input');
  wire('ledColor', 'input');

  // Gate LED + Webhooks
  wire('gateLEDsEnabled', 'change');
  wire('webhookRaceStart', 'change');
  wire('webhookRaceStop', 'change');
  wire('webhookLap', 'change');

  // Battery monitor toggle
  wire('batteryMonitorToggle', 'change');
  // externalAntennaToggle uses direct stagedConfig assignment in its onchange (always stages user's explicit choice)
  wire('wifiTxPowerInput', 'change');

  // Pilot settings
  wire('pilotColor', 'input');

  // WiFi credentials
  wire('ssid', 'input');
  wire('pwd', 'input');

  // NOTE:
  // Your minLap/alarm/maxLaps/announcerRate/etc already call autoSaveConfig()
  // inside their updateX() functions, so they are covered.
}


function updateChannelOptionsForBand(bandIndex = bandSelect.selectedIndex) {
  if (!bandSelect || !channelSelect) return;

  const freqs = freqLookup[bandIndex] || [];

  // Preserve previous channel number ("1".."8") if possible
  const prevValue = channelSelect.value;

  // Rebuild channel options, skipping any frequency === 0
  channelSelect.innerHTML = "";

  let firstEnabledValue = null;

  for (let i = 0; i < 8; i++) {
    const freq = freqs[i] ?? 0;
    if (freq === 0) continue;

    const opt = document.createElement("option");
    opt.value = String(i + 1);            // IMPORTANT: 1-based channel number
    opt.textContent = `Ch ${i + 1}`;      // label (can be just `${i+1}` if you prefer)
    channelSelect.appendChild(opt);

    if (firstEnabledValue === null) firstEnabledValue = opt.value;
  }

  // Restore previous selection if still valid, otherwise pick first available
  const stillExists = Array.from(channelSelect.options).some(o => o.value === prevValue);
  if (stillExists) {
    channelSelect.value = prevValue;
  } else if (firstEnabledValue !== null) {
    channelSelect.value = firstEnabledValue;
  }

  // Some browsers can leave selectedIndex = -1 after rebuild; force a valid selection
  if (channelSelect.selectedIndex < 0 && channelSelect.options.length > 0) {
    channelSelect.selectedIndex = 0;
  }
}

async function loadFirmwareVersion() {
  try {
    const [versionR, biR] = await Promise.all([
      fetch('/api/version'),
      fetch('/buildinfo.json').catch(() => null),
    ]);
    if (!versionR.ok) return;
    const data = await versionR.json();
    if (data && data.firmwareVersion) {
      const v = data.firmwareVersion;
      const footer = document.getElementById('firmwareVersion');
      if (footer) {
        const fwTs = data.buildTimestamp || '';
        let fsTs = '';
        try { if (biR && biR.ok) { const bi = await biR.json(); fsTs = bi.fsTimestamp || ''; } } catch (_) {}
        // Show whichever timestamp is more recent (YYYYMMDD-HHMMSS sorts lexicographically)
        const ts = fwTs || fsTs
          ? `  •  FW: ${fwTs || '?'}  FS: ${fsTs || '?'}`
          : '';
        footer.textContent = `FPVRaceOne Personal Lap Timer v${v}${ts}`;
      }
      const badge = document.getElementById('updateVersionDisplay');
      if (badge) badge.textContent = `v${v}`;
    }
  } catch (e) {
    console.warn('[UI] Failed to load firmware version:', e);
  }
}

// ─── OTA Auto-Update ───────────────────────────────────────────────────────
//
// Flow:
//   1. User taps "Check for Updates" → checkForUpdates() POSTs /api/update/check.
//      The device joins home WiFi, queries GitHub, returns version info.
//   2. If a newer version is available, we confirm with the user, then
//      applyUpdate() POSTs /api/update/apply with the asset URLs.
//   3. The device schedules the apply on its parallel task (returns 202),
//      streams progress over the `updateProgress` SSE channel, and reboots.
//
// All progress UI is driven by handleUpdateProgress().  The function is
// idempotent — if the page is reloaded mid-update, /api/update/status seeds
// the panel with whatever state the firmware is in.

let _otaUpdateInfo = null;  // last successful check result (for applyUpdate)

function setUpdateBusy(busy, label) {
  const btn = document.getElementById('checkUpdatesBtn');
  if (!btn) return;
  btn.disabled = !!busy;
  if (busy && label) btn.textContent = label;
  else if (!busy)    btn.textContent = 'Check for Updates';
}

function showUpdateStatus(message, progressPercent) {
  const panel = document.getElementById('updateStatusPanel');
  const msg   = document.getElementById('updateStatusMessage');
  const bar   = document.getElementById('updateProgressBar');
  if (panel) panel.style.display = '';
  if (msg)   msg.textContent = message || '—';
  if (bar && typeof progressPercent === 'number') {
    bar.style.width = Math.max(0, Math.min(100, progressPercent)) + '%';
  }
}

function hideUpdateStatus() {
  const panel = document.getElementById('updateStatusPanel');
  if (panel) panel.style.display = 'none';
}

// Mirror OtaManager::State enum from lib/OTA/ota.h.
const OTA_STATE = {
  IDLE: 0, CONNECTING: 1, CHECKING: 2, UPDATE_AVAILABLE: 3, UP_TO_DATE: 4,
  DOWNLOADING_FS: 5, DOWNLOADING_FW: 6, REBOOTING: 7, ERROR: 99,
};

function handleUpdateProgress(data) {
  if (!data) return;
  showUpdateStatus(data.message, data.progress);
  // Lock the Check button while the device is mid-flight.
  const busy = (data.state === OTA_STATE.CONNECTING ||
                data.state === OTA_STATE.CHECKING   ||
                data.state === OTA_STATE.DOWNLOADING_FS ||
                data.state === OTA_STATE.DOWNLOADING_FW ||
                data.state === OTA_STATE.REBOOTING);
  setUpdateBusy(busy);

  // Wake any in-flight checkForUpdates() awaiting a terminal state.  This is
  // the recovery path when the original /api/update/check HTTP response was
  // dropped by the AP-retune disconnect — SSE delivers the verdict instead.
  if (_otaCheckResolver &&
      (data.state === OTA_STATE.UP_TO_DATE ||
       data.state === OTA_STATE.UPDATE_AVAILABLE ||
       data.state === OTA_STATE.ERROR)) {
    const r = _otaCheckResolver;
    _otaCheckResolver = null;
    r({ ok: data.state !== OTA_STATE.ERROR, state: data.state, message: data.message });
  }

  if (data.state === OTA_STATE.REBOOTING) {
    // Device is about to drop the connection.  Show a clear note so the user
    // doesn't think the page is broken when SSE goes silent.
    setTimeout(() => {
      showUpdateStatus('Device is rebooting. Reconnect to FPVRaceOne_XXXX, then hard refresh this page (Ctrl-Shift-R) in 30–60 seconds.', 100);
    }, 500);
  }
}

// Promise resolver used by handleUpdateProgress() to wake up the in-flight
// checkForUpdates() when SSE delivers a terminal state.  Kept as a safety
// net for the rare case where the /api/update/check HTTP response is lost
// (network hiccup) — the SSE channel will still report UP_TO_DATE /
// UPDATE_AVAILABLE / ERROR and we resume the awaiter with that verdict.
let _otaCheckResolver = null;

// Promise that resolves when SSE reports a terminal state (>= UPDATE_AVAILABLE).
// Resolves to {ok: true,  state, message} on UP_TO_DATE / UPDATE_AVAILABLE.
// Resolves to {ok: false, state, message} on ERROR.
function _waitForCheckTerminal() {
  return new Promise((resolve) => { _otaCheckResolver = resolve; });
}

async function checkForUpdates() {
  const ssidEl = document.getElementById('ssid');
  if (!ssidEl || !ssidEl.value.trim()) {
    alert('Please set your Home WiFi SSID under Settings → Firmware Update first — the device needs it to reach GitHub.');
    return;
  }

  // Probe the backend for whether this device's mode would disrupt the mesh
  // during the check (master with connected pilots; client connected to a
  // master).  The extra paragraph is tucked into the splash dialog so the
  // director sees the per-mode warning before committing.
  let disruptionMsg = '';
  try {
    const dr = await fetch('/api/update/disruption-check');
    if (dr.ok) {
      const dj = await dr.json();
      if (dj && dj.wouldDisrupt && dj.message) disruptionMsg = dj.message;
    }
  } catch (_) { /* older firmware — leave disruptionMsg empty */ }

  // Splash dialog — the AP stays up during the check now, so no more
  // "browser will disconnect" scare paragraph.  The disruption block is
  // still shown when the device is a master with connected pilots or a
  // client attached to a master (mesh is briefly interrupted while the
  // device queries GitHub).
  const confirmed = await new Promise(resolve => {
    const overlay = document.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:9999;display:flex;align-items:center;justify-content:center;';
    const box = document.createElement('div');
    box.style.cssText = 'background:var(--card-bg,#fff);border-radius:10px;padding:28px 32px;max-width:500px;width:90%;box-shadow:0 8px 32px rgba(0,0,0,0.3);font-family:inherit;';
    const disruptionBlock = disruptionMsg
      ? `<div style="background:#fde6e6;border:1px solid #c0392b;border-radius:6px;padding:10px 14px;color:#5a0000;font-size:13px;margin-bottom:14px;line-height:1.5;">
           ${disruptionMsg}
         </div>` : '';
    box.innerHTML = `
      <h3 style="margin:0 0 12px;font-size:17px;color:#222;">Check for firmware updates?</h3>
      ${disruptionBlock}
      <p style="margin:0 0 20px;font-size:14px;color:#444;line-height:1.5;">
        The device will briefly join your home WiFi to query GitHub for the latest release. This usually takes 15–30 seconds.
      </p>
      <div style="display:flex;gap:12px;justify-content:flex-end;">
        <button id="_otaCancel"   style="padding:8px 20px;border-radius:6px;border:1px solid #aaa;background:#f5f5f5;color:#333;cursor:pointer;font-size:14px;">Cancel</button>
        <button id="_otaContinue" style="padding:8px 20px;border-radius:6px;border:none;background:var(--primary-color,#2196F3);color:#fff;cursor:pointer;font-size:14px;font-weight:600;">Continue</button>
      </div>`;
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    document.getElementById('_otaCancel').onclick   = () => { overlay.remove(); resolve(false); };
    document.getElementById('_otaContinue').onclick = () => { overlay.remove(); resolve(true);  };
  });
  if (!confirmed) return;

  setUpdateBusy(true, 'Checking…');
  showUpdateStatus('Connecting to home WiFi and contacting GitHub…', 10);
  // Progress updates from here on come via the updateProgress SSE channel,
  // which handleUpdateProgress() routes into showUpdateStatus() automatically.
  // No blocking working-overlay any more — the button "Checking…" state and
  // the Update Status panel are enough now that the AP stays up.

  // Arm the SSE terminal-state waiter BEFORE POSTing /api/update/check so we
  // can't miss the resolver arming on a fast device.  /api/update/check
  // returns 202 immediately (kicks off the check on the parallel task); the
  // real verdict arrives via the updateProgress SSE channel a few seconds
  // later.  The full info payload — including the options[] list of
  // installable versions — is then fetched from /api/update/status.
  const ssePromise = _waitForCheckTerminal();
  try {
    const r = await fetch('/api/update/check', { method: 'POST' });
    if (!r.ok) {
      const data = await r.json().catch(() => ({}));
      showUpdateStatus('Check failed: ' + (data.error || `HTTP ${r.status}`), 0);
      setUpdateBusy(false);
      _otaCheckResolver = null;
      return;
    }
  } catch (e) {
    // Fire-and-forget — the check runs on the parallel task even if the
    // HTTP response was lost.  Fall through to await the SSE terminal.
  }

  // Wait for the SSE-delivered terminal state (UP_TO_DATE, UPDATE_AVAILABLE,
  // or ERROR).  handleUpdateProgress() resolves this promise the moment the
  // OTA state machine reaches one of those.
  const terminal = await ssePromise;
  if (!terminal.ok) {
    showUpdateStatus('Check failed: ' + (terminal.message || 'unknown error'), 0);
    setUpdateBusy(false);
    return;
  }

  // Fetch the full info payload — /api/update/status returns the cached
  // UpdateInfo including the options[] list of installable versions.
  let info = null;
  try {
    const sr = await fetch('/api/update/status', { cache: 'no-store' });
    if (sr.ok) info = await sr.json();
  } catch (_) {}

  // Scroll the Update Status panel into view so the verdict isn't hidden
  // below the fold — deferred to the next frame so any showUpdateStatus()
  // that follows has applied its display:'' before scroll offsets compute.
  requestAnimationFrame(() => {
    const panel = document.getElementById('updateStatusPanel');
    if (panel && typeof panel.scrollIntoView === 'function') {
      panel.scrollIntoView({ behavior: 'smooth', block: 'end' });
    }
  });

  if (info && info.state === OTA_STATE.ERROR) {
    if (info.message) showUpdateStatus(info.message, 0);
    setUpdateBusy(false);
    return;
  }

  if (!info) {
    showUpdateStatus('Could not retrieve update info from the device.', 0);
    setUpdateBusy(false);
    return;
  }

  _otaUpdateInfo = info;

  // Render the inline version picker.  Even when the newest release is the
  // currently-installed version (no upgrade available), the picker lets the
  // user downgrade or re-install — so we render it whenever the device
  // returned at least one option, regardless of `available`.
  renderUpdateOptions(info);

  if (!info.available) {
    // Write the terminal verdict explicitly so the user sees the actual
    // outcome (e.g. "You're on the latest release" or "No releases
    // published yet") instead of the stale "Connecting to home WiFi…"
    // that showUpdateStatus() painted at the start of the check.
    //
    // For pre-release builds specifically, if the device reports no stable
    // release available, point the user at the Include Pre-releases toggle
    // — the most common cause of "no update found" on a -beta build is
    // GitHub's /releases/latest excluding pre-releases by design.  Only
    // append the hint if the toggle is OFF; if it's already ON, the user
    // already did the thing the hint asks for and the message is misleading
    // (the real cause then is "no pre-releases tagged yet, or all drafts").
    const onBeta = /-(?:beta|rc|alpha|dev|dirty)/i.test(info.currentVersion || '');
    const channelIsPrerelease = (document.getElementById('otaChannelSelect')?.value === '1');
    // Only suggest switching channel when the user is on a -beta build AND
    // they're looking at the Published channel (and got nothing).  If they're
    // already on Pre-releases and got nothing, the issue is "no pre-releases
    // tagged yet" and the channel suggestion would be wrong.
    const hintIfBeta = onBeta && !info.latestVersion && !channelIsPrerelease
      ? '\nYou\'re on a pre-release build — switch Release Channel to "Pre-releases" above to see beta / RC builds.'
      : '';
    const msg = (info.message || `You're on the latest version (${info.currentVersion || '?'}).`)
                + hintIfBeta;
    showUpdateStatus(msg, 100);
    setUpdateBusy(false);
    return;
  }

  // Update available — show the status banner and let the inline picker
  // drive the actual Install.  No popup confirm; the user chooses which
  // version to install (newest, older, or re-install current) from the
  // Available Versions list right below the status panel.
  showUpdateStatus(`Update available: ${info.latestVersion}. Pick a version below to install.`, 100);
  setUpdateBusy(false);
}

// Renders the inline version picker into #updateOptionsList.  Each row is
// one ReleaseOption from /api/update/status, with a colored badge keyed to
// kind (0=upgrade green, 1=current gray "Installed", 2=downgrade amber)
// and an Install button.  Downgrade clicks confirm() before applying so a
// stray click doesn't silently move the device backward.
function renderUpdateOptions(info) {
  const panel = document.getElementById('updateOptionsPanel');
  const list  = document.getElementById('updateOptionsList');
  if (!panel || !list) return;
  const options = Array.isArray(info && info.options) ? info.options : [];
  if (options.length === 0) {
    panel.style.display = 'none';
    list.innerHTML = '';
    return;
  }
  // Build rows.  innerHTML rather than DOM API because the row count is
  // tiny (≤5) and we control every interpolated value (tag from GitHub
  // releases is alphanumeric + dots/dashes, URLs are pre-validated by
  // the firmware before they hit this payload).
  const BADGES = {
    0: { label: 'Upgrade',   bg: '#2e7d32', fg: '#fff' },   // green
    1: { label: 'Installed', bg: '#5a5a5a', fg: '#fff' },   // gray
    2: { label: 'Downgrade', bg: '#b97400', fg: '#fff' },   // amber
  };
  const BUTTON_LABELS = { 0: 'Install', 1: 'Re-install', 2: 'Install' };
  list.innerHTML = options.map((o, idx) => {
    const b = BADGES[o.kind] || BADGES[0];
    const btn = BUTTON_LABELS[o.kind] || 'Install';
    const tag = String(o.tag || '').replace(/[<>&"]/g, c => ({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]));
    return (
      `<div style="display:flex; align-items:center; gap:10px; padding:8px 4px; border-top:${idx === 0 ? 'none' : '1px solid var(--bg-primary)'};">` +
        `<span style="font-family:monospace; font-size:13px; flex:1;">${tag}</span>` +
        `<span style="background:${b.bg}; color:${b.fg}; padding:2px 8px; border-radius:10px; font-size:11px; font-weight:600;">${b.label}</span>` +
        `<button type="button" onclick="applyUpdateOption(${idx})" class="update-btn update-btn-primary" style="padding:4px 12px; font-size:13px;">${btn}</button>` +
      `</div>`
    );
  }).join('');
  panel.style.display = '';
}

// Apply a specific option by its index in _otaUpdateInfo.options.  Wired up
// from each row's Install button by renderUpdateOptions().  Downgrades get
// an extra confirm() so a misclick can't silently move the device backward.
async function applyUpdateOption(idx) {
  const info = _otaUpdateInfo;
  const opt  = info && Array.isArray(info.options) ? info.options[idx] : null;
  if (!opt || !opt.firmwareUrl || !opt.filesystemUrl) {
    alert('That version is no longer available. Click Check for Updates again.');
    return;
  }
  if (opt.kind === 2) {
    if (!confirm(
      `Downgrade to ${opt.tag}?\n\n` +
      `This is OLDER than the version currently installed (${info.currentVersion}). ` +
      `Older builds may be missing features or bug fixes from the current build, and ` +
      `the saved config / pilot data formats may not be backward-compatible.\n\n` +
      `Proceed with downgrade?`
    )) return;
  } else if (opt.kind === 1) {
    if (!confirm(
      `Re-install the current version (${opt.tag})?\n\n` +
      `Useful for recovering from a corrupted filesystem partition. ` +
      `Your saved config / pilot data is preserved.\n\nProceed?`
    )) return;
  }
  await applyUpdate(opt);
}

async function applyUpdate(opt) {
  // opt may be a ReleaseOption from the picker, or omitted for the legacy
  // "apply the newest" path used by status-recovery flows that don't have
  // a picker selection yet.  Either way, both URLs must be present.
  const info  = _otaUpdateInfo;
  const fwUrl = (opt && opt.firmwareUrl)   || (info && info.firmwareUrl);
  const fsUrl = (opt && opt.filesystemUrl) || (info && info.filesystemUrl);
  const tag   = (opt && opt.tag)           || (info && info.latestVersion) || '';
  if (!fwUrl || !fsUrl) {
    alert('No update available to apply. Check for updates first.');
    return;
  }
  // Hide the picker as soon as an install starts so a stray second click on
  // a different row can't post a second /api/update/apply mid-flight.  It
  // re-shows after the next successful Check for Updates.
  const optsPanel = document.getElementById('updateOptionsPanel');
  if (optsPanel) optsPanel.style.display = 'none';
  setUpdateBusy(true, 'Updating…');
  showUpdateStatus(`Submitting update request for ${tag}…`, 0);
  try {
    const r = await fetch('/api/update/apply', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        firmwareUrl:   fwUrl,
        filesystemUrl: fsUrl,
      }),
    });
    const data = await r.json().catch(() => ({}));
    if (!r.ok && r.status !== 202) {
      const msg = data.error || `HTTP ${r.status}`;
      showUpdateStatus('Update failed: ' + msg, 0);
      setUpdateBusy(false);
      return;
    }
    // From here on, progress comes via the `updateProgress` SSE channel.
    showUpdateStatus('Update started. Watch this panel for progress…', 5);
  } catch (err) {
    console.error('[OTA] Apply failed:', err);
    showUpdateStatus('Update request failed: ' + (err.message || err), 0);
    setUpdateBusy(false);
  }
}

function populateFreqOutput() {
  if (!bandSelect || !channelSelect) return;

  const bandIndex = bandSelect.selectedIndex;
  const freqs = freqLookup[bandIndex] || [];

  // If no channels are available for this band (all 0s), show N/A safely
  if (channelSelect.options.length === 0 || channelSelect.selectedIndex < 0) {
    frequency = 0;
    freqOutput.textContent = `N/A`;
    return;
  }

  // IMPORTANT: channelSelect.value is the actual channel number "1".."8"
  const chanNum = parseInt(channelSelect.value, 10);   // 1..8
  if (!Number.isFinite(chanNum) || chanNum < 1 || chanNum > 8) {
    frequency = 0;
    freqOutput.textContent = `N/A`;
    return;
  }

  const chanIndex = chanNum - 1;                       // 0..7
  frequency = freqs[chanIndex] ?? 0;

  if (frequency === 0) {
    freqOutput.textContent = `N/A`;
    return;
  }

  freqOutput.textContent = `${frequency}`;
}




bcf.addEventListener("change", function handleChange(event) {
  populateFreqOutput();
  stageBandChan();
  autoSaveConfig();
});

// channel / band listeners
if (bandSelect) {
  bandSelect.addEventListener("change", function () {
    updateChannelOptionsForBand();
    populateFreqOutput();
  });
}

if (channelSelect) {
  channelSelect.addEventListener("change", function () {
    populateFreqOutput();
  });
}


// Add auto-save listeners for other inputs
if (announcerSelect) {
  announcerSelect.addEventListener('change', autoSaveConfig);
}
if (pilotNameInput) {
  pilotNameInput.addEventListener('input', autoSaveConfig);
}
const colorInput = document.getElementById('pilotColor');
if (colorInput) {
  colorInput.addEventListener('change', autoSaveConfig);
}
const batteryToggle = document.getElementById('batteryMonitorToggle');
if (batteryToggle) {
  batteryToggle.addEventListener('change', autoSaveConfig);
}

function updateAnnouncerRate(obj, value) {
  // Label preview is fine to update now — DOM-only, reverts on modal reopen.
  obj.parentElement.querySelector('span').textContent = parseFloat(value).toFixed(1);

  if (settingsLoading) {
    // Hydration: mirror the persisted value into the runtime state.
    announcerRate = parseFloat(value);
    audioAnnouncer.setRate(announcerRate);
    return;
  }

  // Defer the runtime rate change — dedup so a slider drag collapses to
  // a single apply on Save.
  deferApply('announcerRate', async () => {
    const v = parseFloat(document.getElementById('rate')?.value || '1.0');
    announcerRate = v;
    audioAnnouncer.setRate(v);
  });
  autoSaveConfig();
}

function updateMinLap(obj, value) {
  obj.parentElement.querySelector('span').textContent = parseFloat(value).toFixed(1) + 's';
  autoSaveConfig();
}

function updateAlarmThreshold(obj, value) {
  obj.parentElement.querySelector('span').textContent = parseFloat(value).toFixed(1) + 'v';
  autoSaveConfig();
}

function updateMaxLaps(obj, value) {
  // Label preview is fine to update now — DOM-only, reverts on modal reopen.
  const previewN = parseInt(value);
  obj.parentElement.querySelector('span').textContent = previewN === 0 ? 'Inf.' : String(previewN);

  if (settingsLoading) {
    // Hydration: sync the runtime global to the persisted value.
    maxLaps = previewN;
    return;
  }

  // Defer the runtime maxLaps change so a cancelled edit does not affect
  // the currently-armed race's stop condition.
  deferApply('maxLaps', async () => {
    const v = parseInt(document.getElementById('maxLaps')?.value || '0');
    maxLaps = v;
  });
  autoSaveConfig();
}

// function getAnnouncerVoices() {
//   $().articulate("getVoices", "#voiceSelect", "System Default Announcer Voice");
// }

// Shared AudioContext for beeps (reused to avoid iOS issues)
var beepAudioContext = null;

// iOS/Safari audio unlock on first user gesture.
// Web Speech + AudioContext both require an in-gesture unlock. Today
// audioAnnouncer.enable() is called from the config-load handler (no
// gesture), which leaves TTS and beeps silent for the FIRST race —
// only the second Start press supplies enough accumulated user
// gestures for iOS to relent.  Installing a document-level capture
// listener here means the user's very first tap anywhere (nav bar,
// tab, anything) primes both audio paths, so by the time they press
// Start audio is already unlocked.  Self-disarms after firing once.
let _iosAudioPrimed = false;
// Base64-encoded ~1 ms silent WAV.  Playing this through an <audio>
// element inside a user gesture is the canonical iOS Safari audio
// unlock — more reliable than oscillator-based tricks, which iOS
// sometimes doesn't count.  Same payload as audio-announcer.js.
const _IOS_SILENT_WAV = 'data:audio/wav;base64,UklGRigAAABXQVZFZm10IBIAAAABAAEARKwAAIhYAQACABAAAABkYXRhAgAAAAEA';
function _primeAudioOnGesture() {
  if (_iosAudioPrimed) return;
  _iosAudioPrimed = true;
  // 1. Play a silent <audio> element — the canonical iOS Safari unlock.
  try {
    const a = new Audio();
    a.src = _IOS_SILENT_WAV;
    a.volume = 0;
    const p = a.play();
    if (p && typeof p.catch === 'function') p.catch(() => {});
  } catch (_) {}
  // 2. Create + resume AudioContext + walk the graph with a zero-gain
  //    oscillator so WebAudio (used by beep()) is unlocked too.
  try {
    if (!beepAudioContext && typeof AudioContext !== 'undefined') {
      beepAudioContext = new AudioContext();
    }
    if (beepAudioContext) {
      if (beepAudioContext.state === 'suspended') beepAudioContext.resume();
      const osc  = beepAudioContext.createOscillator();
      const gain = beepAudioContext.createGain();
      gain.gain.value = 0;
      osc.connect(gain).connect(beepAudioContext.destination);
      osc.start();
      osc.stop(beepAudioContext.currentTime + 0.05);
    }
  } catch (_) {}
  // 3. Prime Web Speech.  iOS lazy-loads its voice list on first
  //    gesture-scoped getVoices() call — priming here avoids a stall
  //    when the first countdown utterance fires.  We don't rely on a
  //    silent utterance for TTS unlock — startRace() speaks the real
  //    "Arm your quad" synchronously in the click gesture instead,
  //    which is what iOS actually accepts.
  try {
    if ('speechSynthesis' in window) speechSynthesis.getVoices();
  } catch (_) {}
}
document.addEventListener('pointerdown', _primeAudioOnGesture, { capture: true });
document.addEventListener('touchstart',  _primeAudioOnGesture, { capture: true });
document.addEventListener('click',       _primeAudioOnGesture, { capture: true });

function beep(duration, frequency, type) {
  // Create or reuse AudioContext.  On iOS Safari the FIRST creation MUST
  // happen inside a user gesture — see startRace()'s AudioContext prime
  // block.  If we end up creating it here (no prior gesture), it starts
  // suspended and iOS won't let us resume it outside a gesture.
  if (!beepAudioContext) {
    try { beepAudioContext = new AudioContext(); } catch (_) { return; }
  }

  // Only play when the context is actually running.  The previous code
  // did resume().then(play), which — on iOS Safari — never fulfilled
  // when called mid-countdown (5 s after the tap), then fulfilled
  // instantly on the NEXT Start tap (a fresh gesture), causing a
  // phantom beep before "Arm your quad".  Dropping the beep silently
  // when the context isn't running eliminates the queued replay.
  if (beepAudioContext.state !== 'running') {
    console.warn('[Beep] context state=' + beepAudioContext.state + ', dropping beep');
    return;
  }
  playBeepTone(duration, frequency, type);
}

function playBeepTone(duration, frequency, type) {
  var oscillator = beepAudioContext.createOscillator();
  oscillator.type = type;
  oscillator.frequency.value = frequency;
  oscillator.connect(beepAudioContext.destination);
  oscillator.start();
  // Beep for specified duration
  setTimeout(function () {
    oscillator.stop();
  }, duration);
}

// Lap 0 row: one cell spanning Lap Time / Gap / Total Time.
//
// Lap 0 is the first gate crossing — where the race clock starts, not a lap
// that was flown — so none of those three columns has a value to show.  One
// label reads as "this is the start", where three dashes read as data that
// failed to arrive.
//
// Shared by both lap-row renderers (addLap and _restoreInProgressLaps) and so
// by all three modes, which all draw into the same #lapTable.  Nothing indexes
// row.cells, so the short row is safe.
function _renderFirstCrossingCell(row) {
  const cell = row.insertCell(1);
  // Spans Lap Time / Gap / Total Time AND the exclude column: lap 0 is never a
  // candidate for exclusion, so an empty button cell there would just invite
  // the click that does nothing.
  cell.colSpan = 4;
  cell.className = 'lap-first-crossing';
  cell.textContent = '1st Cross';
}

// Append the exclude-toggle cell to a real (non-zero) lap row.
function _renderExcludeCell(row, n) {
  const cell = row.insertCell(-1);
  cell.className = 'lap-exclude-col';
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'lap-exclude-btn';
  btn.onclick = () => toggleLapExcluded(n);
  cell.appendChild(btn);
  // Label, tooltip and row styling all come from one place so they cannot
  // disagree with the set.
  applyExcludedLapUI();
}

// Silently restore in-progress laps after page reload — no TTS, no state side-effects
function _restoreInProgressLaps(laps) {
  lapNo = -1;
  lapTimes = [];
  // Exclusions live in this tab only — the firmware has no concept of them, so
  // a page reload mid-race cannot recover which laps were set aside.  Clear
  // rather than carry a stale set onto a freshly rebuilt table.
  excludedLaps.clear();
  const table = document.getElementById('lapTable');
  if (table) while (table.rows.length > 1) table.deleteRow(1);
  let cumMs = 0;
  laps.forEach((l, idx) => {
    const newLap = l.lapTimeMs / 1000;
    lapTimes.push(newLap);
    // Prefer the lap's TRUE number from the firmware over its position in this
    // array.  They diverge whenever the race outran the firmware's lap ring and
    // the oldest laps were dropped — numbering by position would then relabel
    // lap 51 as lap 1 and quietly present a wrong race.  Older firmware omits
    // lapNumber, in which case position is all we have and is correct anyway.
    lapNo = Number.isFinite(l.lapNumber) ? l.lapNumber : idx;
    cumMs += l.lapTimeMs;
    if (table) {
      const row = table.insertRow();
      // data-lap-index stays the ARRAY index — it addresses lapTimes[], which
      // only holds what was restored.
      row.setAttribute('data-lap-index', idx);
      row.setAttribute('data-lap-number', lapNo);
      // Wrapped so CSS can draw the skewed lap badge — a <td> itself cannot be
      // transformed reliably.  See .lap-badge.
      const c1 = row.insertCell(0); c1.innerHTML = `<span class="lap-badge"><b>${lapNo}</b></span>`;
      if (lapNo === 0) {
        // The first gate crossing carries no lap time, gap or total — see the
        // matching branch in addLap().
        _renderFirstCrossingCell(row);
      } else {
        const c2 = row.insertCell(1);
        c2.innerHTML = formatMsDisplay(l.lapTimeMs);
        const gapMs = idx > 0 ? Math.round((newLap - lapTimes[idx - 1]) * 1000) : null;
        const c3 = row.insertCell(2);
        c3.innerHTML = gapMs === null ? '-' : formatMsGap(gapMs);
        const c4 = row.insertCell(3);
        // Cumulative time is only meaningful from lap 0.  If the list was
        // truncated it starts mid-race, so the running total would be wrong —
        // show a dash rather than a confidently incorrect number.
        c4.innerHTML = (lapNo !== idx) ? '-' : formatMsDisplay(cumMs);
        _renderExcludeCell(row, lapNo);
      }
    }
  });
  if (table) { highlightFastestLap(); updateLapCounter(); }
  updateAnalysisView();
}

// The lap callout, exactly as a live race makes it.
//
// Extracted from addLap() so Race History playback can produce IDENTICAL audio
// rather than an approximation of it: same announcer mode, same lap format,
// same phrasing, same formatter.  Two copies of this would drift the moment
// either was edited, and the whole point of playback is that it sounds like
// the race did.
//
// `lapSpeak` is the RAW numeric time string (e.g. "115.634") — the announcer
// converts it; see _preprocessTimeForTts.  `last2Str`/`last3Str` are the
// running 2- and 3-lap sums in seconds, or "" when this lap isn't on that
// cadence.
function announceLapCallout(pilotName, lapNo, lapSpeak, last2Str, last3Str) {
  const mode = announcerSelect?.options?.[announcerSelect.selectedIndex]?.value;
  switch (mode) {
    case "beep":
      beep(100, 330, "square");
      break;
    case "1lap":
      if (lapNo == 0) {
        queueSpeak(`<p>${pilotName} first crossing</p>`);
      } else {
        let text;
        switch (lapFormat) {
          case 'pilottime':
            text = `<p>${pilotName} ${lapSpeak}</p>`;
            break;
          case 'timeonly':
            text = `<p>${lapSpeak}</p>`;
            break;
          default:  // 'full', 'laptime'
            text = `<p>${pilotName} Lap ${lapNo}, ${lapSpeak}</p>`;
        }
        queueSpeak(text);
      }
      break;
    case "2lap":
      if (lapNo == 0) {
        queueSpeak(`<p>${pilotName} first crossing</p>`);
      } else if (last2Str) {
        queueSpeak(`<p>${pilotName} 2 laps ${formatMsSpeak(Math.round(parseFloat(last2Str) * 1000))}</p>`);
      }
      break;
    case "3lap":
      if (lapNo == 0) {
        queueSpeak(`<p>${pilotName} first crossing</p>`);
      } else if (last3Str) {
        queueSpeak(`<p>${pilotName} 3 laps ${formatMsSpeak(Math.round(parseFloat(last3Str) * 1000))}</p>`);
      }
      break;
    default:
      break;
  }
}

function addLap(lapStr) {
  const pilotName = pilotNameInput.value;
  
  const newLap = parseFloat(lapStr);
  lapNo += 1;
  lapTimes.push(newLap);

  lapTimerStartMs = Date.now();         // Reset lap timer
  // Remember WHERE in the race this crossing happened, so a later refresh (or
  // a repeated startRaceDisplayOnly) can re-derive the same lap origin.
  if (raceDisplayStartMs > 0) lastCrossingRaceMs = Date.now() - raceDisplayStartMs;

  // Calculate total time so far
  const totalMs = Math.round(lapTimes.reduce((sum, time) => sum + time, 0) * 1000);

  // Calculate gap from previous lap (for regular laps only, not gate 1)
  let gapMs = null;
  if (lapNo > 1) {
    gapMs = Math.round((newLap - lapTimes[lapTimes.length - 2]) * 1000);
  }
  
  const table = document.getElementById("lapTable");
  const row = table.insertRow();
  row.setAttribute('data-lap-index', lapTimes.length - 1);
  
  row.setAttribute('data-lap-number', lapNo);

  const cell1 = row.insertCell(0);  // Lap No
  // See _restoreInProgressLaps(): the badge needs its own element.
  cell1.innerHTML = `<span class="lap-badge"><b>${lapNo}</b></span>`;

  if (lapNo == 0) {
    // Lap 0 is the first gate crossing — the moment the clock starts, not a
    // timed lap.  Lap Time, Gap and Total Time have no value yet, and three
    // dashes read as missing data rather than as "nothing to show here".
    // Merge the three into one labelled cell.  Kept in step with the same
    // branch in _restoreInProgressLaps().
    _renderFirstCrossingCell(row);
  } else {
    const cell2 = row.insertCell(1);  // Lap Time
    const cell3 = row.insertCell(2);  // Gap
    const cell4 = row.insertCell(3);  // Total Time
    cell2.innerHTML = formatMsDisplay(Math.round(newLap * 1000));
    cell3.innerHTML = gapMs !== null ? formatMsGap(gapMs) : "-";
    cell4.innerHTML = formatMsDisplay(totalMs);
    _renderExcludeCell(row, lapNo);
  }
  
  // Highlight fastest lap
  highlightFastestLap();

  // "Every 2/3 laps" announcer modes speak the combined time of the last N laps,
  // on an N-lap cadence. These were previously read from undeclared variables
  // (last2lapStr / last3lapStr), throwing a ReferenceError that aborted addLap()
  // before the lap counter, analysis view, and max-lap auto-stop could run. Compute
  // them locally from lapTimes (seconds); empty string means "nothing to announce
  // this lap".
  let last2lapStr = "";
  let last3lapStr = "";
  if (lapNo >= 2 && lapNo % 2 === 0 && lapTimes.length >= 2) {
    last2lapStr = String(lapTimes[lapTimes.length - 1] + lapTimes[lapTimes.length - 2]);
  }
  if (lapNo >= 3 && lapNo % 3 === 0 && lapTimes.length >= 3) {
    last3lapStr = String(lapTimes[lapTimes.length - 1] +
                         lapTimes[lapTimes.length - 2] +
                         lapTimes[lapTimes.length - 3]);
  }

  // The audio-announcer's pattern matchers expect raw numeric time (e.g.
  // "12.34") so they can route the spoken time through speakNumber's
  // pre-recorded digit clips — those honor this.rate (audio.playbackRate)
  // reliably across browsers, unlike Web Speech which varies.
  // formatMsSpeak("12 point 3 4") would defeat that.
  const lapSpeak = lapStr;
  // Multi-tab sync (SINGLE mode only): only the initiator tab announces +
  // owns auto-stop.  Spectator tabs still update the lap table and counter
  // above so the display stays in sync — they just skip TTS and the
  // redundant stop POST.  In master/client modes this gate is bypassed
  // (announce as before) so pilot phones still speak their own laps
  // during coordinated races.
  const _syncGateActive = (mnNodeMode === 0);
  if (!_syncGateActive || _iAmRaceInitiator) {
    announceLapCallout(pilotName, lapNo, lapSpeak, last2lapStr, last3lapStr);
  }

  // Update lap counter
  updateLapCounter();

  // Update lap analysis
  updateAnalysisView();

  // Auto-stop race if max laps reached (excluding hole shot, and if maxLaps > 0).
  // In single mode, only the initiator sends /timer/stop (spectators resync
  // via raceState=stopped).  In master/client modes the initiator gate is
  // bypassed — behavior falls back to pre-change (every browser races on
  // its own auto-stop, same as before we added the sync).
  if ((mnNodeMode !== 0 || _iAmRaceInitiator) && maxLaps > 0 && lapNo > 0 && lapNo >= maxLaps) {
    setTimeout(function() {
      if (!stopRaceButton.disabled) {
        stopRace();
        queueSpeak('<p>Race complete</p>');
      }
    }, 500); // Small delay to allow lap announcement
  }
}

function startTimer() {
  // Defensive: clear any previous interval before creating a new one.
  // If a spectator race ended and stopRaceDisplayOnly ran correctly this
  // is a no-op, but if a stale interval reference is still ticking
  // (e.g. from a browser-specific event ordering) we'd otherwise orphan
  // it — stopRace() only holds the LATEST timerInterval reference, so a
  // leaked one would keep updating `timer` forever after Stop.
  clearInterval(timerInterval);
  const _timerStart = Date.now();
  timerInterval = setInterval(function () {
    // One formatter for every clock in the app — see formatMsDisplay().  The
    // hand-rolled version that used to live here was fixed at hundredths and
    // had no hour rollover, so it silently ignored the Lap Time Precision
    // setting and ran past 99 minutes without ever showing an hours field.
    timer.innerHTML = formatMsDisplay(Date.now() - _timerStart);
  }, 10);

  if (usbConnected && transportManager) {
    transportManager.sendCommand('timer/start', 'POST')
      .then((response) => console.log("/timer/start:", response))
      .catch(err => console.error('Failed to start timer:', err));
  } else {
    fetch("/timer/start", {
      method: "POST",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
      },
    })
      .then((response) => response.json())
      .then((response) => console.log("/timer/start:" + JSON.stringify(response)));
  }
}

function queueSpeak(obj) {
  if (!audioEnabled) {
    return;
  }
  audioAnnouncer.queueSpeak(obj);
}

// Start the visual race display without sending /timer/start to the server.
// Used when the master remotely starts the race on this client node.
function startRaceDisplayOnly(offsetMs = 0) {
  updateLapCounter();
  startRaceButton.disabled = true;
  startRaceButton.classList.add('active');
  stopRaceButton.disabled = false;
  addLapButton.disabled = false;
  // This function runs ONLY when the master started the race on this client,
  // so re-apply the lockout immediately after enabling — a client racing under
  // a director must not be able to inject a lap that syncs upstream.
  applyAddLapButtonUI();

  clearInterval(timerInterval);
  const _timerStart = Date.now() - offsetMs;
  // Published so the Race View can render from OUR OWN race clock rather than
  // from a directorState push that is up to ~1 s old by the time it lands (the
  // payload is built up to 250 ms before the fanout runs, and the fanout is
  // sequential, so a client late in the order receives it ~500 ms later still).
  // Extrapolating from the push cannot recover that — it has no idea how old
  // the payload was.  Our own timer is exact, and with scheduled starts it
  // agrees with the master by construction.
  rvOwnRaceStartMs = _timerStart;
  // ...and start ticking immediately.  rvStartTicker() was only being driven
  // by the directorState push, so the Race View clock did not begin updating
  // until that arrived — up to a full broadcast interval after we had actually
  // started racing.
  rvStartTicker();
  rvUpdateBanner();
  _startRaceReanchor();
  timerInterval = setInterval(function () {
    // Same single formatter as the other race clock — see formatMsDisplay().
    timer.innerHTML = formatMsDisplay(Date.now() - _timerStart);
  }, 10);

  // DERIVED, not stamped — so it is identical no matter how many times this
  // runs or in what order relative to the lap restore.  lastCrossingRaceMs is
  // 0 until laps are known, which is exactly right: with no crossing yet, the
  // current lap did begin at race start.
  raceDisplayStartMs   = _timerStart;
  lapTimerStartMs      = _timerStart + lastCrossingRaceMs;
  startLapTimerDisplay();
}

// Record where the last crossing sat in the race, from restored lap data, and
// re-derive the current-lap clock from it.
//
// Uses the lap's own raceElapsedMs rather than summing lap times: once the ring
// has evicted (100 laps), the retained times no longer add up to the elapsed
// race, and the sum would silently drift by however much was dropped.
function setLapTimerFromLastLap(laps) {
  lastCrossingRaceMs = 0;
  if (Array.isArray(laps) && laps.length) {
    const last = laps[laps.length - 1];
    if (typeof last?.raceElapsedMs === 'number') lastCrossingRaceMs = last.raceElapsedMs;
  }
  // Only meaningful once we know where race zero is.  If the race display has
  // not been started yet, startRaceDisplayOnly() will derive this itself.
  if (raceDisplayStartMs > 0) lapTimerStartMs = raceDisplayStartMs + lastCrossingRaceMs;
}

// Stop the visual race display without sending /timer/stop to the server.
// Used when the master remotely stops the race on this client node.
// Re-anchor the browser's race clock to THIS DEVICE's own elapsed.
//
// Every display anchor in this page is ultimately set when a message arrived —
// an SSE "started", a restore fetch — and message arrival is not the same
// instant on two machines.  The devices themselves are synced to a few ms (§8),
// so any visible disagreement between the master's window and a client's is
// introduced entirely by that transit difference, not by the timers.
//
// Same half-RTT correction the firmware uses for its clock probes: ask the
// device what its elapsed is, assume the reply describes the midpoint of the
// round trip, and re-derive the anchor from it.  Both browsers converge on
// their own device's truth, and because the devices agree with each other, the
// two windows then agree too.
//
// Corrections are single-digit milliseconds and applied to the ANCHOR, so the
// clock never jumps backwards on screen — it just stops drifting apart.
async function _reanchorRaceClockFromDevice() {
  try {
    const t0 = Date.now();
    const r  = await fetch('/api/mode');
    if (!r.ok) return;
    const t1 = Date.now();
    const d  = await r.json();
    if (!d.timerRunning) return;
    const elapsed = d.raceElapsedMs || 0;
    // The device observed `elapsed` at roughly the midpoint of the round trip.
    const observedAt = t0 + Math.round((t1 - t0) / 2);
    const anchor     = observedAt - elapsed;

    if (mnNodeMode === 1) {
      if (mnRaceTimerIntervalId) mnRaceStartMs = anchor;
    } else {
      // Shift the lap clock by the same correction so the current lap keeps
      // its position within the race rather than jumping by the adjustment.
      const delta = anchor - raceDisplayStartMs;
      raceDisplayStartMs = anchor;
      if (rvOwnRaceStartMs > 0) rvOwnRaceStartMs += delta;
      lapTimerStartMs = raceDisplayStartMs + lastCrossingRaceMs;
    }
  } catch (_) { /* transient — the next tick tries again */ }
}

function _startRaceReanchor() {
  if (raceReanchorTimer) return;
  // First correction shortly after the start, then slowly — this is trimming
  // milliseconds, not chasing anything that moves.
  setTimeout(_reanchorRaceClockFromDevice, 1500);
  raceReanchorTimer = setInterval(_reanchorRaceClockFromDevice, 20000);
}

function _stopRaceReanchor() {
  if (raceReanchorTimer) { clearInterval(raceReanchorTimer); raceReanchorTimer = null; }
}

function stopRaceDisplayOnly() {
  _stopRaceReanchor();
  clearInterval(timerInterval);
  rvOwnRaceStartMs = 0;   // fall the Race View back to the pushed elapsed
  // The final time STAYS on screen, exactly as it does on the master — whose
  // _mnStopTimer() only clears its interval and leaves the last painted value.
  // Blanking here meant a pilot's own display went to 00:00:00 the instant the
  // director stopped the race, discarding the one number they wanted to read.
  //
  // Nothing is lost by not clearing: both pre-arm handlers already zero this
  // element at the start of the next countdown, which is the right moment for
  // a clean slate.
  stopRaceButton.disabled  = true;
  startRaceButton.disabled = false;
  startRaceButton.classList.remove('active');
  addLapButton.disabled    = true;
  stopLapTimerDisplay();
  updateRaceDataButtonsVisibility();
}

function saveLapFormat() {
  const lapFormatSelect = document.getElementById('lapFormatSelect');
  if (lapFormatSelect) {
    lapFormat = lapFormatSelect.value;
    console.log('Lap format saved:', lapFormat);
    autoSaveConfig(); // Save to device
  }
}

function saveAnnouncerDecimals() {
  const sel = document.getElementById('announcerDecimalsSelect');
  if (sel) {
    announcerDecimals = parseInt(sel.value, 10) || 2;
    console.log('Lap time precision saved:', announcerDecimals);
    refreshLapTimeDisplays();
    autoSaveConfig(); // Save to device
  }
}

// Re-render everything that shows a lap time, so changing the precision takes
// effect on rows ALREADY on screen.  Without this the table keeps a mix of old
// and new formatting until the next lap or a page reload, which reads as a bug.
// Each block is guarded independently — the settings page can be open while the
// race view or multi-node tab has never been initialised.
function refreshLapTimeDisplays() {
  try {
    // Rebuild from the in-memory lap times. Values are stored in seconds, and
    // _restoreInProgressLaps() wants {lapTimeMs}, so convert back.
    if (Array.isArray(lapTimes) && lapTimes.length > 0 &&
        typeof _restoreInProgressLaps === 'function') {
      _restoreInProgressLaps(lapTimes.map(sec => ({ lapTimeMs: Math.round(sec * 1000) })));
    }
  } catch (_) {}
  try { if (typeof updateStatsBoxes === 'function') updateStatsBoxes(); } catch (_) {}
  try {
    if (typeof mnRenderRaceTab === 'function' && Array.isArray(mnCurrentNodes)) {
      mnRenderRaceTab(mnCurrentNodes);
    }
  } catch (_) {}
  try { if (typeof rvRender === 'function') rvRender(); } catch (_) {}
}

function hideRaceDownloadReminder() {
  const banner = document.getElementById("raceTabDownloadReminder");
  if (banner) {
    banner.style.display = "none";
    sessionStorage.setItem("hideRaceDownloadReminder", "1");
  }
}

// Dismiss + persist: same as the regular Dismiss button, plus flips the
// "Always hide download reminder banner" toggle in Settings so the banner
// stays hidden across page reloads and reconnects.  Mirrors the inline
// onchange handler on #alwaysHideBannerToggle (localStorage flag +
// sessionStorage cleanup + applyRaceHistoryModeUI) so the Settings tab
// reflects the change immediately if the user navigates there next.
function dismissAlwaysRaceDownloadReminder() {
  localStorage.setItem("alwaysHideRaceBanner", "1");
  sessionStorage.removeItem("hideRaceDownloadReminder");
  const toggle = document.getElementById("alwaysHideBannerToggle");
  if (toggle) toggle.checked = true;
  const label  = document.getElementById("alwaysHideBannerLabel");
  if (label)  label.textContent = "On";
  hideRaceDownloadReminder();
  applyRaceHistoryModeUI();
}

function saveVoiceSelection() {
  const voiceSelect = document.getElementById('voiceSelect');
  if (voiceSelect) {
    selectedVoice = voiceSelect.value;
    console.log('Voice selection saved:', selectedVoice);
    
    // Update audioAnnouncer voice (this clears cache and updates voice directory)
    if (audioAnnouncer) {
      audioAnnouncer.setVoice(selectedVoice);
    }
    
    // If PiperTTS selected, use piper engine, otherwise use webspeech for fallback
    if (selectedVoice === 'piper') {
      if (audioAnnouncer) {
        audioAnnouncer.setTtsEngine('piper');
      }
    } else {
      // ElevenLabs voices use webspeech for fallback
      if (audioAnnouncer) {
        audioAnnouncer.setTtsEngine('webspeech');
      }
    }
    
    autoSaveConfig(); // Save to device
  }
}

function updateVoiceButtons() {
  // Voice control switched from two side-by-side buttons to a single
  // checkbox-styled switch.  Keep this function name so existing call sites
  // still work; it just syncs the new <input> + label to audioEnabled.
  const toggle = document.getElementById('voiceEnabledToggle');
  const label  = document.getElementById('voiceEnabledLabel');
  if (toggle) toggle.checked = !!audioEnabled;
  if (label)  label.textContent = audioEnabled ? 'On' : 'Off';
}

// Onchange dispatcher for the Voice toggle.  Routes to the existing
// enableAudioLoop / disableAudioLoop so persistence + announcer state stay
// identical to the old two-button flow.
function onVoiceEnabledToggleChange(checked) {
  if (checked) {
    enableAudioLoop().catch(e => console.error('[Script] enableAudioLoop failed:', e));
  } else {
    disableAudioLoop().catch(e => console.error('[Script] disableAudioLoop failed:', e));
  }
}

async function enableAudioLoop() {
  console.log('[Script] Enabling audio...');

  // Runtime enable
  audioEnabled = true;
  try {
    audioAnnouncer.enable();
  } catch (e) {
    console.error('[Script] audioAnnouncer.enable() failed:', e);
  }
  updateVoiceButtons();

  // Immediate persist (flash)
  try {
    await saveVoiceEnabledImmediate(true);

    // Keep UI model in sync (optional but recommended)
    if (typeof baselineConfig === 'object' && baselineConfig) baselineConfig.voiceEnabled = 1;
    if (typeof configData === 'object' && configData) configData.voiceEnabled = 1;

    console.log('[Script] Audio enabled + persisted (voiceEnabled=1)');
  } catch (e) {
    console.error('[Script] Failed to persist voiceEnabled=1:', e);
  }
}

async function disableAudioLoop() {
  console.log('[Script] Disabling audio...');

  // Runtime disable
  audioEnabled = false;
  try {
    if (audioAnnouncer && typeof audioAnnouncer.disable === 'function') {
      audioAnnouncer.disable();
    }
  } catch (e) {
    console.error('[Script] audioAnnouncer.disable() failed:', e);
  }
  updateVoiceButtons();

  // Immediate persist (flash)
  try {
    await saveVoiceEnabledImmediate(false);

    // Keep UI model in sync (optional but recommended)
    if (typeof baselineConfig === 'object' && baselineConfig) baselineConfig.voiceEnabled = 0;
    if (typeof configData === 'object' && configData) configData.voiceEnabled = 0;

    console.log('[Script] Audio disabled + persisted (voiceEnabled=0)');
  } catch (e) {
    console.error('[Script] Failed to persist voiceEnabled=0:', e);
  }
}

// Pilot color preview
function updateColorPreview() {
  const colorSelect = document.getElementById('pilotColor');
  const colorPreview = document.getElementById('colorPreview');
  if (colorSelect && colorPreview) {
    colorPreview.style.backgroundColor = colorSelect.value;
  }
}

// Battery monitoring toggle
function toggleBatteryMonitor(enabled) {
  const batterySection = document.getElementById('batteryMonitoringSection');
  const batteryToggle = document.getElementById('batteryMonitorToggle');

  if (batteryToggle) batteryToggle.checked = !!enabled;
  if (batterySection) batterySection.style.display = enabled ? 'block' : 'none';

  autoSaveConfig();
}


// Generic fetch wrapper that works with both WiFi and USB
async function transportFetch(url, options = {}) {
  const method = options.method || 'GET';
  const path = url.startsWith('/') ? url.substring(1) : url;
  
  if (usbConnected && transportManager) {
    // Parse body if JSON
    let data = null;
    if (options.body) {
      if (options.headers && options.headers['Content-Type'] === 'application/json') {
        data = JSON.parse(options.body);
      } else if (options.body instanceof URLSearchParams || typeof options.body === 'string') {
        // Parse form data
        const params = new URLSearchParams(options.body);
        data = Object.fromEntries(params.entries());
      }
    }
    
    return transportManager.sendCommand(path, method, data);
  } else {
    // Standard WiFi fetch
    const response = await fetch(url, options);
    if (options.headers && options.headers['Accept'] === 'application/json') {
      return response.json();
    } else {
      return response.text();
    }
  }
}

// Helper function for LED commands
async function sendLedCommand(endpoint, params) {
  if (usbConnected && transportManager) {
    return transportManager.sendCommand(`led/${endpoint}`, 'POST', params);
  } else {
    const body = Object.entries(params).map(([k, v]) => `${k}=${v}`).join('&');
    const response = await fetch(`/led/${endpoint}`, {
      method: 'POST',
      headers: {
        'Accept': 'application/json',
        'Content-Type': 'application/x-www-form-urlencoded',
      },
      body: body
    });
    return response.json();
  }
}

// LED control functions
// Update LED preset UI only (show/hide color sections, speed settings)
function updateLedPresetUI() {
  const presetSelect = document.getElementById('ledPreset');
  const solidColorSection = document.getElementById('solidColorSection');
  const fadeColorSection = document.getElementById('fadeColorSection');
  const strobeColorSection = document.getElementById('strobeColorSection');
  const speedSection = document.getElementById('ledSpeedSection');
  const speedNote = document.getElementById('ledSpeedNote');
  const preset = parseInt(presetSelect.value);
  
  // Show/hide color pickers based on preset
  if (solidColorSection) {
    solidColorSection.style.display = (preset === 1) ? 'flex' : 'none';
  }
  if (fadeColorSection) {
    fadeColorSection.style.display = (preset === 3) ? 'flex' : 'none';
  }
  if (strobeColorSection) {
    strobeColorSection.style.display = (preset === 7) ? 'flex' : 'none';
  }
  
  // Hide animation speed for Solid Colour (preset 1), Off (preset 0), and Pilot Colour (preset 9)
  const hideSpeed = (preset === 0 || preset === 1 || preset === 9);
  if (speedSection) {
    speedSection.style.display = hideSpeed ? 'none' : 'flex';
  }
  if (speedNote) {
    speedNote.style.display = hideSpeed ? 'none' : 'block';
  }
}

function changeLedPreset() {
  const presetSelect = document.getElementById('ledPreset');
  const preset = parseInt(presetSelect.value, 10);

  // UI affordance (hides/shows the Speed slider for presets that ignore it)
  // is fine to run immediately — it's reversible local DOM.
  updateLedPresetUI();

  // Defer the /led/* commit until Apply & Save.  Reads the relevant color
  // input at apply time (not now) so the user can adjust both the preset
  // and its color before saving and only one /led command lands.
  if (settingsLoading) {
    // Just sync staged value during modal hydration; don't queue an apply.
    stageConfig('ledPreset', Number.isFinite(preset) ? preset : 0);
    return;
  }
  deferApply('ledPreset', async () => {
    try {
      const p = parseInt(document.getElementById('ledPreset')?.value || '0', 10);
      if (p === 9) {
        const pilotColor = document.getElementById('pilotColor')?.value || '#0080FF';
        await sendLedCommand('color', { color: pilotColor.substring(1) });
      } else if (p === 2) {
        const c = (document.getElementById('ledSolidColor')?.value || '#FF00FF').substring(1);
        await sendLedCommand('color', { color: c });
      } else if (p === 6) {
        const c = (document.getElementById('ledFadeColor')?.value || '#0080FF').substring(1);
        await sendLedCommand('fadecolor', { color: c });
      } else if (p === 7) {
        const c = (document.getElementById('ledStrobeColor')?.value || '#FFFFFF').substring(1);
        await sendLedCommand('strobecolor', { color: c });
      }
    } catch (err) {
      console.error('[LED] Failed to apply preset on save:', err);
    }
  });

  stageConfig('ledPreset', Number.isFinite(preset) ? preset : 0);
}


// LED color/brightness/speed are intentional live-preview controls.  They
// apply to the device immediately AND auto-persist via saveConfigPatchImmediate
// — this matches the design ("see the change now and don't lose it on close")
// without dirtying the Apply & Save button.

function setSolidColor() {
  const colorInput = document.getElementById('ledSolidColor');
  const colorHex = colorInput.value.substring(1);

  sendLedCommand('color', { color: colorHex })
    .catch(err => console.error('Failed to change LED solid color:', err));

  const ledColor = parseInt(colorHex, 16);
  if (!settingsLoading) saveConfigPatchImmediate({ ledColor });
}

function setFadeColor() {
  const colorInput = document.getElementById('ledFadeColor');
  const colorHex = colorInput.value.substring(1);

  sendLedCommand('fadecolor', { color: colorHex })
    .catch(err => console.error('Failed to change LED fade color:', err));

  const ledFadeColor = parseInt(colorHex, 16);
  if (!settingsLoading) saveConfigPatchImmediate({ ledFadeColor });
}

function setStrobeColor() {
  const colorInput = document.getElementById('ledStrobeColor');
  const colorHex = colorInput.value.substring(1);

  sendLedCommand('strobecolor', { color: colorHex })
    .catch(err => console.error('Failed to change LED strobe color:', err));

  const ledStrobeColor = parseInt(colorHex, 16);
  if (!settingsLoading) saveConfigPatchImmediate({ ledStrobeColor });
}

function updateLedBrightness(obj, value) {
  const brightness = parseInt(value, 10);
  obj.parentElement.querySelector('span').textContent = brightness;

  sendLedCommand('brightness', { brightness })
    .catch(err => console.error('Failed to change LED brightness:', err));

  const ledBrightness = Number.isFinite(brightness) ? brightness : 128;
  if (!settingsLoading) saveConfigPatchImmediate({ ledBrightness });
}


function updateLedSpeed(obj, value) {
  const speed = parseInt(value, 10);
  obj.parentElement.querySelector('span').textContent = speed;

  sendLedCommand('speed', { speed })
    .catch(err => console.error('Failed to change LED speed:', err));

  const ledSpeed = Number.isFinite(speed) ? speed : 10;
  if (!settingsLoading) saveConfigPatchImmediate({ ledSpeed });
}

function toggleLedManualOverride(enabled) {
  const enable = enabled ? 1 : 0;

  // Defer the /led/override commit until Apply & Save — flipping the
  // toggle should NOT push device-side override on/off if the user
  // ultimately cancels.
  if (!settingsLoading) {
    deferApply('ledManualOverride', async () => {
      try {
        const el = document.getElementById('ledManualOverride');
        const v = (el && el.checked) ? 1 : 0;
        await sendLedCommand('override', { enable: v });
        console.log('LED manual override:', v ? 'enabled' : 'disabled');
      } catch (err) {
        console.error('Failed to toggle LED manual override on save:', err);
      }
    });
  }

  stageConfig('ledManualOverride', enable);
}

function toggleGateLEDs(enabled) {
  const optionsDiv = document.getElementById('gateLEDOptions');
  if (optionsDiv) {
    optionsDiv.style.display = enabled ? 'block' : 'none';
  }

  // Stage-only (no POST here)
  const gateLEDsEnabledToggle = document.getElementById('gateLEDsEnabled');
  if (gateLEDsEnabledToggle) gateLEDsEnabledToggle.checked = !!enabled;

  autoSaveConfig();
}


function toggleWebhookRaceStart(enabled) {
  const el = document.getElementById('webhookRaceStart');
  if (el) el.checked = !!enabled;
  autoSaveConfig();
}


function toggleWebhookRaceStop(enabled) {
  const el = document.getElementById('webhookRaceStop');
  if (el) el.checked = !!enabled;
  autoSaveConfig();
}

function toggleWebhookLap(enabled) {
  const el = document.getElementById('webhookLap');
  if (el) el.checked = !!enabled;
  autoSaveConfig();
}

function generateAudio() {
  if (!audioEnabled) {
    return;
  }

  const pilotName = pilotNameInput.value;
  queueSpeak(`<div>Testing sound for pilot ${pilotName}</div>`);
  for (let i = 1; i <= 3; i++) {
    queueSpeak('<div>' + i + '</div>')
  }
}

function doSpeak(obj) {
  audioAnnouncer.queueSpeak(obj);
}

function updateLapCounter() {
  if (maxLaps === 0) {
    lapCounter.textContent = `Lap ${Math.max(0, lapNo)}`;
  } else {
    lapCounter.textContent = `Lap ${Math.max(0, lapNo)} / ${maxLaps}`;
  }
  updateLapTablePlaceholder();
}

// A header row on its own reads as a broken table, so an empty lap list shows
// one placeholder row instead.
//
// The row is created and removed here rather than sitting in index.html,
// because every path that clears the table does it with
// `for (i = 1; i < rows.length; i++) deleteRow(1)` — a static placeholder would
// be swept away by the first clear and never come back.
//
// Every one of those clear paths calls updateLapCounter() immediately
// afterwards, and so does addLap(), which is why this hangs off that function.
function updateLapTablePlaceholder() {
  const table = document.getElementById('lapTable');
  if (!table) return;

  const hasLaps = !!table.querySelector('tr[data-lap-index]');
  const existing = document.getElementById('lapPlaceholderRow');

  if (hasLaps) {
    if (existing) existing.remove();
    return;
  }
  if (existing) return;

  const row = table.insertRow();   // no index: appends after the header row
  row.id = 'lapPlaceholderRow';
  row.className = 'lap-placeholder';
  row.innerHTML = '<td><span class="lap-badge"><b>&mdash;</b></span></td>'
                + '<td>&ndash;</td><td>&ndash;</td><td>&ndash;</td>'
                + '<td class="lap-exclude-col"></td>';
}

function highlightFastestLap() {
  if (lapTimes.length === 0) return;
  
  // Find fastest lap (excluding gate 1 at index 0)
  let fastestTime = Infinity;
  let fastestIndex = -1;
  
  for (let i = 1; i < lapTimes.length; i++) {  // Start from 1 to skip gate 1
    // An excluded lap can't hold the fastest-lap highlight either — the
    // highlight and the Fastest statistic must always name the same lap.
    if (isLapExcluded(i)) continue;
    if (lapTimes[i] < fastestTime) {
      fastestTime = lapTimes[i];
      fastestIndex = i;
    }
  }
  
  // Remove highlight from all rows
  const table = document.getElementById("lapTable");
  for (let i = 1; i < table.rows.length; i++) {
    table.rows[i].classList.remove('fastest-lap');
  }
  
  // Add highlight to fastest lap row
  if (fastestIndex >= 0) {
    for (let i = 1; i < table.rows.length; i++) {
      const row = table.rows[i];
      const lapIndex = parseInt(row.getAttribute('data-lap-index'));
      if (lapIndex === fastestIndex) {
        row.classList.add('fastest-lap');
        break;
      }
    }
  }
}

// Shared pre-race countdown used by all modes (single, master, client).
// Speaks armPhrase → "Starting in" → 5 … 4 … 3 … 2 … 1, each count ~1 s apart.
// Returns true if countdown completed, false if aborted via _raceCountdownAborted.
async function _raceCountdown(armPhrase) {
  const _wait = () => new Promise(r => setTimeout(r, 50));
  const _speechDone = async () => {
    // Belt-and-suspenders: never poll forever.  If the announcer ever wedges
    // (iOS Safari has been known to drop utterance.onend after an interrupted
    // speak), we'd rather bail out of the wait and continue than leave the
    // countdown flashing forever with no way to recover except a page reload.
    const t0 = Date.now();
    // Also poll speechSynthesis.speaking/pending so the wait honors a
    // direct speechSynthesis.speak() call (used by startRace's iOS TTS
    // unlock) in addition to audioAnnouncer's internal queue.
    const ttsBusy = () => (typeof speechSynthesis !== 'undefined')
        && (speechSynthesis.speaking || speechSynthesis.pending);
    while (audioAnnouncer.isSpeaking() || audioAnnouncer.audioQueue.length > 0 || ttsBusy()) {
      if (_raceCountdownAborted) return;
      if (Date.now() - t0 > 15000) {
        console.warn('[Race] _speechDone timeout — proceeding without wait');
        return;
      }
      await _wait();
    }
  };

  // Empty armPhrase means the caller (startRace on iOS) already fired a
  // gesture-scoped speechSynthesis.speak() directly for the arm phrase.
  // Don't queue it again — just wait for it (and any direct utterance)
  // to finish before moving to "Starting in 5".
  if (armPhrase) queueSpeak(`<p>${armPhrase}</p>`);
  await _speechDone();
  if (_raceCountdownAborted) return false;
  // "Starting in 5" as one utterance — no inter-utterance gap from Web Speech API.
  // We used to wait an extra 1000 ms here on top of the speech-done time, which
  // made the gap between "five" and the next-spoken "four" feel like ~2 s.
  // Drop the extra pause so the rhythm matches the rest of the countdown (the
  // 4→3→2→1 loop already pads each iteration to a 1 s cadence).
  queueSpeak("<p>Starting in 5</p>");
  await _speechDone();
  if (_raceCountdownAborted) return false;

  for (let i = 4; i >= 1; i--) {
    if (_raceCountdownAborted) return false;
    const t0 = Date.now();
    queueSpeak(`<p>${i}</p>`);
    await _speechDone();
    if (_raceCountdownAborted) return false;
    const pad = 1000 - (Date.now() - t0);
    if (pad > 0) await new Promise(r => setTimeout(r, pad));
    if (_raceCountdownAborted) return false;
  }
  return true;
}

async function startRace() {
  // Warn client pilots who start solo while not connected to master
  if (mnNodeMode === 2 && !mnMasterConnected) {
    if (!confirm('Due to additional overhead, it is not recommended to run solo races in client mode while not connected to a master node. Continue?')) {
      return;
    }
  }

  // Offer to clear existing lap data before starting.
  //
  // Yes / No / Cancel: the old confirm() offered only "clear and start" or
  // "keep and start", with no way out once the dialog was up.  Cancel returns
  // before anything is disabled and before /timer/prearm is fired below, so it
  // genuinely backs out rather than starting a race you did not want.
  if (lapTimes.length > 0) {
    const choice = await _showThreeOptionModal(
      'You have existing lap data. Clear it before starting?',
      'Yes', 'No', 'Cancel'
    );
    if (choice === 2) return;   // Cancel — no pre-arm, no state change
    if (choice === 0) clearLaps();
  }

  updateLapCounter();
  _raceCountdownAborted = false;
  _iAmRaceInitiator = true;   // this tab owns the announcer for this race
  _localRaceStartTs = Date.now();
  startRaceButton.disabled = true;
  startRaceButton.classList.add('active');
  stopRaceButton.disabled = false;  // allow cancelling during countdown
  // The console's run-state pill keys off stopRaceButton.disabled, and every
  // other call site for this is on a stop/clear path — without this the pill
  // would only ever be updated on the way OUT of a race.
  updateRaceDataButtonsVisibility();

  // iOS Safari: fire the FIRST TTS call DIRECTLY and SYNCHRONOUSLY here,
  // still inside the click gesture.  Going through audioAnnouncer's
  // queueSpeak → processQueue → speak → playWebSpeech chain adds enough
  // async microtasks that iOS treats the actual speechSynthesis.speak()
  // call as outside the gesture and silently drops it (verified: HUD
  // showed speaking=false pending=false, no onstart/onend).  Speaking
  // the real "Arm your quad" utterance here at real volume unlocks TTS
  // for the rest of the countdown AND doubles as the announcement.
  // _raceCountdown is called with '' below so it skips its own queueSpeak
  // of the arm phrase (would cause a double-speak).
  let _spokenDirectly = false;
  if (audioEnabled && 'speechSynthesis' in window) {
    try {
      const u = new SpeechSynthesisUtterance('Arm your quad');
      // Use the announcer's configured rate if available so cadence matches.
      if (audioAnnouncer && typeof audioAnnouncer.rate === 'number') u.rate = audioAnnouncer.rate;
      speechSynthesis.speak(u);
      _spokenDirectly = true;
    } catch (_) {}
  }

  // Fire prearm in single (0) AND client (2) modes so the server can broadcast
  // raceState=prearming to any other tab viewing this unit for display sync.
  // Master (1) uses its own masterRaceState path via mnStartRace, so skip it.
  if (mnNodeMode !== 1) fetch('/timer/prearm', { method: 'POST' }).catch(() => {});

  // iOS/Safari: create + resume the AudioContext INSIDE the user gesture
  // so iOS lets us play beeps 5 s later without a fresh tap.  Skipping
  // the creation here is what caused the silent-start-beep bug — beep()
  // would then create it mid-countdown (no gesture) and iOS parked it
  // suspended for good.  Also prime with a zero-gain oscillator so the
  // audio graph is actually walked before the real start beep fires.
  try {
    if (!beepAudioContext) beepAudioContext = new AudioContext();
    if (beepAudioContext.state === 'suspended') await beepAudioContext.resume();
    if (beepAudioContext.state === 'running') {
      const silentOsc  = beepAudioContext.createOscillator();
      const silentGain = beepAudioContext.createGain();
      silentGain.gain.value = 0;
      silentOsc.connect(silentGain).connect(beepAudioContext.destination);
      silentOsc.start();
      silentOsc.stop(beepAudioContext.currentTime + 0.05);
    }
  } catch (err) {
    console.warn('[Race] AudioContext prime failed:', err);
  }

  const completed = await _raceCountdown(_spokenDirectly ? '' : 'Arm your quad');
  if (!completed) {
    // Countdown was aborted — reset button state without starting
    startRaceButton.disabled = false;
    startRaceButton.classList.remove('active');
    stopRaceButton.disabled = true;
    return;
  }

  // Play start beep and begin race
  beep(1, 1, "square"); // needed for some reason to make sure we fire the first beep
  beep(500, 880, "square");

  // Vibrate for mobile devices (works even in silent mode on iOS)
  if (navigator.vibrate) {
    navigator.vibrate(500); // 500ms vibration
  }

  startTimer();
  startRaceButton.classList.remove('active');
  stopRaceButton.disabled = false;
  addLapButton.disabled = false;
  // Every site that enables this button re-applies the gate immediately after.
  // Keeping that uniform is cheaper than reasoning about which paths a client
  // under a race director can reach — the call only ever forces disabled, so
  // it is harmless where the lockout does not apply.
  applyAddLapButtonUI();

  // A fresh race: no crossing yet, so the current lap begins at race zero.
  raceDisplayStartMs = Date.now();
  lastCrossingRaceMs = 0;
  lapTimerStartMs    = raceDisplayStartMs;

  startLapTimerDisplay();
}

function stopRace() {
  if (mnMasterRaceActive) {
    if (!confirm('The race director started this race. Stopping will record a DNF for you. Quit race anyway?')) return;
  }
  _raceCountdownAborted = true;
  // Clear any queued audio to prevent race start sounds
  if (audioAnnouncer) {
    audioAnnouncer.clearQueue();
  }
  queueSpeak('<p>Race stopped</p>');
  clearInterval(timerInterval);
  timer.innerHTML = "00:00:00s";

  if (usbConnected && transportManager) {
    transportManager.sendCommand('timer/stop', 'POST')
      .then((response) => console.log("/timer/stop:", response))
      .catch(err => console.error('Failed to stop timer:', err));
  } else {
    fetch("/timer/stop", {
      method: "POST",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
      },
    })
      .then((response) => response.json())
      .then((response) => console.log("/timer/stop:" + JSON.stringify(response)));
  }

  stopRaceButton.disabled = true;
  startRaceButton.disabled = false;
  addLapButton.disabled = true;

  // Quit notification is handled server-side: /timer/stop sets _quitPending,
  // which process() forwards to the master. No JS call needed here.
  if (mnMasterRaceActive) mnMasterRaceActive = false;

  stopLapTimerDisplay();

  // Show Download/Transfer buttons now that race is stopped
  updateRaceDataButtonsVisibility();

  // Note: Race data remains visible after stopping.
  // Use "Transfer to Race History" button to save it, or "Clear Laps" to remove it.
  // Race data is NOT automatically transferred to Race History anymore.
}

function clearLaps() {
  // Note: Race data is NOT automatically saved to Race History.
  // If you want to save before clearing, use "Transfer to Race History" button first.

  // Clear server-side lap storage so a page reload doesn't re-populate the table.
  // Master mode uses /api/multinode/clearLaps which already handles this; skip for master.
  if (mnNodeMode !== 1) {
    fetch('/timer/clearLaps', { method: 'POST' }).catch(() => {});
  }

  var tableHeaderRowCount = 1;
  var rowCount = lapTable.rows.length;
  for (var i = tableHeaderRowCount; i < rowCount; i++) {
    lapTable.deleteRow(tableHeaderRowCount);
  }
  lapNo = -1;
  lapTimes = [];
  excludedLaps.clear();
  updateLapCounter();

  // Clear lap analysis
  document.getElementById('analysisContent').innerHTML = 
    '<p class="no-data">Complete at least 1 lap to see analysis</p>';
  document.getElementById('statFastest').textContent = '--';
  document.getElementById('statFastestLapNo').textContent = '';
  document.getElementById('statFastest3Consec').textContent = '--';
  document.getElementById('statFastest3ConsecLaps').textContent = '';
  document.getElementById('statMedian').textContent = '--';
  document.getElementById('statBest3').textContent = '--';
  document.getElementById('statBest3Laps').textContent = '';

  // Hide race data buttons when no data
  updateRaceDataButtonsVisibility();
}

function updateRaceDataButtonsVisibility() {
  const buttonsDiv = document.getElementById('raceDataButtons');
  if (buttonsDiv) {
    // Only show buttons if we have lap data AND the race is stopped (stop button is disabled)
    const raceIsStopped = stopRaceButton.disabled;
    // 'flex', not 'block': #raceDataButtons is a .race-actions row, and an
    // inline display:block would beat the class and stack the buttons.
    buttonsDiv.style.display = (lapTimes.length > 0 && raceIsStopped) ? 'flex' : 'none';
  }
  // clearLaps() reaches here but not updateLapCounter(), so the placeholder is
  // restored from this path too.
  updateLapTablePlaceholder();
  // The console's run-state pill reads the same signal the buttons do: Stop is
  // enabled exactly while a race is live.
  const statePill = document.getElementById('raceStatePill');
  if (statePill) {
    statePill.hidden = stopRaceButton.disabled;
  }
}

function downloadCurrentRaceData() {
  if (lapTimes.length === 0) {
    alert('No race data to download');
    return;
  }

  // Stats are computed over the ELIGIBLE laps so the figures in the file match
  // what the Race tab showed when it was downloaded.  The full lapTimes array
  // is still exported — excluding a lap hides it from the summary, it does not
  // delete it — and excludedLaps travels alongside so a re-import can restore
  // both the laps and the decision about them.
  const validLaps = eligibleLapsForStats().map(l => l.time);
  const fastest = validLaps.length > 0 ? Math.min(...validLaps) : 0;
  const sorted = [...validLaps].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  const median = sorted.length > 0 ? (sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid]) : 0;

  let best3Total = 0;
  if (validLaps.length >= 3) {
    const best3 = sorted.slice(0, 3);
    best3Total = best3.reduce((sum, t) => sum + t, 0);
  }

  const bandValue = bandSelect.options[bandSelect.selectedIndex].value;
  const channelValue = parseInt(channelSelect.options[channelSelect.selectedIndex].value);

  const raceData = {
    timestamp: Math.floor(Date.now() / 1000),
    lapTimes: lapTimes.map(t => Math.round(t * 1000)),
    // Lap numbers omitted from the summary.  Optional and additive: a file
    // written before this existed simply has no key, and imports with nothing
    // excluded — which is exactly the old behaviour.
    excludedLaps: Array.from(excludedLaps).sort((a, b) => a - b),
    fastestLap: Math.round(fastest * 1000),
    medianLap: Math.round(median * 1000),
    best3LapsTotal: Math.round(best3Total * 1000),
    pilotName: pilotNameInput.value || '',
    frequency: frequency,
    band: bandValue,
    channel: channelValue
  };

  // Create download
  const dataStr = JSON.stringify({ races: [raceData] }, null, 2);
  const dataBlob = new Blob([dataStr], { type: 'application/json' });
  const url = URL.createObjectURL(dataBlob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `SingleRace-${raceData.timestamp}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

function transferToRaceHistory() {
  if (lapTimes.length === 0) {
    alert('No race data to transfer');
    return;
  }

  // Save current race to backend (which will overwrite in RAM-only mode)
  saveCurrentRace();

  alert('Race transferred to Race History. You can view it in the Race History tab.');
}

// EventSource initialization moved to setupWiFiEvents() function above

function setBandChannelIndex(freq) {
  for (let i = 0; i < freqLookup.length; i++) {
    for (let j = 0; j < freqLookup[i].length; j++) {
      if (freqLookup[i][j] == freq) {
        bandSelect.selectedIndex = i;

        // Rebuild channel dropdown for this band (hides 0-freq channels)
        updateChannelOptionsForBand(i);

        // Select channel by 1-based VALUE ("1".."8"), not by selectedIndex
        const desired = String(j + 1);
        const exists = Array.from(channelSelect.options).some(o => o.value === desired);
        if (exists) {
          channelSelect.value = desired;
        } else if (channelSelect.options.length > 0) {
          channelSelect.selectedIndex = 0;
        }

        populateFreqOutput();
        return;
      }
    }
  }
}



// The theme the firmware ships as its default (lib/CONFIG/config.cpp).  Keep
// the two in sync — this is the value used when nothing is persisted yet.
const DEFAULT_THEME = 'fpvraceone';

// Themes that existed before the 12-palette reskin and may still be sitting in
// a device's NVS.  An unrecognised slug matches no [data-theme] block, so the
// page would fall through to :root — which used to mean a bare white UI.  Every
// unit in the field has one of these saved, so they are mapped rather than left
// to break.  Four were actually selectable in the old picker (oceanic, github,
// onelight, lightowl); the rest were reachable only by hand-editing config.
const THEME_ALIASES = {
  // "Night Gate" was renamed to "FPVRaceOne" when its palette was rebuilt from
  // the header logo.  Devices that saved the old slug follow the rename.
  nightgate:      'fpvraceone',
  oceanic:        DEFAULT_THEME,   // the old default — dark, so → dark default
  // "Daybright" was renamed "FPVRaceOne Day" when its blue was matched to the
  // logo.  Devices holding the old slug follow the rename, as do all the legacy
  // LIGHT themes that already resolved to it.
  daybright:      'fpvraceoneday',
  lighter:        'fpvraceoneday', // every legacy LIGHT theme → the new light one
  github:         'fpvraceoneday',
  onelight:       'fpvraceoneday',
  lightowl:       'fpvraceoneday',
  solarizedlight: 'fpvraceoneday',
  darker:         DEFAULT_THEME,   // legacy DARK themes → the new dark default
  palenight:      DEFAULT_THEME,
  deepocean:      DEFAULT_THEME,
  forest:         DEFAULT_THEME,
  skyblue:        DEFAULT_THEME,
  sandybeach:     DEFAULT_THEME,
  volcano:        DEFAULT_THEME,
  space:          DEFAULT_THEME,
  monokai:        DEFAULT_THEME,
  dracula:        DEFAULT_THEME,
  githubdark:     DEFAULT_THEME,
  arcdark:        DEFAULT_THEME,
  onedark:        DEFAULT_THEME,
  solarizeddark:  DEFAULT_THEME,
  nightowl:       DEFAULT_THEME,
  moonlight:      DEFAULT_THEME,
  synthwave:      DEFAULT_THEME,
};

// Map a stored/selected theme onto one that actually has a CSS block.
function resolveTheme(theme) {
  if (!theme) return DEFAULT_THEME;
  return THEME_ALIASES[theme] || theme;
}

// Theme functionality.  Bucket A: setting <html data-theme> is a visible side
// effect (the logo and favicon no longer change with the theme).  If we applied
// on change and the user then cancelled Settings, the page would keep the wrong
// theme until reload — the "applied but not saved" bug.  Defer the DOM apply
// until Apply & Save; the picker itself still shows the pending value.
function changeTheme() {
    const theme = resolveTheme(document.getElementById('themeSelect').value);

    // Modal-open hydration: this fires as the form populates from /config.
    // In that case we DO want the visible state to match the persisted
    // value — the theme was previously saved, so applying it is correct.
    if (settingsLoading) {
      document.documentElement.setAttribute('data-theme', theme);
      return;
    }

    // User-triggered change: stage + defer the visual apply.  Reads the
    // live picker at apply time so a rapid A→B→C sequence collapses to a
    // single "apply theme C" on Save.
    deferApply('theme', async () => {
      const t = resolveTheme(document.getElementById('themeSelect')?.value);
      document.documentElement.setAttribute('data-theme', t);
      // Keep the first-paint mirror in step on APPLY, not just on the next
      // /config load — otherwise a reload immediately after saving would paint
      // the previous theme.
      try { localStorage.setItem('theme', t); } catch (e) { /* storage off */ }
    });
    autoSaveConfig();
  }

  // updateThemeLogos() used to live here.  Nothing in the header is
  // theme-dependent any more: the brand lockup carries its own colours, and the
  // favicon is now a single chevron mark shared by all 12 themes.  The function
  // had no body left once the light/dark favicon swap went away, so it and its
  // three call sites were removed rather than left as a no-op.

function loadDarkMode() {
    // Runs at onload — BEFORE /config has answered.
    //
    // This must NOT read the picker.  At this point <select id="themeSelect">
    // still holds the `selected` attribute from the HTML, i.e. the DEFAULT
    // theme, so using it stamped the default over whatever the device had
    // saved.  Normally the /config load a moment later corrected it, which is
    // why the bug looked intermittent: when /config was slow or failed — a
    // re-flash reboot being exactly that — the default stayed put while the
    // Settings picker showed the real value.
    //
    // The localStorage mirror is the authority until /config answers.  Nothing
    // stored (a genuinely new browser) means no attribute, which falls through
    // to :root, i.e. the default — the intended behaviour for a fresh device.
    let cached = null;
    try { cached = localStorage.getItem('theme'); } catch (e) { /* storage off */ }
    if (cached) {
      document.documentElement.setAttribute('data-theme', resolveTheme(cached));
    }
  }

// Manual lap addition
function addManualLap() {
  // ── Enforcement, not decoration ───────────────────────────────────────────
  //
  // A client racing under a race director must not be able to fabricate a
  // crossing.  Its laps sync upstream, so an injected lap does not merely
  // mislead the pilot — it corrupts the director's record of the race.
  //
  // This guard lives HERE, at the single chokepoint every route passes through,
  // because hiding and disabling the button only covers one of them.  The Dev
  // Mode pilot-name click calls this function directly, and did so regardless
  // of master-race state until this check existed.
  //
  // Deliberately scoped to mnMasterRaceActive rather than "connected to a
  // master": a client running its OWN solo race, or one with "ignore race
  // director" set, never has that flag raised, and its manual laps are its own
  // business.
  if (mnNodeMode === 2 && mnMasterRaceActive) {
    console.warn('[DevMode] Manual lap blocked — racing under the race director.');
    return;
  }

  // Get current timer value and convert to milliseconds
  const timerText = timer.innerHTML;
  const match = timerText.match(/(\d{2}):(\d{2}):(\d{2})s/);
  if (match) {
    const minutes = parseInt(match[1]);
    const seconds = parseInt(match[2]);
    const centiseconds = parseInt(match[3]);
    const totalMs = (minutes * 60000) + (seconds * 1000) + (centiseconds * 10);
    
    // Calculate lap time in milliseconds
    const lapTimeMs = totalMs - (lapNo >= 0 ? lapTimes.reduce((a, b) => a + (b * 1000), 0) : 0);
    
    // Send lap to backend to broadcast to all clients (including OSD)
    if (usbConnected && transportManager) {
      transportManager.sendCommand('timer/addLap', 'POST', { lapTime: lapTimeMs })
        .then(data => console.log('Manual lap broadcasted:', data))
        .catch(err => console.error('Failed to broadcast manual lap:', err));
    } else {
      fetch('/timer/addLap', {
        method: 'POST',
        headers: {
          'Accept': 'application/json',
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ lapTime: lapTimeMs })
      })
        .then(response => response.json())
        .then(data => console.log('Manual lap broadcasted:', data))
        .catch(err => console.error('Failed to broadcast manual lap:', err));
    }
    
    // Note: The lap will be added via EventSource lap event
    // No need to call addLap() here as it will come back through the event stream
  }
}

// Lap Analysis
let currentAnalysisMode = 'history';

// Color palette for bar variations
const barColors = [
  ['#42A5F5', '#1E88E5'], // Blue
  ['#66BB6A', '#43A047'], // Green
  ['#FFA726', '#FB8C00'], // Orange
  ['#AB47BC', '#8E24AA'], // Purple
  ['#26C6DA', '#00ACC1'], // Cyan
  ['#FFCA28', '#FFB300'], // Amber
  ['#EF5350', '#E53935'], // Red
  ['#5C6BC0', '#3F51B5'], // Indigo
  ['#EC407A', '#D81B60'], // Pink
  ['#78909C', '#607D8B'], // Blue Grey
];

function switchAnalysisMode(mode) {
  // Only switch if mode is different to prevent clearing when clicking same tab
  if (currentAnalysisMode === mode) {
    return;
  }

  currentAnalysisMode = mode;
  // Update tab styling
  document.querySelectorAll('.analysis-tab').forEach(tab => {
    tab.classList.remove('active');
  });

  // Add active class to the clicked button using mode to find it
  const activeButton = mode === 'history'
    ? document.querySelector('.analysis-tab[onclick*="history"]')
    : document.querySelector('.analysis-tab[onclick*="fastestRound"]');
  if (activeButton) {
    activeButton.classList.add('active');
  }

  // Re-render analysis
  updateAnalysisView();
}

function updateAnalysisView() {
  if (lapTimes.length === 0) {
    document.getElementById('analysisContent').innerHTML =
      '<p class="no-data">Complete at least 1 lap to see analysis</p>';
    return;
  }
  
  // Update stats boxes
  updateStatsBoxes();
  
  // Update chart view
  switch(currentAnalysisMode) {
    case 'history':
      renderLapHistory();
      break;
    case 'fastestRound':
      renderFastestRound();
      break;
  }
}

function updateStatsBoxes() {
  if (lapTimes.length === 0) {
    document.getElementById('statFastest').textContent = '--';
    document.getElementById('statFastestLapNo').textContent = '';
    document.getElementById('statFastest3Consec').textContent = '--';
    document.getElementById('statFastest3ConsecLaps').textContent = '';
    document.getElementById('statMedian').textContent = '--';
    document.getElementById('statBest3').textContent = '--';
    document.getElementById('statBest3Laps').textContent = '';
    return;
  }
  
  // Every statistic below is computed over the ELIGIBLE laps: after the first
  // crossing, minus any the user excluded.  Each entry carries its real lap
  // number so the "Lap 4" / "L2, L5, L6" captions stay correct after filtering
  // — reporting positions in a filtered array would name the wrong laps.
  const eligible = eligibleLapsForStats();
  const excludedCount = Math.max(0, (lapTimes.length - 1) - eligible.length);
  const needNote = (n) => excludedCount > 0 ? `Need ${n} (excl. ${excludedCount})` : `Need ${n}`;

  // Fastest Lap
  if (eligible.length === 0) {
    document.getElementById('statFastest').textContent = '--';
    document.getElementById('statFastestLapNo').textContent = needNote('1 lap');
  } else {
    let best = eligible[0];
    for (const l of eligible) if (l.time < best.time) best = l;
    document.getElementById('statFastest').textContent = formatMsDisplay(Math.round(best.time * 1000));
    document.getElementById('statFastestLapNo').textContent = `Lap ${best.lapNo}`;
  }

  // Fastest 3 Consecutive Laps (RaceGOW format).
  //
  // "Consecutive" means consecutive LAPS FLOWN, so an excluded lap breaks the
  // run rather than closing over it — laps 2 and 4 are not consecutive just
  // because 3 was discarded.  Hence the lapNo contiguity test.
  let fastestConsecTime = Infinity;
  let fastestConsecStart = -1;
  for (let i = 0; i + 2 < eligible.length; i++) {
    const a = eligible[i], b = eligible[i + 1], c = eligible[i + 2];
    if (b.lapNo !== a.lapNo + 1 || c.lapNo !== b.lapNo + 1) continue;
    const consecTime = a.time + b.time + c.time;
    if (consecTime < fastestConsecTime) {
      fastestConsecTime = consecTime;
      fastestConsecStart = a.lapNo;
    }
  }
  if (fastestConsecStart >= 0) {
    document.getElementById('statFastest3Consec').textContent = formatMsDisplay(Math.round(fastestConsecTime * 1000));
    document.getElementById('statFastest3ConsecLaps').textContent =
      `L${fastestConsecStart}-L${fastestConsecStart + 1}-L${fastestConsecStart + 2}`;
  } else {
    document.getElementById('statFastest3Consec').textContent = '--';
    document.getElementById('statFastest3ConsecLaps').textContent = needNote('3 in a row');
  }

  // Median Lap
  if (eligible.length === 0) {
    document.getElementById('statMedian').textContent = '--';
  } else {
    const sorted = eligible.map(l => l.time).sort((a, b) => a - b);
    const mid = Math.floor(sorted.length / 2);
    const median = sorted.length % 2 === 0
      ? (sorted[mid - 1] + sorted[mid]) / 2
      : sorted[mid];
    document.getElementById('statMedian').textContent = formatMsDisplay(Math.round(median * 1000));
  }

  // Best 3 Laps (sum of the 3 fastest individual laps — need not be consecutive)
  if (eligible.length >= 3) {
    const best3 = [...eligible].sort((a, b) => a.time - b.time).slice(0, 3);
    const totalTime = best3.reduce((sum, l) => sum + l.time, 0);
    const lapNumbers = best3.map(l => l.lapNo).sort((a, b) => a - b).map(n => `L${n}`).join(', ');
    document.getElementById('statBest3').textContent = formatMsDisplay(Math.round(totalTime * 1000));
    document.getElementById('statBest3Laps').textContent = lapNumbers;
  } else {
    document.getElementById('statBest3').textContent = '--';
    document.getElementById('statBest3Laps').textContent = needNote('3 laps');
  }
}

function renderLapHistory() {
  // Show last 10 laps (or all if less than 10)
  const recentLaps = lapTimes.slice(-10);
  const startIndex = Math.max(0, lapTimes.length - 10);
  const maxTime = Math.max(...recentLaps);
  
  let html = '<div class="analysis-bars">';
  recentLaps.forEach((time, index) => {
    const lapNumber = startIndex + index;
    const colorIndex = (startIndex + index) % barColors.length;
    if (lapNumber === 0) {
      html += createBarItemWithColor(`1st Cross`, time, maxTime, formatMsDisplay(Math.round(time * 1000)), colorIndex);
    } else {
      html += createBarItemWithColor(`Lap ${lapNumber}`, time, maxTime, formatMsDisplay(Math.round(time * 1000)), colorIndex);
    }
    
  });
  html += '</div>';
  
  if (lapTimes.length > 10) {
    html += `<p style="text-align: center; margin-top: 16px; color: var(--secondary-color); font-size: 14px;">Showing last 10 of ${lapTimes.length} laps</p>`;
  }
  
  document.getElementById('analysisContent').innerHTML = html;
}

// Fastest 3 consecutive laps.  Shared by the Race tab and the Race History
// detail view, which previously held two identical copies of this — both with
// the same four bugs.
//
// `laps` is lap times in SECONDS indexed by lap number, exactly as lapTimes is:
// index 0 is the FIRST CROSSING, index N is Lap N.
//
// Rules, matching updateAnalysisView() and recomputeRaceStats():
//   - index 0 never participates.  It is the hole shot — the time from race
//     start to the first gate pass — not a lap, and it is almost always the
//     shortest value in the array, so including it made it win nearly every
//     round it appeared in.
//   - excluded laps are skipped.
//   - "consecutive" means consecutive LAPS FLOWN, so an excluded lap breaks the
//     run rather than closing over it.
//
// Returns { laps: [{lapNo, time} x3], total }, or null if there is no valid
// window.  Note that needing 3 LAPS means needing 4 array entries.
function findFastestRound(laps, excluded) {
  if (!Array.isArray(laps)) return null;
  const eligible = [];
  for (let i = 1; i < laps.length; i++) {
    if (excluded && excluded.has(i)) continue;
    eligible.push({ lapNo: i, time: laps[i] });
  }

  let best = null;
  for (let i = 0; i + 2 < eligible.length; i++) {
    const a = eligible[i], b = eligible[i + 1], c = eligible[i + 2];
    if (b.lapNo !== a.lapNo + 1 || c.lapNo !== b.lapNo + 1) continue;
    const total = a.time + b.time + c.time;
    if (!best || total < best.total) best = { laps: [a, b, c], total };
  }
  return best;
}

// Bars + total for a findFastestRound() result.  Labels carry the REAL lap
// numbers — the old code printed `bestStartIndex + 1`, which was off by one
// even when the window itself was right.
function _fastestRoundHtml(best) {
  const maxTime = Math.max(...best.laps.map(l => l.time));
  let html = '<div class="analysis-bars">';
  best.laps.forEach((l, i) => {
    html += createBarItemWithColor(
      `Lap ${l.lapNo}`, l.time, maxTime, formatMsDisplay(Math.round(l.time * 1000)), i);
  });
  html += '</div>';
  html += `<p style="text-align: center; margin-top: 16px; font-weight: bold; color: var(--primary-color);">Total: ${formatMsDisplay(Math.round(best.total * 1000))}</p>`;
  return html;
}

function renderFastestRound() {
  const best = findFastestRound(lapTimes, excludedLaps);
  if (!best) {
    document.getElementById('analysisContent').innerHTML =
      '<p class="no-data">Complete at least 3 consecutive laps to see fastest round</p>';
    return;
  }
  document.getElementById('analysisContent').innerHTML = _fastestRoundHtml(best);
}

// `opts.excludeLapNo` adds the per-lap Exclude/Include control used by the Race
// History detail view; `opts.excluded` dims the bar to match.  Omitted by the
// live Race tab, which has its own table-based control.
function createBarItemWithColor(label, time, maxTime, displayTime, colorIndex, opts = {}) {
  // Guard the width math: an all-zero or imported race can make maxTime 0 (or the
  // caller can pass -Infinity from Math.max() of an empty slice), giving NaN/Infinity
  // percentages and a broken `width: NaN%`. Clamp to a valid [0,100] range.
  const safeMax = (Number.isFinite(maxTime) && maxTime > 0) ? maxTime : 1;
  const safeTime = Number.isFinite(time) ? time : 0;
  let percentage = (safeTime / safeMax) * 100;
  percentage = Math.max(0, Math.min(100, Number.isFinite(percentage) ? percentage : 0));
  const colors = barColors[colorIndex % barColors.length];
  const n = opts.excludeLapNo;
  const btn = (Number.isFinite(n) && n > 0)
    ? `<button type="button" class="lap-exclude-btn bar-exclude-btn"
               aria-pressed="${opts.excluded ? 'true' : 'false'}"
               title="${opts.excluded
                 ? `Lap ${n} is excluded from the summary — click to put it back`
                 : `Exclude lap ${n} from Fastest / Median / Best 3`}"
               onclick="toggleDetailLapExcluded(${n})">${opts.excluded ? 'Include' : 'Exclude'}</button>`
    : '';
  return `
    <div class="bar-item${opts.excluded ? ' bar-item-excluded' : ''}">
      <div class="bar-label">${label}</div>
      <div class="bar-container">
        <div class="bar-fill" style="width: ${percentage}%; background: linear-gradient(90deg, ${colors[0]}, ${colors[1]});">
          <span class="bar-time">${displayTime}</span>
        </div>
      </div>
      ${btn}
    </div>
  `;
}

// Race History Functions
let raceHistoryData = [];

// Set true the moment a saved race's exclusions are edited on this page.
//
// The device knows nothing about exclusions — race history is RAM-only here and
// /api/races/download serves the firmware's copy — so an edit exists ONLY in
// this tab until it is downloaded.  The Download button is gated on this flag
// so it reads as "there is something here the device doesn't have", rather than
// offering a download identical to what the device would hand you anyway.
let raceHistoryDirty = false;

// File name of the most recently imported races file, so Download Race can
// write back to the same name.  Empty when the history came from the device
// rather than from a file, in which case a fresh timestamped name is used.
let importedRacesFileName = '';

// race timestamp -> excludedLaps[].  THIS TAB IS THE AUTHORITY for exclusions.
//
// The firmware has no excludedLaps field, so anything read back from /races
// arrives without them.  Both an import and a local toggle therefore lose the
// exclusions the moment loadRaceHistory() runs — which import does immediately,
// and which also happens after a delete or a metadata edit.  The stored
// fastestLap / medianLap / best3LapsTotal DO survive, because those are fields
// the device round-trips, which is why the totals looked right while every
// button read "Exclude".
//
// Recording them here and re-applying after every load closes that gap.
const raceExclusions = new Map();

function rememberRaceExclusions(timestamp, laps) {
  if (!Number.isFinite(timestamp)) return;
  const clean = (Array.isArray(laps) ? laps : [])
    .map(n => parseInt(n, 10))
    .filter(n => Number.isFinite(n) && n > 0)
    .sort((a, b) => a - b);
  if (clean.length) raceExclusions.set(timestamp, clean);
  else raceExclusions.delete(timestamp);
}

// Re-apply remembered exclusions onto raceHistoryData and bring each race's
// summary figures back in line with them.
function applyRememberedExclusions() {
  raceHistoryData.forEach(r => {
    const remembered = raceExclusions.get(r.timestamp);
    if (remembered) {
      r.excludedLaps = [...remembered];
      // The device returned totals computed at save time.  Recompute anyway so
      // the figures provably match the exclusions we just restored, rather than
      // trusting that they were saved in step.
      recomputeRaceStats(r);
    } else if (!Array.isArray(r.excludedLaps)) {
      r.excludedLaps = [];
    }
  });
}

// Recompute a saved race's summary figures from its lapTimes (ms) and its
// excludedLaps, in place.
//
// Mirrors the live-race maths in updateAnalysisView(): lap 0 is the first
// crossing and never counts; excluded laps drop out; "3 consecutive" means
// consecutive LAPS FLOWN, so an excluded lap breaks the run rather than
// closing over it.
function recomputeRaceStats(race) {
  if (!race || !Array.isArray(race.lapTimes)) return;
  const excluded = new Set(Array.isArray(race.excludedLaps) ? race.excludedLaps : []);

  const eligible = [];
  for (let i = 1; i < race.lapTimes.length; i++) {
    if (excluded.has(i)) continue;
    eligible.push({ ms: race.lapTimes[i], lapNo: i });
  }

  race.fastestLap = eligible.length
    ? eligible.reduce((b, l) => (l.ms < b.ms ? l : b), eligible[0]).ms
    : 0;

  if (eligible.length) {
    const sorted = eligible.map(l => l.ms).sort((a, b) => a - b);
    const mid = Math.floor(sorted.length / 2);
    race.medianLap = sorted.length % 2 === 0
      ? Math.round((sorted[mid - 1] + sorted[mid]) / 2)
      : sorted[mid];
  } else {
    race.medianLap = 0;
  }

  race.best3LapsTotal = eligible.length >= 3
    ? eligible.map(l => l.ms).sort((a, b) => a - b).slice(0, 3).reduce((s, m) => s + m, 0)
    : 0;
}

// Toggle one lap's exclusion on the race currently open in the detail view.
function toggleDetailLapExcluded(lapNo) {
  const race = currentDetailRace;
  if (!race || !Number.isFinite(lapNo) || lapNo <= 0) return;

  if (!Array.isArray(race.excludedLaps)) race.excludedLaps = [];
  const at = race.excludedLaps.indexOf(lapNo);
  if (at >= 0) race.excludedLaps.splice(at, 1);
  else race.excludedLaps.push(lapNo);
  race.excludedLaps.sort((a, b) => a - b);

  // Record against the timestamp so this choice survives the next
  // loadRaceHistory() — which a delete, a metadata edit or another import all
  // trigger, and any of which would otherwise silently revert it.
  rememberRaceExclusions(race.timestamp, race.excludedLaps);

  recomputeRaceStats(race);

  // Repaint the summary boxes so Fastest / Median / Best 3 move with the
  // decision instead of waiting for the detail view to be reopened.
  document.getElementById('detailFastest').textContent = formatMsDisplay(race.fastestLap);
  document.getElementById('detailMedian').textContent  = formatMsDisplay(race.medianLap);
  document.getElementById('detailBest3').textContent   = formatMsDisplay(race.best3LapsTotal);

  raceHistoryDirty = true;
  updateHistoryDownloadButton();

  // Re-render whichever detail tab is open.
  const tabs = document.querySelectorAll('#raceDetails .analysis-tab');
  if (tabs[1]?.classList.contains('active')) renderDetailFastestRound();
  else renderDetailHistory();
}

// Enable the Download button only once there is an edit the device doesn't have.
function updateHistoryDownloadButton() {
  const btn = document.getElementById('downloadRacesBtn');
  if (!btn) return;
  btn.disabled = !raceHistoryDirty;
  btn.title = raceHistoryDirty
    ? 'Download all races, including the laps you excluded'
    : 'Exclude or include a lap to enable the download';
}
// Defaulting to `false` (RAM-only) means the "race history is not saved"
// banner is visible on first paint for new users.  /races returns
// `persistent: true` only when the firmware has an SD card or a dedicated
// race-storage partition — current C6 hardware has neither, so the default
// here already matches reality.  Setting this to `true` would briefly hide
// the banner before /races came back and corrected it.
let raceHistoryPersistent = false;
let ledConnected = false;
let currentDetailRace = null;

function saveCurrentRace() {
  if (lapTimes.length === 0) return;
  
  // Calculate stats over the ELIGIBLE laps (after the first crossing, minus any
  // the user excluded) so the saved race agrees with what the Race tab showed.
  const validLaps = eligibleLapsForStats().map(l => l.time);
  const fastest = validLaps.length > 0 ? Math.min(...validLaps) : 0;
  const sorted = [...validLaps].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  const median = sorted.length > 0 ? (sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid]) : 0;

  let best3Total = 0;
  if (validLaps.length >= 3) {
    const best3 = sorted.slice(0, 3);
    best3Total = best3.reduce((sum, t) => sum + t, 0);
  }

  // Get current pilot and frequency info
  const bandValue = bandSelect.options[bandSelect.selectedIndex].value;
  const channelValue = parseInt(channelSelect.options[channelSelect.selectedIndex].value);

  const raceData = {
    timestamp: Math.floor(Date.now() / 1000),
    lapTimes: lapTimes.map(t => Math.round(t * 1000)), // Convert to milliseconds
    // Carried through history and downloads so the exclusions survive a reload,
    // a save-and-reopen, and an export/import round trip.
    excludedLaps: Array.from(excludedLaps).sort((a, b) => a - b),
    fastestLap: Math.round(fastest * 1000),
    medianLap: Math.round(median * 1000),
    best3LapsTotal: Math.round(best3Total * 1000),
    pilotName: pilotNameInput.value || '',
    frequency: frequency,
    band: bandValue,
    channel: channelValue
  };
  
  fetch('/races/save', {
    method: 'POST',
    headers: {
      'Accept': 'application/json',
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(raceData)
  })
  .then(response => response.json())
  .then(data => {
    console.log('Race saved:', data);
    loadRaceHistory();
  })
  .catch(error => console.error('Error saving race:', error));
}

function setButtonLabel(el, label) {
  if (!el) return;
  // <button> uses textContent; <input type="button|submit"> uses value
  if ('value' in el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA')) {
    el.value = label;
  }
  el.textContent = label; // safe for <button>
  el.setAttribute('aria-label', label);
}

function applyRaceHistoryModeUI() {
  const importBtn = document.getElementById('importRacesBtn');
  const clearBtn = document.getElementById('clearAllRacesBtn');

  const storageLabel = document.getElementById('raceHistoryStorageLabel');
  const raceTabBanner = document.getElementById('raceTabDownloadReminder');

  setLEDSettingsVisible(ledConnected);

  setButtonLabel(
    importBtn,
    raceHistoryPersistent ? 'Import Races' : 'Import Single Race (overrides current data)'
  );

  if (clearBtn) {
    clearBtn.style.display = raceHistoryPersistent ? '' : 'none';
  }

  if (storageLabel) {
    if (raceHistoryPersistent) {
      storageLabel.textContent = 'Storage: SD card (race history is saved on the device).';
    } else {
      storageLabel.textContent = 'Storage: RAM only (race history is NOT saved after power off).';
    }
  }

  // Banner logic (RAM-only reminder), with "hide" persisted for this browser session
  if (raceTabBanner) {
    const userHidden = sessionStorage.getItem("hideRaceDownloadReminder") === "1";
    const alwaysHidden = localStorage.getItem("alwaysHideRaceBanner") === "1"; // default OFF — only "1" hides

    if (raceHistoryPersistent || userHidden || alwaysHidden) {
      raceTabBanner.style.display = 'none';
    } else {
      // Use flex so the "hide" link can be right-justified
      raceTabBanner.style.display = 'flex';
      raceTabBanner.style.alignItems = 'center';
      raceTabBanner.style.justifyContent = 'space-between';
      raceTabBanner.style.gap = '10px';

      raceTabBanner.style.border = '1px solid rgba(255, 200, 0, 0.6)';
      raceTabBanner.style.background = 'rgba(255, 200, 0, 0.12)';
    }
  }
}




async function loadRaceHistory() {
  try {
    // IMPORTANT: use transportFetch so USB mode works too
    const data = await transportFetch('/races', {
      method: 'GET',
      headers: { 'Accept': 'application/json' }
    });

    raceHistoryData = data.races || [];
    raceHistoryPersistent = (data.persistent !== false);

    // Restore exclusions the device cannot store, and normalise an empty array
    // onto everything else so the first toggle isn't operating on undefined.
    applyRememberedExclusions();

    // Anything we just restored is, by definition, not in the device's copy —
    // so there IS something worth downloading.  A load with nothing remembered
    // leaves the button disabled as before.
    raceHistoryDirty = raceHistoryData.some(r => (r.excludedLaps || []).length > 0);
    updateHistoryDownloadButton();

    applyRaceHistoryModeUI();
    renderRaceHistory();
  } catch (error) {
    console.error('Error loading races:', error);
  }
}

function renderRaceHistory() {
  const listContainer = document.getElementById('raceHistoryList');
  const raceDetails = document.getElementById('raceDetails');

  if (raceHistoryData.length === 0) {
    listContainer.innerHTML = '<p class="no-data">No races saved yet</p>';
    if (raceDetails) {
      raceDetails.style.display = 'none';
      currentDetailRace = null;
    }
    return;
  }

  let html = '';
  raceHistoryData.forEach((race, index) => {
    const date = new Date(race.timestamp * 1000);
    const dateStr = date.toLocaleDateString() + ' ' + date.toLocaleTimeString();
    const fastestLapStr = formatMsDisplay(race.fastestLap);
    // Lap count should exclude Gate 1 (first entry)
    const actualLapCount = race.lapTimes.length > 0 ? race.lapTimes.length - 1 : 0;
    // Calculate total race time (ms — convert to formatMsDisplay)
    const totalMs = race.lapTimes.reduce((sum, t) => sum + t, 0);
    const name = race.name || '';
    const tag = race.tag || '';
    const pilotName = race.pilotName || race.pilotCallsign || '';
    // Channel display: separate band from channel index with a space + parens,
    // otherwise "DJI03/04-10/20" + channel "2" reads as "DJI03/04-10/202".
    const freqDisplay = race.frequency
      ? `${race.band} (${race.channel}) (${race.frequency}MHz)`
      : '';
    const trackDisplay = race.trackName ? race.trackName : '';
    const distanceDisplay = race.totalDistance ? `${race.totalDistance.toFixed(1)}m` : '';

    html += `
      <div class="race-item" data-race-index="${index}" onclick="viewRaceDetails(${index})">
        <div class="race-item-buttons">
          ${raceHistoryPersistent ? `<button class="race-item-button" onclick="event.stopPropagation(); openEditModal(${index})">Edit</button>` : ''}
          ${raceHistoryPersistent ? `<button class="race-item-button" style="border-color: #e74c3c; color: #e74c3c;" onclick="event.stopPropagation(); deleteRace(${race.timestamp})">Delete</button>` : ''}
        </div>
        <div class="race-item-header">
          <div>
            ${tag ? '<span class="race-tag">' + tag + '</span>' : ''}
            <div class="race-date">${dateStr}</div>
            ${name ? '<div class="race-name">' + name + '</div>' : ''}
            ${pilotName ? '<div style="font-size: 14px; color: var(--secondary-color); margin-top: 4px;">Pilot: ' + pilotName + '</div>' : ''}
            ${freqDisplay ? '<div style="font-size: 14px; color: var(--secondary-color);">Channel: ' + freqDisplay + '</div>' : ''}
            ${trackDisplay ? '<div style="font-size: 14px; color: var(--secondary-color);">Track: ' + trackDisplay + (distanceDisplay ? ' (' + distanceDisplay + ')' : '') + '</div>' : ''}
          </div>
        </div>
        <div class="race-item-stats">
          <div class="race-item-stat">Laps: <strong>${actualLapCount}</strong></div>
          <div class="race-item-stat">Fastest: <strong>${fastestLapStr}</strong></div>
          <div class="race-item-stat">Total Time: <strong>${formatMsDisplay(totalMs)}</strong></div>
          ${distanceDisplay ? '<div class="race-item-stat">Distance: <strong>' + distanceDisplay + '</strong></div>' : ''}
        </div>
      </div>
    `;
  });

  listContainer.innerHTML = html;

  // Auto-show details for the currently selected race, or first race if none selected
  let indexToShow = 0;
  if (currentDetailRace !== null) {
    const currentIndex = raceHistoryData.findIndex(r => r.timestamp === currentDetailRace.timestamp);
    if (currentIndex !== -1) {
      indexToShow = currentIndex;
    }
  }

  // Always show race details when there are races
  viewRaceDetails(indexToShow);
}

function viewRaceDetails(index) {
  currentDetailRace = raceHistoryData[index];
  const race = currentDetailRace;
  // Inline onclick="viewRaceDetails(N)" handlers are baked into the rendered HTML and
  // can go stale if raceHistoryData shrinks (a race was deleted) between render and
  // click. Also imported races may omit lapTimes. Guard before dereferencing.
  if (!race || !Array.isArray(race.lapTimes)) return;
  const date = new Date(race.timestamp * 1000);
  const dateStr = date.toLocaleDateString() + ' ' + date.toLocaleTimeString();

  // Sum of all lap times in ms — formatMsDisplay handles the MM:SS:CSs split.
  const totalMs = race.lapTimes.reduce((sum, t) => sum + t, 0);

  document.getElementById('raceDetailsTitle').textContent = `Race - ${dateStr}`;
  document.getElementById('detailFastest').textContent   = formatMsDisplay(race.fastestLap);
  document.getElementById('detailMedian').textContent    = formatMsDisplay(race.medianLap);
  document.getElementById('detailBest3').textContent     = formatMsDisplay(race.best3LapsTotal);
  document.getElementById('detailTotalTime').textContent = formatMsDisplay(totalMs);

  // Keep race details in its original position (below the race list) and always visible
  const detailsDiv = document.getElementById('raceDetails');
  detailsDiv.style.display = 'block';

  // Render the race timeline
  renderRaceTimeline(race);

  // Force render to ensure content shows even if tab was previously active
  switchDetailMode('history', true);
}

function renderRaceTimeline(race) {
  const container = document.getElementById('raceTimeline');
  if (!container) return;
  
  // Clear existing events (keep the bar)
  const existingEvents = container.querySelectorAll('.timeline-event');
  existingEvents.forEach(event => event.remove());
  
  const lapTimes = race.lapTimes.map(t => t / 1000); // Convert to seconds
  const totalTime = lapTimes.reduce((sum, t) => sum + t, 0);
  
  // Create events array with cumulative times
  const events = [];
  let cumulativeTime = 0;
  
  // Race Start (time 0)
  events.push({
    type: 'start',
    label: 'Race Start',
    time: 0,
    percentage: 0
  });
  
  // Gate 1 and Laps
  lapTimes.forEach((lapTime, index) => {
    cumulativeTime += lapTime;
    const percentage = (cumulativeTime / totalTime) * 100;
    
    if (index === 0) {
      // Gate 1
      events.push({
        type: 'gate',
        label: '1st Cross',
        time: cumulativeTime,
        percentage: percentage
      });
    } else {
      // Regular laps.  Mark the ones the pilot excluded from the summary —
      // without this the detail view shows "Fastest: Lap 4" next to a visibly
      // quicker Lap 1 with no explanation for the discrepancy.
      const excluded = Array.isArray(race.excludedLaps) && race.excludedLaps.includes(index);
      events.push({
        type: 'lap',
        label: excluded ? `Lap ${index} (excluded)` : `Lap ${index}`,
        time: cumulativeTime,
        percentage: percentage
      });
    }
  });
  
  // Race Stop (at total time)
  events.push({
    type: 'stop',
    label: 'Race Stop',
    time: totalTime,
    percentage: 100
  });
  
  // Render events on timeline
  events.forEach((event, index) => {
    const eventDiv = document.createElement('div');
    eventDiv.className = 'timeline-event';
    
    // Race Start and Stop go above, everything else below
    if (event.type === 'start' || event.type === 'stop') {
      eventDiv.classList.add('above');
    } else {
      eventDiv.classList.add('below');
    }
    
    eventDiv.style.left = `${event.percentage}%`;
    const eventTimeStr = formatMsDisplay(Math.round(event.time * 1000));
    eventDiv.title = `${event.label} - ${eventTimeStr}`;

    eventDiv.innerHTML = `
      <div class="timeline-flag ${event.type}"></div>
      <div class="timeline-label">${event.label}</div>
      <div class="timeline-time">${eventTimeStr}</div>
    `;
    
    container.appendChild(eventDiv);
  });
  
  // Add lap time indicators between events
  for (let i = 1; i < events.length; i++) {
    const prevEvent = events[i - 1];
    const currentEvent = events[i];
    const lapTime = currentEvent.time - prevEvent.time;
    const midPoint = (prevEvent.percentage + currentEvent.percentage) / 2;
    
    // Only show lap time if there's enough space
    if (currentEvent.percentage - prevEvent.percentage > 5) {
      const lapTimeDiv = document.createElement('div');
      lapTimeDiv.className = 'timeline-lap-time';
      lapTimeDiv.style.left = `${midPoint}%`;
      lapTimeDiv.textContent = formatMsDisplay(Math.round(lapTime * 1000));
      lapTimeDiv.title = `Time between ${prevEvent.label} and ${currentEvent.label}`;
      container.appendChild(lapTimeDiv);
    }
  }
}

function closeRaceDetails() {
  stopPlayback(); // Stop any ongoing playback
  document.getElementById('raceDetails').style.display = 'none';
  currentDetailRace = null;
}

// Race Playback
let playbackInterval = null;
let playbackTimeouts = [];
let playbackStartTime = 0;
let playbackTotalTime = 0;

// Is playback allowed to speak right now?  Read at each callout rather than
// captured when Play is pressed, so the checkbox works mid-playback.
function isPlaybackVoiceEnabled() {
  return document.getElementById('playbackVoice')?.checked !== false;
}

// Bound to the Voice Callouts checkbox.  Unchecking has to flush the announcer
// as well as stop future laps — speech already handed over would otherwise keep
// talking for several seconds after the user asked for silence.
function onPlaybackVoiceToggled() {
  if (isPlaybackVoiceEnabled()) return;
  if (audioAnnouncer && typeof audioAnnouncer.clearQueue === 'function') {
    try { audioAnnouncer.clearQueue(); } catch (e) { console.warn('clearQueue failed:', e); }
  }
}

function playbackRace() {
  if (!currentDetailRace) return;
  
  const playBtn = document.getElementById('playbackBtn');
  const stopBtn = document.getElementById('stopPlaybackBtn');
  const enableWebhooks = document.getElementById('playbackWebhooks').checked;
  const playhead = document.getElementById('timelinePlayhead');
  
  playBtn.style.display = 'none';
  stopBtn.style.display = 'inline-block';
  
  const lapTimes = currentDetailRace.lapTimes.map(t => t / 1000); // Convert to seconds
  playbackTotalTime = lapTimes.reduce((sum, t) => sum + t, 0);
  playbackStartTime = Date.now();
  
  // Show and start playhead animation
  if (playhead) {
    playhead.classList.add('active');
    playhead.style.left = '0%';
  }
  
  // Update playhead position smoothly
  playbackInterval = setInterval(() => {
    const elapsed = (Date.now() - playbackStartTime) / 1000; // seconds
    const percentage = Math.min((elapsed / playbackTotalTime) * 100, 100);
    if (playhead) {
      playhead.style.left = `${percentage}%`;
    }
    if (percentage >= 100) {
      clearInterval(playbackInterval);
    }
  }, 50); // Update every 50ms for smooth animation
  
  let cumulativeTime = 0;

  // Announce the start, so a playback opens the way a race does instead of
  // sitting silent until the first crossing.
  if (isPlaybackVoiceEnabled()) {
    queueSpeak('<p>Race Start</p>');
  }

  // Broadcast race start
  if (enableWebhooks) {
    fetch('/timer/playbackStart', {
      method: 'POST',
      headers: {
        'Accept': 'application/json',
        'Content-Type': 'application/json'
      },
      body: JSON.stringify({ raceData: currentDetailRace })
    }).catch(err => console.error('Playback start failed:', err));
  }
  
  // Schedule each lap event
  lapTimes.forEach((lapTime, index) => {
    cumulativeTime += lapTime;
    const delay = cumulativeTime * 1000; // Convert to milliseconds
    
    const timeout = setTimeout(() => {
      // Broadcast lap event
      const lapTimeMs = Math.round(lapTime * 1000);
      
      if (enableWebhooks) {
        fetch('/timer/playbackLap', {
          method: 'POST',
          headers: {
            'Accept': 'application/json',
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({ 
            lapTime: lapTimeMs,
            lapNumber: index,
            isGate1: index === 0
          })
        }).catch(err => console.error('Playback lap failed:', err));
      }
      
      // Highlight the corresponding timeline flag
      highlightTimelineEvent(index);

      // Voice — the same callout the live race made, via the same function.
      // The 2-/3-lap cadence is reproduced exactly as addLap() computes it:
      // announce on every 2nd / 3rd lap, summing the trailing window.
      //
      // Checked HERE, per lap, rather than captured at Play time: the toggle is
      // meant to take effect the moment it is clicked, part-way through a
      // playback included.  (Turning it off also flushes whatever is already
      // queued — see the onchange handler on #playbackVoice.)
      if (isPlaybackVoiceEnabled()) {
        let last2 = "", last3 = "";
        if (index >= 2 && index % 2 === 0) {
          last2 = String(lapTimes[index] + lapTimes[index - 1]);
        }
        if (index >= 3 && index % 3 === 0) {
          last3 = String(lapTimes[index] + lapTimes[index - 1] + lapTimes[index - 2]);
        }
        announceLapCallout(
          currentDetailRace.pilotName || pilotNameInput.value || '',
          index,
          formatLapForSpeech(lapTimeMs),
          last2, last3
        );
      }

      console.log(`Playback: ${index === 0 ? '1st Cross' : 'Lap ' + index} - ${lapTime.toFixed(3)}s`);
    }, delay);
    
    playbackTimeouts.push(timeout);
  });
  
  // Schedule race stop
  const totalTime = lapTimes.reduce((sum, t) => sum + t, 0);
  const stopTimeout = setTimeout(() => {
    if (enableWebhooks) {
      fetch('/timer/playbackStop', {
        method: 'POST',
        headers: {
          'Accept': 'application/json',
          'Content-Type': 'application/json'
        }
      }).catch(err => console.error('Playback stop failed:', err));
    }
    
    console.log('Playback: Race complete');
    stopPlayback();
  }, totalTime * 1000);
  
  playbackTimeouts.push(stopTimeout);
}

function stopPlayback() {
  const playBtn = document.getElementById('playbackBtn');
  const stopBtn = document.getElementById('stopPlaybackBtn');
  const playhead = document.getElementById('timelinePlayhead');
  
  if (playBtn) playBtn.style.display = 'inline-block';
  if (stopBtn) stopBtn.style.display = 'none';
  
  // Hide playhead
  if (playhead) {
    playhead.classList.remove('active');
  }
  
  // Clear playhead animation interval
  if (playbackInterval) {
    clearInterval(playbackInterval);
    playbackInterval = null;
  }
  
  // Silence any callouts already queued or mid-sentence.  Clearing the
  // timeouts below only stops FUTURE laps from being announced — speech
  // already handed to the announcer would otherwise keep talking over a
  // playback the user has explicitly stopped.
  if (audioAnnouncer && typeof audioAnnouncer.clearQueue === 'function') {
    try { audioAnnouncer.clearQueue(); } catch (e) { console.warn('clearQueue failed:', e); }
  }

  // Clear all scheduled timeouts
  playbackTimeouts.forEach(timeout => clearTimeout(timeout));
  playbackTimeouts = [];
  
  // Remove timeline highlights
  document.querySelectorAll('.timeline-event').forEach(event => {
    event.style.transform = event.style.transform.replace(' scale(1.3)', '');
  });
}

function highlightTimelineEvent(index) {
  const events = document.querySelectorAll('.timeline-event');
  // index + 1 to skip the "Race Start" event
  if (events[index + 1]) {
    events[index + 1].style.transform = 'translate(-50%, -50%) scale(1.3)';
    setTimeout(() => {
      events[index + 1].style.transform = 'translate(-50%, -50%)';
    }, 500);
  }
}

function switchDetailMode(mode, forceRender = false) {
  const tabs = document.querySelectorAll('#raceDetails .analysis-tab');
  const isAlreadyActive = mode === 'history'
    ? tabs[0]?.classList.contains('active')
    : tabs[1]?.classList.contains('active');

  // Only prevent re-render if already active AND not forced
  if (isAlreadyActive && !forceRender) {
    return;
  }

  tabs.forEach(tab => tab.classList.remove('active'));

  if (mode === 'history') {
    tabs[0].classList.add('active');
    renderDetailHistory();
  } else if (mode === 'fastestRound') {
    tabs[1].classList.add('active');
    renderDetailFastestRound();
  }
}

function renderDetailHistory() {
  if (!currentDetailRace) return;
  
  const lapTimes = currentDetailRace.lapTimes.map(t => t / 1000);
  // Show every lap of the saved race, not just a recent window.
  const displayLaps = lapTimes;
  const maxTime = lapTimes.length ? Math.max(...displayLaps) : 0;
  
  // Get track distance if available
  const trackDistance = currentDetailRace.totalDistance || 0;
  const hasTrackData = trackDistance > 0 && currentDetailRace.lapTimes.length > 0;
  const perLapDistance = hasTrackData ? trackDistance / currentDetailRace.lapTimes.length : 0;
  
  // Calculate total race time.  Excluded laps still count here — excluding a
  // lap removes it from the summary, it does not un-fly it.
  const totalTime = lapTimes.reduce((sum, t) => sum + t, 0);
  const excludedSet = new Set(
    Array.isArray(currentDetailRace.excludedLaps) ? currentDetailRace.excludedLaps : []);

  let html = '<div class="analysis-bars">';
  displayLaps.forEach((time, index) => {
    const actualIndex = index;
    let label;
        
    // First entry is Gate 1 (start), not a lap
    if (actualIndex === 0) {
      label = '1st Cross';
    } else {
      label = `Lap ${actualIndex}`;
    }
    
    const timeStr = formatMsDisplay(Math.round(time * 1000));

    // Add distance info if available: "Lap x - y/z m"
    if (hasTrackData && actualIndex > 0) {
      label = `${timeStr}\n${label} - ${perLapDistance.toFixed(0)}m`;
    } else if (hasTrackData && actualIndex === 0) {
      label = `${timeStr}\n1st Cross (Start)`;
    } else if (actualIndex === 0) {
      label = '1st Cross';
    }

    const displayTime = hasTrackData ? '' : timeStr; // Don't show time in bar if it's in label
    // Lap 0 gets no control — the first crossing is not a timed lap and is
    // already outside every statistic.
    html += createBarItemWithColor(label, time, maxTime, displayTime, index, {
      excludeLapNo: actualIndex > 0 ? actualIndex : null,
      excluded: excludedSet.has(actualIndex)
    });
  });
  html += '</div>';
  html += `<p style="text-align: center; margin-top: 16px; font-weight: bold; color: var(--primary-color);">Total Race Time: ${formatMsDisplay(Math.round(totalTime * 1000))}</p>`;
  
  document.getElementById('detailContent').innerHTML = html;
}

function renderDetailFastestRound() {
  if (!currentDetailRace) return;

  const laps = currentDetailRace.lapTimes.map(t => t / 1000);
  const excluded = new Set(
    Array.isArray(currentDetailRace.excludedLaps) ? currentDetailRace.excludedLaps : []);

  const best = findFastestRound(laps, excluded);
  if (!best) {
    document.getElementById('detailContent').innerHTML =
      '<p class="no-data">Not enough consecutive laps for a fastest round</p>';
    return;
  }
  document.getElementById('detailContent').innerHTML = _fastestRoundHtml(best);
}

// Download the race history AS THIS TAB HOLDS IT, not as the device holds it.
//
// This used to link straight to /api/races/download, which serves the
// firmware's copy.  That copy has no concept of excluded laps — the firmware
// was never told about them — so every exclusion made on this page was silently
// dropped from the downloaded file.  Building the JSON client-side is what
// makes "those choices persist when the download button is clicked" true.
//
// The shape matches what importRaces() expects, so a file downloaded here
// re-imports cleanly, exclusions and all.
function downloadRaces() {
  if (!Array.isArray(raceHistoryData) || raceHistoryData.length === 0) {
    alert('No races to download');
    return;
  }

  const races = raceHistoryData.map(r => ({
    timestamp:      r.timestamp || 0,
    lapTimes:       Array.isArray(r.lapTimes) ? r.lapTimes : [],
    excludedLaps:   Array.isArray(r.excludedLaps) ? [...r.excludedLaps].sort((a, b) => a - b) : [],
    fastestLap:     r.fastestLap || 0,
    medianLap:      r.medianLap || 0,
    best3LapsTotal: r.best3LapsTotal || 0,
    pilotName:      r.pilotName || '',
    frequency:      r.frequency || 0,
    band:           r.band || '',
    channel:        r.channel || 0,
    name:           r.name || '',
    tag:            r.tag || '',
    trackId:        r.trackId || 0,
    trackName:      r.trackName || '',
    totalDistance:  (typeof r.totalDistance === 'number') ? r.totalDistance : 0.0
  }));

  const blob = new Blob([JSON.stringify({ races }, null, 2)], { type: 'application/json' });
  const url  = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  // Same name as the file this history was imported from, so an edit-and-save
  // round trip replaces the original instead of accumulating copies.  Falls
  // back to a timestamped name when the history came from the device.
  a.download = importedRacesFileName || `races-${Math.floor(Date.now() / 1000)}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);

  // The file now matches this tab, so there is nothing outstanding to save.
  raceHistoryDirty = false;
  updateHistoryDownloadButton();
}

// Per-race Download button removed 2026-09-15: it linked to the firmware's copy
// of the race (/races/downloadOne), which has no excludedLaps, so it would have
// handed back a file missing the exclusions shown on screen.  The Download Race
// button in the history controls builds from this tab's data instead.
// downloadSingleRace() deleted with it — no remaining callers.

let editingRaceIndex = null;

function openEditModal(index) {
  editingRaceIndex = index;
  const race = raceHistoryData[index];
  // Same stale-index risk as viewRaceDetails: the index comes from baked-in HTML and
  // may no longer be valid. Bail rather than throw on an undefined race.
  if (!race) return;

  document.getElementById('raceName').value = race.name || '';
  document.getElementById('raceTag').value = race.tag || '';
  document.getElementById('raceDistance').value = race.totalDistance || 0;
  
  // Populate lap times for marshalling mode
  renderEditLapsList(race.lapTimes);
  
  document.getElementById('editRaceModal').style.display = 'flex';
}

function renderEditLapsList(lapTimes) {
  const container = document.getElementById('editLapsList');
  let html = '';
  
  lapTimes.forEach((lapTime, index) => {
    const lapSeconds = (lapTime / 1000).toFixed(3);
    const lapLabel = index === 0 ? '1st Cross' : `Lap ${index}`;
    html += `
      <div style="display: flex; align-items: center; gap: 8px; padding: 8px; background-color: var(--bg-secondary); border-radius: 4px;">
        <span style="min-width: 60px; font-weight: ${index === 0 ? 'bold' : 'normal'}; color: ${index === 0 ? 'var(--accent-color)' : 'var(--primary-color)'}">${lapLabel}</span>
        <input type="number" step="0.001" min="0" value="${lapSeconds}" 
               data-lap-index="${index}" 
               style="flex: 1; padding: 6px; background-color: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 4px; color: var(--primary-color);" 
               title="Edit lap time in seconds" />
        <span style="min-width: 20px;">s</span>
        <button onclick="deleteLapFromEdit(${index})" 
                style="padding: 4px 10px; background-color: var(--danger-color); border: none; border-radius: 4px; color: white; cursor: pointer; font-size: 18px; line-height: 1;" 
                title="Delete this lap">&times;</button>
      </div>
    `;
  });
  
  container.innerHTML = html;
}

function deleteLapFromEdit(index) {
  if (editingRaceIndex === null) return;
  const race = raceHistoryData[editingRaceIndex];
  
  if (race.lapTimes.length <= 1) {
    alert('Cannot delete the last lap. Delete the entire race instead.');
    return;
  }
  
  if (confirm('Delete this lap?')) {
    race.lapTimes.splice(index, 1);
    renderEditLapsList(race.lapTimes);
  }
}

function addNewLapToEdit() {
  if (editingRaceIndex === null) return;
  const race = raceHistoryData[editingRaceIndex];
  
  // Add a new lap with a default value (average of existing laps)
  let defaultValue = 0;
  if (race.lapTimes.length > 0) {
    const sum = race.lapTimes.reduce((a, b) => a + b, 0);
    defaultValue = Math.round(sum / race.lapTimes.length);
  } else {
    defaultValue = 10000; // 10 seconds default
  }
  
  race.lapTimes.push(defaultValue);
  renderEditLapsList(race.lapTimes);
  
  // Scroll to bottom to show the new lap
  const container = document.getElementById('editLapsList');
  container.scrollTop = container.scrollHeight;
}

function closeEditModal() {
  document.getElementById('editRaceModal').style.display = 'none';
  editingRaceIndex = null;
}

function closeEditModalOnBackdrop(event) {
  // Only close if clicking the backdrop (not the modal content)
  if (event.target.id === 'editRaceModal') {
    closeEditModal();
  }
}

function saveRaceEdit() {
  if (editingRaceIndex === null) return;
  
  const race = raceHistoryData[editingRaceIndex];
  const name = document.getElementById('raceName').value;
  const tag = document.getElementById('raceTag').value;
  const distance = parseFloat(document.getElementById('raceDistance').value) || 0;
  
  // Collect updated lap times from inputs
  const lapInputs = document.querySelectorAll('#editLapsList input[type="number"]');
  const updatedLapTimes = [];
  let hasError = false;
  
  lapInputs.forEach(input => {
    const value = parseFloat(input.value);
    if (isNaN(value) || value <= 0) {
      hasError = true;
      input.style.borderColor = '#e74c3c';
    } else {
      input.style.borderColor = '';
      // Convert seconds to milliseconds
      updatedLapTimes.push(Math.round(value * 1000));
    }
  });
  
  if (hasError) {
    alert('Please enter valid lap times (positive numbers)');
    return;
  }
  
  if (updatedLapTimes.length === 0) {
    alert('Cannot save race with no laps. Delete the race instead.');
    return;
  }
  
  // First update metadata (name/tag/distance)
  const formData = new URLSearchParams();
  formData.append('timestamp', race.timestamp);
  formData.append('name', name);
  formData.append('tag', tag);
  formData.append('totalDistance', distance);
  
  fetch('/races/update', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded'
    },
    body: formData
  })
  .then(response => response.json())
  .then(data => {
    console.log('Race metadata updated:', data);
    
    // Then update lap times if they changed
    return fetch('/races/updateLaps', {
      method: 'POST',
      headers: {
        'Accept': 'application/json',
        'Content-Type': 'application/json'
      },
      body: JSON.stringify({
        timestamp: race.timestamp,
        lapTimes: updatedLapTimes
      })
    });
  })
  .then(response => response.json())
  .then(data => {
    console.log('Race laps updated:', data);
    loadRaceHistory();
    closeEditModal();
  })
  .catch(error => {
    console.error('Error updating race:', error);
    alert('Error updating race');
  });
}

async function importRaces(input) {
  const file = input?.files?.[0];
  if (!file) return;

  // Remember the name so Download Race writes back over the same file rather
  // than scattering races-<timestamp>.json copies beside it.  Editing
  // exclusions and re-downloading is a round trip on ONE file, and the browser
  // will offer to replace it by name.
  importedRacesFileName = file.name || '';

  // Allow re-selecting the same file
  input.value = '';

  let json;
  try {
    const text = await file.text();
    json = JSON.parse(text);
  } catch (e) {
    console.error('Error parsing races JSON:', e);
    alert('Invalid JSON file');
    return;
  }

  // Reject multi-pilot race files
  if (json?.type === 'MultiRace') {
    alert('This is a multi-pilot race file. Import a single pilot race here or use the Import Race button in master mode.');
    return;
  }

  // Normalize expected shape: { races:[...] } or [...] (array)
  const racesArray = Array.isArray(json) ? json : (Array.isArray(json?.races) ? json.races : null);
  if (!racesArray) {
    alert('Race file format not recognized. Expected {"races":[...]} or an array.');
    return;
  }

  // Capture the exclusions BEFORE uploading.  The upload round-trips through
  // the device, which drops the field, so the file is the only place they
  // exist — read them now or lose them.
  racesArray.forEach(r => {
    if (Array.isArray(r?.excludedLaps) && r.excludedLaps.length) {
      rememberRaceExclusions(Number(r.timestamp), r.excludedLaps);
    }
  });

  // 1) Try bulk upload first (fast path)
  try {
    const resp = await fetch('/races/upload', {
      method: 'POST',
      headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
      body: JSON.stringify({ races: racesArray })
    });

    const ct = (resp.headers.get('content-type') || '').toLowerCase();
    const bodyText = await resp.text().catch(() => '');

    if (!resp.ok) {
      throw new Error(`Bulk upload failed: HTTP ${resp.status} ${resp.statusText} ${bodyText}`);
    }

    let result = null;
    if (ct.includes('application/json') && bodyText.trim()) {
      result = JSON.parse(bodyText);
    } else {
      // If server responded OK but not JSON, still treat as success
      result = { status: "OK" };
    }

    if (result?.status === "OK") {
      console.log('Races imported (bulk):', result);
      await loadRaceHistory();
      alert('Races imported successfully!');
      return;
    }

    throw new Error(`Bulk upload returned non-OK: ${bodyText || JSON.stringify(result)}`);
  } catch (bulkErr) {
    console.warn('[ImportRaces] Bulk upload failed; falling back to per-race save:', bulkErr);
  }

  // 2) Fallback: upload each race via /races/save (smaller payloads)
  try {
    let successCount = 0;

    for (let i = 0; i < racesArray.length; i++) {
      const r = racesArray[i];

      // The firmware /races/save expects RaceSession-ish fields.
      // We keep the structure tolerant.
      const payload = {
        timestamp: r.timestamp || r.time || Date.now(),
        fastestLap: r.fastestLap || 0,
        medianLap: r.medianLap || 0,
        best3LapsTotal: r.best3LapsTotal || 0,
        pilotName: r.pilotName || r.pilotCallsign || "",
        frequency: r.frequency || 0,
        band: r.band || "",
        channel: r.channel || 0,
        trackId: r.trackId || 0,
        trackName: r.trackName || "",
        totalDistance: (typeof r.totalDistance === 'number') ? r.totalDistance : 0.0,
        lapTimes: Array.isArray(r.lapTimes) ? r.lapTimes : [],
        // Preserve the summary exclusions through an import.  Absent in files
        // written before the feature existed, which normalise to "none
        // excluded" — the behaviour those files were saved with.
        excludedLaps: Array.isArray(r.excludedLaps)
          ? r.excludedLaps.map(n => parseInt(n, 10)).filter(n => Number.isFinite(n) && n > 0)
          : []
      };

      const resp = await fetch('/races/save', {
        method: 'POST',
        headers: { 'Accept': 'application/json', 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      const txt = await resp.text().catch(() => '');
      if (!resp.ok) {
        throw new Error(`Race ${i + 1}/${racesArray.length} failed: HTTP ${resp.status} ${resp.statusText} ${txt}`);
      }

      // If response is JSON, check status
      let ok = true;
      try {
        const ct = (resp.headers.get('content-type') || '').toLowerCase();
        if (ct.includes('application/json') && txt.trim()) {
          const j = JSON.parse(txt);
          if (j?.status && j.status !== 'OK') ok = false;
        }
      } catch (e) {}

      if (!ok) {
        throw new Error(`Race ${i + 1}/${racesArray.length} returned non-OK: ${txt}`);
      }

      successCount++;
    }

    await loadRaceHistory();
    alert(`Races imported successfully! (${successCount}/${racesArray.length})`);
  } catch (e) {
    console.error('Error importing races (fallback):', e);
    alert(`Error importing races: ${e?.message || e}`);
  }
}

function deleteRace(timestamp) {
  if (!confirm('Delete this race?')) return;
  
  const formData = new URLSearchParams();
  formData.append('timestamp', timestamp);
  
  fetch('/races/delete', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded'
    },
    body: formData
  })
  .then(response => response.json())
  .then(data => {
    console.log('Race deleted:', data);
    loadRaceHistory();
    if (currentDetailRace && currentDetailRace.timestamp === timestamp) {
      closeRaceDetails();
    }
  })
  .catch(error => console.error('Error deleting race:', error));
}

function clearAllRaces() {
  if (!confirm('Are you sure you want to clear all race history? This cannot be undone.')) return;
  
  fetch('/races/clear', {
    method: 'POST',
    headers: {
      'Accept': 'application/json',
      'Content-Type': 'application/json'
    }
  })
  .then(response => response.json())
  .then(data => {
    console.log('All races cleared:', data);
    loadRaceHistory();
    closeRaceDetails();
  })
  .catch(error => console.error('Error clearing races:', error));
}

// Config Download/Import Functions
function downloadConfig() {
  // Warn if there are unsaved staged changes — the export reflects what's
  // *committed to firmware*, so unsaved UI tweaks will not be in the file.
  if (typeof stagedDirty !== 'undefined' && stagedDirty) {
    if (!confirm(
        'You have unsaved configuration changes. The downloaded file will ' +
        'reflect the device\'s currently SAVED state, not your unsaved edits.\n\n' +
        'Cancel and click Apply & Save first, or proceed to download the saved snapshot.'
    )) {
      return;
    }
  }

  fetch('/config')
    .then(response => response.json())
    .then(config => {
      // `config` is what /config returns — i.e. every field Config::toJson()
      // serialises, which covers the entire laptimer_config_t struct.  Type
      // and naming are authoritative (uint32 colours stay uint32; hex strings
      // stay strings).  We only *add* a small set of browser-only fields that
      // the firmware doesn't know about — never override anything from /config
      // (the previous version did, which broke round-tripping for fields with
      //  a DOM-string vs firmware-uint32 mismatch like pilotColor and the
      //  LED fade/strobe colours).
      const fullConfig = {
        ...config,                                                    // authoritative firmware state
        // Browser-only state not stored on the device:
        audioEnabled,                                                 // current Enable Voice state in this tab
        ttsEngine: localStorage.getItem('ttsEngine') || 'piper',      // TTS engine choice (not in firmware config)
        // Metadata:
        timestamp: new Date().toISOString(),
        exportSchemaVersion: 1,
      };

      const dataStr = JSON.stringify(fullConfig, null, 2);
      const dataBlob = new Blob([dataStr], { type: 'application/json' });
      const url = URL.createObjectURL(dataBlob);
      const link = document.createElement('a');
      link.href = url;
      link.download = `fpvraceone-config-${new Date().toISOString().slice(0,10)}.json`;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      URL.revokeObjectURL(url);
    })
    .catch(error => {
      console.error('Error downloading config:', error);
      alert('Error downloading configuration: ' + (error.message || error));
    });
}

function importConfig(input) {
  const file = input.files[0];
  if (!file) return;

  const reader = new FileReader();
  reader.onload = function(e) {
    let config;
    try {
      config = JSON.parse(e.target.result);
    } catch (error) {
      console.error('Error parsing config JSON:', error);
      alert('Invalid configuration file — could not parse JSON.');
      input.value = '';
      return;
    }

    // Send the ENTIRE imported config object to the firmware.  Config::fromJson
    // is defensive: it wraps every field in `if (source.containsKey(...))`, so
    // unknown / browser-only keys (timestamp, audioEnabled, ttsEngine,
    // ledSolidColor, batteryMonitoring) are silently ignored.
    //
    // This avoids the old cherry-pick approach that silently dropped any
    // firmware field whose JSON name wasn't on the hard-coded allow-list —
    // including (most recently) gate1Bootstrap, v1Smoothing, nodeMode,
    // masterSSID, masterPassword, mnSkipMasterStart, otaIncludePrereleases,
    // wifiExtAntenna, wifiTxPower, pilotColor, theme, voiceEnabled.
    fetch('/config', {
      method: 'POST',
      headers: {
        'Accept': 'application/json',
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(config),
    })
    .then(response => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return response.json();
    })
    .then(data => {
      console.log('[ConfigImport] Firmware applied:', data);

      // Mirror the small set of fields the browser also caches in
      // localStorage.  We reload right after, so the page will re-fetch the
      // authoritative values from /config — this just avoids a brief flash
      // of the previous theme / voice / lap-format on first paint.
      // Store the RESOLVED slug: the <head> script applies this verbatim, so a
      // legacy alias here would paint the default until /config corrected it.
      if (config.theme)         localStorage.setItem('theme',         resolveTheme(config.theme));
      if (config.lapFormat)     localStorage.setItem('lapFormat',     config.lapFormat);
      if (config.selectedVoice) localStorage.setItem('selectedVoice', config.selectedVoice);
      if (config.ttsEngine)     localStorage.setItem('ttsEngine',     config.ttsEngine);
      if (config.pilotColor)    localStorage.setItem('pilotColor',    config.pilotColor);

      alert('Configuration imported successfully — page will reload to apply.');
      setTimeout(() => location.reload(), 500);
    })
    .catch(error => {
      console.error('[ConfigImport] Error applying config:', error);
      alert('Error importing configuration: ' + (error.message || error));
    });
  };
  reader.readAsText(file);
  input.value = '';   // reset so re-selecting the same file triggers another onchange
}

// Recording capacity is now NEGOTIATED, not assumed.
//
// The firmware used to hold a fixed 5000-sample buffer in static RAM for the
// whole life of every boot — 25,000 bytes, a third of all static RAM, for a
// bench feature.  It now allocates on demand and grants the largest tier the
// heap can spare (5000 / 3000 / 1500 / 750 / 375 samples), returning the
// granted size from POST /calibration/start.
//
// These constants are therefore only the DEFAULT / upper bound.  The live
// values live in wizardState.maxSamples and wizardState.autoStopSamples, set
// from the start response.  Use those, not these, anywhere the cap matters.
//
// 20 ms is the sample interval in laptimer.cpp's CALIBRATION_WIZARD branch.
// When the buffer fills, further samples are silently dropped — so auto-stop a
// beat before the cap, giving the polling loop time to react and surface a
// friendly prompt instead.
//
// DECLARATION ORDER MATTERS: these must precede wizardState below, which reads
// them in its initializer.  `const` is not hoisted the way `var` is — it sits in
// the temporal dead zone until this line executes, so referencing it earlier
// throws a ReferenceError at load and kills every statement after it in the
// file.  That is not a syntax error, so `node --check` does not catch it.
const WIZARD_MAX_SAMPLES        = 5000;
const WIZARD_SAMPLE_INTERVAL_MS = 20;
const WIZARD_MAX_DURATION_SEC   = (WIZARD_MAX_SAMPLES * WIZARD_SAMPLE_INTERVAL_MS) / 1000;  // 100
const WIZARD_AUTO_STOP_SAMPLES  = 4900;   // ~98 s, leaves ~2 s of poll headroom under the cap
// Stop this far below the granted cap, scaled the same way (98% of capacity).
const WIZARD_AUTO_STOP_FRACTION = 0.98;

// Calibration Wizard
let wizardState = {
  recording: false,
  data: [],
  markers: [], // Array of {index, lap: 1|2|3} - only peaks!
  currentLap: 1,
  chart: null,
  calculatedEnter: 0,
  calculatedExit: 0,
  // Capacity granted by the firmware for THIS session (see the notes on
  // WIZARD_MAX_SAMPLES).  Defaults assume the full buffer so a firmware that
  // doesn't report capacity still behaves exactly as before.
  maxSamples: WIZARD_MAX_SAMPLES,
  autoStopSamples: WIZARD_AUTO_STOP_SAMPLES,
  maxSeconds: WIZARD_MAX_DURATION_SEC
};

// Calibration wizard target.  0 = local (the wizard runs against this device's
// own /calibration/* endpoints — single-mode behaviour).  >0 = remote: the
// wizard polls /api/multinode/calibration/* with this nodeId and the master
// proxies every call to the matching client.  Set by mnStartCalibrationWizard
// before opening the wizard; cleared by closeCalibrationWizard.
let wizardTargetNodeId = 0;

// Build a wizard URL.  In remote mode every wizard call routes through the
// master's proxy with the target nodeId appended; in local mode the original
// /calibration/<suffix> path is used.  extraQuery is concatenated with the
// correct separator (& or ?) so callers don't have to think about it.
function _wizardPath(suffix, extraQuery) {
  if (wizardTargetNodeId > 0) {
    let url = `/api/multinode/calibration/${suffix}?nodeId=${wizardTargetNodeId}`;
    if (extraQuery) url += `&${extraQuery}`;
    return url;
  }
  let url = `/calibration/${suffix}`;
  if (extraQuery) url += `?${extraQuery}`;
  return url;
}

// Open the wizard ARMED — instructions on screen, nothing recording yet.
//
// Recording used to begin the instant this ran, which meant the pilot spent the
// buffer reading the instructions: the window is ~100 s total, and the clock was
// already going before they had walked back to the gate.  The device only starts
// sampling when they press Start Recording (beginCalibrationRecording).
//
// Every caller of this function means "open the wizard" — the Calibration tab
// button, the start-over path at the duration cap, the re-fly after a peak
// spread warning, and the master's client handoff — so they all get the armed
// state without changing a single call site.
function startCalibrationWizard() {
  // Stop any previous run cleanly
  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }
  if (wizardAbortController) {
    try { wizardAbortController.abort(); } catch (e) {}
  }
  wizardAbortController = new AbortController();

  // Reset wizard state
  wizardState = {
    recording: false,
    data: [],
    markers: [],
    currentLap: 1,
    chart: null,
    maxSamples: WIZARD_MAX_SAMPLES,
    autoStopSamples: WIZARD_AUTO_STOP_SAMPLES,
    maxSeconds: WIZARD_MAX_DURATION_SEC,
    calculatedEnter: 0,
    calculatedExit: 0
  };

  // Show modal and recording screen
  document.getElementById('calibrationWizardModal').style.display = 'flex';
  document.getElementById('wizardRecording').style.display = 'block';
  document.getElementById('wizardMarking').style.display = 'none';
  document.getElementById('wizardResults').style.display = 'none';
  const capNote = document.getElementById('wizardCapacityNote');
  if (capNote) { capNote.textContent = ''; capNote.style.display = 'none'; }
  // Clear the previous run's save confirmation.  Without this a second wizard
  // would open showing a stale "Saved to the timer" and a hidden Apply button
  // before the new recording has produced anything at all.
  const savedNote = document.getElementById('wizardResultsSavedNote');
  if (savedNote) { savedNote.textContent = ''; savedNote.style.display = 'none'; }
  const applyBtn = document.getElementById('wizardApplyButton');
  if (applyBtn) applyBtn.style.display = '';
  const resultsCloseBtn = document.getElementById('wizardResultsCloseButton');
  if (resultsCloseBtn) resultsCloseBtn.textContent = 'Close';

  _setWizardRecordingUI(false);
}

// Flip the recording panel between its two states.  One button does both jobs,
// so its label and handler are the state — there is no way for the screen to
// claim it is recording while the device is not, or the reverse.
function _setWizardRecordingUI(recording) {
  const indicator = document.getElementById('wizardRecordingIndicator');
  const counter   = document.getElementById('wizardSampleCount');
  const btn       = document.getElementById('wizardRecordButton');
  const hint      = document.getElementById('wizardArmedHint');

  if (indicator) indicator.style.display = recording ? '' : 'none';
  if (counter) {
    counter.style.display = recording ? '' : 'none';
    if (!recording) counter.textContent = 'Samples: 0';
  }
  if (hint) hint.style.display = recording ? 'none' : '';
  if (btn) {
    btn.textContent = recording ? 'Stop Recording' : 'Start Recording';
    btn.classList.toggle('wizard-record-active', !!recording);
  }
}

// The single Start/Stop Recording button.  Dispatches on the state the wizard
// is actually in rather than on which label is showing.
function toggleCalibrationRecording() {
  if (wizardState.recording) {
    stopCalibrationWizard();
  } else {
    beginCalibrationRecording();
  }
}

// Ask the device for a recording buffer and start sampling.  Everything from
// here down is what used to run automatically when the modal opened.
function beginCalibrationRecording() {
  if (wizardState.recording) return;   // double-click guard

  const btn = document.getElementById('wizardRecordButton');
  if (btn) btn.disabled = true;        // no second POST while this one is open

  fetch(_wizardPath('start'), { method: 'POST', signal: wizardAbortController.signal })
    .then(async (response) => {
      if (!response.ok) {
        const t = await response.text().catch(() => '');
        // 503 is specifically "not enough memory to allocate the recording
        // buffer" — a distinct, actionable condition, not a generic failure.
        // Surface the firmware's own message rather than burying it in a
        // console log the user will never see.
        if (response.status === 503) {
          let msg = 'The timer does not have enough free memory to start calibration. '
                  + 'Reboot the timer and try again.';
          try {
            const j = JSON.parse(t);
            if (j && j.error) msg = j.error;
          } catch (_) { /* not JSON — keep the default wording */ }
          const err = new Error(msg);
          err.userMessage = msg;
          throw err;
        }
        throw new Error(`POST calibration/start failed: HTTP ${response.status} ${response.statusText} ${t}`);
      }
      // The response carries the capacity the firmware actually granted.  Parse
      // it if we can, but never make recording depend on parsing succeeding —
      // an older firmware returns {"status":"OK"} with no capacity fields, and
      // must still work.
      let granted = null;
      try {
        const body = await response.json();
        if (body && Number.isFinite(body.maxSamples) && body.maxSamples > 0) {
          granted = body.maxSamples;
        }
      } catch (_) { /* older firmware, or non-JSON — fall back below */ }

      wizardState.maxSamples = granted || WIZARD_MAX_SAMPLES;
      wizardState.autoStopSamples = granted
        ? Math.max(1, Math.floor(granted * WIZARD_AUTO_STOP_FRACTION))
        : WIZARD_AUTO_STOP_SAMPLES;
      wizardState.maxSeconds =
        (wizardState.maxSamples * WIZARD_SAMPLE_INTERVAL_MS) / 1000;

      // Only mention the limit when it is NOT the full 100 s — otherwise this
      // is noise about something the user never had reason to think about.
      if (granted && granted < WIZARD_MAX_SAMPLES) {
        const note = document.getElementById('wizardCapacityNote');
        if (note) {
          note.textContent =
            `Memory is limited right now — recording up to ${Math.round(wizardState.maxSeconds)} s.`;
          note.style.display = 'block';
        }
      }

      wizardState.recording = true;
      // Only now does the screen claim to be recording — after the device has
      // confirmed it granted a buffer.
      if (btn) btn.disabled = false;
      _setWizardRecordingUI(true);
      wizardRecordingLoop();
    })
    .catch(error => {
      if (btn) btn.disabled = false;
      if (error?.name === 'AbortError') return;
      console.error('Error starting calibration wizard:', error);
      alert(error?.userMessage || 'Error starting calibration wizard');
      // Stay on the armed screen rather than closing.  A failed start is often
      // transient (the 503 is "reboot and try again"), and dumping the user out
      // of the wizard makes them re-open it to find out.
      _setWizardRecordingUI(false);
    });
}


async function fetchCalibrationMeta(signal) {
  const resp = await fetch(_wizardPath('data', 'limit=0'), { signal });
  if (!resp.ok) {
    const t = await resp.text().catch(() => '');
    throw new Error(`GET calibration/data?limit=0 failed: HTTP ${resp.status} ${resp.statusText} ${t}`);
  }
  return await resp.json(); // { total: N }
}

async function fetchCalibrationPage(offset, limit, signal) {
  const resp = await fetch(_wizardPath('data', `offset=${offset}&limit=${limit}`), { signal });
  if (!resp.ok) {
    const t = await resp.text().catch(() => '');
    throw new Error(`GET calibration/data page failed: HTTP ${resp.status} ${resp.statusText} ${t}`);
  }
  return await resp.json(); // { total, offset, limit, count, data:[...] }
}

// Pause that respects the wizard's AbortController, so cancelling during a
// download doesn't have to wait out the backoff.
function _wizardSleep(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      return reject(new DOMException('Aborted', 'AbortError'));
    }
    const id = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    function onAbort() {
      clearTimeout(id);
      reject(new DOMException('Aborted', 'AbortError'));
    }
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

// One page, with retries.
//
// The device refuses connections when these requests go out back-to-back: the
// recording loop polls the very same endpoint every 200 ms for minutes without
// a single failure, while an unpaced download fails with "Failed to fetch"
// partway through.  Spacing is the variable, so pace the requests and retry the
// ones that still lose the race.  The offset is carried into the final error
// because how FAR the download got is the first thing worth knowing.
async function _fetchCalibrationPageRetrying(offset, limit, signal, attempts = 4) {
  let lastErr = null;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      return await fetchCalibrationPage(offset, limit, signal);
    } catch (e) {
      if (e?.name === 'AbortError') throw e;
      lastErr = e;
      console.warn(`[calibration] page at offset ${offset} attempt ${attempt}/${attempts} failed:`, e);
      if (attempt < attempts) await _wizardSleep(150 * attempt, signal);   // 150 / 300 / 450 ms
    }
  }
  throw new Error(
    `page at offset ${offset} failed after ${attempts} attempts — ${lastErr?.message || lastErr}`);
}

async function fetchAllCalibrationData(signal) {
  const meta = await fetchCalibrationMeta(signal);
  const total = meta.total || 0;

  const all = [];
  let offset = 0;

  while (offset < total) {
    const page = await _fetchCalibrationPageRetrying(offset, CALIBRATION_PAGE_SIZE, signal);
    if (Array.isArray(page.data) && page.data.length) {
      all.push(...page.data);
      offset += page.data.length;
      // Breathing room between pages.  200 ms is not a tuned number — it is
      // exactly the cadence wizardRecordingLoop() sustains against this same
      // endpoint for the whole recording, hundreds of consecutive connections,
      // without ever failing.  Ten pages therefore cost ~2 s of deliberate
      // waiting, which is invisible behind the spinner.
      if (offset < total) await _wizardSleep(200, signal);
    } else {
      break;
    }
  }

  return { total, data: all };
}


function wizardRecordingLoop() {
  if (!wizardState.recording) return;

  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }

  fetchCalibrationMeta()
    .then(meta => {
      const total = meta.total || 0;
      document.getElementById('wizardSampleCount').textContent = `Samples: ${total}`;
      if (total >= wizardState.autoStopSamples) {
        // Hit the firmware buffer cap.  Auto-stop before the firmware starts
        // silently dropping samples and prompt the user to keep this recording
        // or retry with a shorter session.
        wizardHandleMaxDurationReached();
        return;
      }
      if (wizardState.recording) {
        wizardRecordingTimerId = setTimeout(wizardRecordingLoop, 200);
      }
    })
    .catch(error => {
      console.error('[wizardRecordingLoop] Error fetching calibration meta:', error);
      if (wizardState.recording) {
        wizardRecordingTimerId = setTimeout(wizardRecordingLoop, 500);
      }
    });
}

// Invoked from wizardRecordingLoop when the sample count reaches the auto-stop
// threshold (~98 s into a 100 s buffer).  Halts polling, asks the user whether
// to use what was captured or restart with a shorter session, and routes to
// the matching path.  Confirm-style prompt rather than a forced retry so a
// pilot who happened to fit 3 clean laps inside the cap doesn't lose data.
async function wizardHandleMaxDurationReached() {
  wizardState.recording = false;
  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }
  // The limit is per-session: the firmware grants whatever recording capacity
  // the heap can spare, so quote the real number rather than a fixed 100 s.
  const limitSec = Math.round(wizardState.maxSeconds || WIZARD_MAX_DURATION_SEC);
  const mins = Math.floor(limitSec / 60);
  const secs = String(limitSec % 60).padStart(2, '0');
  const useThisRecording = confirm(
    `Recording reached the maximum duration of ${limitSec} seconds (${mins}:${secs}).\n\n` +
    `Click OK to mark gate crossings with this recording, or Cancel to start over ` +
    `with a shorter session (fly to the next gate and back 3 times faster).`
  );
  if (useThisRecording) {
    // Use the normal stop path so the wizard transitions to Marking with the
    // captured data — no special-case handling required downstream.
    await stopCalibrationWizard();
  } else {
    // Back to the armed screen.  The firmware's sample count is not reset here
    // any more — startCalibrationWizard() no longer posts /calibration/start.
    // It is reset when the pilot presses Start Recording, since
    // LapTimer::startCalibrationWizard() frees and reallocates the buffers.
    // In between, the device keeps sampling into a buffer that is already full
    // and silently drops the overflow, which costs nothing.
    startCalibrationWizard();
  }
}

// Show / hide the wizard's "Calculating…" spinner panel.  Used during the two
// places where the UI used to look frozen for several seconds: fetching the
// recorded calibration buffer after Stop Recording, and the FWHM/threshold
// math triggered by Calculate Thresholds.
function showWizardProcessing(title, detail) {
  const panel  = document.getElementById('wizardProcessing');
  const tEl    = document.getElementById('wizardProcessingTitle');
  const dEl    = document.getElementById('wizardProcessingDetail');
  if (tEl && title)  tEl.textContent  = title;
  if (dEl && detail) dEl.textContent  = detail;
  if (panel) panel.style.display = '';
}
function hideWizardProcessing() {
  const panel = document.getElementById('wizardProcessing');
  if (panel) panel.style.display = 'none';
}

// Force a paint cycle so a freshly-shown spinner actually appears on screen
// before we start a long synchronous task.  Without this the browser may
// batch layout work and skip displaying the spinner entirely.
function nextPaint() {
  return new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
}

async function stopCalibrationWizard() {
  wizardState.recording = false;

  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }

  // Hide recording UI immediately and put up the spinner.  The fetch + draw
  // below can take a few seconds for large recordings, so showing the
  // recording UI during that time would feel like the Stop button is frozen.
  document.getElementById('wizardRecording').style.display = 'none';
  showWizardProcessing('Processing recording…', 'Downloading samples from the device and detecting peaks.');

  // How many samples the device said it had.  Reported in the failure alert
  // below, because it is the one fact that separates "the device gave us
  // nothing" from "we had the data and the rendering broke" — a distinction the
  // old generic message hid, and the reason the stop-order bug went five weeks
  // without being understood.
  let downloaded = 0;

  try {
    // ORDER MATTERS — download the recording BEFORE stopping the device.
    //
    // POST /calibration/stop frees the firmware's recording buffers and zeroes
    // the sample count (LapTimer::stopCalibrationWizard -> _freeCalibrationBuffers).
    // Downloading afterwards always read back total=0, so EVERY run of the
    // wizard died on the "Not enough data recorded" check below no matter what
    // was flown.  Broken 2026-08-09 by 4652937, which moved the buffers from
    // static arrays to on-demand calloc/free; the stop-then-download order was
    // correct for the eight months before that.  Do not move the stop back up.
    //
    // The device keeps sampling for the second or two this download takes.
    // Harmless: fetchAllCalibrationData pins `total` before requesting the
    // first page and the buffer is append-only, so anything recorded during the
    // download simply isn't read.
    try {
      const { total, data } = await fetchAllCalibrationData(wizardAbortController?.signal);
      downloaded = total;
      wizardState.data = data;
      console.log('Calibration data received:', total, 'reported,', data.length, 'rows');
    } catch (e) {
      if (e?.name === 'AbortError') throw e;
      throw new Error(`Downloading the recording failed — ${e?.message || e}`);
    }

    // Recording is in hand — now release the device.  Best-effort on purpose:
    // a failed stop must not throw away data we already hold, and the next
    // /calibration/start frees and reallocates regardless.
    try {
      const resp = await fetch(_wizardPath('stop'), { method: 'POST', signal: wizardAbortController?.signal });
      if (!resp.ok) {
        console.warn(`POST /calibration/stop returned HTTP ${resp.status} ${resp.statusText}`);
      }
      await resp.text().catch(() => '');   // consume body (may be empty in some builds)
    } catch (e) {
      if (e?.name !== 'AbortError') {
        console.warn('calibration/stop failed after download (data already held):', e);
      }
    }

    if (!Array.isArray(wizardState.data) || wizardState.data.length < 10) {
      hideWizardProcessing();
      // Quote the count.  This message used to be indistinguishable from the
      // stop-order bug that returned an empty recording no matter what was
      // flown; "0 sample(s)" says plainly that the device sent nothing, which
      // is never something the pilot can fix by flying more passes.
      alert(`Not enough data recorded — the device returned ${wizardState.data?.length ?? 0} sample(s).\n\n`
          + `Please try again with at least 3 clear gate passes.`);
      closeCalibrationWizard();
      return;
    }

    try {
      enterCalibrationOverviewModeFromWizard();
    } catch (e) {
      throw new Error(`Drawing the recording overview failed — ${e?.message || e}`);
    }

    hideWizardProcessing();
    document.getElementById('wizardMarking').style.display = 'block';

    try {
      drawWizardChart();
      // After auto-peak-detect runs inside drawWizardChart, evaluate whether the
      // three detected peaks are reasonably consistent.  Surface a warning if
      // not — the wizard keys Enter to the weakest peak, so a single off-line
      // pass would loosen the calibration unnecessarily.
      evaluatePeakSpreadAndWarn();
    } catch (e) {
      throw new Error(`Peak detection failed — ${e?.message || e}`);
    }
  } catch (error) {
    // The download now honours wizardAbortController, so a user who closes the
    // wizard mid-download lands here.  That is a deliberate cancel, not a
    // failure — closeCalibrationWizard() has already torn the UI down.
    if (error?.name === 'AbortError') return;
    console.error('Error stopping calibration wizard:', error);
    hideWizardProcessing();
    // Say WHICH stage failed and how much data we actually got.  A bare "Error
    // processing calibration data" costs the user a whole re-fly to learn
    // nothing, and is indistinguishable from a dozen different causes.
    alert(`Error processing calibration data.\n\n${error?.message || error}\n\n`
        + `The device reported ${downloaded} sample(s).`);
    closeCalibrationWizard();
  }
}

function smoothArray(values, windowSize) {
  const out = [];
  const half = Math.floor(windowSize / 2);
  for (let i = 0; i < values.length; i++) {
    let sum = 0;
    let count = 0;
    const start = Math.max(0, i - half);
    const end = Math.min(values.length - 1, i + half);
    for (let j = start; j <= end; j++) {
      sum += values[j];
      count++;
    }
    out.push(sum / count);
  }
  return out;
}

// Find up to N prominent peaks that are well separated.
// Returns indices into `values` sorted in time order.
function detectTopPeaks(values, desiredCount = 3) {
  const n = values.length;
  if (n < 20) return [];

  const minVal = Math.min(...values);
  const maxVal = Math.max(...values);
  const range = maxVal - minVal;
  if (range <= 0) return [];

  // Only consider peaks in the top 30% of the signal range — gate passes
  // produce strong signals; accepting weaker candidates causes nearby
  // noise bumps to win over well-separated true laps.
  const threshold = minVal + range * 0.70;

  // Local-max check radius: wide enough to skip rapid noise spikes and handle broad peaks
  const r = 20;
  const candidates = [];
  for (let i = r; i < n - r; i++) {
    const v = values[i];
    if (v < threshold) continue;
    let isMax = true;
    for (let k = 1; k <= r; k++) {
      if (values[i - k] > v || values[i + k] > v) { isMax = false; break; }
    }
    if (isMax) candidates.push({ index: i, value: v });
  }

  if (candidates.length === 0) return [];

  // Sort tallest first so greedy selection picks the strongest passes
  candidates.sort((a, b) => b.value - a.value);

  // Minimum separation between two accepted peaks, in SAMPLES.
  //
  // This is a physical quantity — how close together two gate crossings can
  // plausibly be — so it is derived from time, not from the length of the
  // recording.  It used to be `max(80, n * 0.20)`, which made detection depend
  // on how long the pilot left the timer running: three passes flown in the
  // first 15 s of a 100 s recording sit ~4 % of `n` apart, so a 20 % rule threw
  // away every peak after the tallest and the wizard silently fell back to
  // manual marking.  Stopping the recording promptly after the same three
  // passes shrank `n` and made the identical flight work, which is exactly the
  // symptom that surfaced this.
  //
  // 2 s is comfortably below any real lap while being far wider than one pass
  // (~0.5 s of above-threshold signal), and the r=20 local-max radius already
  // prevents two candidates landing inside a single peak.
  const MIN_PASS_SEPARATION_MS = 2000;
  const minSep = Math.round(MIN_PASS_SEPARATION_MS / WIZARD_SAMPLE_INTERVAL_MS);
  const chosen = [];
  for (const c of candidates) {
    if (chosen.length >= desiredCount) break;
    if (!chosen.some(p => Math.abs(p.index - c.index) < minSep)) chosen.push(c);
  }

  // Relax once if we didn't find enough — a very tight indoor course can put
  // crossings closer together than the nominal minimum.
  if (chosen.length < desiredCount) {
    const relaxedSep = Math.round(minSep / 2);
    for (const c of candidates) {
      if (chosen.length >= desiredCount) break;
      if (!chosen.some(p => Math.abs(p.index - c.index) < relaxedSep)) chosen.push(c);
    }
  }

  return chosen
    .slice(0, desiredCount)
    .sort((a, b) => a.index - b.index)
    .map(p => p.index);
}

function autoPopulateWizardPeaksIfEmpty(rawRssi, smoothedRssi) {
  if (wizardState.markers.length !== 0) return;

  // Detection curve is lightly smoothed (15+10 ≈ 25-sample boxcar) so narrow
  // tall peaks survive while broadband noise is suppressed.  Heavier smoothing
  // crushes peaks below the 30%-of-range threshold, which is what was
  // happening when fast passes failed to auto-populate.
  // The display curve (smoothedRssi, 15-sample) is then used for the dot snap
  // so the final position tracks the visual peak, not a noise spike.
  const detectionCurve = smoothArray(smoothedRssi, 10);
  const SNAP_WINDOW = 40;
  const peakIdx = detectTopPeaks(detectionCurve, 3).map(idx => {
    const lo = Math.max(0, idx - SNAP_WINDOW);
    const hi = Math.min(smoothedRssi.length - 1, idx + SNAP_WINDOW);
    let bestIdx = idx, bestVal = -Infinity;
    for (let i = lo; i <= hi; i++) {
      if (smoothedRssi[i] > bestVal) { bestVal = smoothedRssi[i]; bestIdx = i; }
    }
    return bestIdx;
  });

  if (peakIdx.length === 3) {
    wizardState.markers = peakIdx.map((idx, i) => ({ index: idx, lap: i + 1 }));
    wizardState.currentLap = 4;
    updateWizardStatus('Auto-detected 3 peaks. Tap a dot to move it, then "Calculate Thresholds".');
    document.getElementById('wizardUndoButton').disabled = false;
    document.getElementById('wizardCalculateButton').disabled = false;
    console.log('[Wizard] Auto-peaks:', wizardState.markers.map(m => ({ lap: m.lap, index: m.index, rssi: rawRssi[m.index] })));
  } else {
    updateWizardStatus(`Mark Peak ${wizardState.currentLap}`);
    console.log('[Wizard] Auto-peak detection found', peakIdx.length, 'peaks; leaving manual.');
  }
}


function drawWizardChart() {
  const canvas = document.getElementById('wizardChart');
  const ctx = canvas.getContext('2d');

  // Match the drawing buffer to the canvas's CSS-computed display size so
  // the chart fills its container without distortion.  CSS clamps the
  // height (see #wizardChart in style.css) so the bottom controls always
  // remain in view on phones in portrait orientation.
  canvas.width  = canvas.offsetWidth  || 600;
  canvas.height = canvas.offsetHeight || 320;

  const width = canvas.width;
  const height = canvas.height;
  const padding = 40;
  const chartWidth = width - 2 * padding;
  const chartHeight = height - 2 * padding;
  
  // Get RSSI values
  const rssiValues = wizardState.data.map(d => d.rssi);
  const minRssi = Math.min(...rssiValues);
  const maxRssi = Math.max(...rssiValues);
  const rssiRange = maxRssi - minRssi;
  
  // Apply visual smoothing with moving average (window size 15 for smoother appearance)
  // IMPORTANT: This is ONLY for visual display - does NOT affect actual data
  const smoothedRssi = [];
  const windowSize = 15;
  for (let i = 0; i < rssiValues.length; i++) {
    let sum = 0;
    let count = 0;
    for (let j = Math.max(0, i - Math.floor(windowSize / 2)); j <= Math.min(rssiValues.length - 1, i + Math.floor(windowSize / 2)); j++) {
      sum += rssiValues[j];
      count++;
    }
    smoothedRssi.push(sum / count);
  }

  // Auto-detect and pre-populate 3 peaks the first time we draw the marking chart
  autoPopulateWizardPeaksIfEmpty(rssiValues, smoothedRssi);

  
  // Clear canvas
  ctx.fillStyle = getComputedStyle(document.body).getPropertyValue('--bg-primary').trim();
  ctx.fillRect(0, 0, width, height);
  
  // Draw axes
  ctx.strokeStyle = getComputedStyle(document.body).getPropertyValue('--border-color').trim();
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(padding, padding);
  ctx.lineTo(padding, height - padding);
  ctx.lineTo(width - padding, height - padding);
  ctx.stroke();

  // Y-axis scale labels (raw RSSI) — 5 evenly-spaced ticks from min to max
  const textColor = getComputedStyle(document.body).getPropertyValue('--text-color').trim() || '#ddd';
  const gridColor = 'rgba(255,255,255,0.10)';
  ctx.fillStyle = textColor;
  ctx.font = '11px Arial';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  const yTicks = 5;
  for (let t = 0; t <= yTicks; t++) {
    const frac = t / yTicks;
    const value = Math.round(minRssi + frac * rssiRange);
    const y = height - padding - frac * chartHeight;
    ctx.fillText(String(value), padding - 6, y);
    ctx.strokeStyle = gridColor;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }
  ctx.textAlign = 'left';
  ctx.textBaseline = 'alphabetic';

  // Draw filled area under RSSI line (similar to SmoothieChart style)
  ctx.fillStyle = 'rgba(0, 212, 255, 0.4)';
  ctx.beginPath();
  
  for (let i = 0; i < smoothedRssi.length; i++) {
    const x = padding + (i / (smoothedRssi.length - 1)) * chartWidth;
    const rssi = smoothedRssi[i];
    const y = height - padding - ((rssi - minRssi) / rssiRange) * chartHeight;
    
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  // Complete the filled area by drawing to bottom corners
  ctx.lineTo(width - padding, height - padding);
  ctx.lineTo(padding, height - padding);
  ctx.closePath();
  ctx.fill();
  
  // Draw RSSI line on top (smoothed for visual clarity)
  ctx.strokeStyle = '#00d4ff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  
  for (let i = 0; i < smoothedRssi.length; i++) {
    const x = padding + (i / (smoothedRssi.length - 1)) * chartWidth;
    const rssi = smoothedRssi[i];
    const y = height - padding - ((rssi - minRssi) / rssiRange) * chartHeight;
    
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  ctx.stroke();
  
  // Draw peak markers — Y uses the smoothed value so the dot sits on the
  // visible waveform; label shows the raw RSSI value for calibration reference.
  wizardState.markers.forEach(marker => {
    const x = padding + (marker.index / (wizardState.data.length - 1)) * chartWidth;
    const rawRssiVal   = wizardState.data[marker.index].rssi;
    const smoothedVal  = smoothedRssi[marker.index];
    const y = height - padding - ((smoothedVal - minRssi) / rssiRange) * chartHeight;

    ctx.fillStyle = '#ff5555';
    ctx.beginPath();
    ctx.arc(x, y, 8, 0, 2 * Math.PI);
    ctx.fill();

    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold 13px Arial';
    ctx.textAlign = 'center';
    ctx.fillText(`P${marker.lap}: ${rawRssiVal}`, x, y - 13);
  });
  
  // Add click/tap listener
  canvas.onclick = function(event) {
    const rect = canvas.getBoundingClientRect();
    const clickX = event.clientX - rect.left;

    const dataIndex = Math.round(((clickX - padding) / chartWidth) * (wizardState.data.length - 1));
    if (dataIndex < 0 || dataIndex >= wizardState.data.length) return;

    if (wizardState.currentLap > 3 && wizardState.markers.length === 3) {
      // All peaks already placed (including auto-detected). Move the nearest
      // marker to where the user tapped so they can fine-tune the positions.
      let nearestMarkerIdx = 0;
      let nearestDist = Infinity;
      wizardState.markers.forEach((m, i) => {
        const dist = Math.abs(m.index - dataIndex);
        if (dist < nearestDist) { nearestDist = dist; nearestMarkerIdx = i; }
      });
      wizardState.markers[nearestMarkerIdx].index = dataIndex;
      drawWizardChart();
    } else {
      addWizardMarker(dataIndex);
    }
  };
}

function addWizardMarker(index) {
  // Check if we're done
  if (wizardState.currentLap > 3) return;
  
  // Add peak marker
  wizardState.markers.push({
    index: index,
    lap: wizardState.currentLap
  });
  
  // Move to next lap
  wizardState.currentLap++;
  if (wizardState.currentLap <= 3) {
    updateWizardStatus(`Mark Peak ${wizardState.currentLap}`);
  } else {
    updateWizardStatus('All peaks marked! Click "Calculate Thresholds"');
    document.getElementById('wizardCalculateButton').disabled = false;
  }
  
  // Enable undo button
  document.getElementById('wizardUndoButton').disabled = false;
  
  // Redraw chart
  drawWizardChart();
}

function undoLastMarker() {
  if (wizardState.markers.length === 0) return;
  
  // Remove last marker
  const removed = wizardState.markers.pop();
  
  // Update state
  wizardState.currentLap = removed.lap;
  updateWizardStatus(`Mark Peak ${wizardState.currentLap}`);
  
  // Disable buttons if needed
  if (wizardState.markers.length === 0) {
    document.getElementById('wizardUndoButton').disabled = true;
  }
  document.getElementById('wizardCalculateButton').disabled = true;
  
  // Redraw chart
  drawWizardChart();
}

function updateWizardStatus(text) {
  document.getElementById('wizardMarkingStatus').textContent = text;
}

async function calculateThresholds() {
  if (wizardState.markers.length !== 3) {
    alert('Please mark all 3 peaks before calculating thresholds');
    return;
  }

  // Show spinner BEFORE the heavy synchronous math runs.  Without the paint
  // wait, the browser will batch the spinner's first frame with the final
  // results render and the user just sees a frozen UI for a few seconds.
  document.getElementById('wizardMarking').style.display = 'none';
  showWizardProcessing('Calculating thresholds…', 'Analysing peak shapes — this usually takes a few seconds.');
  await nextPaint();

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  function windowMax(a, b) {
    a = Math.max(0, Math.min(a, wizardState.data.length - 1));
    b = Math.max(0, Math.min(b, wizardState.data.length - 1));
    if (b < a) [a, b] = [b, a];
    let m = -Infinity;
    for (let i = a; i <= b; i++) m = Math.max(m, wizardState.data[i].rssi);
    return m;
  }

  const markers = [...wizardState.markers].sort((a, b) => a.index - b.index);
  const allRssiValues = wizardState.data.map(d => d.rssi);

  // Re-find the true raw peak near each marker (handles smoothing offset).
  const PEAK_WINDOW = 20;
  const peakRssis = markers.map(m =>
    windowMax(m.index - PEAK_WINDOW, m.index + PEAK_WINDOW)
  );

  // Enter is keyed to the WEAKEST peak observed during calibration and
  // placed at 95 % of it — ~5 % headroom for lap-to-lap variation.
  // Tight enough that neighbouring-gate RSSI lift in close-pattern layouts
  // typically stays below threshold, while leaving enough margin that real
  // passes with slightly different lines still trigger.  Pilots can nudge
  // higher manually after calibration if their setup tolerates it.
  const minPeak = Math.min(...peakRssis);
  let calculatedEnter = Math.round(minPeak * 0.95);

  // Exit is 4 RSSI units below Enter — tight hysteresis keeps the detection
  // band close to the gate antenna while still avoiding state oscillation at
  // the boundary.  Noise-floor check below raises Exit if ambient is close
  // to threshold.
  const EXIT_GAP = 4;
  let calculatedExit = calculatedEnter - EXIT_GAP;

  // Noise floor: 35th-percentile of the recording keeps exit above ambient.
  const sorted = [...allRssiValues].sort((a, b) => a - b);
  const noiseFloor = sorted[Math.floor(sorted.length * 0.35)];
  calculatedExit = Math.max(noiseFloor + 3, calculatedExit);

  // Final clamp — exit must stay above MIN_RSSI and at least 4 below enter.
  const MIN_RSSI = 30;
  const MAX_RSSI = 255;
  calculatedEnter = clamp(calculatedEnter, MIN_RSSI, MAX_RSSI);
  calculatedExit  = clamp(calculatedExit,  MIN_RSSI, calculatedEnter - 4);

  wizardState.calculatedEnter = calculatedEnter;
  wizardState.calculatedExit  = calculatedExit;

  console.log('[Wizard] peaks:', peakRssis, 'minPeak:', minPeak, 'noiseFloor:', noiseFloor);
  console.log('[Wizard] final:', { enter: calculatedEnter, exit: calculatedExit });

  // Show results
  hideWizardProcessing();
  document.getElementById('wizardResults').style.display = 'block';
  document.getElementById('calculatedEnterRssi').textContent = calculatedEnter;
  document.getElementById('calculatedExitRssi').textContent = calculatedExit;

  // Save straight away rather than waiting for an Apply click.  The pilot flew
  // three passes to get here; a calibration should not be lost because the
  // obvious-looking "Close" was pressed on a screen that appeared to be just a
  // summary.  Fine-tuning still happens afterwards on the Calibration tab.
  _setWizardResultsSaved(await _persistCalculatedThresholds());
}

// Persist the wizard's calculated thresholds.  Returns true on success.
//
// Split out of applyCalculatedThresholds() so the Results screen can save the
// moment it appears.  Reaching that screen means the pilot flew the laps and
// asked for the numbers; making them click a second button to keep the result
// only creates a way to lose a calibration that cost three passes to obtain.
// Deliberately does NOT close the wizard — that is the caller's decision.
async function _persistCalculatedThresholds() {
  const enter = wizardState.calculatedEnter;
  const exit  = wizardState.calculatedExit;

  // In master mode — for both a client target (wizardTargetNodeId > 0) AND
  // the master's own card (wizardTargetNodeId === 0) — route through the
  // /api/multinode/editPilot endpoint.  The firmware-side handler has a
  // self-edit branch that applies the patch to the master's own Config when
  // nodeId === 0, which is the persistent save path for the host case.
  //
  // The old "wizardTargetNodeId > 0 ? proxy : local" split sent the master
  // self-edit through a full-snapshot saveConfig() that didn't advance
  // baselineConfig or clear stagedConfig — leaving the dirty-tracking
  // flags stuck and effectively never re-syncing the Calibration tab UI.
  if (mnNodeMode === 1) {
    const nodeId = wizardTargetNodeId;
    try {
      const r = await fetch('/api/multinode/editPilot', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ nodeId, enterRssi: enter, exitRssi: exit }),
      });
      if (!r.ok) {
        const target = (nodeId === 0) ? 'master' : 'client';
        alert(`Could not push calibration to ${target} — is the node still connected?`);
        return false;
      }
      // Update the cached node entry so a reopen of the Edit Pilot modal
      // shows the new values without waiting for the next director-state
      // push.  Works for the master entry too now that
      // _buildDirectorStatePayload includes enterRssi/exitRssi on it.
      const node = (mnCurrentNodes || []).find(n => n.nodeId === nodeId);
      if (node) { node.enterRssi = enter; node.exitRssi = exit; }

      // Master-self case: also sync the Calibration tab's local state so
      // visiting that tab later shows the up-to-date values, the staged-
      // config "dirty" indicator clears (we just persisted), and the
      // Start Wizard button isn't gated behind a phantom Save RSSI click.
      if (nodeId === 0) {
        enterRssi = enter;
        exitRssi  = exit;
        if (enterRssiInput)  enterRssiInput.value  = enter;
        if (exitRssiInput)   exitRssiInput.value   = exit;
        if (enterRssiSpan)   enterRssiSpan.textContent = enter;
        if (exitRssiSpan)    exitRssiSpan.textContent  = exit;
        if (baselineConfig && typeof baselineConfig === 'object') {
          baselineConfig.enterRssi = enter;
          baselineConfig.exitRssi  = exit;
        }
        delete stagedConfig.enterRssi;
        delete stagedConfig.exitRssi;
        if (Object.keys(stagedConfig).length === 0) stagedDirty = false;
        if (typeof updateSaveButton === 'function') updateSaveButton();
        _markRssiSaved(enter, exit);
      }

      // For BOTH master-self and client wizards launched from the modal,
      // the master's Calibration tab is now sitting in overview mode with
      // Start Wizard disabled — the user never opened that tab and won't
      // expect to need a Save RSSI click to re-arm.  Clear it.
      if (calibOverviewMode) exitCalibrationOverviewModeByUserAction();
    } catch (e) {
      alert('Could not push calibration: ' + (e && e.message ? e.message : e));
      return false;
    }
    return true;
  }

  // Single mode (mnNodeMode === 0): original local behaviour.  The wizard
  // was launched from the Calibration tab itself, so leaving overview mode
  // active is intentional — the user can fine-tune the recommended
  // thresholds against the visible recording before committing via the
  // Save RSSI button.
  if (enterRssiInput) { enterRssiInput.value = enter; updateEnterRssi(enterRssiInput, enter); }
  if (exitRssiInput)  { exitRssiInput.value  = exit;  updateExitRssi(exitRssiInput,  exit);  }

  // Persist ONLY the two values the wizard computed.
  //
  // This used to call saveConfig(), which commits the ENTIRE staged delta —
  // every unsaved Configuration-tab edit the user happens to have in flight.
  // That was fine while the save sat behind a deliberate "Apply" click, but it
  // now fires automatically when the Results screen appears, and quietly
  // committing someone's unrelated pending edits is not something they asked
  // for.  saveConfig() also early-returns unless stagedDirty is set, which ties
  // the wizard's save to staging state it does not own.
  //
  // saveConfigPatchImmediate sends just these two keys, advances baselineConfig
  // and drops the matching staged entries, so the Save button doesn't light up
  // for a change we just persisted.
  await saveConfigPatchImmediate({ enterRssi: enter, exitRssi: exit });
  _markRssiSaved(enter, exit);
  // Optimistic, matching every other Save path here: saveConfigPatchImmediate
  // logs a failed POST rather than throwing, so there is nothing to report.
  return true;
}

// Results screen "Apply Thresholds" button.  With the automatic save in
// calculateThresholds() this is now the RETRY path — it stays on screen only
// when that save failed.  Saving again is harmless either way.
async function applyCalculatedThresholds() {
  if (await _persistCalculatedThresholds()) {
    closeCalibrationWizard();
  }
}

// Reflect the outcome of the automatic save on the Results screen: confirm it,
// or leave the manual Apply button in place so a failure can be retried.
function _setWizardResultsSaved(saved) {
  const note  = document.getElementById('wizardResultsSavedNote');
  const apply = document.getElementById('wizardApplyButton');
  const close = document.getElementById('wizardResultsCloseButton');

  if (note) {
    note.textContent = saved
      ? 'Saved to the timer. Adjust the sliders on the Calibration tab and press Save RSSI Thresholds if you want to fine-tune.'
      : 'NOT saved — the timer did not accept the new thresholds. Press Apply Thresholds to try again.';
    note.className = saved ? 'wizard-saved-note' : 'wizard-saved-note wizard-saved-note-failed';
    note.style.display = 'block';
  }
  // Hiding Apply on success keeps the screen honest: there is nothing left to
  // apply, and a button that re-does what already happened invites the reading
  // that closing without it would discard the calibration.
  if (apply) apply.style.display = saved ? 'none' : '';
  if (close) close.textContent = saved ? 'Done' : 'Close';
}

// Evaluates the three auto-detected peaks for amplitude consistency and
// surfaces a warning banner if they differ by more than ~15 %.
//
// Threshold: weakest / strongest < 0.85 (i.e. >15 % spread relative to the
// strongest peak).  Conservative on purpose — a false-positive warning is
// cheap (the user can dismiss with one click) but a false-negative leaves
// them with a sub-optimal calibration without ever knowing.
//
// Reads the auto-populated wizardState.markers; uses raw rssi values from
// wizardState.data, not the smoothed display values, so the metric reflects
// what the firmware actually saw.
function evaluatePeakSpreadAndWarn() {
  const banner = document.getElementById('wizardPeakSpreadWarning');
  const detail = document.getElementById('wizardPeakSpreadDetail');
  if (!banner) return;

  // Default-hide so the warning never persists across re-runs of the wizard
  // when the new recording is fine.
  banner.style.display = 'none';

  if (!Array.isArray(wizardState.markers) || wizardState.markers.length !== 3) {
    return;  // Auto-detect didn't find 3 peaks — user will mark manually
  }
  if (!Array.isArray(wizardState.data) || wizardState.data.length === 0) {
    return;
  }

  // Use a small window around each marker to find the true raw peak (handles
  // smoothing offset between the display curve and the underlying samples).
  const PEAK_WINDOW = 20;
  const peakValues = wizardState.markers.map(m => {
    const lo = Math.max(0, m.index - PEAK_WINDOW);
    const hi = Math.min(wizardState.data.length - 1, m.index + PEAK_WINDOW);
    let best = -Infinity;
    for (let i = lo; i <= hi; i++) {
      if (wizardState.data[i].rssi > best) best = wizardState.data[i].rssi;
    }
    return best;
  });

  const minPeak = Math.min(...peakValues);
  const maxPeak = Math.max(...peakValues);
  if (maxPeak <= 0) return;

  const ratio = minPeak / maxPeak;
  const spreadPct = Math.round((1 - ratio) * 100);

  console.log('[Wizard] Peak spread check:',
              { peaks: peakValues, ratio: ratio.toFixed(3), spreadPct });

  // 0.85 ratio = 15 % spread.  Below that, surface the warning.
  if (ratio < 0.85) {
    if (detail) {
      detail.textContent =
        `your three calibration peaks differ by ${spreadPct}% ` +
        `(weakest ${minPeak}, strongest ${maxPeak}).`;
    }
    banner.style.display = '';
  }
}

function dismissPeakSpreadWarning() {
  const banner = document.getElementById('wizardPeakSpreadWarning');
  if (banner) banner.style.display = 'none';
}

// Tear down the in-progress recording, then immediately re-enter the wizard
// at the recording step so the user can re-fly the calibration laps.
async function restartCalibrationWizard() {
  // Hide the warning so it doesn't flash through the new recording step.
  dismissPeakSpreadWarning();
  // Tell the device to drop out of CALIBRATION_WIZARD state cleanly.
  // Best-effort — if the POST fails we still proceed; startCalibrationWizard
  // will issue its own calibration/start which the firmware will accept.
  try {
    await fetch(_wizardPath('stop'), { method: 'POST' });
  } catch (_) {}
  // startCalibrationWizard resets wizardState and reshows wizardRecording.
  startCalibrationWizard();
}

function cancelCalibrationWizard() {
  // stop loop immediately
  wizardState.recording = false;
  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }

  // Tell firmware to stop (best effort), then close
  fetch(_wizardPath('stop'), { method: 'POST' })
    .then(() => closeCalibrationWizard())
    .catch(() => closeCalibrationWizard());
}

function closeCalibrationWizard() {
  wizardState.recording = false;

  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }

  if (wizardAbortController) {
    try { wizardAbortController.abort(); } catch (e) {}
    wizardAbortController = null;
  }

  document.getElementById('calibrationWizardModal').style.display = 'none';
  document.getElementById('wizardRecording').style.display = 'none';
  document.getElementById('wizardMarking').style.display = 'none';
  document.getElementById('wizardResults').style.display = 'none';
  hideWizardProcessing();
  dismissPeakSpreadWarning();
  // Reset remote target — next wizard run defaults back to local unless
  // mnStartCalibrationWizard sets it again.
  wizardTargetNodeId = 0;
  document.querySelectorAll('.wizardPilotSuffix').forEach(el => { el.textContent = ''; });
}

// WiFi Settings Functions
function applyWiFiSettings() {
  const ssid = document.getElementById('ssid')?.value;
  const pwd = document.getElementById('pwd')?.value;
  
  if (!ssid) {
    alert('Please enter a WiFi SSID');
    return;
  }
  
  if (!pwd) {
    if (!confirm('WiFi password is empty. Continue?')) {
      return;
    }
  }
  
  if (!confirm('Apply WiFi settings? The device will restart.')) {
    return;
  }
  
  // Save configuration first
  saveConfig();
  
  // Give a moment for save to complete
  setTimeout(() => {
    // Send reboot command
    if (usbConnected && transportManager) {
      transportManager.sendCommand('reboot', 'POST')
        .then(() => {
          alert('WiFi settings applied. Device is restarting...');
        })
        .catch(err => console.error('Failed to restart device:', err));
    } else {
      fetch('/reboot', {
        method: 'POST',
        headers: {
          'Accept': 'application/json',
          'Content-Type': 'application/json'
        }
      })
        .then(() => {
          alert('WiFi settings applied. Device is restarting...');
        })
        .catch(err => console.error('Failed to restart device:', err));
    }
  }, 500);
}

// Pipeline EMA smoothing slider (0=lightest, 5=upstream default, 10=heaviest).
function updateV1Smoothing(value) {
  const v = Math.min(10, Math.max(0, parseInt(value, 10) || 0));
  const slider = document.getElementById('v1Smoothing');
  const span = document.getElementById('v1SmoothingSpan');
  if (slider) slider.value = v;
  if (span) span.textContent = v;
  if (typeof autoSaveConfig === 'function') autoSaveConfig();
}

function stepV1Smoothing(delta) {
  const slider = document.getElementById('v1Smoothing');
  if (!slider) return;
  const v = Math.min(10, Math.max(0, (parseInt(slider.value, 10) || 0) + delta));
  updateV1Smoothing(v);
}


function rebootDevice() {
  if (!confirm('Reboot the device now?')) return;

  const btn = document.getElementById('rebootBtn');
  if (btn) { btn.disabled = true; btn.textContent = 'Rebooting...'; }

  const doReboot = () => {
    if (usbConnected && transportManager) {
      transportManager.sendCommand('reboot', 'POST').catch(() => {});
    } else {
      fetch('/reboot', { method: 'POST' }).catch(() => {});
    }
  };

  // Save any pending changes first, then reboot
  if (stagedDirty) {
    saveConfig();
    setTimeout(doReboot, 500);
  } else {
    doReboot();
  }
}

function resetWiFiSettings() {
  if (!confirm('Reset WiFi settings to current values?')) {
    return;
  }
  
  // Reload config from device
  if (usbConnected && transportManager) {
    transportManager.sendCommand('config', 'GET')
      .then(configData => {
        if (configData.ssid !== undefined) document.getElementById('ssid').value = configData.ssid;
        if (configData.pwd !== undefined) document.getElementById('pwd').value = configData.pwd;
      })
      .catch(err => console.error('Failed to fetch config:', err));
  } else {
    fetch('/config')
      .then(response => response.json())
      .then(configData => {
        if (configData.ssid !== undefined) document.getElementById('ssid').value = configData.ssid;
        if (configData.pwd !== undefined) document.getElementById('pwd').value = configData.pwd;
      })
      .catch(err => console.error('Failed to fetch config:', err));
  }
}

// Settings Modal Functions
// Settings Modal Functions
// (openSettingsModal is defined later with full config loading)

function closeSettingsModal(force = false) {
  // Only prompt if we're dirty and not forcing close
  if (!force && !confirmDiscardUnsavedChanges()) {
    return; // user cancelled
  }

  // Safe to close → clear staged state AND drop any deferred applies
  // (e.g. a pending /led/preset call from a preset change the user did
  // not save).  Without this drop, the next Save in a later session
  // would replay the orphaned change.
  stagedDirty = false;
  stagedConfig = {};
  _clearPendingApply();
  updateSaveButton();

  const modal = document.getElementById('settingsModal');
  if (modal) modal.classList.remove('active');
}

// Close modal when clicking on the overlay background
document.addEventListener('click', function(event) {
  const modal = document.getElementById('settingsModal');
  if (modal && event.target === modal && modal.classList.contains('active')) {
    closeSettingsModal(false);
  }
});

// Close modal on Escape key
document.addEventListener('keydown', function(event) {
  if (event.key === 'Escape') {
    const modal = document.getElementById('settingsModal');
    if (modal && modal.classList.contains('active')) {
      closeSettingsModal(false);
    }
  }
});

function switchSettingsSection(sectionName) {
  // Hide all sections
  const sections = document.querySelectorAll('.settings-section');
  sections.forEach(section => section.classList.remove('active'));

  // Show selected section
  const targetSection = document.getElementById(`settings-${sectionName}`);
  if (targetSection) {
    targetSection.classList.add('active');
  }

  // Reset the scroll of the shared scrollable container back to the top
  // whenever we switch sections.  All settings sections share the same
  // .settings-content parent (overflow-y:auto) — without this reset, a
  // user who scrolled down in one section would land at the same scroll
  // position in the next one, which feels wrong because the new section's
  // header is off-screen.
  const scrollContainer = document.querySelector('.settings-content');
  if (scrollContainer) scrollContainer.scrollTop = 0;

  // Update nav items — resolve the matching one via its onclick attribute
  // instead of the deprecated implicit `event` global.  The previous
  // implementation broke when this function was called programmatically
  // (e.g. from setLEDSettingsVisible after LED support is disabled), where
  // `event` was either undefined or a stale window.event whose target had
  // no .closest() method — throwing "closest is not a function".  Looking
  // the nav item up by its onclick attribute works for both click-driven
  // and programmatic callers, and keeps every existing HTML caller
  // (`onclick="switchSettingsSection('foo')"`) working with no changes.
  const marker = `switchSettingsSection('${sectionName}')`;
  const navItems = document.querySelectorAll('.settings-nav-item');
  navItems.forEach(item => {
    item.classList.remove('active');
    if ((item.getAttribute('onclick') || '').includes(marker)) {
      item.classList.add('active');
    }
  });
}

// Self-Test Functions
function runSelfTest() {
  const button = document.getElementById('runTestsButton');
  const loadingDiv = document.getElementById('testLoading');
  const resultsDiv = document.getElementById('testResults');
  const resultsListDiv = document.getElementById('testResultsList');
  
  // Show loading, hide results
  button.disabled = true;
  button.textContent = 'Running Tests...';
  loadingDiv.style.display = 'block';
  resultsDiv.style.display = 'none';
  
  fetch('/api/selftest')
    .then(response => response.json())
    .then(data => {
      // Hide loading
      loadingDiv.style.display = 'none';
      
      // Build results HTML
      let html = '';
      let allPassed = true;
      
      data.tests.forEach(test => {
        if (!test.passed) allPassed = false;
        
        const statusIcon = test.passed ? '✓' : '✗';
        const statusColor = test.passed ? '#4ade80' : '#ff5555';
        const bgColor = test.passed ? 'rgba(74, 222, 128, 0.1)' : 'rgba(255, 85, 85, 0.1)';
        
        html += `
          <div style="margin-bottom: 12px; padding: 12px; background-color: ${bgColor}; border-left: 4px solid ${statusColor}; border-radius: 4px;">
            <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px;">
              <div style="display: flex; align-items: center; gap: 8px;">
                <span style="font-size: 20px; color: ${statusColor};">${statusIcon}</span>
                <span style="font-weight: bold; font-size: 16px;">${test.name}</span>
              </div>
              <span style="font-size: 12px; color: var(--secondary-color);">${test.duration}ms</span>
            </div>
            <div style="font-size: 14px; color: var(--secondary-color); margin-left: 28px;">
              ${test.details}
            </div>
          </div>
        `;
      });
      
      // Add summary
      const passedCount = data.tests.filter(t => t.passed).length;
      const totalCount = data.tests.length;
      const summaryColor = allPassed ? '#4ade80' : '#ff9f43';
      
      html = `
        <div style="margin-bottom: 20px; padding: 16px; background-color: var(--bg-secondary); border-radius: 8px; text-align: center;">
          <div style="font-size: 18px; font-weight: bold; margin-bottom: 8px; color: ${summaryColor};">
            ${allPassed ? 'All Tests Passed!' : 'Some Tests Failed'}
          </div>
          <div style="font-size: 14px; color: var(--secondary-color);">
            ${passedCount} / ${totalCount} tests passed
          </div>
        </div>
      ` + html;
      
      resultsListDiv.innerHTML = html;
      resultsDiv.style.display = 'block';
      
      // Re-enable button
      button.disabled = false;
      button.textContent = 'Run All Tests Again';
    })
    .catch(error => {
      console.error('Error running self-test:', error);
      loadingDiv.style.display = 'none';
      resultsListDiv.innerHTML = `
        <div style="padding: 16px; background-color: rgba(255, 85, 85, 0.1); border-left: 4px solid #ff5555; border-radius: 4px;">
          <div style="font-weight: bold; color: #ff5555; margin-bottom: 6px;">Error Running Tests</div>
          <div style="font-size: 14px; color: var(--secondary-color);">${error.message}</div>
        </div>
      `;
      resultsDiv.style.display = 'block';
      button.disabled = false;
      button.textContent = 'Run All Tests';
    });
}

// ============================================
// Serial Monitor Functions
// ============================================

let serialMonitorActive = false;
let serialMonitorPollInterval = null;
let serialMonitorBuffer = [];
let lastSeenTimestampBanner = 0;   // advances always
let lastSeenTimestampUI = 0;       // advances only when serial monitor is open
const MAX_SERIAL_LINES = 500;

function toggleSerialMonitor() {
  if (serialMonitorActive) {
    stopSerialMonitor();
  } else {
    startSerialMonitor();
  }
}

function startDebugListener(rateMs = 3000) {
  // Clear any existing interval before starting a new one
  stopDebugListener();
  serialMonitorPollInterval = setInterval(pollDebugLogs, rateMs);
  // On the very first call, consume existing log entries to advance the timestamp
  // cursor WITHOUT triggering the calibration banner — those entries are from boot
  // and are no longer actionable. Only new entries (after this point) matter.
  _pollDebugLogsInitial();
}

// Fetch existing log entries once to advance the timestamp cursor without
// showing the calibration banner. Called once at page load so stale boot-time
// "Setting frequency to" entries don't trigger the banner spuriously.
function _pollDebugLogsInitial() {
  fetch('/api/debuglog')
    .then(r => r.json())
    .then(data => {
      if (data.logs && data.logs.length > 0) {
        const latest = data.logs[data.logs.length - 1].timestamp;
        if (latest > lastSeenTimestampBanner) lastSeenTimestampBanner = latest;
      }
    })
    .catch(() => {});
}

function stopDebugListener() {
  if (serialMonitorPollInterval) {
    clearInterval(serialMonitorPollInterval);
    serialMonitorPollInterval = null;
  }
}

function startSerialMonitor() {
  const button = document.getElementById('serialMonitorToggle');
  const monitor = document.getElementById('serialMonitor');

  button.textContent = 'Stop Monitor';
  button.style.backgroundColor = '#ff5555';
  serialMonitorActive = true;
  lastSeenTimestampUI = 0;

  // Speed up polling while monitor is open
  startDebugListener(300);

  // Clear monitor and show starting message
  monitor.innerHTML = '<div style="color: #4ade80;">[SYSTEM] Serial monitor started</div>';
}

// Call this on every incoming log line (cheap string checks)
function handleLogForCalibrationBanner(line) {
  if (!line) return;

  if (line.includes('Setting frequency to')) {
    showCalibrationBanner();
  } else if (line.includes('RX5808 Tune done') ||
             line.includes('RX5808 frequency verified properly')) {
    // "Tune done" is the real end-of-tune signal.  Hiding only on "verified
    // properly" tied this banner to SPI register READBACK, which most RX5808
    // modules do not support — verifyFrequency() then logs "frequency not
    // matching" instead and the banner never cleared.  There is no timeout
    // behind it, so it stayed up indefinitely.  "verified properly" is kept as
    // a second trigger for modules that do read back.
    hideCalibrationBanner();
  }
}

function pollDebugLogs() {  
  fetch('/api/debuglog')
    .then(response => response.json())
    .then(data => {
      if (data.logs && data.logs.length > 0) {
        // Add new logs that we haven't seen yet
        data.logs.forEach(log => {
          // 1) Always process for banner (real-time UX)
          if (log.timestamp > lastSeenTimestampBanner) {
            handleLogForCalibrationBanner(log.message);
            lastSeenTimestampBanner = log.timestamp;
          }

          // 2) Only advance the UI pointer when the serial monitor is open
          if (serialMonitorActive && log.timestamp > lastSeenTimestampUI) {
            appendSerialLine(log.message, '#00ff00', log.timestamp);
            lastSeenTimestampUI = log.timestamp;
          }
        });
      }
    })
    .catch(error => {
      if (serialMonitorActive) {
        console.error('Failed to fetch debug logs:', error);
      }
    });
}

function stopSerialMonitor() {
  const button = document.getElementById('serialMonitorToggle');
  const monitor = document.getElementById('serialMonitor');

  button.textContent = 'Start Monitor';
  button.style.backgroundColor = '';
  serialMonitorActive = false;

  // Drop back to slow background polling (calibration banner only)
  startDebugListener(3000);

  const line = document.createElement('div');
  line.style.color = '#888';
  line.textContent = '[SYSTEM] Serial monitor stopped';
  monitor.appendChild(line);
}

function appendSerialLine(text, color = '#00ff00', deviceTimestamp = null) {
  const monitor = document.getElementById('serialMonitor');
  const autoScroll = document.getElementById('serialAutoScroll')?.checked;
  
  // Add line to buffer
  serialMonitorBuffer.push({ text, color, deviceTimestamp });
  
  // Trim buffer if too large
  if (serialMonitorBuffer.length > MAX_SERIAL_LINES) {
    serialMonitorBuffer.shift();
    // Rebuild monitor from buffer
    rebuildSerialMonitor();
  } else {
    // Just append new line
    const line = document.createElement('div');
    line.style.color = color;
    line.textContent = text;
    monitor.appendChild(line);
  }
  
  // Auto-scroll to bottom
  if (autoScroll) {
    monitor.scrollTop = monitor.scrollHeight;
  }
}

function rebuildSerialMonitor() {
  const monitor = document.getElementById('serialMonitor');
  monitor.innerHTML = '';
  
  serialMonitorBuffer.forEach(({ text, color }) => {
    const line = document.createElement('div');
    line.style.color = color;
    line.textContent = text;
    monitor.appendChild(line);
  });
}

function clearSerialMonitor() {
  const monitor = document.getElementById('serialMonitor');
  serialMonitorBuffer = [];
  monitor.innerHTML = serialMonitorActive 
    ? '<div style="color: #888;">Monitor cleared...</div>' 
    : '<div style="color: #888;">Serial monitor stopped. Click "Start Monitor" to begin.</div>';
}

// ============================================
// Running Lap Timer Display
// ============================================

function startLapTimerDisplay() {
  if (window.lapTimerDisplayInterval) {
    clearInterval(window.lapTimerDisplayInterval);
  }
  updateLapTimerDisplay();
  window.lapTimerDisplayInterval = setInterval(updateLapTimerDisplay, 100);
}

function stopLapTimerDisplay() {
  if (window.lapTimerDisplayInterval) {
    clearInterval(window.lapTimerDisplayInterval);
    window.lapTimerDisplayInterval = null;
  }
}

function updateLapTimerDisplay() {
  const lapCounter = document.getElementById('lapCounter');
  if (!lapCounter) return;

  let lapText = (maxLaps === 0)
    ? `Lap ${Math.max(0, lapNo)}`
    : `Lap ${Math.max(0, lapNo)} / ${maxLaps}`;

  if (lapTimerStartMs > 0) {
    // Current-lap elapsed, in the same format and at the same precision as every
    // other time on screen.  This was a fourth hand-rolled clock: fixed at
    // hundredths regardless of the Lap Time Precision setting, and with no hours
    // field.  See formatMsDisplay().
    lapText += ` - ${formatMsDisplay(Date.now() - lapTimerStartMs)}`;
  }

  lapCounter.textContent = lapText;
}

// ============================================
// ===== WEBHOOK MANAGEMENT =====

function setWebhooksUI(enabled) {
  const webhooksContent = document.getElementById('webhooksContent');
  if (webhooksContent) {
    webhooksContent.style.display = enabled ? 'block' : 'none';
  }
  if (enabled) {
    loadWebhooks();
  }
}

function toggleWebhooks(enabled, opts = {}) {
  // Keep your existing UI behavior (show/hide related stuff) if you have it,
  // but DO NOT write to backend here.
  const webhooksCheckbox = document.getElementById('webhooksEnabled');
  if (webhooksCheckbox) webhooksCheckbox.checked = !!enabled;

  // Reveal / hide the IP entry, configured-webhooks list, and test button.
  // Without this the user toggles the switch on but sees no way to add an IP.
  setWebhooksUI(!!enabled);

  // Stage-only — `opts.save === false` is used by the config-load path so
  // restoring saved state doesn't dirty the Save Config button.
  if (opts.save !== false) autoSaveConfig();
}

function loadWebhooks() {
  fetch('/webhooks')
    .then(response => response.json())
    .then(data => {
      displayWebhooks(data.webhooks || []);
    })
    .catch(error => console.error('Error loading webhooks:', error));
}

function displayWebhooks(webhooks) {
  // Keep a stable list so Save Config can send it (webhookIPs)
  window.currentWebhookIPs = Array.isArray(webhooks) ? webhooks.slice() : [];

  // Stage it (unless settingsLoading)
  stageConfig('webhookIPs', window.currentWebhookIPs);

  const webhooksListContent = document.getElementById('webhooksListContent');
  if (!webhooksListContent) return;

  if (webhooks.length === 0) {
    webhooksListContent.innerHTML =
      '<p style="color: var(--secondary-color); text-align: center; padding: 16px;">No webhooks configured yet</p>';
    return;
  }

  let html = '<div style="display: flex; flex-direction: column; gap: 8px;">';

  webhooks.forEach(ip => {
    html += `
      <div style="display: flex; justify-content: space-between; align-items: center; padding: 12px; background-color: var(--bg-secondary); border-radius: 8px; border-left: 4px solid var(--accent-color);">
        <div>
          <div style="font-weight: bold; font-size: 15px;">${ip}</div>
          <div style="font-size: 13px; color: var(--secondary-color); margin-top: 2px;">http://${ip}/Lap, /RaceStart, /RaceStop</div>
        </div>
        <button onclick="removeWebhook('${ip}')" style="padding: 6px 10px; font-size: 14px; background-color: var(--danger-color); color: #fff;">Remove</button>
      </div>
    `;
  });

  html += '</div>';
  webhooksListContent.innerHTML = html;
}

function showCalibrationBanner() {
  const banner = document.getElementById('calibrationBanner');
  if (banner) banner.style.display = 'block';
}

function hideCalibrationBanner() {
  const banner = document.getElementById('calibrationBanner');
  if (banner) banner.style.display = 'none';
}

function addWebhook() {
  const ipInput = document.getElementById('webhookIP');
  const ip = ipInput.value.trim();
  
  if (!ip) {
    alert('Please enter an IP address');
    return;
  }
  
  // Basic IP validation
  const ipPattern = /^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/;
  if (!ipPattern.test(ip)) {
    alert('Please enter a valid IP address (e.g., 192.168.0.75)');
    return;
  }
  
  fetch('/webhooks/add', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: 'ip=' + encodeURIComponent(ip)
  })
  .then(response => response.json())
  .then(data => {
    if (data.status === 'OK') {
      ipInput.value = ''; // Clear input
      loadWebhooks(); // Reload list
      console.log('Webhook added:', ip);
    } else {
      alert('Error adding webhook: ' + (data.message || 'Unknown error'));
    }
  })
  .catch(error => {
    console.error('Error adding webhook:', error);
    alert('Error adding webhook');
  });
}

function removeWebhook(ip) {
  if (!confirm('Remove webhook for ' + ip + '?')) {
    return;
  }
  
  fetch('/webhooks/remove', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: 'ip=' + encodeURIComponent(ip)
  })
  .then(response => response.json())
  .then(data => {
    if (data.status === 'OK') {
      loadWebhooks(); // Reload list
      console.log('Webhook removed:', ip);
    } else {
      alert('Error removing webhook');
    }
  })
  .catch(error => {
    console.error('Error removing webhook:', error);
    alert('Error removing webhook');
  });
}

function testWebhook() {
  fetch('/webhooks/trigger/flash', {
    method: 'POST'
  })
  .then(response => {
    if (!response.ok) {
      return response.json().then(data => {
        throw new Error(data.message || 'Unknown error');
      });
    }
    return response.json();
  })
  .then(data => {
    if (data.status === 'OK') {
      alert('Test flash sent to all configured webhooks!\n\nCheck your LED controller to verify.');
    } else {
      alert('Error: ' + (data.message || 'Unknown error'));
    }
  })
  .catch(error => {
    console.error('Error testing webhook:', error);
    alert('Error sending test webhook: ' + error.message);
  });
}

function openSettingsModal() {
  settingsLoading = true;
  // Trigger serial config dump for debugging (fire-and-forget)
  fetch('/api/debugconfig').catch(() => {});
  const modal = document.getElementById('settingsModal');
  if (modal) {
    modal.classList.add('active');

    // Load full config to populate all settings
    fetch('/config')
      .then(response => response.json())
      .then(config => {
        baselineConfig = { ...config };   // snapshot what the device says right now
        // Populate all device config fields
        //if (config.freq !== undefined) setBandChannelIndex(config.freq);
        // Prefer band+chan (unambiguous). Fall back to freq for older configs.
        if (config.band !== undefined && config.chan !== undefined) {
          // Apply band
          const b = Math.max(0, Math.min(bandSelect.options.length - 1, (config.band | 0)));
          bandSelect.selectedIndex = b;

          // Rebuild channel dropdown for that band (hides 0-freq channels)
          updateChannelOptionsForBand(b);

          // chan is 0-based; dropdown values are "1".."8"
          const desiredValue = String(((config.chan | 0) + 1));
          const exists = Array.from(channelSelect.options).some(o => o.value === desiredValue);
          if (exists) channelSelect.value = desiredValue;

          populateFreqOutput();
        } else if (config.freq !== undefined) {
          setBandChannelIndex(config.freq); // legacy fallback
          populateFreqOutput();
        }
        if (config.minLap !== undefined) {
          minLapInput.value = (parseFloat(config.minLap) / 10).toFixed(1);
          updateMinLap(minLapInput, minLapInput.value);
        }
        if (config.alarm !== undefined) {
          alarmThreshold.value = (parseFloat(config.alarm) / 10).toFixed(1);
          updateAlarmThreshold(alarmThreshold, alarmThreshold.value);
        }
        if (config.anType !== undefined) announcerSelect.selectedIndex = config.anType;
        if (config.anRate !== undefined) {
          announcerRateInput.value = (parseFloat(config.anRate) / 10).toFixed(1);
          updateAnnouncerRate(announcerRateInput, announcerRateInput.value);
        }
        if (config.enterRssi !== undefined && enterRssiInput) {
          enterRssiInput.value = config.enterRssi;
          updateEnterRssi(enterRssiInput, enterRssiInput.value);
        }
        if (config.exitRssi !== undefined && exitRssiInput) {
          exitRssiInput.value = config.exitRssi;
          updateExitRssi(exitRssiInput, exitRssiInput.value);
        }
        if (config.name !== undefined && pilotNameInput) pilotNameInput.value = config.name;
        if (config.ssid !== undefined && ssidInput) ssidInput.value = config.ssid;
        if (config.pwd !== undefined && pwdInput) pwdInput.value = config.pwd;
        if (config.maxLaps !== undefined) {
          maxLapsInput.value = config.maxLaps;
          updateMaxLaps(maxLapsInput, maxLapsInput.value);
        }
        
        // LED settings
        const ledBrightnessInput = document.getElementById('ledBrightness');
        if (config.ledBrightness !== undefined && ledBrightnessInput) {
          ledBrightnessInput.value = config.ledBrightness;
          ledBrightnessInput.parentElement.querySelector('span').textContent = config.ledBrightness;
        }
        
        // Pilot settings
        const colorInput = document.getElementById('pilotColor');
        if (colorInput && config.pilotColor !== undefined) {
          const hexColor = '#' + ('000000' + config.pilotColor.toString(16)).slice(-6).toUpperCase();
          colorInput.value = hexColor;
          updateColorPreview();
        }
        
        // Voice and lap format settings
        const voiceSelect = document.getElementById('voiceSelect');
        const lapFormatSelect = document.getElementById('lapFormatSelect');
        if (voiceSelect && config.selectedVoice) {
          voiceSelect.value = config.selectedVoice;
          selectedVoice = config.selectedVoice;
        }
        if (lapFormatSelect && config.lapFormat) {
          lapFormatSelect.value = config.lapFormat;
          lapFormat = config.lapFormat;
        }
        const anDecSelect = document.getElementById('announcerDecimalsSelect');
        if (config.anDecimals !== undefined) {
          announcerDecimals = parseInt(config.anDecimals, 10) || 2;
          if (anDecSelect) anDecSelect.value = String(announcerDecimals);
        }
        // Sync voice enabled state from device so it's never out of date
        if (config.voiceEnabled !== undefined) {
          audioEnabled = !!config.voiceEnabled;
          updateVoiceButtons();
        }
        
        // Theme setting — resolved, so a legacy slug selects the palette the
        // page is actually showing instead of leaving the picker blank (a
        // <select> silently rejects a value with no matching <option>).
        const themeSelect = document.getElementById('themeSelect');
        if (themeSelect && config.theme) {
          themeSelect.value = resolveTheme(config.theme);
        }
        
        // Gate LEDs and webhook event settings
        const gateLEDsEnabledToggle = document.getElementById('gateLEDsEnabled');
        const webhookRaceStartToggle = document.getElementById('webhookRaceStart');
        const webhookRaceStopToggle = document.getElementById('webhookRaceStop');
        const webhookLapToggle = document.getElementById('webhookLap');
        const gateLEDOptions = document.getElementById('gateLEDOptions');
        
        if (gateLEDsEnabledToggle && config.gateLEDsEnabled !== undefined) {
          gateLEDsEnabledToggle.checked = config.gateLEDsEnabled === 1;
          if (gateLEDOptions) {
            gateLEDOptions.style.display = config.gateLEDsEnabled === 1 ? 'block' : 'none';
          }
        }
        
        if (webhookRaceStartToggle && config.webhookRaceStart !== undefined) {
          webhookRaceStartToggle.checked = config.webhookRaceStart === 1;
        }
        
        if (webhookRaceStopToggle && config.webhookRaceStop !== undefined) {
          webhookRaceStopToggle.checked = config.webhookRaceStop === 1;
        }
        
        if (webhookLapToggle && config.webhookLap !== undefined) {
          webhookLapToggle.checked = config.webhookLap === 1;
        }
        
        // Webhooks
        const webhooksEnabled = config.webhooksEnabled === 1;
        const webhooksCheckbox = document.getElementById('webhooksEnabled');
        if (webhooksCheckbox) {
          webhooksCheckbox.checked = webhooksEnabled;
          toggleWebhooks(webhooksEnabled, { save: false });
        }
        // RSSI sensitivity
        const rssiSensitivitySelect = document.getElementById('rssiSensitivity');
        if (rssiSensitivitySelect && config.rssiSens !== undefined) {
          rssiSensitivitySelect.value = String(config.rssiSens);
        }

        // WiFi antenna settings
        const extAntennaToggle = document.getElementById('externalAntennaToggle');
        const antennaLabel = document.getElementById('antennaLabel');
        console.log('[DEBUG-MODAL] wifiExtAntenna from /config:', config.wifiExtAntenna, 'type:', typeof config.wifiExtAntenna);
        console.log('[DEBUG-MODAL] voiceEnabled from /config:', config.voiceEnabled, 'type:', typeof config.voiceEnabled);
        if (extAntennaToggle && config.wifiExtAntenna !== undefined) {
          extAntennaToggle.checked = config.wifiExtAntenna === 1;
          if (antennaLabel) antennaLabel.textContent = config.wifiExtAntenna === 1 ? 'External' : 'Internal';
        }
        const txPowerInput = document.getElementById('wifiTxPowerInput');
        if (txPowerInput && config.wifiTxPower !== undefined) {
          txPowerInput.value = config.wifiTxPower;
        }

        // Signal processing
        const gate1Toggle = document.getElementById('gate1BootstrapToggle');
        const gate1Label  = document.getElementById('gate1BootstrapLabel');
        if (gate1Toggle && config.gate1Bootstrap !== undefined) {
          gate1Toggle.checked = (parseInt(config.gate1Bootstrap, 10) || 0) === 1;
          if (gate1Label) gate1Label.textContent = gate1Toggle.checked ? 'On' : 'Off';
        }
        const v1SmoothingSlider = document.getElementById('v1Smoothing');
        const v1SmoothingSpan   = document.getElementById('v1SmoothingSpan');
        if (v1SmoothingSlider && config.v1Smoothing !== undefined) {
          const v = Math.min(10, Math.max(0, parseInt(config.v1Smoothing, 10)));
          v1SmoothingSlider.value = Number.isFinite(v) ? v : 5;
          if (v1SmoothingSpan) v1SmoothingSpan.textContent = v1SmoothingSlider.value;
        }

        // Multi-node settings
        // Update mnNodeMode FIRST so onNodeModeChange() (which calls
        // updateApplyMultiNodeButtonState) sees the correct live mode.
        if (config.nodeMode !== undefined) {
          mnNodeMode = config.nodeMode;
          onRaceTabOpen();  // switch Race tab to master view if needed
          applyCalibMasterNote();
        }
        const nodeModeSelect = document.getElementById('nodeModeSelect');
        if (nodeModeSelect && config.nodeMode !== undefined) {
          nodeModeSelect.value = String(config.nodeMode);
          onNodeModeChange();
        }
        const masterSSIDInput = document.getElementById('masterSSIDInput');
        if (masterSSIDInput && config.masterSSID !== undefined) {
          masterSSIDInput.value = config.masterSSID;
          if (config.masterSSID) _savedMasterSSID = config.masterSSID;
          if (mnNodeMode === 2) { mnStatusSSID = config.masterSSID; mnUpdateRaceStatusBar(); }
        }
        const mnSkipToggle = document.getElementById('mnSkipMasterStartToggle');
        if (mnSkipToggle && config.mnSkipMasterStart !== undefined) {
          mnSkipToggle.checked = !!config.mnSkipMasterStart;
          mnClientSkipEnabled = !!config.mnSkipMasterStart;
        }
        const mnAudioToggle = document.getElementById('mnClientRaceAudioToggle');
        if (mnAudioToggle && config.mnClientRaceAudio !== undefined) {
          mnAudioToggle.checked = !!config.mnClientRaceAudio;
          mnClientRaceAudio = !!config.mnClientRaceAudio;
        }

        const devModeToggle = document.getElementById('devModeToggle');
        const devModeLabel  = document.getElementById('devModeLabel');
        if (devModeToggle && config.devMode !== undefined) {
          devModeToggle.checked = !!config.devMode;
          mnDevMode = !!config.devMode;
          if (devModeLabel) devModeLabel.textContent = config.devMode ? 'On' : 'Off';
          const _pnd = document.getElementById('pilotNameDisplay');
          if (_pnd) _pnd.style.cursor = config.devMode ? 'pointer' : 'default';
          applyAddLapButtonUI();    // Dev Mode gates the button's visibility
          applySystemMonitorUI();   // ...and the Diagnostics log panel
        }

        const otaChannelSel = document.getElementById('otaChannelSelect');
        if (otaChannelSel && config.otaIncludePrereleases !== undefined) {
          otaChannelSel.value = config.otaIncludePrereleases ? '1' : '0';
        }

        // All UI fields populated — now unlock staging so user changes can be tracked
        clearStagedConfig();
        settingsLoading = false;

      })
      .catch(error => {
        console.error('Error loading config:', error);
        clearStagedConfig();   // discard any stale staged values from startup
        settingsLoading = false;
      });
    
    // Switch to general section by default
    switchSettingsSection('general');
  }
}

// ════════════════════════════════════════════════════════════════════
//  MULTI-NODE
// ════════════════════════════════════════════════════════════════════

let mnPollingInterval    = null;
let mnRaceRunning        = false;
let mnMasterRaceActive   = false;  // true when master initiated the current race
let mnMyNodeId           = 0;      // this client's assigned node ID
let mnNodeMode           = 0;      // 0=standalone, 1=master, 2=client (cached from /api/mode)

// Show/hide the master-only note above the Calibration tab's Start Wizard
// button.  In master mode the tab still only calibrates the master's own
// RX5808 — client calibration happens via the pilot pencil on the Race
// tab.  Called from the config-load path once mnNodeMode is populated.
function applyCalibMasterNote() {
  const note = document.getElementById('calibMasterNote');
  if (!note) return;
  note.style.display = (mnNodeMode === 1) ? 'block' : 'none';
}

// Visibility + hard lockout for the manual "Add Lap" button.
//
// Two separate rules, deliberately not merged:
//
//  1. HIDDEN unless Dev Mode is on, for single and client.  Injecting a lap by
//     hand is a development affordance, not something a pilot should find on
//     their own race screen.  The MASTER keeps it regardless — that is the race
//     director's console, where adding a missed lap is a legitimate correction.
//
//  2. DISABLED when this client is racing under a master, even with Dev Mode
//     on.  A client's laps are synced upstream, so a fabricated crossing does
//     not just mislead the pilot — it corrupts the director's record of the
//     race.  mnMasterRaceActive is only ever set when the client is actually
//     following the director (both masterRaceState handlers short-circuit on
//     mnClientSkipEnabled), so an "ignore race director" client is unaffected
//     and keeps its Dev Mode button.
//
// Only ever FORCES disabled — it never enables.  Whether the button is live
// otherwise is owned by the existing race start/stop paths, and this must not
// fight them.
function applyAddLapButtonUI() {
  const btn = document.getElementById('addLapButton');
  if (!btn) return;

  const isMaster = (mnNodeMode === 1);
  btn.hidden = !isMaster && !mnDevMode;

  if (mnNodeMode === 2 && mnMasterRaceActive) {
    btn.disabled = true;
  }
}

// Show the Diagnostics System Monitor only in Dev Mode.
//
// The log stream is engineering output — slot letters, lap-sync state, RSSI
// internals — and a pilot has no use for it.  Same reasoning as Add Lap, and
// deliberately the same switch, so there is one "show me the internals" control
// rather than two.
//
// Only the PANEL is hidden.  The log ring behind it keeps filling in every
// build: the calibration banner reads that same stream through
// startDebugListener() to know when a retune has finished, so the background
// poll must survive being hidden.  stopSerialMonitor() drops the poll back to
// its slow 3 s cadence rather than ending it, which is exactly what we want.
function applySystemMonitorUI() {
  const section = document.getElementById('systemMonitorSection');
  if (!section) return;
  section.hidden = !mnDevMode;

  // Turning Dev Mode off with the monitor running would otherwise leave it
  // polling at 300 ms behind a hidden panel, forever.
  if (!mnDevMode && serialMonitorActive) stopSerialMonitor();
}
let mnCurrentNodes       = [];     // latest node list from multiNodeState SSE / polling
// ── Node-list freshness ──────────────────────────────────────────────────────
// mnCurrentNodes starts empty, and a slot with no entry renders "Not connected".
// That meant an UNREACHABLE timer looked exactly like a timer reporting that
// nobody is connected — the UI stated as fact something it had no information
// about.  Seen 2026-08-08: during a heap collapse the master could not accept
// TCP connections, so every /api/multinode/nodes poll failed; the browser showed
// all seven pilots disconnected while the master's own log listed them all
// connected.  Both were "right" — they were answering different questions.
//
// For a race director mid-event that is the worst failure mode available: it
// looks like the field just dropped out.  A stale display that admits it is
// stale is far better than a confident wrong one.  These two flags split the
// single boolean into three states — unknown, stale, and genuinely empty.
let mnPollEverSucceeded  = false;  // false until the first successful node poll
let mnPollFailStreak     = 0;      // consecutive failed polls; reset on success
const MN_POLL_STALE_AFTER = 3;     // ~6 s at the 2 s poll interval
let mnImportedNodes      = null;   // non-null while viewing an imported race; blocks polling from overwriting
let mnRaceTimerIntervalId = null;
let _pageInitDone        = false;  // true once DOMContentLoaded IIFE completes successfully
let mnRaceStartMs         = 0;
let mnMasterConnected     = false; // client: true when registered with master
let mnClientPollInterval  = null;  // client: timer for periodic /api/mode polls
let mnDevMode             = false; // dev mode: click pilot name to simulate a lap
let mnStatusSSID          = '';    // SSID string for the race status bar (master mode: own SSID; client mode: master SSID)
let mnMyOwnSSID           = '';    // this device's own SSID — needed so the client banner can show its own MAC suffix

/** Show/hide client-specific fields in the Multi-Node settings section */
let _savedMasterSSID = '';

function onNodeModeChange() {
  const sel = document.getElementById('nodeModeSelect');
  if (!sel) return;
  const mode = parseInt(sel.value, 10);
  const clientFields  = document.getElementById('mn-client-fields');
  const masterInfo    = document.getElementById('mn-master-info');
  const warningText   = document.getElementById('mn-ip-warning-text');
  const ssidInput     = document.getElementById('masterSSIDInput');

  const skipRow      = document.getElementById('mnSkipMasterStartRow');
  const audioRow     = document.getElementById('mnClientRaceAudioRow');

  if (mode === 2) {
    // Switching TO client — restore any previously entered SSID
    if (clientFields) clientFields.style.display = '';
    if (ssidInput && _savedMasterSSID) ssidInput.value = _savedMasterSSID;
    if (skipRow)  skipRow.style.display  = '';
    if (audioRow) audioRow.style.display = '';
  } else {
    // Switching AWAY from client — save the current SSID value before hiding
    if (ssidInput && ssidInput.value.trim()) _savedMasterSSID = ssidInput.value.trim();
    if (clientFields) clientFields.style.display = 'none';
    if (skipRow)  skipRow.style.display  = 'none';
    if (audioRow) audioRow.style.display = 'none';
  }

  // Recruit panel is only meaningful on a device that's currently running as
  // master — recruitment scans nearby APs and pushes config to them.  Show
  // it based on the LIVE firmware mode, not the staged selection.
  const recruitSection = document.getElementById('mn-recruit-section');
  if (recruitSection) recruitSection.style.display = (mnNodeMode === 1) ? '' : 'none';

  // Show IP-change warning when selected mode has a different IP than the live firmware mode
  const deviceIsMaster   = (mnNodeMode === 1);
  const selectingMaster  = (mode === 1);
  const ipWillChange     = (selectingMaster !== deviceIsMaster);

  if (masterInfo) masterInfo.style.display = ipWillChange ? '' : 'none';
  if (warningText) {
    warningText.textContent = selectingMaster
      ? 'IP address will change to 192.168.5.1'
      : 'IP address will change to 192.168.4.1';
  }

  updateApplyMultiNodeButtonState();
}

// Enable the "Apply Multi-Node & Reboot" button only when the dropdown
// differs from the firmware's currently-running node mode (mnNodeMode).
// Reboot is the only way to commit a mode change, so the button is meaningless
// when nothing has changed.
function updateApplyMultiNodeButtonState() {
  const btn = document.getElementById('applyMultiNodeBtn');
  const sel = document.getElementById('nodeModeSelect');
  if (!btn || !sel) return;
  const selected = parseInt(sel.value, 10);
  const changed = Number.isFinite(selected) && selected !== mnNodeMode;
  btn.disabled = !changed;
  btn.style.opacity = changed ? '1' : '0.5';
  btn.style.cursor = changed ? 'pointer' : 'not-allowed';
}

/** Recruit nearby FPVRaceOne units as clients of this master.
 *
 * Confirms with the director, fires POST /api/multinode/recruit, then shows a
 * full-screen overlay while the master's AP is down (the browser will lose
 * connection during this window).  Polls the master's IP until it answers
 * again, fetches the recruit summary, toasts the result, and dismisses the
 * overlay.
 */
async function recruitNearbyUnits() {
  const forceEl = document.getElementById('recruitForceToggle');
  const force   = !!(forceEl && forceEl.checked);

  // Warning popup — the owners of nearby nodes need to consent.
  const confirmed = await new Promise(resolve => {
    const overlay = document.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:9999;display:flex;align-items:center;justify-content:center;';
    const box = document.createElement('div');
    box.style.cssText = 'background:var(--card-bg,#fff);border-radius:10px;padding:28px 32px;max-width:460px;width:90%;box-shadow:0 8px 32px rgba(0,0,0,0.3);font-family:inherit;';
    const forceWarning = force
      ? `<div style="background:#fde6e6;border:1px solid #c0392b;border-radius:6px;padding:10px 14px;color:#5a0000;font-size:13px;margin-bottom:14px;">
           <strong>Force mode is ON.</strong> This will reconfigure <em>every</em> FPVRaceOne unit in range — including units already running as a master or as a client of a different master.
         </div>` : '';
    box.innerHTML = `
      <h3 style="margin:0 0 12px;font-size:17px;color:#222;">Recruit nearby units?</h3>
      ${forceWarning}
      <p style="margin:0 0 12px;font-size:14px;color:#444;line-height:1.5;">
        This will scan for every FPVRaceOne unit in range and configure them as clients of this master, then reboot them.
      </p>
      <p style="margin:0 0 16px;font-size:13px;color:#666;line-height:1.5;">
        Make sure the owners of nearby nodes are aware before proceeding — their devices will be reconfigured and rebooted automatically.
      </p>
      <p style="margin:0 0 20px;font-size:13px;color:#666;line-height:1.5;">
        This master's WiFi access point will go offline for up to ~60 seconds while it works. Your browser will lose its connection until the master comes back up.
      </p>
      <div style="display:flex;gap:12px;justify-content:flex-end;">
        <button id="_rnCancel"   style="padding:8px 20px;border-radius:6px;border:1px solid #aaa;background:#f5f5f5;color:#333;cursor:pointer;font-size:14px;">Cancel</button>
        <button id="_rnContinue" style="padding:8px 20px;border-radius:6px;border:none;background:var(--primary-color,#2196F3);color:#fff;cursor:pointer;font-size:14px;font-weight:600;">Continue</button>
      </div>`;
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    document.getElementById('_rnCancel').onclick   = () => { overlay.remove(); resolve(false); };
    document.getElementById('_rnContinue').onclick = () => { overlay.remove(); resolve(true);  };
  });
  if (!confirmed) return;

  // Flag the recruit phase BEFORE the master drops its AP — showDisconnectedBanner
  // checks this and stays out of the way so the user only sees the recruit overlay.
  window.__recruitInProgress = true;
  hideDisconnectedBanner();

  // Working overlay — opaque background and a z-index above the disconnect
  // banner (9999) so it's the only thing the director sees while the AP is
  // down.  Paint it BEFORE firing the recruit POST: that fetch can sit pending
  // for several seconds while the master is queueing the job and dropping its
  // AP, and if we awaited it the user would see a frozen UI before the overlay
  // ever appears.
  const overlay = document.createElement('div');
  overlay.id = '_recruitOverlay';
  overlay.style.cssText = 'position:fixed;inset:0;background:#1a1a1a;z-index:10001;display:flex;flex-direction:column;align-items:center;justify-content:center;color:#fff;font-family:inherit;text-align:center;padding:24px;';
  overlay.innerHTML = `
    <div style="font-size:26px;font-weight:700;margin-bottom:14px;">Configuring nearby units as clients…</div>
    <div style="font-size:15px;opacity:0.85;margin-bottom:6px;max-width:520px;line-height:1.55;">
      This master node is currently disconnected and recruiting other FPVRaceOne units.<br>
      The access point will return automatically when configuration is complete.<br>
      (Note you may need to reconnect to the master's wifi)
    </div>
    <div id="_recruitStatusLine" style="font-size:13px;opacity:0.7;margin-top:18px;">Waiting for master to come back…</div>`;
  document.body.appendChild(overlay);
  // Yield once so the browser actually paints the overlay before we kick off
  // the POST — without this it can still feel like a delay on slower devices
  // because the POST starts synchronously in the same task.
  await new Promise(r => setTimeout(r, 0));

  // Fire-and-forget — we don't await so the overlay stays interactive.  The
  // master returns 202 quickly, but its response may not flush before the AP
  // goes down; either way we just want the request on the wire.
  fetch('/api/multinode/recruit?force=' + (force ? '1' : '0'), { method: 'POST' })
    .catch(e => console.warn('[Recruit] POST may have failed mid-flight', e));

  // Wait 5 s before polling so the master has time to actually drop its AP.
  await new Promise(r => setTimeout(r, 5000));

  // Poll for the master to come back online + fetch the summary.
  const statusLine = document.getElementById('_recruitStatusLine');
  let summary = null;
  const maxAttempts = 90;  // 90 × 1s = 90s hard cap
  for (let i = 0; i < maxAttempts; i++) {
    try {
      const r = await fetch('/api/multinode/recruit/status', { cache: 'no-store' });
      if (r.ok) {
        const j = await r.json();
        if (j.valid && !j.inProgress) { summary = j; break; }
        if (j.inProgress && statusLine) statusLine.textContent = 'Master responding — recruit still running…';
      }
    } catch (_) { /* still down — keep polling */ }
    await new Promise(r => setTimeout(r, 1000));
  }

  overlay.remove();
  // Allow the normal disconnect banner to fire again if the master is unhealthy.
  window.__recruitInProgress = false;

  if (summary) {
    alert(`Recruit complete:\n\nFound: ${summary.found}\nRecruited: ${summary.recruited}\nSkipped: ${summary.skipped}\nFailed: ${summary.failed}`);
  } else {
    alert('Recruit may still be in progress — the master did not respond within the timeout. Refresh in a few seconds.');
  }
}

/** Apply multi-node settings and reboot */
async function applyMultiNodeSettings() {
  const nodeModeEl = document.getElementById('nodeModeSelect');
  const masterSSIDEl = document.getElementById('masterSSIDInput');
  const nodeMode = nodeModeEl ? parseInt(nodeModeEl.value, 10) : 0;
  const masterSSID = masterSSIDEl ? masterSSIDEl.value.trim() : '';

  if (nodeMode === 2 && !masterSSID) {
    alert('Please enter the Master SSID before applying Client mode.');
    return;
  }

  const becomingMaster  = (nodeMode === 1);
  const leavingMaster   = (mnNodeMode === 1 && !becomingMaster);
  const ipChanging      = becomingMaster || leavingMaster;
  const newIP           = becomingMaster ? '192.168.5.1' : '192.168.4.1';

  // Confirmation dialog
  const confirmed = await new Promise(resolve => {
    const overlay = document.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:9999;display:flex;align-items:center;justify-content:center;';
    const box = document.createElement('div');
    box.style.cssText = 'background:var(--card-bg,#fff);border-radius:10px;padding:28px 32px;max-width:400px;width:90%;box-shadow:0 8px 32px rgba(0,0,0,0.3);font-family:inherit;';
    box.innerHTML = `
      <h3 style="margin:0 0 12px;font-size:17px;color:#222;">Apply Multi-Node &amp; Reboot?</h3>
      ${ipChanging ? `<div style="background:#fff3cd;border:1px solid #f0ad4e;border-radius:6px;padding:10px 14px;color:#5a3e00;font-size:13px;margin-bottom:16px;">
        ⚠️ <strong>IP address will change to ${newIP}</strong> after reboot.<br>
        Your browser will be redirected automatically.
      </div>` : ''}
      <p style="margin:0 0 20px;font-size:14px;color:#555;">The device will save settings and reboot. This takes about 10 seconds.</p>
      <div style="display:flex;gap:12px;justify-content:flex-end;">
        <button id="_mnCancel" style="padding:8px 20px;border-radius:6px;border:1px solid #aaa;background:#f5f5f5;color:#333;cursor:pointer;font-size:14px;">Cancel</button>
        <button id="_mnContinue" style="padding:8px 20px;border-radius:6px;border:none;background:var(--primary-color,#2196F3);color:#fff;cursor:pointer;font-size:14px;font-weight:600;">Continue</button>
      </div>`;
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    document.getElementById('_mnCancel').onclick  = () => { overlay.remove(); resolve(false); };
    document.getElementById('_mnContinue').onclick = () => { overlay.remove(); resolve(true); };
  });

  if (!confirmed) return;

  // Force-stage nodeMode and masterSSID so saveConfig() always sends them,
  // even if the user never triggered autoSaveConfig on the dropdown.
  stageConfig('nodeMode', nodeMode);
  stageConfig('masterSSID', masterSSID);
  stagedDirty = true;

  try {
    await saveConfig();
    await new Promise(r => setTimeout(r, 500));
  } catch (e) {
    console.error('[MULTINODE] Apply failed:', e);
    alert('Failed to apply multi-node settings. Check connection and try again.');
    return;
  }

  // Fire reboot (response may never arrive — that's expected)
  fetch('/reboot', { method: 'POST' }).catch(() => {});

  // Show overlay and poll new IP until device responds, then redirect.
  const targetURL = `http://${newIP}/`;
  const msg = document.createElement('div');
  msg.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.75);z-index:9999;display:flex;flex-direction:column;align-items:center;justify-content:center;color:#fff;font-family:inherit;text-align:center;padding:24px;';
  const statusLine = document.createElement('div');
  statusLine.style.cssText = 'font-size:14px;opacity:0.7;margin-top:10px;';
  statusLine.textContent = 'Waiting for device…';
  const manualLink = document.createElement('div');
  manualLink.style.cssText = 'font-size:13px;opacity:0;margin-top:14px;transition:opacity 0.5s;';
  manualLink.innerHTML = `Taking longer than expected? <a href="${targetURL}" style="color:#7ecfff;">Click here to go manually</a>`;
  msg.innerHTML = `
    <div style="font-size:24px;font-weight:700;margin-bottom:10px;">Rebooting…</div>
    <div style="font-size:15px;opacity:0.9;margin-bottom:4px;">Redirecting to <strong>${targetURL}</strong></div>
    ${ipChanging ? `<div style="font-size:13px;opacity:0.65;margin-bottom:4px;">(Make sure you reconnect to the module's WiFi if needed)</div>` : ''}`;
  msg.appendChild(statusLine);
  msg.appendChild(manualLink);
  document.body.appendChild(msg);

  // After 18 s show the manual link in case the user needs to switch WiFi first
  setTimeout(() => { manualLink.style.opacity = '1'; }, 18000);

  // Poll every 500 ms; fetch mode:'no-cors' resolves (opaque) when reachable,
  // throws NetworkError when not — works cross-origin without CORS headers.
  // Wait 2 s first so the device has time to actually start rebooting.
  await new Promise(r => setTimeout(r, 2000));
  let dots = 0;
  const maxAttempts = 60; // 60 × 500 ms = 30 s hard cap
  for (let i = 0; i < maxAttempts; i++) {
    try {
      await fetch(targetURL, { method: 'GET', mode: 'no-cors', cache: 'no-store' });
      statusLine.textContent = 'Device online — redirecting!';
      // Stop all polling so in-flight requests don't compete with the new page's
      // static-file loads for TCP slots on the ESP32.
      mnStopPolling();
      mnStopClientPoll();
      if (connectionStatusUpdateInterval) { clearInterval(connectionStatusUpdateInterval); connectionStatusUpdateInterval = null; }
      if (keepaliveWatchdogTimer) { clearInterval(keepaliveWatchdogTimer); keepaliveWatchdogTimer = null; }
      if (eventSource) { eventSource.close(); eventSource = null; }
      // Give the device 1.5 s to drain any lingering TCP connections before
      // the browser opens fresh ones for the redirected page's assets.
      await new Promise(r => setTimeout(r, 1500));
      window.location.href = targetURL;
      return;
    } catch (_) {
      dots = (dots + 1) % 4;
      statusLine.textContent = 'Waiting for device' + '.'.repeat(dots + 1);
      await new Promise(r => setTimeout(r, 500));
    }
  }
  // Hard cap — redirect anyway
  mnStopPolling();
  mnStopClientPoll();
  if (connectionStatusUpdateInterval) { clearInterval(connectionStatusUpdateInterval); connectionStatusUpdateInterval = null; }
  if (eventSource) { eventSource.close(); eventSource = null; }
  window.location.href = targetURL;
}

/** Scan for nearby FPVRaceOne_ networks and show results in the settings panel */
async function mnScanNetworks() {
  const resultsEl = document.getElementById('mnScanResults');
  if (resultsEl) resultsEl.textContent = 'Scanning…';
  try {
    const r = await fetch('/api/multinode/scan');
    if (!r.ok) throw new Error(r.status);
    const data = await r.json();
    const nets = data.networks || [];
    if (!resultsEl) return;
    if (nets.length === 0) {
      resultsEl.textContent = 'No FPVRaceOne devices found nearby.';
      return;
    }

    // Auto-select if exactly one master is detected
    const masters = nets.filter(n => n.isMaster);
    if (masters.length === 1) mnSelectSSID(masters[0].ssid);

    resultsEl.innerHTML = '';
    nets.forEach(n => {
      const row = document.createElement('div');
      row.style.cssText = `display:flex;align-items:center;gap:8px;margin-bottom:4px;${n.isMaster ? '' : 'opacity:0.55;'}`;
      const badge = n.isMaster
        ? '<span style="font-size:11px;font-weight:600;background:#2196f3;color:#fff;padding:2px 6px;border-radius:4px;">Master</span>'
        : '<span style="font-size:11px;color:var(--secondary-color);">Client</span>';
      row.innerHTML = `${badge}<span style="flex:1;">${n.ssid}</span>
        <span style="color:var(--secondary-color);font-size:12px;">${n.rssi} dBm ch${n.channel}</span>
        <button style="padding:3px 10px;font-size:12px;background:var(--primary-color);color:#fff;border:none;border-radius:4px;cursor:pointer;"
                onclick="mnSelectSSID('${n.ssid}')">Select</button>`;
      resultsEl.appendChild(row);
    });
  } catch (e) {
    if (resultsEl) resultsEl.textContent = 'Scan failed: ' + e.message;
  }
}

function mnSelectSSID(ssid) {
  const el = document.getElementById('masterSSIDInput');
  if (el) { el.value = ssid; autoSaveConfig(); }
  const resultsEl = document.getElementById('mnScanResults');
  if (resultsEl) resultsEl.textContent = 'Selected: ' + ssid;
}

/** Start polling /api/multinode/nodes on the Multi-Node tab (master view) */
function mnStartPolling() {
  if (mnPollingInterval) return;
  mnPollingInterval = setInterval(mnRefreshNodes, 2000);
  mnRefreshNodes();
}

function mnStopPolling() {
  if (mnPollingInterval) { clearInterval(mnPollingInterval); mnPollingInterval = null; }
}

function mnStartClientPoll() {
  if (mnClientPollInterval) return;
  mnClientPollInterval = setInterval(async () => {
    try {
      const r = await fetch('/api/mode');
      if (!r.ok) return;
      const d = await r.json();
      mnMasterConnected = d.masterConnected || false;
      mnMyNodeId        = d.myNodeId        || 0;
      mnUpdateRaceStatusBar();
    } catch (_) {}
  }, 15000);
}

function mnStopClientPoll() {
  if (mnClientPollInterval) { clearInterval(mnClientPollInterval); mnClientPollInterval = null; }
}

async function mnRefreshNodes() {
  let ok = false;
  try {
    const r = await fetch('/api/multinode/nodes');
    if (r.ok) {
      const data = await r.json();
      // Same merge the SSE path does.  /api/multinode/nodes is built by the
      // SAME _buildDirectorStatePayload() as the push, so once the §10 gate is
      // open it carries digests + lapDeltas and NO per-node `laps` arrays.
      // Rendering it raw wipes every client's laps to 0, and because this poll
      // interleaves with the SSE push (and fires again on every multiNodeLap)
      // the table visibly fills and blanks over and over.
      const nodes = rvMergeNodes(Array.isArray(data.nodes) ? data.nodes : [],
                                 Array.isArray(data.lapDeltas) ? data.lapDeltas : [],
                                 (data.race || {}).raceId);
      ok = true;
      mnRenderNodes(nodes);
      if (mnImportedNodes === null) mnRenderRaceTab(nodes);
    }
  } catch (_) {}

  // A failed poll must NOT be rendered as "no nodes".  Keep the last known list
  // on screen and record the failure; mnRenderRaceTab turns a run of failures
  // into a visible staleness banner.  See the notes on mnPollEverSucceeded.
  if (ok) {
    mnPollEverSucceeded = true;
    mnPollFailStreak    = 0;
  } else if (++mnPollFailStreak === MN_POLL_STALE_AFTER
             && mnImportedNodes === null && Array.isArray(mnCurrentNodes)) {
    // Re-render once, on the transition, so the banner appears.  Only on the
    // transition: mnRenderRaceTab skips identical HTML, so repeating this every
    // 2 s would be harmless but pointless — and the banner is deliberately
    // static text (no "last seen 12s ago") to keep that HTML comparison a hit.
    // A ticking timestamp would rebuild the DOM every poll and drop in-flight
    // clicks, which is the problem __mnLastRaceTabHtml exists to prevent.
    try { mnRenderRaceTab(mnCurrentNodes); } catch (_) {}
  }
}

// Format milliseconds as a TTS-friendly string ("3 minutes 49 point 4 6" or "49 point 5 0").
function formatMsSpeak(ms) {
  if (!ms || ms <= 0) return '0';
  const d   = lapDecimals();
  const h   = Math.floor(ms / 3600000);
  const m   = Math.floor((ms % 3600000) / 60000);
  const s   = Math.floor((ms % 60000) / 1000);
  // Sub-second remainder at the configured precision: tenths, hundredths or
  // thousandths.  Digits are spoken individually (space-separated) so that
  // e.g. 045 reads "zero four five" rather than "forty-five", which would be
  // a different time.
  const sub = Math.floor((ms % 1000) / Math.pow(10, 3 - d));
  const dec = sub.toString().padStart(d, '0').split('').join(' ');
  const tail = `${s} point ${dec}`;
  // Mirror formatMsDisplay's field structure: MM:SS:frac below an hour,
  // HH:MM:SS:frac above.  Past an hour the minutes term is spoken even when
  // it is zero ("1 hour 0 minutes 5 point 5 0"), because the display shows a
  // minutes field there too and the two must describe the same thing.
  if (h > 0) return `${h} hour${h !== 1 ? 's' : ''} ${m} minute${m !== 1 ? 's' : ''} ${tail}`;
  if (m > 0) return `${m} minute${m !== 1 ? 's' : ''} ${tail}`;
  return tail;
}

// Format milliseconds as M:SS.frac (e.g. 1:23.45) for the race view.
// Always 2-digit seconds so times don't jitter visually ("5.05" → "05.05");
// minutes are only shown when non-zero.  Fractional precision follows the
// Spoken Precision setting so the leaderboard matches the callouts.
function formatMsRace(ms) {
  if (!ms || ms <= 0) return '—';
  const m  = Math.floor(ms / 60000);
  const s  = Math.floor((ms % 60000) / 1000);
  const ss = s.toString().padStart(2, '0');
  const cc = lapFracStr(ms);
  return m > 0 ? `${m}:${ss}.${cc}` : `${ss}.${cc}`;
}

// Format milliseconds as MM:SS:FRACs (e.g. 01:23:45s). Switches to
// HH:MM:SS:FRACs at 1 hour.  Fractional precision follows the Spoken Precision
// setting, so every lap time on screen matches what the announcer says.
function formatMsDisplay(ms) {
  if (!ms || ms <= 0) return `00:00:${'0'.repeat(lapDecimals())}s`;
  const h  = Math.floor(ms / 3600000);
  const m  = Math.floor((ms % 3600000) / 60000);
  const s  = Math.floor((ms % 60000) / 1000);
  const mm = m.toString().padStart(2, '0');
  const ss = s.toString().padStart(2, '0');
  const cc = lapFracStr(ms);
  if (h > 0) return `${h.toString().padStart(2, '0')}:${mm}:${ss}:${cc}s`;
  return `${mm}:${ss}:${cc}s`;
}

// Format a signed gap in ms as e.g. +00:01:23s or -00:01:23s.
function formatMsGap(ms) {
  if (ms === 0) return '00:00:00s';
  return (ms > 0 ? '+' : '-') + formatMsDisplay(Math.abs(ms));
}

// SSIDs follow the FPVRaceOne_<6-hex> format. Pull the last 6 chars when the
// full SSID is present, empty string otherwise.
function _ssidSuffix(ssid) {
  return (ssid && ssid.length >= 6) ? ssid.substring(ssid.length - 6) : '';
}

// Slot UI label.  Backend stays numeric (NodeInfo.nodeId, master config keys,
// /api/multinode/* payloads) but every user-visible reference uses A-G so the
// slot identifier doesn't get visually confused with the leaderboard rank
// numbers shown above the cards.  Master (nodeId 0) returns empty so card
// headers don't get a confusing "@" prefix.
function _slotLetter(nodeId) {
  const id = Number(nodeId);
  if (!Number.isFinite(id) || id < 1 || id > 26) return '';
  return String.fromCharCode(64 + id);
}

// Return '#000' or '#fff' for use as text colour on top of `hexColor`.
// WCAG relative luminance: y = 0.2126·R + 0.7152·G + 0.0722·B (sRGB).  A
// 0.6 threshold puts gold / green / cyan / white / spring-green on black
// text and leaves red / orange / blue / purple / pink / hot-pink / gray /
// black / brown on white text — matches the user's flagged five exactly.
function _pilotCardTextColor(hexColor) {
  if (typeof hexColor !== 'string') return '#fff';
  const h = hexColor.replace('#', '');
  if (h.length < 6) return '#fff';
  const r = parseInt(h.substr(0, 2), 16) / 255;
  const g = parseInt(h.substr(2, 2), 16) / 255;
  const b = parseInt(h.substr(4, 2), 16) / 255;
  if (!Number.isFinite(r) || !Number.isFinite(g) || !Number.isFinite(b)) return '#fff';
  const lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  return lum > 0.6 ? '#000' : '#fff';
}

// Update the status bar above the race clock based on current multi-node mode.
function mnUpdateRaceStatusBar() {
  rvShowTabIfClient();
  const bar  = document.getElementById('mn-race-status-bar');
  const text = document.getElementById('mn-race-status-text');
  if (!bar || !text) return;
  if (mnNodeMode === 1) {
    bar.style.display = '';
    text.textContent  = `Multi-Node (Master) — ${mnStatusSSID || 'FPVRaceOne'}`;
  } else if (mnNodeMode === 2) {
    bar.style.display = '';
    const mySuffix     = _ssidSuffix(mnMyOwnSSID);
    const masterSuffix = _ssidSuffix(mnStatusSSID) || mnStatusSSID || '';
    const prefix       = mySuffix ? `Multi-Node (Client - ${mySuffix})` : 'Multi-Node (Client)';
    if (!mnMasterConnected) {
      text.textContent = `${prefix} — Disconnected from ${masterSuffix}`.trim();
    } else if (mnMyNodeId > 0) {
      text.textContent = `${prefix} ${mnMyNodeId} — Connected to ${masterSuffix}`.trim();
    } else {
      text.textContent = `${prefix} — Searching for master node...`;
    }
  } else {
    bar.style.display = 'none';
  }
}

// Show/hide the correct race view depending on node mode. Called on tab open and on page load.
function onRaceTabOpen() {
  const singleView = document.getElementById('single-race-view');
  const masterView  = document.getElementById('master-race-view');
  if (!singleView || !masterView) return;
  if (mnNodeMode === 1) {
    singleView.style.display = 'none';
    masterView.style.display  = '';
    mnRenderRaceTab(mnCurrentNodes);
    // Start polling so node data loads immediately without visiting Multi-Node tab first.
    // Use mnStartPolling() directly — mnNodeMode is already set by the caller so we don't
    // need mnInitTab()'s extra /api/mode fetch, which would add to the TCP connection burst
    // on page load and could kill the SSE or cause /api/laps/current to fail silently.
    if (!mnPollingInterval) mnStartPolling();
  } else {
    singleView.style.display  = '';
    masterView.style.display  = 'none';
  }
  mnUpdateRaceStatusBar();
  updateHistoryTabMode();
}

// Rename the Race History tab and swap its content based on node mode.
function updateHistoryTabMode() {
  const navLink       = document.getElementById('nav-link-history');
  const masterSection = document.getElementById('mn-history-import-section');
  const singleSection = document.getElementById('mn-history-single-section');
  const historyList   = document.getElementById('raceHistoryList');
  const raceDetails   = document.getElementById('raceDetails');

  if (mnNodeMode === 1) {
    if (navLink)       navLink.textContent  = 'Import Race';
    if (masterSection) masterSection.style.display = '';
    if (singleSection) singleSection.style.display = 'none';
    if (historyList)   historyList.style.display   = 'none';
    if (raceDetails)   raceDetails.style.display   = 'none';
  } else {
    if (navLink)       navLink.textContent  = 'Race History';
    if (masterSection) masterSection.style.display = 'none';
    if (singleSection) singleSection.style.display = '';
    if (historyList)   historyList.style.display   = '';
  }
}

// Dev mode: simulate a lap for a pilot by clicking their name card.
async function mnDevTriggerLap(nodeId, nextLapNumber, callsign) {
  if (!mnDevMode) return;
  // Use actual race clock: elapsed since start minus this pilot's already-logged total
  let lapMs;
  if (mnRaceStartMs > 0) {
    const elapsedMs = Date.now() - mnRaceStartMs;
    if (nodeId === 0) {
      const pilot = _mnMasterEntry();
      const pilotTotalMs = pilot ? (pilot.totalMs || 0) : 0;
      lapMs = Math.max(500, elapsedMs - pilotTotalMs);
    } else {
      const pilot = (mnCurrentNodes || []).find(n => n.nodeId === nodeId);
      const pilotLaps = (pilot && Array.isArray(pilot.laps)) ? pilot.laps : [];
      const pilotTotalMs = pilotLaps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
      lapMs = Math.max(500, elapsedMs - pilotTotalMs);
    }
  } else {
    lapMs = Math.round((Math.random() * 65000) + 25000);  // fallback if race not started
  }
  if (nodeId === 0) {
    // Update local state immediately for instant display.
    const lapSec = lapMs / 1000;
    // toFixed(3), not (2): addLap() parseFloat()s this into the STORED lap
    // time, so rounding here would permanently quantise multi-node laps to
    // 10 ms and make 3-decimal display and announcements impossible. Display
    // precision is applied at render time by formatMsDisplay/formatMsRace.
    if (typeof addLap === 'function') addLap(lapSec.toFixed(3));
    mnRenderRaceTab(mnCurrentNodes);
    // Persist server-side without SSE broadcast (avoids double-adding via the 'lap' event).
    fetch('/timer/persistLap', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ lapTime: lapMs }),
    }).catch(() => {});
  } else {
    try {
      await fetch('/api/multinode/lap', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ nodeId, lapNumber: nextLapNumber, lapTimeMs: lapMs }),
      });
      setTimeout(() => mnRefreshNodes(), 200);
    } catch (e) { console.warn('[DEV] lap inject failed:', e); }
  }
  // TTS/display: SSE 'lap' → addLap() for nodeId 0; SSE 'multiNodeLap' for nodeId 1-7.
}

// Build the master's own pilot entry. Prefers the nodeId:0 entry from the polling
// response (authoritative, includes server-persisted laps). Falls back to the
// window.lapTimes JS global for the brief window before the first poll completes.
function _mnMasterEntry() {
  const nameEl   = document.getElementById('pname');
  const colorEl  = document.getElementById('pilotColor');
  const name     = nameEl ? nameEl.value : 'Master';
  const colorHex = colorEl ? colorEl.value : '#0080FF';
  const colorInt = parseInt(colorHex.replace('#', ''), 16) || 0x0080FF;

  // Try polling data first (authoritative after first poll).
  const polled = Array.isArray(mnCurrentNodes)
    ? mnCurrentNodes.find(n => n.nodeId === 0 && n.isMaster)
    : null;

  // Build laps from whichever source is more up to date.
  // window.lapTimes grows immediately when a lap is tapped; polled data trails by up to 2s.
  // After a page refresh lapTimes is empty and polled data (from the pre-fetch) is used.
  const localLaps = (Array.isArray(window.lapTimes) ? window.lapTimes : [])
    .map((t, i) => ({ lapNumber: i, lapTimeMs: Math.round(t * 1000) }));
  const polledLaps = (polled && Array.isArray(polled.laps)) ? polled.laps : [];
  const laps = localLaps.length >= polledLaps.length ? localLaps : polledLaps;

  // Gate 1 (lapNumber 0) is not a real lap — exclude from count and stats.
  const realLaps  = laps.filter(l => l.lapNumber > 0);
  const lapCount  = realLaps.length;
  const allLapsMs = laps.reduce((s, l) => s + l.lapTimeMs, 0);       // for cumul display
  const totalMs   = realLaps.reduce((s, l) => s + l.lapTimeMs, 0);   // for stats
  const avgMs     = lapCount > 0 ? totalMs / lapCount : 0;
  const fastestMs = lapCount > 0 ? Math.min(...realLaps.map(l => l.lapTimeMs)) : Infinity;
  return { nodeId: 0, pilotName: name, pilotColor: colorInt,
           online: true, running: false, quitEarly: false, isMaster: true,
           laps, lapCount, allLapsMs, totalMs, avgMs, fastestMs };
}

// Render the master race tab: RotorHazard-style summary table + per-pilot lap columns.
// Always shows 8 slots: 1 master + 7 client slots (populated or empty).
//
// opts.containerId — render into a different container (defaults to mn-race-container).
// opts.readOnly    — when true, strips edit buttons and TAP handlers for the client Race View.
// opts.skipMnState — when true, skip mnCurrentNodes assignment + mnUpdateRaceDataButtons (used by client view).
// Cached HTML per container.  The 2-second polling loop calls mnRenderRaceTab
// even when nothing changed; setting innerHTML re-creates every DOM node, which
// kills hover states and — worse — drops in-flight clicks because the button
// the user mousedown'd on no longer exists by mouseup.  Compare new HTML
// against the last write per container and skip the assignment when identical.
window.__mnLastRaceTabHtml = window.__mnLastRaceTabHtml || {};
// Set true by mnRenderRaceTab when it deferred because the Edit Pilot modal was
// open; checked by mnClosePilotModal to fire the deferred render.
window.__mnRaceTabRenderPending = false;

function mnRenderRaceTab(nodes, opts) {
  opts = opts || {};
  const containerId = opts.containerId || 'mn-race-container';
  const readOnly    = !!opts.readOnly;
  // raceRunning is the master's race state.  On the master UI this is the local
  // mnRaceRunning flag (set on Start All / Stop All).  On the client Race View
  // we don't have a local race timer, so the caller passes the master's state
  // from the director-state push — without this, isSoloRacing fires for every
  // client racing under the director.
  const raceRunning = (opts.raceRunning !== undefined) ? !!opts.raceRunning : mnRaceRunning;

  if (!opts.skipMnState) {
    mnCurrentNodes = nodes;
    const masterView = document.getElementById('master-race-view');
    if (!masterView || masterView.style.display === 'none') return;
    const b1 = document.getElementById('mnStartRaceBtnMain'); if (b1) b1.disabled = mnRaceRunning;
    const b2 = document.getElementById('mnStopRaceBtnMain');  if (b2) b2.disabled = !mnRaceRunning;

    // Don't rebuild the DOM while the Edit Pilot modal is open.  The cards
    // underneath are obscured anyway, and re-rendering them every 2s with
    // innerHTML= breaks the buttons the user is about to click after closing
    // the modal: mousedown on a stale button + re-render + mouseup = no click.
    // Stash a flag so mnClosePilotModal can flush a fresh render afterward.
    const pilotModal = document.getElementById('mnPilotModal');
    if (pilotModal && pilotModal.style.display === 'flex') {
      window.__mnRaceTabRenderPending = true;
      return;
    }
  }

  const container = document.getElementById(containerId);
  if (!container) return;

  // Build full 8-slot list: master first, then 7 client slots (filled or empty).
  // In read-only client view, the master entry comes from the pushed nodes array
  // (we don't have authoritative master pilot info locally) — _mnMasterEntry()
  // would read THIS device's pname input instead, which is the client's pilot.
  const master = readOnly
    ? (() => {
        const pushed = nodes.find(n => n.nodeId === 0 && n.isMaster);
        if (!pushed) {
          return { nodeId: 0, pilotName: 'Race Director', online: false, isMaster: true,
                   lapCount: 0, laps: [], allLapsMs: 0, totalMs: 0, avgMs: 0, fastestMs: Infinity };
        }
        const laps      = Array.isArray(pushed.laps) ? pushed.laps.slice().sort((a, b) => a.lapNumber - b.lapNumber) : [];
        const realLaps  = laps.filter(l => l.lapNumber > 0);
        const lapCount  = realLaps.length;
        const allLapsMs = laps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
        const totalMs   = realLaps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
        const avgMs     = lapCount > 0 ? totalMs / lapCount : 0;
        const fastestMs = lapCount > 0 ? Math.min(...realLaps.map(l => l.lapTimeMs || Infinity)) : Infinity;
        return { ...pushed, pilotName: pushed.pilotName || 'Race Director',
                 laps, lapCount, allLapsMs, totalMs, avgMs, fastestMs };
      })()
    : _mnMasterEntry();
  const clients = Array.from({ length: 7 }, (_, i) => {
    const nodeId = i + 1;
    const found  = nodes.find(n => n.nodeId === nodeId);
    if (found) {
      const laps      = Array.isArray(found.laps) ? found.laps.slice().sort((a, b) => a.lapNumber - b.lapNumber) : [];
      const realLaps  = laps.filter(l => l.lapNumber > 0);
      const lapCount  = realLaps.length;
      const allLapsMs = laps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
      const totalMs   = realLaps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
      const avgMs     = lapCount > 0 ? totalMs / lapCount : 0;
      const fastestMs = lapCount > 0 ? Math.min(...realLaps.map(l => l.lapTimeMs || Infinity)) : Infinity;
      return { ...found, laps, lapCount, allLapsMs, totalMs, avgMs, fastestMs };
    }
    return { nodeId, pilotName: null, online: false, empty: true, lapCount: 0, laps: [], allLapsMs: 0, totalMs: 0, avgMs: 0, fastestMs: Infinity };
  });

  const allSlots    = [master, ...clients];
  const activeSlots = allSlots.filter(n => !n.empty);

  // Rank active pilots for summary table: most laps first, then fastest total.
  // Exclude: skipEnabled pilots (always independent), excluded nodes, and solo-racers
  // (running while no master race is active — they rejoin the leaderboard once Start All fires).
  // _mnExcludeNodes provides client-side pre-exclusion during the countdown before the server's
  // excludedFromCurrentRace flag arrives via SSE.
  const _pendingExcludes = new Set(window._mnExcludeNodes || []);
  const ranked = [...activeSlots].filter(n =>
    !n.skipEnabled && !n.excludedFromCurrentRace && !_pendingExcludes.has(n.nodeId) && !(n.running && !n.isMaster && !raceRunning)
  ).sort((a, b) => {
    if (b.lapCount !== a.lapCount) return b.lapCount - a.lapCount;
    if (a.lapCount === 0) return 0;
    return a.totalMs - b.totalMs;
  });

  const globalFastestMs = activeSlots.filter(p => p.lapCount > 0)
    .reduce((best, p) => Math.min(best, p.fastestMs), Infinity);

  // ── Slot state: unknown vs genuinely empty ─────────────────────
  // An empty slot means "nobody is in it" ONLY once we've actually heard from
  // the source of truth.  Before that we know nothing, and saying "Not
  // connected" would be inventing an answer.  Master view's source is the
  // /api/multinode/nodes poll; the read-only client view's is the director-state
  // push, whose arrival is implied by a non-empty nodes array.
  const slotStateKnown = readOnly ? nodes.length > 0 : mnPollEverSucceeded;
  const emptySlotLabel = slotStateKnown
    ? 'Not connected'
    : (readOnly ? 'Waiting for race director…' : 'Waiting for timer…');

  // ── Summary leaderboard table ──────────────────────────────────
  let html = '';
  // Stale: we had data, then the timer stopped answering.  Keep showing the last
  // known race — a director mid-event needs the lap counts — but never let it
  // masquerade as live.
  if (!readOnly && mnPollFailStreak >= MN_POLL_STALE_AFTER) {
    html += '<div class="mn-stale-banner">'
          + '&#9888; Not receiving updates from this timer — showing last known data.'
          + '</div>';
  }
  html += '<table class="mn-leaderboard"><thead><tr>';
  html += '<th></th><th>Pilot</th><th>Laps</th><th>Total</th><th>Avg</th><th>Fastest</th>';
  html += '</tr></thead><tbody>';

  ranked.forEach((n, i) => {
    const color    = '#' + ((n.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0');
    const callsign = n.pilotName || (n.isMaster ? 'Master' : 'Node ' + _slotLetter(n.nodeId));
    // Inline, immediately after the callsign — these label the pilot, so they
    // belong beside the name.  They used to be float:right, which parked them
    // against the far edge of a very wide Pilot column, reading as a stray word
    // in the middle of the row.  They also carried no background at all, unlike
    // every other .mn-card-badge, so .mn-lb-tag gives them one.
    const hostTag  = n.isMaster ? ' <span class="mn-card-badge mn-lb-tag">Host</span>' : '';
    const meTag    = (readOnly && !n.isMaster && n.nodeId === mnMyNodeId)
      ? ' <span class="mn-card-badge mn-card-badge-me mn-lb-tag">Me</span>' : '';
    let badge = '';
    if (n.quitEarly) badge = ' <span class="mn-status-dnf">DNF</span>';
    const isFastPilot = isFinite(globalFastestMs) && n.fastestMs === globalFastestMs;
    const statusDotColor = n.online !== false ? '#4caf50' : '#f44336';
    html += `<tr>
      <td class="mn-lb-rank">${i + 1}</td>
      <td class="mn-lb-pilot">
        <span style="display:inline-block;width:10px;height:10px;border-radius:50%;background:${statusDotColor};margin-right:6px;vertical-align:middle;" title="${n.online !== false ? 'Connected' : 'Offline'}"></span>${callsign}${hostTag}${meTag}${badge}
      </td>
      <td class="mn-lb-mono">${n.lapCount}</td>
      <td class="mn-lb-mono">${n.lapCount > 0 ? formatMsRace(n.totalMs)            : '—'}</td>
      <td class="mn-lb-mono">${n.lapCount > 0 ? formatMsRace(Math.round(n.avgMs)) : '—'}</td>
      <td class="mn-lb-mono${isFastPilot ? ' mn-lb-fastest' : ''}">${n.lapCount > 0 ? formatMsRace(n.fastestMs) : '—'}</td>
    </tr>`;
  });
  html += '</tbody></table>';

  // ── Console readouts ───────────────────────────────────────────
  // Scoped to the shell this render is targeting, and addressed by CLASS: the
  // master view and the client mirror are both in the document at once, so a
  // shared id would always resolve to whichever came first.
  const _rootEl = document.getElementById(containerId);
  const _shell  = _rootEl ? _rootEl.closest('.race-shell') : null;
  if (_shell) {
    const _name = (n) => n.pilotName
      || (n.isMaster ? (readOnly ? 'Race Director' : 'Host') : 'Node ' + _slotLetter(n.nodeId));
    const _set = (sel, txt) => {
      const el = _shell.querySelector(sel);
      if (el) el.textContent = txt;
    };
    const _leader    = ranked[0];
    const _fastPilot = isFinite(globalFastestMs)
      ? ranked.find(n => n.fastestMs === globalFastestMs)
      : null;

    _set('.rm-pilots', `${ranked.length} of ${allSlots.length}`);
    _set('.rm-fastest', _fastPilot
      ? `${formatMsRace(globalFastestMs)} · ${_name(_fastPilot)}`
      : '—');
    _set('.rm-leader', (_leader && _leader.lapCount > 0)
      ? `${_name(_leader)} · ${_leader.lapCount} lap${_leader.lapCount === 1 ? '' : 's'}`
      : '—');
  }

  // ── Per-pilot lap columns — always 8 slots ─────────────────────
  html += '<div class="mn-pilot-cards">';
  allSlots.forEach(n => {
    const pilotColor = '#' + ((n.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0');
    const callsign = n.pilotName || (n.isMaster ? 'Master' : 'Node ' + _slotLetter(n.nodeId));

    if (n.empty) {
      html += `<div class="mn-pilot-card mn-pilot-card-empty">
        <div class="mn-pilot-card-header mn-pilot-card-header-empty">
          <span class="mn-card-swatch mn-card-swatch-empty"></span>
          <span class="mn-card-name">Slot ${_slotLetter(n.nodeId)}</span>
        </div>
        <div class="mn-pilot-card-sub"><span class="mn-card-slot">Empty</span></div>
        <div class="mn-pilot-card-laps mn-card-empty-label">${emptySlotLabel}</div>
      </div>`;
      return;
    }

    // A registered pilot whose heartbeat went silent: keep the card populated
    // (pilot name + lap stats stay visible so the director still sees what
    // they did before disconnecting) but desaturate to gray and flag with a
    // "Disconnected" badge.  Slot stays held — the director kicks manually.
    // Host (n.isMaster) is by definition always online so it never enters
    // this branch.
    const isDisconnected = !n.isMaster && n.online === false;
    const color = isDisconnected ? '#5a5a5a' : pilotColor;

    const _isExcludedThisRace = n.excludedFromCurrentRace || _pendingExcludes.has(n.nodeId);
    // Grace window after Stop All — keep the orange "solo" badge color off while
    // the master is still waiting for the last clients to acknowledge the stop.
    // Pilots who genuinely have skipEnabled or were excluded still show solo so
    // those legitimate cases aren't masked.
    const _mnRecentlyStoppedBadge = (typeof window.__mnLastRaceStopAt === 'number')
      && (Date.now() - window.__mnLastRaceStopAt) < 5000;
    const _mnGraceSuppressSolo = _mnRecentlyStoppedBadge && !n.skipEnabled && !_isExcludedThisRace;
    const isSoloRacing = n.running && !n.isMaster &&
      (!raceRunning || _isExcludedThisRace) && !_mnGraceSuppressSolo;
    const canTap  = !readOnly && mnDevMode && raceRunning && !n.independent && !isSoloRacing && !isDisconnected;
    // The header is no longer FILLED with the pilot's colour — the colour is a
    // swatch beside the name instead.  That is what retires the luminance
    // check: there is no arbitrary user-picked background under the label any
    // more, so the text is just --text-color and reads the same on all 14
    // themes.  This was _pilotCardTextColor()'s only caller — the function is
    // now unused and can be deleted if no filled-colour surface comes back.
    const devAttr = canTap
      ? ` onclick="mnDevTriggerLap(${n.nodeId},${n.laps.length},'${callsign.replace(/'/g,"\\'")}');" title="Dev: click to simulate lap" style="cursor:pointer;"`
      : '';
    // Position comes from the same ranked array the standings table uses, so
    // the card and the table can never disagree.  Solo / excluded / skipped
    // pilots are not in it, hence the -1 guard.
    const rankIdx   = Array.isArray(ranked) ? ranked.findIndex(r => r.nodeId === n.nodeId) : -1;
    const posLabel  = rankIdx >= 0 ? `P${rankIdx + 1}` : '';
    const slotLabel = n.isMaster ? 'Host' : `Slot ${_slotLetter(n.nodeId)}`;
    const subLeft   = posLabel ? `${posLabel} &middot; ${slotLabel}` : slotLabel;
    const bestLabel = (n.laps && n.laps.length && isFinite(n.fastestMs) && n.fastestMs > 0)
      ? `<span class="mn-card-best">${formatMsRace(n.fastestMs)}</span>` : '';
    // Show Racing badge as soon as raceRunning flips (pre-arm) for non-excluded nodes, matching the master card.
    const isRacing = n.isMaster
      ? raceRunning
      : (n.running || (raceRunning && !_isExcludedThisRace && !n.skipEnabled));
    const racingBadgeColor = isSoloRacing ? 'rgba(255,140,0,0.75)' : 'rgba(0,160,0,0.5)';
    // Edit button: hidden in read-only (client viewing) mode.  The master
    // shows it on EVERY card including its own (n.isMaster) so the director
    // can quickly edit the host pilot from the Race tab — the modal hides
    // Kick + Swap for nodeId === 0 since they don't apply to the host.
    const cardEditBtn = readOnly ? '' : `<button class="mn-edit-btn mn-card-edit-btn" onclick="event.stopPropagation();mnOpenPilotModal(${n.nodeId})" title="Edit pilot" style="margin-right:5px;vertical-align:middle;"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zm17.71-10.21a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg></button>`;
    // "Me" moves to the theme accent.  It used to be near-black on the pilot's
    // own colour; on the panel that reads as no badge at all, and this is the
    // one badge a pilot is actually looking for on their own screen.
    const meBadge = (readOnly && !n.isMaster && n.nodeId === mnMyNodeId)
      ? ' <span class="mn-card-badge mn-card-badge-me">Me</span>' : '';
    const slotPrefix = n.isMaster ? '' : `${_slotLetter(n.nodeId)}: `;
    const disconnectedBadge = isDisconnected
      ? ' <span class="mn-card-badge" style="background:rgba(208,80,80,0.85);color:#fff;">Disconnected</span>'
      : '';
    // Peer lap data on a client is a cache, not a replica (§3), so a shortfall
    // is disclosed rather than repaired.  Saying "3 laps not shown" is honest;
    // rendering standings that are quietly wrong is not.
    const staleBadge = (n.lapsStale > 0)
      ? ` <span class="mn-card-badge" style="background:rgba(180,118,26,0.85);color:#fff;" title="Peer lap data is a cache and is not auto-repaired. Refresh to pull the full history from the master.">${n.lapsStale} not shown</span>`
      : '';
    // A node that never received a start acknowledgement has no clock anchor,
    // so its laps cannot be placed on the field's timeline (§8).  Its own
    // times are real; only cross-pilot comparison is unavailable.
    const unanchoredBadge = (n.unanchored && !n.isMaster)
      ? ' <span class="mn-card-badge" style="background:rgba(90,90,90,0.85);color:#fff;" title="No clock anchor — lap times are accurate, but this pilot cannot be ordered against the field.">Unsynced</span>'
      : '';
    // Two header rows: identity on top (swatch + name + edit), status beneath
    // (position, slot, badges, the pilot's own best lap).  The badges keep the
    // exact inline colours they had — they are semantic, not thematic — but
    // they now sit on the panel rather than on the pilot's colour, so the
    // white-text override in .mn-card-badge still holds.
    html += `<div class="mn-pilot-card${isDisconnected ? ' mn-pilot-card-offline' : ''}${rankIdx === 0 ? ' mn-pilot-card-lead' : ''}">`
          + `<div class="mn-pilot-card-header"${devAttr}>`
          +   `<span class="mn-card-swatch" style="background:${color};"></span>`
          +   `<span class="mn-card-name">${slotPrefix}${callsign}</span>`
          +   cardEditBtn
          + `</div>`
          + `<div class="mn-pilot-card-sub">`
          +   `<span class="mn-card-slot">${subLeft}</span>`
          +   `${n.isMaster ? ' <span class="mn-card-badge" style="background:rgba(120,120,120,0.35);">Host</span>' : ''}`
          +   `${meBadge}${disconnectedBadge}${staleBadge}${unanchoredBadge}`
          +   `${isRacing && !isDisconnected ? ` <span class="mn-card-badge" style="background:${racingBadgeColor};">Racing</span>` : ''}`
          +   `${canTap ? ' <span class="mn-card-badge" style="background:rgba(120,120,120,0.4);font-size:9px;">TAP</span>' : ''}`
          +   bestLabel
          + `</div>`
          + `<div class="mn-pilot-card-laps">`;

    // Not racing but has skip-master-start enabled
    if (!n.running && !n.isMaster && n.skipEnabled) {
      html += `<div class="mn-card-solo-label mn-card-solo-label-idle">Not racing<br>(ignoring race director)</div>`;
    }

    // Solo race in progress.
    //   • skipEnabled pilots ("Ignore Race Director" toggle on): show their
    //     lap times like any other pilot, with a small "Ignoring race director"
    //     note above so the director still knows they're outside the official
    //     race.  The leaderboard already excludes them via !n.skipEnabled, so
    //     their times are visible without polluting the ranking — exactly
    //     like a DNF entry.  The skip-enabled client's own Multi Race tab
    //     uses the same renderer, so they keep observing the director's race
    //     and seeing their own solo times in parallel.
    //   • Excluded-for-this-race or pre-start solo runners: keep the
    //     placeholder behaviour so the director sees at-a-glance which
    //     non-skip pilots are off the field.
    // Grace window: within 5 s of a Stop All click, suppress the label for
    // non-skip / non-excluded pilots — those are clients whose stop POST or
    // heartbeat hasn't round-tripped yet.  Any pilot whose running flag is
    // still true *after* the grace window genuinely is solo racing.
    const _mnRecentlyStopped = (typeof window.__mnLastRaceStopAt === 'number')
      && (Date.now() - window.__mnLastRaceStopAt) < 5000;
    const _mnSoloPending = _mnRecentlyStopped && !n.skipEnabled && !_isExcludedThisRace;
    const showSkipNote = n.running && !n.isMaster && n.skipEnabled && !_mnSoloPending;
    const showSoloPlaceholder = !showSkipNote && n.running && !n.isMaster
      && (_isExcludedThisRace || !raceRunning) && !_mnSoloPending;
    if (showSkipNote) {
      html += `<div class="mn-card-solo-label" style="padding:6px 8px;font-size:11px;color:#f0a040;">Ignoring race director</div>`;
    }
    if (showSoloPlaceholder) {
      const soloLabel = _isExcludedThisRace
        ? 'Solo race in progress<br>(ignoring for this race)'
        : 'Solo race in progress<br>(race director can override)';
      html += `<div class="mn-card-solo-label">${soloLabel}</div>`;
    } else if (n.laps.length === 0) {
      html += `<div class="mn-card-lap" style="justify-content:center;color:var(--secondary-color);padding:10px;">—</div>`;
    } else {
      let cumMs = n.allLapsMs;
      for (let i = n.laps.length - 1; i >= 0; i--) {
        const l      = n.laps[i];
        const isGate = l.lapNumber === 0;
        const isBest = !isGate && l.lapTimeMs === n.fastestMs;
        html += `<div class="mn-card-lap${isBest ? ' mn-card-lap-best' : ''}">
          <span class="mn-card-lap-num">${l.lapNumber}</span>
          <div class="mn-card-lap-times">
            <span class="mn-card-lap-time">${isGate ? '—' : formatMsRace(l.lapTimeMs)}${isBest ? ' ★' : ''}</span>
            <span class="mn-card-lap-cumul">${formatMsRace(cumMs)}</span>
          </div>
        </div>`;
        cumMs -= l.lapTimeMs;
      }
    }
    html += '</div></div>';
  });
  html += '</div>';

  // Skip the DOM swap when output hasn't actually changed.  Most 2-second polls
  // return identical data; without this guard each one nukes hover state and
  // can drop an in-flight click on the edit-pilot button.
  if (window.__mnLastRaceTabHtml[containerId] !== html) {
    container.innerHTML = html;
    window.__mnLastRaceTabHtml[containerId] = html;
  }
  if (!readOnly) mnUpdateRaceDataButtons();
}

// ── Node pilot edit modal ─────────────────────────────────────────────────

let _mnModalNodeId = null;

function mnModalUpdateChannels(selectChannelIndex) {
  const bandSel = document.getElementById('mnPilotModalBand');
  const chanSel = document.getElementById('mnPilotModalChannel');
  if (!bandSel || !chanSel) return;
  const bandIndex = parseInt(bandSel.value) || 0;
  const freqs = freqLookup[bandIndex] || [];
  chanSel.innerHTML = '';
  freqs.forEach((f, i) => {
    if (f === 0) return;
    const opt = document.createElement('option');
    opt.value = String(i + 1);  // 1-based value
    opt.textContent = String(i + 1);
    chanSel.appendChild(opt);
  });
  // Select by 0-based index if provided, else keep first
  if (selectChannelIndex !== undefined) {
    const desired = String(selectChannelIndex + 1);
    if (Array.from(chanSel.options).some(o => o.value === desired)) chanSel.value = desired;
  }
  mnModalUpdateFreq();
}

function mnModalUpdateFreq() {
  const bandSel = document.getElementById('mnPilotModalBand');
  const chanSel = document.getElementById('mnPilotModalChannel');
  const freqEl  = document.getElementById('mnPilotModalFreq');
  if (!bandSel || !chanSel || !freqEl) return;
  const bandIndex = parseInt(bandSel.value) || 0;
  const chanNum   = parseInt(chanSel.value) || 1;
  const freq = (freqLookup[bandIndex] || [])[chanNum - 1] || 0;
  freqEl.textContent = freq ? freq + ' MHz' : 'N/A';
}

// ─── Live RSSI feed for the Edit Pilot modal ────────────────────────────────
// Standalone SmoothieChart instance + 5 Hz poll loop, mirrored from the
// Calibration tab chart (data/script.js createRssiChart + addRssiPoint) but
// scoped to its own canvas/series so the two never collide.  Source data comes
// from the master proxy /api/multinode/rssi?nodeId=N which forwards to the
// client's /timer/rssi snapshot endpoint.

let _mnModalRssiChart      = null;
let _mnModalRssiSeries     = null;
let _mnModalRssiPollTimer  = null;
// nodeId 0 = the master's own card (poll local /timer/rssi), >0 = a client
// (poll via /api/multinode/rssi proxy).  Track "is the loop running?" in a
// separate flag so a valid nodeId of 0 doesn't trip a `if (!nodeId)` guard.
let _mnModalRssiActive     = false;
let _mnModalRssiNodeId     = -1;
let _mnModalRssiMin        = 255;
let _mnModalRssiMax        = 0;
// Y-axis range is established ONCE at modal open from the threshold values
// (so both lines are guaranteed visible), then expands only when trace
// samples land outside it.  It deliberately doesn't react to slider drags
// — otherwise moving one slider rescales the chart and visually shifts the
// other threshold line and the trace.
let _mnModalRssiAxisInit   = false;
let _mnModalRssiAxisMin    = 0;
let _mnModalRssiAxisMax    = 200;

// Wait until the canvas has non-zero layout width.  When the modal goes from
// display:none to display:flex the browser hasn't finished layout yet, so a
// streamTo() call in that window leaves the chart with width 0 — and it never
// repaints because SmoothieChart's resize() checks `width !== lastWidth` and
// undefined→0→0 sticks at 0 on the next frame.
async function _waitForCanvasLayout(canvas, maxMs = 1500) {
  const start = Date.now();
  while (Date.now() - start < maxMs) {
    if (canvas && canvas.offsetWidth > 0 && canvas.offsetHeight > 0) return true;
    await new Promise(r => requestAnimationFrame(() => r()));
  }
  return canvas && canvas.offsetWidth > 0;
}

async function _mnModalRssiInit() {
  if (_mnModalRssiChart) return;
  try {
    await loadScript('smoothie.js');
  } catch (e) {
    console.error('[mnRssi] smoothie.js load failed:', e);
    return;
  }
  if (typeof SmoothieChart === 'undefined' || typeof TimeSeries === 'undefined') {
    console.error('[mnRssi] SmoothieChart not on window after load — IIFE export problem?');
    return;
  }
  try {
    _mnModalRssiSeries = new TimeSeries();
    _mnModalRssiChart = new SmoothieChart({
      responsive: true,
      millisPerPixel: 50,
      interpolation: 'linear',
      scaleSmoothing: 1.0,
      // millisPerLine: 0 — same reasoning as the calibration chart above; these
      // two are the same instrument and should not disagree.
      grid: { strokeStyle: "rgba(255,255,255,0.18)", fillStyle: "#0f1722", sharpLines: true, verticalSections: 4, millisPerLine: 0, borderVisible: false },
      labels: { precision: 0, fillStyle: "rgba(255,255,255,0.85)", fontSize: 11, showIntermediateLabels: true },
      maxValue: 200,
      minValue: 50,
    });
    _mnModalRssiChart.addTimeSeries(_mnModalRssiSeries, {
      lineWidth: 1.7,
      strokeStyle: "hsl(214, 53%, 60%)",
      fillStyle: "hsla(214, 53%, 60%, 0.4)",
    });
    const canvas = document.getElementById('mnPilotModalRssiChart');
    if (!canvas) { console.error('[mnRssi] canvas element missing'); return; }
    await _waitForCanvasLayout(canvas);
    _mnModalRssiChart.streamTo(canvas, 50);
    console.log('[mnRssi] init ok — canvas', canvas.offsetWidth, '×', canvas.offsetHeight);
  } catch (e) {
    console.error('[mnRssi] init failed:', e);
  }
}

async function _mnModalRssiStart(nodeId) {
  const section = document.getElementById('mnPilotModalRssiSection');
  const canvas  = document.getElementById('mnPilotModalRssiChart');
  const hint    = document.getElementById('mnPilotModalRssiHint');

  // The race-mode block that used to live here is now in onMnModalRssiToggle —
  // it puts up a confirmation dialog when the user flips the switch ON during
  // a race so they accept the bandwidth + latency cost explicitly.  By the
  // time we get here the user has either OK'd that prompt or never saw it,
  // so we just bring the chart up unconditionally.
  if (canvas) canvas.style.display = '';
  if (hint)   hint.style.display   = 'none';

  await _mnModalRssiInit();
  _mnModalRssiNodeId   = nodeId;
  _mnModalRssiActive   = true;
  _mnModalRssiMin      = 255;
  _mnModalRssiMax      = 0;
  _mnModalRssiAxisInit = false;   // re-establish y-axis from this node's thresholds
  window.__mnRssiLoggedErr = false;
  const valEl = document.getElementById('mnPilotModalRssiVal');
  if (valEl) valEl.textContent = '—';
  if (_mnModalRssiSeries && typeof _mnModalRssiSeries.clear === 'function') {
    _mnModalRssiSeries.clear();
  }
  if (_mnModalRssiChart) _mnModalRssiChart.start();
  if (_mnModalRssiPollTimer) clearInterval(_mnModalRssiPollTimer);
  // Fire one poll immediately, then settle into the regular cadence.
  // 200 ms (5 Hz) is fast enough to catch a racing drone's brief pass over
  // the gate antenna.  To keep this from starving the client's heartbeat
  // to the master, the firmware-side RSSI proxy treats each successful
  // round-trip as proof of life (multiNode->touchNode) — so the watchdog
  // never fires while the user has the live view open.  The Calibration
  // tab's chart is unaffected — it uses SSE directly from the local
  // device, no HTTP polling.
  _mnModalRssiPoll();
  _mnModalRssiPollTimer = setInterval(_mnModalRssiPoll, 200);
}

// Toggle handler for the on/off switch next to the "Live RSSI" title.
// Off: stop polling and gray out the readout — useful when the director
// just wants to see / adjust thresholds without burning WiFi bandwidth.
// On: resume polling for whichever node the modal is currently editing.
// If a race is in progress we surface a confirm() first — every poll is
// a master→client HTTP round-trip at 5 Hz that competes with lap-event
// traffic on the same WiFi, so the director should opt in to that cost
// explicitly.  If they decline, snap the toggle back to OFF.
function onMnModalRssiToggle(checked) {
  const valEl = document.getElementById('mnPilotModalRssiVal');
  if (checked) {
    if (mnRaceRunning) {
      const ok = confirm(
        'A race is in progress.\n\n' +
        'Turning on the live RSSI view will poll this node at 5 Hz over WiFi while the race is running. ' +
        'That can introduce noticeable lag in lap-event delivery and other multi-node traffic until you turn it back off.\n\n' +
        'Enable live view anyway?'
      );
      if (!ok) {
        const t = document.getElementById('mnPilotModalRssiToggle');
        if (t) t.checked = false;
        if (valEl) valEl.textContent = '(off)';
        return;
      }
    }
    if (_mnModalNodeId !== null && _mnModalNodeId !== undefined) {
      _mnModalRssiStart(_mnModalNodeId).catch(e => console.warn('[mnRssi] resume failed:', e));
    }
  } else {
    _mnModalRssiStop();
    if (valEl) valEl.textContent = '(paused)';
  }
}

// Wipe the chart back to a blank dark canvas — used on modal open so the
// previous pilot's trace doesn't linger after stop() (SmoothieChart's last
// painted frame stays on the canvas until something repaints).  Clears
// the data series, the min/max tracking, the axis-init flag, the
// horizontalLines, and paints the canvas to its background colour.
function _mnModalRssiClearCanvas() {
  _mnModalRssiMin      = 255;
  _mnModalRssiMax      = 0;
  _mnModalRssiAxisInit = false;
  if (_mnModalRssiSeries && typeof _mnModalRssiSeries.clear === 'function') {
    _mnModalRssiSeries.clear();
  }
  if (_mnModalRssiChart) {
    _mnModalRssiChart.options.horizontalLines = [];
  }
  const canvas = document.getElementById('mnPilotModalRssiChart');
  if (canvas) {
    const ctx = canvas.getContext('2d');
    if (ctx) {
      ctx.save();
      // setTransform(1,0,0,1,0,0) undoes any dpr scale from a prior
      // SmoothieChart resize so the fill covers the full canvas.
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.fillStyle = '#0f1722';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.restore();
    }
  }
}

function _mnModalRssiStop() {
  if (_mnModalRssiPollTimer) {
    clearInterval(_mnModalRssiPollTimer);
    _mnModalRssiPollTimer = null;
  }
  if (_mnModalRssiChart) _mnModalRssiChart.stop();
  _mnModalRssiActive = false;
  _mnModalRssiNodeId = -1;
}

// Push the current Enter/Exit slider values into the chart's horizontalLines
// and adjust the y-axis range so both lines stay visible.  Called from the
// slider input handlers so dragging moves the lines immediately, and from
// the 200 ms poll so any programmatic slider change (modal open, +/- buttons)
// also flows through.
function _mnModalRssiSyncThresholds() {
  if (!_mnModalRssiChart) return;
  const enterEl = document.getElementById('mnPilotModalEnter');
  const exitEl  = document.getElementById('mnPilotModalExit');
  const enterVal = enterEl ? (parseInt(enterEl.value, 10) || 120) : 120;
  const exitVal  = exitEl  ? (parseInt(exitEl.value,  10) || 100) : 100;
  _mnModalRssiChart.options.horizontalLines = [
    { color: "hsl(8.2, 86.5%, 53.7%)", lineWidth: 1.7, value: enterVal },
    { color: "hsl(25, 85%, 55%)",       lineWidth: 1.7, value: exitVal  },
  ];
  // Auto-zoom the y-axis on every sync (slider drag OR poll tick) so the
  // chart reframes around the dragged threshold without the user needing
  // to close and reopen the modal.  Range is whichever is broader between
  // (a) the current Enter/Exit slider values padded by 15 either side,
  // and (b) the observed trace min/max padded by 10.  Both clauses are
  // included with Math.min/max so a threshold pushed far above the trace
  // (or vice versa) still keeps everything visible.
  //
  // The previous "lock-once-then-only-expand" behaviour kept the other
  // threshold visually pinned during slider drags but at the cost of the
  // dragged line eventually sliding off-screen — the director then had
  // to close and reopen to recalibrate.  Live zoom is the explicit ask.
  const PAD = 15;
  const haveTrace = (_mnModalRssiMin < 255 && _mnModalRssiMax > 0);
  const traceLow  = haveTrace ? _mnModalRssiMin : exitVal;
  const traceHigh = haveTrace ? _mnModalRssiMax : enterVal;
  const axisMin   = Math.max(0,   Math.min(exitVal  - PAD, traceLow  - 10));
  const axisMax   = Math.min(255, Math.max(enterVal + PAD, traceHigh + 10));
  _mnModalRssiChart.options.minValue = axisMin;
  _mnModalRssiChart.options.maxValue = axisMax;
  if (window.__mnRssiDebug) {
    console.log('[mnRssi] thresholds:', { enterVal, exitVal, axisMin, axisMax });
  }
}

async function _mnModalRssiPoll() {
  if (!_mnModalRssiActive || !_mnModalRssiChart) return;
  const valEl = document.getElementById('mnPilotModalRssiVal');
  // Master's own card (nodeId === 0) reads its local /timer/rssi directly.
  // Client cards go through the master proxy which forwards to the
  // selected client's /timer/rssi over the AP.
  const url = (_mnModalRssiNodeId === 0)
    ? '/timer/rssi'
    : `/api/multinode/rssi?nodeId=${_mnModalRssiNodeId}`;
  try {
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) {
      if (valEl) valEl.textContent = `(no data — ${r.status})`;
      if (!window.__mnRssiLoggedErr) {
        console.warn('[mnRssi] poll', r.status, url);
        window.__mnRssiLoggedErr = true;
      }
      return;
    }
    const data = await r.json();
    const rssi = parseInt(data.rssi, 10);
    if (!Number.isFinite(rssi)) return;
    if (valEl) valEl.textContent = String(rssi);
    window.__mnRssiLoggedErr = false;

    _mnModalRssiMax = Math.max(_mnModalRssiMax, rssi);
    _mnModalRssiMin = Math.min(_mnModalRssiMin, rssi);

    // sync now consults _mnModalRssiMin/Max directly, so wandering trace
    // values flow into the axis range without a separate expansion clause
    // here.  Slider drags and poll ticks both go through the same path.
    _mnModalRssiSyncThresholds();
    _mnModalRssiSeries.append(Date.now(), rssi);
  } catch (e) {
    if (valEl) valEl.textContent = '(fetch error)';
    if (!window.__mnRssiLoggedErr) {
      console.warn('[mnRssi] fetch threw on', url, e);
      window.__mnRssiLoggedErr = true;
    }
  }
}

// Populate the Move-to-Slot dropdown with letters A–G.  Each option shows
// occupant info so the director can decide whether a swap is safe.  The
// section is hidden for the master's own card (nodeId 0) — the master
// always occupies slot 0 and can't be reassigned.
function _mnPopulateMoveSlotDropdown(currentNodeId) {
  const section = document.getElementById('mnPilotModalMoveSection');
  const sel     = document.getElementById('mnPilotModalMoveSlot');
  if (!sel || !section) return;
  if (currentNodeId === 0) { section.style.display = 'none'; return; }
  section.style.display = '';
  sel.innerHTML = '';
  const MAX = 7;  // MULTINODE_MAX_NODES
  for (let i = 1; i <= MAX; i++) {
    const letter = _slotLetter(i);
    const occupant = (mnCurrentNodes || []).find(n => n.nodeId === i && !n.isMaster);
    let text;
    if (i === currentNodeId) {
      text = `${letter} (current)`;
    } else if (occupant) {
      const name = occupant.pilotName || 'occupied';
      text = `${letter} – ${name} (swap)`;
    } else {
      text = `${letter} – empty`;
    }
    const opt = document.createElement('option');
    opt.value = String(i);
    opt.textContent = text;
    if (i === currentNodeId) opt.selected = true;
    sel.appendChild(opt);
  }
}

// Lock every editable control in the Edit Pilot modal except the Kick From
// Slot button (and Close).  Used when the target pilot's heartbeat has gone
// silent — Save + Calibration + Move + Live RSSI all need the STA link to
// actually work, so disabling them prevents the director from queuing edits
// that would silently fail.  A banner at the top of the modal explains why.
function _mnSetPilotModalLocked(locked) {
  const FIELD_IDS = [
    'mnPilotModalName',
    'mnPilotModalColor',
    'mnPilotModalBand',
    'mnPilotModalChannel',
    'mnPilotModalMoveSlot',
    'mnPilotModalEnter',
    'mnPilotModalExit',
    'mnPilotModalSkip',
    'mnPilotModalRssiToggle',
    'mnPilotModalCalibrateBtn',
  ];
  for (const id of FIELD_IDS) {
    const el = document.getElementById(id);
    if (el) el.disabled = locked;
  }
  // Save button has no id — find by its onclick handler inside the modal.
  const saveBtn = document.querySelector('#mnPilotModal button[onclick="mnSavePilotModal()"]');
  if (saveBtn) saveBtn.disabled = locked;

  // Disconnect banner — create lazily on first lock so the markup stays
  // self-contained here rather than threaded through index.html.
  let banner = document.getElementById('mnPilotModalDisconnectedBanner');
  if (locked && !banner) {
    banner = document.createElement('div');
    banner.id = 'mnPilotModalDisconnectedBanner';
    banner.style.cssText = 'background:rgba(208,80,80,0.15);border:1px solid rgba(208,80,80,0.6);color:#c0392b;padding:8px 12px;border-radius:6px;font-size:13px;margin:0 0 12px;font-weight:500;';
    banner.textContent = 'This pilot is disconnected — only "Kick From Slot" is available until they reconnect.';
    const subtitle = document.getElementById('mnPilotModalSubtitle');
    if (subtitle && subtitle.parentNode) {
      subtitle.parentNode.insertBefore(banner, subtitle.nextSibling);
    }
  }
  if (banner) banner.style.display = locked ? '' : 'none';
}

function mnOpenPilotModal(nodeId) {
  _mnModalNodeId = nodeId;
  const node = mnCurrentNodes.find(n => n.nodeId === nodeId);
  const name     = node ? (node.pilotName || '') : '';
  const colorHex = node ? '#' + ((node.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0') : '#0080ff';
  const apSuffix = (node && node.apSuffix) ? node.apSuffix : '';
  // Row 1: "Edit Pilot - <name>" (or just "Edit Pilot" if name is empty)
  // Row 2: "Node X - <apSuffix>" so the director can correlate slot to SSID
  document.getElementById('mnPilotModalTitle').textContent =
    name ? `Edit Pilot - ${name}` : 'Edit Pilot';
  document.getElementById('mnPilotModalSubtitle').textContent =
    apSuffix ? `Node ${_slotLetter(nodeId)} - ${apSuffix}` : `Node ${_slotLetter(nodeId)}`;
  document.getElementById('mnPilotModalName').value  = name;
  const mnColorSelect  = document.getElementById('mnPilotModalColor');
  const mnColorPreview = document.getElementById('mnPilotModalColorPreview');
  const colorUpper = colorHex.toUpperCase();
  let matched = false;
  for (const opt of mnColorSelect.options) {
    if (opt.value.toUpperCase() === colorUpper) { opt.selected = true; matched = true; break; }
  }
  if (!matched) mnColorSelect.options[5].selected = true; // fallback Blue
  mnColorPreview.style.backgroundColor = mnColorSelect.value;
  // Band / channel
  const bandSel = document.getElementById('mnPilotModalBand');
  if (bandSel && node) {
    bandSel.value = String(node.bandIndex || 0);
    mnModalUpdateChannels(node.channelIndex || 0);
  }
  const skipEl = document.getElementById('mnPilotModalSkip');
  if (skipEl) skipEl.checked = !!(node && node.skipEnabled);
  // Enter/Exit RSSI — populate from the node's last-reported values (sent by
  // the client in registration; updated by the wizard / Save flow).
  const enterEl = document.getElementById('mnPilotModalEnter');
  const exitEl  = document.getElementById('mnPilotModalExit');
  const enterSp = document.getElementById('mnPilotModalEnterSpan');
  const exitSp  = document.getElementById('mnPilotModalExitSpan');
  const enterVal = (node && Number.isFinite(node.enterRssi) && node.enterRssi > 0) ? node.enterRssi : 120;
  const exitVal  = (node && Number.isFinite(node.exitRssi)  && node.exitRssi  > 0) ? node.exitRssi  : 100;
  if (enterEl) enterEl.value = enterVal;
  if (exitEl)  exitEl.value  = exitVal;
  if (enterSp) enterSp.textContent = enterVal;
  if (exitSp)  exitSp.textContent  = exitVal;

  // Move-to-slot dropdown.  Hidden for master's own card (nodeId 0); for
  // clients, populate with A–G showing occupant + selecting the current slot.
  _mnPopulateMoveSlotDropdown(nodeId);

  // Kick section — also hidden for master's own card.  Move + Kick are the
  // two operations that don't apply to the host pilot; everything else
  // (name, color, band, RSSI sliders, calibration wizard, skip flag) does.
  const kickSection = document.getElementById('mnPilotModalKickSection');
  if (kickSection) kickSection.style.display = (nodeId === 0) ? 'none' : '';

  // Disconnected pilot: lock down every field + Save + Calibration + Move +
  // Live RSSI — the only useful action is Kick From Slot to free the slot
  // for someone else.  Save would silently fail anyway since the master→
  // client config push needs the STA link.  Read once at modal-open time
  // (not live) so a brief flap mid-edit doesn't disable the form under the
  // director's hands.
  _mnSetPilotModalLocked(!!(node && !node.isMaster && node.online === false));

  document.getElementById('mnPilotModal').style.display = 'flex';
  setTimeout(() => document.getElementById('mnPilotModalName').focus(), 50);

  // Live RSSI view defaults to OFF every modal open.  Each poll is a
  // master→client HTTP round-trip and most edits don't need the trace, so
  // we don't burn WiFi by default — the director flips the toggle when
  // they actually want to see the signal.
  const rssiToggle = document.getElementById('mnPilotModalRssiToggle');
  if (rssiToggle) rssiToggle.checked = false;
  const valEl = document.getElementById('mnPilotModalRssiVal');
  if (valEl) valEl.textContent = '(off)';
  // Wipe any leftover trace from the previous pilot — the canvas keeps the
  // last painted frame after stop() until something repaints it.
  _mnModalRssiClearCanvas();
}

// Slider helpers for the modal — mirror updateEnterRssi / updateExitRssi /
// stepRssi from the Calibration page so the +/- buttons feel identical.  Only
// mutate the modal's local state here; persistence happens in mnSavePilotModal.
function mnModalUpdateEnterRssi(value) {
  const v       = Math.max(20, Math.min(255, parseInt(value, 10) || 0));
  const exitEl  = document.getElementById('mnPilotModalExit');
  const exitV   = exitEl ? parseInt(exitEl.value, 10) || 0 : 0;
  const clamped = Math.max(v, exitV + 1);  // enter must be > exit
  const enterEl = document.getElementById('mnPilotModalEnter');
  const span    = document.getElementById('mnPilotModalEnterSpan');
  if (enterEl) enterEl.value = clamped;
  if (span)    span.textContent = clamped;
  // Immediate chart-line update so the red threshold tracks the drag without
  // waiting for the next 200 ms poll tick.
  _mnModalRssiSyncThresholds();
}

function mnModalUpdateExitRssi(value) {
  const v       = Math.max(20, Math.min(255, parseInt(value, 10) || 0));
  const enterEl = document.getElementById('mnPilotModalEnter');
  const enterV  = enterEl ? parseInt(enterEl.value, 10) || 0 : 0;
  const clamped = Math.min(v, enterV - 1);  // exit must be < enter
  const exitEl  = document.getElementById('mnPilotModalExit');
  const span    = document.getElementById('mnPilotModalExitSpan');
  if (exitEl) exitEl.value = clamped;
  if (span)   span.textContent = clamped;
  // Same as enter: immediate chart-line update on drag.
  _mnModalRssiSyncThresholds();
}

function mnModalStepRssi(which, delta) {
  if (which === 'enter') {
    const el = document.getElementById('mnPilotModalEnter');
    if (!el) return;
    mnModalUpdateEnterRssi((parseInt(el.value, 10) || 0) + delta);
  } else {
    const el = document.getElementById('mnPilotModalExit');
    if (!el) return;
    mnModalUpdateExitRssi((parseInt(el.value, 10) || 0) + delta);
  }
}

// Launch the existing calibration wizard against the currently-edited client.
// Confirms with the director, blocks if a race is active, sets
// wizardTargetNodeId, closes the modal, then opens the wizard.  The wizard's
// own UI takes over and routes every fetch through the master's proxy.
function mnStartCalibrationWizardForModal() {
  console.log('[mnCalWizard] click — _mnModalNodeId =', _mnModalNodeId);
  try {
    // Explicit null/undefined check — nodeId 0 (master's own card) is a
    // valid target and routes through _wizardPath's local-mode branch
    // (wizardTargetNodeId > 0 ? proxy : local).
    if (_mnModalNodeId === null || _mnModalNodeId === undefined) { alert('No pilot selected.'); return; }
    const nodeId = _mnModalNodeId;
    const node = (mnCurrentNodes || []).find(n => n.nodeId === nodeId);
    const pilotLabel = (node && node.pilotName) ? node.pilotName : ('Node ' + _slotLetter(nodeId));

    // Local block — server also enforces, but check here for a friendlier message.
    if (mnRaceRunning) {
      alert('Cannot run calibration during an active race. Stop the race first.');
      return;
    }
    if (node && node.running) {
      alert(`${pilotLabel} is currently running their own race — cannot start calibration.`);
      return;
    }

    const ok = confirm(
      `Run the calibration wizard for ${pilotLabel}?\n\n` +
      `${pilotLabel} will need to fly past the gate while you mark each pass. ` +
      `This will overwrite the current Enter/Exit RSSI on that node.\n\n` +
      `It is highly recommended that VTX power be set to the same fixed number for calibration and racing.`
    );
    if (!ok) return;

    // Hand off to the existing wizard.  Close the modal first so the wizard
    // modal isn't stacked on top of it.
    mnClosePilotModal();
    wizardTargetNodeId = nodeId;
    // Suffix every wizard page header with the pilot name so the director
    // always knows which client they're calibrating.
    document.querySelectorAll('.wizardPilotSuffix').forEach(el => {
      el.textContent = ' (' + pilotLabel + ')';
    });
    console.log('[mnCalWizard] handing off to startCalibrationWizard for nodeId', nodeId);
    if (typeof startCalibrationWizard !== 'function') {
      alert('Wizard code not loaded — refresh the page.');
      return;
    }
    startCalibrationWizard();
  } catch (e) {
    console.error('[mnCalWizard] failed:', e);
    alert('Could not start the wizard: ' + (e && e.message ? e.message : e));
  }
}

function mnClosePilotModal() {
  document.getElementById('mnPilotModal').style.display = 'none';
  _mnModalNodeId = null;
  // Stop the live RSSI poll + chart so we don't keep hitting the master proxy
  // and the chart's animation loop stops chewing CPU.
  _mnModalRssiStop();
  // While the modal was open, mnRenderRaceTab was deferred to avoid swapping
  // the buttons out from under the user's mouse.  Flush a fresh render now so
  // any data that arrived during that window catches up immediately.
  if (window.__mnRaceTabRenderPending) {
    window.__mnRaceTabRenderPending = false;
    if (Array.isArray(mnCurrentNodes)) mnRenderRaceTab(mnCurrentNodes);
  }
}

function mnClosePilotModalBackdrop(evt) {
  if (evt.target === document.getElementById('mnPilotModal')) mnClosePilotModal();
}

async function mnSavePilotModal() {
  // Explicit null check — nodeId 0 (master's own card) is a valid Save target.
  if (_mnModalNodeId === null || _mnModalNodeId === undefined) return;
  const nodeId     = _mnModalNodeId;
  const name       = document.getElementById('mnPilotModalName').value.trim();
  const colorHex     = document.getElementById('mnPilotModalColor').value;
  // parseInt('000000', 16) === 0, and `0 || 0x0080FF` evaluates to 0x0080FF —
  // which silently rewrote a user's Black selection back to Blue.  Check via
  // Number.isFinite so 0 (Black) survives.
  const _parsedColor = parseInt(colorHex.replace('#', ''), 16);
  const pilotColor   = Number.isFinite(_parsedColor) ? _parsedColor : 0x0080FF;
  const bandSel    = document.getElementById('mnPilotModalBand');
  const chanSel    = document.getElementById('mnPilotModalChannel');
  const bandIndex      = bandSel  ? parseInt(bandSel.value)  : 0;
  const chanIndex      = chanSel  ? parseInt(chanSel.value) - 1 : 0;  // value is 1-based, store 0-based
  const freq           = (freqLookup[bandIndex] || [])[chanIndex] || 0;
  const skipEl         = document.getElementById('mnPilotModalSkip');
  const skipMasterStart = skipEl ? (skipEl.checked ? 1 : 0) : undefined;
  // Move-to-slot selection — only acted on if it differs from the current
  // slot AND this isn't the master's own card (master can't move).
  const moveEl     = document.getElementById('mnPilotModalMoveSlot');
  const targetSlot = (moveEl && nodeId !== 0) ? (parseInt(moveEl.value, 10) || nodeId) : nodeId;
  const movePending = (targetSlot !== nodeId);
  // Don't auto-close on Save anymore — director typically wants to tweak,
  // save, watch the Live RSSI react, tweak again, etc.  The modal closes
  // explicitly via the Close button.  Exception: a pending slot Move/Swap
  // changes _mnModalNodeId out from under the form, so we close at the
  // end of that path to avoid leaving the user editing a now-mismatched
  // pilot card.
  try {
    const body = { nodeId, pilotName: name, pilotColor, band: bandIndex, chan: chanIndex, freq };
    if (skipMasterStart !== undefined) body.skipMasterStart = skipMasterStart;
    // Enter/Exit RSSI sliders — push their current values so the client picks
    // them up.  The master's editPilot proxy treats these as optional fields.
    const enterEl = document.getElementById('mnPilotModalEnter');
    const exitEl  = document.getElementById('mnPilotModalExit');
    if (enterEl) body.enterRssi = parseInt(enterEl.value, 10) || 0;
    if (exitEl)  body.exitRssi  = parseInt(exitEl.value,  10) || 0;
    const r = await fetch('/api/multinode/editPilot', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (!r.ok) alert('Update failed \u2014 is the client device reachable?');
    // Master self-edit: also sync the Calibration tab's local inputs +
    // staged-config tracking so switching to that tab shows the new
    // values and doesn't surface a phantom "dirty" indicator.  The
    // firmware /api/multinode/editPilot self-edit branch fires
    // pushMultiNodeState which refreshes mnCurrentNodes via SSE, but
    // the Calibration tab's <input id="enter"> / <input id="exit">
    // and the global enterRssi / exitRssi values are local JS state
    // that the SSE handler doesn't touch.
    if (r.ok && nodeId === 0) {
      if (body.enterRssi !== undefined) {
        enterRssi = body.enterRssi;
        if (enterRssiInput) enterRssiInput.value = body.enterRssi;
        if (enterRssiSpan)  enterRssiSpan.textContent = body.enterRssi;
        if (baselineConfig && typeof baselineConfig === 'object') baselineConfig.enterRssi = body.enterRssi;
        delete stagedConfig.enterRssi;
      }
      if (body.exitRssi !== undefined) {
        exitRssi = body.exitRssi;
        if (exitRssiInput) exitRssiInput.value = body.exitRssi;
        if (exitRssiSpan)  exitRssiSpan.textContent = body.exitRssi;
        if (baselineConfig && typeof baselineConfig === 'object') baselineConfig.exitRssi = body.exitRssi;
        delete stagedConfig.exitRssi;
      }
      if (Object.keys(stagedConfig).length === 0) stagedDirty = false;
      if (typeof updateSaveButton === 'function') updateSaveButton();
      _markRssiSaved(
        body.enterRssi !== undefined ? body.enterRssi : enterRssi,
        body.exitRssi  !== undefined ? body.exitRssi  : exitRssi
      );
    }
    // Slot move runs AFTER editPilot so the name / color / band edits land
    // against the source slot first; the move then carries the same node
    // (and any occupant of the target slot) to their new ids.
    if (movePending) {
      const mr = await fetch(`/api/multinode/move?from=${nodeId}&to=${targetSlot}`, { method: 'POST' });
      if (!mr.ok) alert('Slot move failed \u2014 client may be offline.');
      // After a swap the modal's _mnModalNodeId refers to a different pilot
      // than what's visually on screen \u2014 close so the next interaction
      // starts from the refreshed Race tab.
      mnClosePilotModal();
    } else {
      // Stayed-on-card path: briefly flash the Save button so the user
      // knows the write happened (no auto-close to signal success).
      const saveBtn = document.querySelector('#mnPilotModal button[onclick="mnSavePilotModal()"]');
      if (saveBtn) {
        const orig = saveBtn.textContent;
        saveBtn.textContent = 'Saved \u2713';
        saveBtn.disabled = true;
        setTimeout(() => { saveBtn.textContent = orig; saveBtn.disabled = false; }, 900);
      }
    }
    mnRefreshNodes();
  } catch (e) { console.error('editPilot failed', e); alert('Update failed'); }
}

async function mnRemoveFromModal() {
  if (!_mnModalNodeId) return;
  const nodeId   = _mnModalNodeId;
  const node     = mnCurrentNodes.find(n => n.nodeId === nodeId);
  const slot = _slotLetter(nodeId);
  const callsign = node ? (node.pilotName || 'Node ' + slot) : 'Node ' + slot;
  if (!confirm(`Kick "${callsign}" from slot ${slot}?\n\nTheir device will pause reconnection attempts for 1 minute. After that they can rejoin and will be assigned the next available slot.`)) return;
  mnClosePilotModal();
  try {
    await fetch('/api/multinode/kickNode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ nodeId })
    });
    mnRefreshNodes();
  } catch (e) { console.error('kickNode failed', e); }
}

async function mnRemoveNode(nodeId, callsign) {
  if (!confirm(`Remove "${callsign}" from slot ${_slotLetter(nodeId)}?\n\nThe pilot can reconnect and will be assigned the next available slot.`)) return;
  try {
    await fetch('/api/multinode/removeNode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ nodeId })
    });
    mnRefreshNodes();
  } catch (e) { console.error('removeNode failed', e); }
}

// ── Pilot Backup / Restore ────────────────────────────────────────────────
//
// Saves per-MAC pilot settings to a JSON file the director can re-load on the
// same physical hardware later.  Calibration RSSI is hardware-specific
// (different antenna, receiver, board) so MAC — not slot id — is the
// matching key.  A pilot saved from "slot A" on unit X will only restore to
// unit X regardless of which slot X currently occupies.
//
// File format v1:
//   { format: 'fpvraceone-pilot-backup', version: 1,
//     savedAt: ISO timestamp, masterMac: '…', pilots: […] }
// Each pilot entry: { mac, isMaster, slot, pilotName, pilotColor,
//                     band, chan, freq, enterRssi, exitRssi, skipMasterStart }
//
// Restore preview is keyed on MAC: matched-online → selectable, matched-offline
// or never-seen → listed but disabled / grayed out.

window.__mnRestoreState = null;   // populated by mnRestoreOpenFile, read by mnRestoreApply

function _mnNormalizeMac(m) {
  return (m || '').toString().trim().toUpperCase();
}

function mnBackupPilots() {
  const nodes = Array.isArray(mnCurrentNodes) ? mnCurrentNodes : [];
  if (nodes.length === 0) {
    alert('No node data to back up yet — wait for the Race tab to populate, then try again.');
    return;
  }
  const pilots = [];
  for (const n of nodes) {
    const mac = _mnNormalizeMac(n.mac);
    if (!mac) continue;  // entry with no MAC cannot be restored deterministically
    pilots.push({
      mac,
      isMaster: !!n.isMaster,
      slot: n.nodeId,
      pilotName: n.pilotName || '',
      pilotColor: (typeof n.pilotColor === 'number') ? n.pilotColor : 0x0080FF,
      band: (typeof n.bandIndex === 'number') ? n.bandIndex : 0,
      chan: (typeof n.channelIndex === 'number') ? n.channelIndex : 0,
      freq: (typeof n.frequency === 'number') ? n.frequency : 0,
      enterRssi: (typeof n.enterRssi === 'number') ? n.enterRssi : 0,
      exitRssi:  (typeof n.exitRssi  === 'number') ? n.exitRssi  : 0,
      skipMasterStart: n.skipEnabled ? 1 : 0,
    });
  }
  if (pilots.length === 0) {
    alert('No pilots with known MAC addresses to back up. Recruit your client units first, then try again.');
    return;
  }
  const masterEntry = pilots.find(p => p.isMaster);
  const payload = {
    format: 'fpvraceone-pilot-backup',
    version: 1,
    savedAt: new Date().toISOString(),
    masterMac: masterEntry ? masterEntry.mac : '',
    pilots,
  };
  // Filename: fpvraceone-pilots-YYYY-MM-DD-HHMM.json (local time, sort-friendly).
  const d  = new Date();
  const pad = n => String(n).padStart(2, '0');
  const stamp = `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}`;
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement('a');
  a.href     = url;
  a.download = `fpvraceone-pilots-${stamp}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function mnRestoreOpenFile(input) {
  const file = input && input.files && input.files[0];
  if (!file) return;
  // Block restore while a race is running — mid-race config changes are
  // disruptive and the editPilot proxy doesn't guard band/channel changes
  // against an active timer.
  if (typeof mnRaceRunning !== 'undefined' && mnRaceRunning) {
    alert('Stop the current race before restoring pilot settings.');
    return;
  }
  const reader = new FileReader();
  reader.onload = (e) => {
    let parsed;
    try { parsed = JSON.parse(e.target.result); }
    catch (err) { alert('Could not parse the file as JSON.'); return; }
    if (!parsed || parsed.format !== 'fpvraceone-pilot-backup' || !Array.isArray(parsed.pilots)) {
      alert('Not a valid FPVRaceOne pilot backup file.');
      return;
    }
    _mnRenderRestorePreview(parsed);
  };
  reader.onerror = () => alert('Failed to read the selected file.');
  reader.readAsText(file);
}

function _mnRenderRestorePreview(payload) {
  const list = document.getElementById('mnRestoreList');
  const subtitle = document.getElementById('mnRestoreModalSubtitle');
  const summary = document.getElementById('mnRestoreSummary');
  const applyBtn = document.getElementById('mnRestoreApplyBtn');
  if (!list || !subtitle || !summary || !applyBtn) return;

  // Build a MAC → current node map.
  const macToNode = new Map();
  for (const n of (Array.isArray(mnCurrentNodes) ? mnCurrentNodes : [])) {
    const mac = _mnNormalizeMac(n.mac);
    if (mac) macToNode.set(mac, n);
  }

  const rows = payload.pilots.map((p, i) => {
    const mac     = _mnNormalizeMac(p.mac);
    const current = macToNode.get(mac);
    const matched = !!current;
    const online  = matched && current.online !== false;
    const slotNow = matched ? _slotLetter(current.nodeId) : '';
    const slotSaved = (typeof p.slot === 'number' && p.slot > 0) ? _slotLetter(p.slot) : (p.isMaster ? 'Master' : '');
    let statusText;
    let statusColor;
    if (!matched)       { statusText = 'Node not detected';            statusColor = '#888'; }
    else if (!online)   { statusText = 'Matched, currently offline';  statusColor = '#c0392b'; }
    else if (current.isMaster) { statusText = 'This master';           statusColor = '#2e8b57'; }
    else                { statusText = `Online — now in slot ${slotNow}`; statusColor = '#2e8b57'; }

    const canRestore = matched && online;
    const colorHex = '#' + (((p.pilotColor || 0) >>> 0) & 0xFFFFFF).toString(16).padStart(6, '0');
    const macTail = mac ? mac.slice(-8) : '—';
    const dim     = canRestore ? '1' : '0.45';
    return `
      <label style="display:flex; align-items:center; gap:10px; padding:8px 10px; border-bottom:1px solid var(--border-color, rgba(128,128,128,0.18)); opacity:${dim}; cursor:${canRestore ? 'pointer' : 'not-allowed'};">
        <input type="checkbox" data-idx="${i}" ${canRestore ? 'checked' : 'disabled'} style="flex:0 0 auto;">
        <span style="display:inline-block; width:14px; height:14px; border-radius:3px; background:${colorHex}; border:1px solid rgba(0,0,0,0.25); flex:0 0 auto;"></span>
        <div style="flex:1 1 auto; min-width:0;">
          <div style="font-weight:600;">${_escapeHtml(p.pilotName || '(unnamed)')}${slotSaved ? ` <span style="font-size:12px; color:var(--secondary-color); font-weight:400;">saved as ${slotSaved}</span>` : ''}</div>
          <div style="font-size:11px; color:var(--secondary-color);">MAC …${macTail} · ${p.freq || '?'} MHz · enter ${p.enterRssi || 0} / exit ${p.exitRssi || 0}</div>
          <div style="font-size:11px; color:${statusColor};">${statusText}</div>
        </div>
      </label>
    `;
  }).join('');
  list.innerHTML = rows || '<p style="padding:16px; text-align:center; color:var(--secondary-color);">No pilot entries in this file.</p>';

  // Header counts.
  const total   = payload.pilots.length;
  const matched = payload.pilots.filter(p => macToNode.has(_mnNormalizeMac(p.mac))).length;
  const online  = payload.pilots.filter(p => {
    const n = macToNode.get(_mnNormalizeMac(p.mac));
    return n && n.online !== false;
  }).length;
  const stamp = payload.savedAt ? new Date(payload.savedAt).toLocaleString() : 'unknown date';
  subtitle.textContent = `Saved ${stamp} — ${total} pilot${total !== 1 ? 's' : ''}, ${matched} matched, ${online} ready to restore`;
  summary.textContent = (online < total)
    ? 'Greyed entries can’t be restored right now (unit offline or not detected).'
    : '';

  applyBtn.disabled = (online === 0);
  // Stash for the apply step — we re-resolve current slot per-entry at apply
  // time in case the user recruited / kicked anyone while the modal was open.
  window.__mnRestoreState = { payload };

  document.getElementById('mnRestoreModal').style.display = 'flex';
}

function mnCloseRestoreModal() {
  const m = document.getElementById('mnRestoreModal');
  if (m) m.style.display = 'none';
  window.__mnRestoreState = null;
}

async function mnRestoreApply() {
  const state = window.__mnRestoreState;
  if (!state || !state.payload) { mnCloseRestoreModal(); return; }
  const checks = document.querySelectorAll('#mnRestoreList input[type="checkbox"][data-idx]');
  const selected = [];
  checks.forEach(c => {
    if (c.checked && !c.disabled) {
      const idx = parseInt(c.getAttribute('data-idx'), 10);
      if (Number.isFinite(idx)) selected.push(state.payload.pilots[idx]);
    }
  });
  if (selected.length === 0) { mnCloseRestoreModal(); return; }

  // Re-resolve MAC → current nodeId at apply time so kicks/recruits while
  // the modal was open don't push settings to the wrong slot.
  const macToNode = new Map();
  for (const n of (Array.isArray(mnCurrentNodes) ? mnCurrentNodes : [])) {
    const mac = _mnNormalizeMac(n.mac);
    if (mac) macToNode.set(mac, n);
  }

  const applyBtn = document.getElementById('mnRestoreApplyBtn');
  const summary  = document.getElementById('mnRestoreSummary');
  if (applyBtn) { applyBtn.disabled = true; applyBtn.textContent = 'Restoring…'; }

  let ok = 0, fail = 0, skipped = 0;
  for (const p of selected) {
    const node = macToNode.get(_mnNormalizeMac(p.mac));
    if (!node || node.online === false) { skipped++; continue; }
    const body = {
      nodeId:         node.nodeId,
      pilotName:      p.pilotName || '',
      pilotColor:     (typeof p.pilotColor === 'number') ? p.pilotColor : 0x0080FF,
      band:           (typeof p.band === 'number') ? p.band : 0,
      chan:           (typeof p.chan === 'number') ? p.chan : 0,
      freq:           (typeof p.freq === 'number') ? p.freq : 0,
      enterRssi:      (typeof p.enterRssi === 'number') ? p.enterRssi : 0,
      exitRssi:       (typeof p.exitRssi  === 'number') ? p.exitRssi  : 0,
      skipMasterStart: p.skipMasterStart ? 1 : 0,
    };
    try {
      const r = await fetch('/api/multinode/editPilot', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (r.ok) ok++; else fail++;
    } catch (_) { fail++; }
    if (summary) summary.textContent = `Restored ${ok} / ${selected.length}…`;
  }

  if (applyBtn) { applyBtn.disabled = false; applyBtn.textContent = 'Restore Selected'; }
  mnCloseRestoreModal();
  mnRefreshNodes();

  let msg = `Restored ${ok} pilot${ok !== 1 ? 's' : ''}.`;
  if (fail)    msg += `\n${fail} failed (the unit may have gone offline).`;
  if (skipped) msg += `\n${skipped} skipped (no longer matched).`;
  if (ok)      msg += `\n\nPlease wait a few seconds while each node is reconfigured — the cards will update as the settings round-trip back from the clients.`;
  alert(msg);
}

function _escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
  ));
}

// Multi-Node tab: simple node status cards (race data is on the Race tab).
function mnRenderNodes(nodes) {

  const container = document.getElementById('mn-nodes-container');
  if (!container) return;

  if (nodes.length === 0) {
    container.innerHTML = '<p style="text-align:center; color: var(--secondary-color); padding: 32px 0;">No client nodes registered yet. Set client devices to Client mode and connect them to this AP.</p>';
    return;
  }

  let html = '<div class="mn-node-grid">';
  nodes.forEach(n => {
    const colorHex    = '#' + ((n.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0');
    const onlineCls   = n.online ? 'mn-node-online' : 'mn-node-offline';
    const callsign    = n.pilotName || 'Node ' + _slotLetter(n.nodeId);
    let runDotCls, runLabel;
    if      (n.quitEarly) { runDotCls = 'mn-run-dot dnf';     runLabel = 'DNF'; }
    else if (n.running)   { runDotCls = 'mn-run-dot running'; runLabel = 'Racing'; }
    else                  { runDotCls = 'mn-run-dot stopped'; runLabel = 'Stopped'; }

    html += `<div class="mn-node-card ${onlineCls}">
      <div class="mn-node-header">
        <span class="mn-node-dot" style="background:${colorHex}"></span>
        <strong>${callsign}</strong>
        <span class="mn-node-status-pill ${n.online ? 'mn-pill-online' : 'mn-pill-offline'}">${n.online ? 'Online' : 'Offline'}</span>
        <span style="margin-left:auto;font-size:12px;color:var(--secondary-color);">Node ${_slotLetter(n.nodeId)}</span>
        <button class="mn-edit-btn" style="margin-left:6px;" onclick="mnOpenPilotModal(${n.nodeId})" title="Edit pilot">
          <svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zm17.71-10.21a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
        </button>
      </div>
      <div class="mn-node-race-status">
        <span class="${runDotCls}"></span>
        <span class="mn-run-label">${runLabel}</span>
      </div>
      <div style="font-size:13px;">
        ${n.pilotName || 'Node ' + _slotLetter(n.nodeId)} — Laps: <strong>${n.lapCount || 0}</strong>
      </div>
    </div>`;
  });
  html += '</div>';
  container.innerHTML = html;
}

/** Fetch current mode and refresh multi-node state (Race tab status bar + polling). */
async function mnInitTab() {
  try {
    const r = await fetch('/api/mode');
    if (!r.ok) return;
    const data     = await r.json();
    const nodeMode = data.nodeMode || 0;
    mnNodeMode     = nodeMode;
    if (data.ssid) mnMyOwnSSID = data.ssid;

    if (nodeMode === 1) {
      mnStatusSSID = data.ssid || '';
      mnStartPolling();
    } else if (nodeMode === 2) {
      mnMyNodeId         = data.myNodeId        || 0;
      mnMasterRaceActive = data.masterRaceActive || false;
      mnMasterConnected  = data.masterConnected  || false;
      // Fetch master SSID from config for the status bar
      const r2 = await fetch('/config');
      if (r2.ok) {
        const cfg = await r2.json();
        mnStatusSSID = cfg.masterSSID || '';
      }
      mnStartClientPoll();
    } else {
      mnStopPolling();
      mnStopClientPoll();
    }
    mnUpdateRaceStatusBar();
  } catch (e) {
    console.warn('[MULTINODE] mnInitTab failed:', e);
  }
}

// Multi-node race timer.  Delegates so the master's clock, the client's clock,
// the Race View clock and the lap table all read identically — MM:SS:frac, and
// HH:MM:SS:frac past an hour, at the user's chosen precision.
function _mnFormatRaceTimer(ms) {
  return formatMsDisplay(ms);
}

function _mnStartTimer(offsetMs = 0) {
  mnRaceStartMs = Date.now() - offsetMs;
  // 10 ms, matching the client's single-race timer.
  //
  // This display shows centiseconds but only repainted every 100 ms, so at any
  // instant it could read up to a full centisecond-and-a-half stale — enough on
  // its own to make a correctly synced fleet look 30-50 ms out when two windows
  // are compared side by side.  A display's refresh rate should not be coarser
  // than the units it prints.
  mnRaceTimerIntervalId = setInterval(() => {
    const el = document.getElementById('mn-race-timer');
    if (el) el.textContent = _mnFormatRaceTimer(Date.now() - mnRaceStartMs);
  }, 10);
  _startRaceReanchor();
}

function _mnStopTimer() {
  _stopRaceReanchor();
  clearInterval(mnRaceTimerIntervalId);
  mnRaceTimerIntervalId = null;
}

// ===============================================================
// Race View (client-mode read-only mirror of the director's view)
// ===============================================================
// State pushed by the master via /api/multinode/directorState arrives on this
// node's SSE channel as a "directorState" event. We cache the payload, render
// it into the Race View tab, and extrapolate the race timer locally between
// pushes so it ticks smoothly.

let rvLastNodes        = [];
let rvRaceRunning      = false;
let rvPrearmActive     = false;
let rvElapsedAtPushMs  = 0;
let rvLocalReceiveMs   = 0;
let rvTimerInterval    = null;
let rvImportedNodes    = null;  // non-null while viewing an imported file; blocks live pushes from overwriting

function rvShowTabIfClient() {
  const li  = document.getElementById('nav-li-raceview');
  const tab = document.getElementById('raceview');
  const show = (mnNodeMode === 2);
  if (li)  li.style.display  = show ? '' : 'none';
  if (!show && tab && tab.style.display !== 'none') {
    // If we're somehow on the Race View tab in a non-client mode, switch away.
    tab.style.display = 'none';
    const raceTab = document.getElementById('race');
    // '' not 'block' — see the note in openTab(); .tabcontent is a flex column.
    if (raceTab) raceTab.style.display = '';
  }
  // In client mode the "Race" tab is the pilot's own solo race; the "Race View"
  // tab mirrors the multi-node race director's view.  Rename both to make that
  // distinction obvious from the nav bar.
  const raceLink     = document.getElementById('nav-link-race');
  const raceviewLink = document.getElementById('nav-link-raceview');
  if (raceLink)     raceLink.textContent     = show ? 'Single Race' : 'Race';
  if (raceviewLink) raceviewLink.textContent = show ? 'Multi Race'  : 'Race View';
}

// Race View clock.  Delegates to the shared formatter — it previously used a
// DOT before the fraction while every other clock used a colon, and had no
// hours field, so the same instant could read two different ways on two
// screens of the same app.
function rvFormatTimer(ms) {
  if (!isFinite(ms) || ms < 0) ms = 0;
  return formatMsDisplay(ms);
}

function rvUpdateTimerDisplay() {
  const el = document.getElementById('rv-race-timer');
  if (!el) return;
  // Prefer this device's own race clock.  It is exact and needs no
  // extrapolation; the pushed value is only a fallback for a pilot who is
  // ignoring the race director (skipEnabled), whose own timer never started.
  //
  // Deliberately NOT gated on rvRaceRunning.  That flag arrives from the
  // directorState push, which can be a couple of seconds behind our own start
  // — and while we waited for it the clock sat frozen at 00:00.00 and then
  // jumped.  Our own timer starting IS the authoritative signal that we are
  // racing; nothing needs to confirm it from outside.
  if (rvOwnRaceStartMs > 0) {
    el.textContent = rvFormatTimer(Math.max(0, Date.now() - rvOwnRaceStartMs));
    return;
  }
  const baseMs = rvElapsedAtPushMs;
  const extrapolateMs = rvRaceRunning ? Math.max(0, Date.now() - rvLocalReceiveMs) : 0;
  el.textContent = rvFormatTimer(baseMs + extrapolateMs);
}

function rvStartTicker() {
  if (rvTimerInterval) return;
  rvTimerInterval = setInterval(rvUpdateTimerDisplay, 100);
}

function rvStopTicker() {
  if (rvTimerInterval) { clearInterval(rvTimerInterval); rvTimerInterval = null; }
  rvUpdateTimerDisplay();
}

function rvUpdateBanner() {
  const banner = document.getElementById('rv-status-banner');
  if (!banner) return;
  if (!mnMasterConnected) {
    banner.textContent = 'Disconnected from race director';
    banner.style.background = '#c0392b';
    return;
  }
  // Our own timer running outranks a pushed prearm flag.  A payload built
  // during the countdown can still be in flight when the race begins, and
  // "Arm your quad" must never reappear over a race that is already under way.
  if (rvPrearmActive && rvOwnRaceStartMs === 0) {
    banner.textContent = 'Arm your quad';
    banner.style.background = '#e67e22';   // orange — "act now"
  } else if (rvRaceRunning || rvOwnRaceStartMs > 0) {
    banner.textContent = 'Race in progress';
    banner.style.background = '#2e7d32';
  } else {
    banner.textContent = 'Waiting for race director to start race...';
    banner.style.background = '#1565c0';
  }
}

function rvRender() {
  mnRenderRaceTab(rvLastNodes, {
    containerId: 'rv-race-container',
    readOnly:    true,
    skipMnState: true,
    // Pass master's race state — fixes Solo-race-in-progress bug.  Include prearm
    // so the Racing badge appears during the countdown, matching what the
    // master's own UI shows (where mnRaceRunning is set before _raceCountdown).
    raceRunning: rvRaceRunning || rvPrearmActive,
  });
  rvUpdateBanner();
  rvUpdateTimerDisplay();
  rvUpdateDownloadButton();
}

// ── Peer lap cache (Lap Sync Protocol §3, §6.5) ─────────────────────────────
// Tier 2 of the ownership model: peer laps here are a CACHE — best-effort and
// lossy by design.  They are fed by the bounded lapDeltas window and are never
// automatically repaired, because only the owner of a lap is obliged to heal
// it.  What we can always do is DETECT a shortfall, via the per-node digest
// carried in the same payload, and say so rather than render wrong standings.
let rvLapCache   = {};   // nodeId -> [{seq, lapTimeMs, raceElapsedMs, orderMs}]
let rvCacheEpoch = 0;    // raceId the cache belongs to

// ── Peer-history backfill (§6.5) ────────────────────────────────────────────
// lapDeltas is a LIVE stream capped at 32 entries per push, so the browser's
// cache is only as complete as the pushes it happened to be open for.  Open the
// page mid-race, or refresh it, and the cache starts empty with no way to catch
// up — the payload carries digests and the last 32 deltas, nothing more.
//
// /api/multinode/laps is the master's authoritative per-node history (built for
// client resync).  This is the browser using that same channel to close its own
// gaps, which is what makes "the browser holds peer history" survivable across a
// reload.
//
// Deliberately conservative: ONE request in flight fleet-wide and a per-node
// cooldown.  A page load with seven nodes must not fan out seven parallel GETs
// into the master's async_tcp task, which is the same thread serving the race.
let   rvBackfillBusy   = false;
const rvBackfillNextAt = {};          // nodeId -> earliest retry timestamp
const RV_BACKFILL_COOLDOWN_MS = 5000;
const RV_BACKFILL_CHUNK       = 25;

// First sequence number this cache is missing, or -1 when complete.
// Walks from the node's window floor so a gap in the MIDDLE is found, not just
// a short tail — a dropped delta leaves a hole the tail check would skip past.
function rvFirstMissingSeq(cached, oldestSeq, total) {
  let want = oldestSeq;
  for (const l of cached) {
    if (l.seq < want) continue;      // already retired or duplicate
    if (l.seq > want) break;         // hole at `want`
    want++;
  }
  return want < total ? want : -1;
}

function rvMaybeBackfill(node, cached, raceId) {
  const nodeId = node.nodeId;
  // nodeId 0 is the master's own row.  buildLapChunk() resolves the node via
  // findNode(), which only knows CLIENTS, so node=0 would 404 forever.
  if (!nodeId) return;
  const total  = Number(node.lapCount)  || 0;
  const oldest = Number(node.oldestSeq) || 0;
  if (total <= 0) return;

  const from = rvFirstMissingSeq(cached, oldest, total);
  if (from < 0) return;                       // nothing missing
  if (rvBackfillBusy) return;                 // one at a time, fleet-wide
  const now = Date.now();
  if ((rvBackfillNextAt[nodeId] || 0) > now) return;

  rvBackfillBusy = true;
  rvBackfillNextAt[nodeId] = now + RV_BACKFILL_COOLDOWN_MS;

  fetch(`/api/multinode/laps?node=${nodeId}&since=${from}&limit=${RV_BACKFILL_CHUNK}`)
    .then(r => (r.ok ? r.json() : null))
    .then(d => {
      if (!d || !Array.isArray(d.laps)) return;
      // Epoch guard: a chunk from a previous race must never merge into this
      // one's standings.  Same rule the firmware applies on its own resync.
      if (raceId && d.raceId && d.raceId !== raceId) return;
      const list = (rvLapCache[nodeId] ||= []);
      let added = 0;
      for (const l of d.laps) {
        if (list.some(x => x.seq === l.seq)) continue;
        list.push({ seq: l.seq, lapTimeMs: l.lapTimeMs,
                    raceElapsedMs: l.raceElapsedMs, orderMs: 0 });
        added++;
      }
      if (added) {
        list.sort((a, b) => a.seq - b.seq);
        // More to fetch: clear the cooldown so the next push continues paging
        // immediately instead of trickling one chunk per 5 s.
        if (d.more) rvBackfillNextAt[nodeId] = 0;
      }
    })
    .catch(() => { /* transient: the cooldown retries on a later push */ })
    .finally(() => { rvBackfillBusy = false; });
}

// Is this pilot's lack of a clock anchor an ACTUAL problem worth telling the
// director about?
//
// Only when they have laps that cannot be placed on the field's timeline.
// `anchored` is false by default — before any race, after a Clear, during
// pre-arm, and for a pilot who sat this one out — and in every one of those
// cases there is nothing to misorder and nothing to warn about.  Showing the
// badge for the default state made a healthy, freshly booted fleet look broken,
// which is worse than saying nothing: a warning that is usually wrong trains
// people to ignore it when it is finally right.
function rvIsUnanchored(n) {
  if (n.anchored !== false) return false;
  if (n.excludedFromCurrentRace) return false;
  return (Number(n.lapCount) || 0) > 0;
}

function rvMergeNodes(nodes, deltas, raceId) {
  // A new epoch invalidates everything: laps from a previous race must never
  // merge into this one's standings.
  if (raceId && raceId !== rvCacheEpoch) {
    rvLapCache   = {};
    rvCacheEpoch = raceId;
  }

  // Fold this broadcast's deltas into the cache.  Deduped by seq so a repeated
  // payload (the SSE build runs faster than the fanout drains) cannot double-count.
  for (const d of deltas) {
    if (!d || d.nodeId === undefined) continue;
    // Rule 3: a client ignores anything bearing its own nodeId.  Its own laps
    // are authoritative locally and must never be overwritten by the master's
    // aggregate copy of them.
    if (mnMyNodeId > 0 && d.nodeId === mnMyNodeId) continue;
    const list = (rvLapCache[d.nodeId] ||= []);
    if (!list.some(l => l.seq === d.seq)) {
      list.push({ seq: d.seq, lapTimeMs: d.lapTimeMs,
                  raceElapsedMs: d.raceElapsedMs, orderMs: d.orderMs });
      list.sort((a, b) => a.seq - b.seq);
    }
  }

  return nodes.map(n => {
    // Transition period: while any node on the fleet still predates the
    // protocol, the master keeps sending full lap arrays and those remain the
    // better source.  Once the gate flips they stop arriving and the cache
    // takes over. Handle both without the UI needing to know which.
    if (Array.isArray(n.laps) && n.laps.length) {
      return { ...n, lapsStale: 0, unanchored: rvIsUnanchored(n) };
    }

    const cached = (mnMyNodeId > 0 && n.nodeId === mnMyNodeId)
                 ? null                       // our own row is rendered from local data
                 : (rvLapCache[n.nodeId] || []);

    if (!cached) return { ...n, lapsStale: 0, unanchored: rvIsUnanchored(n) };

    // Digest comparison (§6.5).  lapCount is the authoritative total; anything
    // we are missing was dropped by the delta cap or arrived while we were away.
    const known  = typeof n.lapCount === 'number' ? n.lapCount : cached.length;
    const missing = Math.max(0, known - cached.length);

    // ...and now we DO repair it.  Disclosure alone was not enough: a refresh
    // mid-race left the director looking at zeros with no way back, because the
    // delta stream only carries what arrives from that moment on.  The badge
    // still shows while the gap is being closed.
    if (missing > 0) rvMaybeBackfill(n, cached, raceId);

    return {
      ...n,
      laps: cached.map(l => ({ lapNumber: l.seq, lapTimeMs: l.lapTimeMs })),
      lapsStale:  missing,
      unanchored: rvIsUnanchored(n)
    };
  });
}

function rvHandleDirectorState(payload) {
  // payload = { nodes: [...], race: { running, elapsedMs, prearmActive } }
  try {
    const race        = payload.race || {};
    const prevPrearm  = rvPrearmActive;
    const prevRunning = rvRaceRunning;
    rvRaceRunning     = !!race.running;
    rvPrearmActive    = !!race.prearmActive;
    rvElapsedAtPushMs = Number.isFinite(race.elapsedMs) ? race.elapsedMs : 0;
    rvLocalReceiveMs  = Date.now();

    // If master is starting a new race, drop the imported-view freeze so live
    // data takes over again.
    if ((rvPrearmActive && !prevPrearm) || (rvRaceRunning && !prevRunning)) {
      rvImportedNodes = null;
    }

    // While viewing an imported file, ignore the live node list — but still
    // accept race/connection state so the banner and ticker stay accurate.
    if (!rvImportedNodes) {
      rvLastNodes = rvMergeNodes(Array.isArray(payload.nodes) ? payload.nodes : [],
                                 Array.isArray(payload.lapDeltas) ? payload.lapDeltas : [],
                                 race.raceId);
    }

    if (rvRaceRunning) rvStartTicker(); else rvStopTicker();
    // Edge-trigger: prearm just turned on.
    if (rvPrearmActive && !prevPrearm) {
      // If audio toggle enabled, run the shared countdown locally so this
      // pilot hears "Arm your quad" + 5..1 + beep alongside the director.
      if (mnClientRaceAudio) {
        _raceCountdownAborted = false;
        _raceCountdown('Arm your quad').catch(() => {});
      }
      // Auto-switch to Multi Race tab if the pilot is sitting on Single Race
      // and isn't currently running a solo race.  startRaceButton.disabled is
      // the existing "race is running" signal (set by startRace, cleared by
      // stopRace) — when it's enabled, the pilot is idle and benefits from
      // seeing the director's countdown view instead of their own idle one.
      const singleRaceTab = document.getElementById('race');
      const startBtn      = document.getElementById('startRaceButton');
      const onSingleTab   = singleRaceTab && singleRaceTab.style.display !== 'none';
      const inSoloRace    = startBtn && startBtn.disabled;
      if (onSingleTab && !inSoloRace) {
        const raceviewLink = document.getElementById('nav-link-raceview');
        if (raceviewLink) raceviewLink.click();
      }
    }
    rvRender();
  } catch (e) {
    console.warn('[RaceView] failed to apply directorState', e);
  }
}

// ── Client Multi Race download / import ─────────────────────────────────────
// Same wire format as the master's download (type:'MultiRace', nodes:[...]):
// a file written here imports cleanly on a master or another client, and a
// file written by the master imports cleanly here.

function rvUpdateDownloadButton() {
  const div = document.getElementById('rv-race-data-buttons');
  if (!div) return;
  const hasData = (rvLastNodes || []).some(n => (n.laps || []).length > 0);
  const stopped = !rvRaceRunning && !rvPrearmActive;
  // 'flex' — this is a .race-actions row; inline display:block would beat the
  // class and stack the buttons instead of laying them out in a row.
  div.style.display = (stopped && hasData) ? 'flex' : 'none';
  const rvPill = document.getElementById('rvRaceStatePill');
  if (rvPill) rvPill.hidden = stopped;
}

function downloadClientRaceData() {
  const allNodes = [];
  (rvLastNodes || []).forEach(n => {
    if (!(n.laps || []).length) return;
    allNodes.push({
      nodeId:     n.nodeId,
      isMaster:   !!n.isMaster,
      pilotName:  n.pilotName || (n.isMaster ? 'Master' : 'Node ' + _slotLetter(n.nodeId)),
      pilotColor: n.pilotColor || 0x0080FF,
      quitEarly:  !!n.quitEarly,
      laps:       (n.laps || []).map(l => ({ lapNumber: l.lapNumber, lapTimeMs: l.lapTimeMs || 0 })),
    });
  });

  if (allNodes.length === 0) { alert('No race data to download.'); return; }

  const ts   = Math.floor(Date.now() / 1000);
  const blob = new Blob(
    [JSON.stringify({ type: 'MultiRace', timestamp: ts, nodes: allNodes }, null, 2)],
    { type: 'application/json' }
  );
  const url = URL.createObjectURL(blob);
  const a   = document.createElement('a');
  a.href = url;
  a.download = `MultiRace-${ts}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

async function importClientRace(input) {
  const file = input?.files?.[0];
  if (!file) return;
  input.value = '';

  let json;
  try { json = JSON.parse(await file.text()); }
  catch (_e) { alert('Invalid JSON file — could not parse.'); return; }

  // Detect a single-pilot file accidentally pointed here
  if (Array.isArray(json?.races) || (Array.isArray(json) && json[0]?.lapTimes !== undefined)) {
    alert('This is a single-pilot race file. Import it from the Race History tab instead.');
    return;
  }
  if (json?.type !== 'MultiRace' || !Array.isArray(json?.nodes)) {
    alert('File format not recognised. Expected a MultiRace file.');
    return;
  }

  // Compute stats per node so the leaderboard renders correctly (same shape
  // mnRenderRaceTab expects from the live push).
  rvLastNodes = json.nodes.map(n => {
    const laps      = n.laps || [];
    const realLaps  = laps.filter(l => l.lapNumber > 0);
    const lapCount  = realLaps.length;
    const totalMs   = realLaps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
    const avgMs     = lapCount > 0 ? Math.round(totalMs / lapCount) : 0;
    const fastestMs = lapCount > 0 ? Math.min(...realLaps.map(l => l.lapTimeMs || Infinity)) : Infinity;
    return {
      ...n,
      online:                  true,
      running:                 false,
      independent:             false,
      skipEnabled:             n.skipEnabled || false,
      excludedFromCurrentRace: false,
      lapCount, totalMs, avgMs, fastestMs,
    };
  });

  // Freeze: live director-state pushes won't replace this view until the
  // master starts a new race (handled in rvHandleDirectorState).
  rvImportedNodes   = rvLastNodes;
  rvRaceRunning     = false;
  rvPrearmActive    = false;
  rvElapsedAtPushMs = 0;
  rvStopTicker();
  const timerEl = document.getElementById('rv-race-timer');
  if (timerEl) timerEl.textContent = rvFormatTimer(0);

  rvRender();
  alert(`Race imported: ${json.nodes.length} pilot${json.nodes.length !== 1 ? 's' : ''}.`);
}

// Shows a modal with three labelled buttons. Returns 0, 1, or 2.
function _showThreeOptionModal(message, btn1Text, btn2Text, btn3Text) {
  return new Promise(resolve => {
    const overlay = document.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.6);z-index:9999;display:flex;align-items:center;justify-content:center;';
    const box = document.createElement('div');
    box.style.cssText = 'background:var(--card-bg,#1e1e1e);color:var(--text-color,#eee);padding:24px 28px;border-radius:12px;max-width:400px;text-align:center;box-shadow:0 6px 28px rgba(0,0,0,0.6);';
    box.innerHTML = `<p style="margin:0 0 20px;font-size:0.95rem;line-height:1.5;">${message}</p>`;
    const btnStyles = ['background:#4caf50;color:#fff;', 'background:#ff9800;color:#fff;', 'background:#555;color:#eee;'];
    [btn1Text, btn2Text, btn3Text].forEach((text, i) => {
      const btn = document.createElement('button');
      btn.textContent = text;
      btn.style.cssText = `${btnStyles[i]}margin:5px;padding:9px 16px;border:none;border-radius:7px;cursor:pointer;font-size:0.9rem;`;
      btn.onclick = () => { document.body.removeChild(overlay); resolve(i); };
      box.appendChild(btn);
    });
    overlay.appendChild(box);
    document.body.appendChild(overlay);
  });
}

async function mnStartRace() {
  mnImportedNodes = null;  // leave import view so live polling resumes
  // Offer to clear existing lap data before starting
  const hasMasterLaps = lapTimes.length > 0;
  const hasClientLaps = (mnCurrentNodes || []).some(n => !n.isMaster && (n.laps || []).length > 0);
  if (hasMasterLaps || hasClientLaps) {
    // Yes / No / Cancel, not OK / Cancel.  The old two-button confirm had no
    // way to back out: "Cancel" meant "don't clear, but start anyway", so a
    // misclick committed you to a race.
    //
    // The note is there because this choice is narrower than it looks.  It
    // gates ONLY /api/multinode/clearLaps.  Every client is wiped at pre-arm
    // regardless — /timer/masterArm calls clearLapData() unconditionally so a
    // client cannot carry solo-race data into the director's race, which is
    // what the race-epoch design depends on.  Saying "No" and then finding the
    // clients empty looked like a bug; it is the pre-arm doing its job.
    const choice = await _showThreeOptionModal(
      'You have existing lap data. Clear it before starting?' +
      '<br><br><small style="opacity:0.8;">Client laps are cleared at pre-arm either way — ' +
      'this choice controls the host\'s own laps.</small>',
      'Yes', 'No', 'Cancel'
    );
    if (choice === 2) return;   // Cancel — nothing sent, no pre-arm, buttons untouched
    if (choice === 0) {
      await fetch('/api/multinode/clearLaps', { method: 'POST' }).catch(() => {});
      clearLaps();
      const timerEl = document.getElementById('mn-race-timer');
      if (timerEl) timerEl.textContent = _mnFormatRaceTimer(0);
    }
  }

  // Check for non-independent pilots solo racing — they'd be overridden by Start All
  const nonIndSolo = (mnCurrentNodes || []).filter(n => n.running && !n.isMaster && !n.independent && !mnRaceRunning);
  if (nonIndSolo.length > 0) {
    const names = nonIndSolo.map(n => n.pilotName || `Node ${_slotLetter(n.nodeId)}`).join(', ');
    const choice = await _showThreeOptionModal(
      `<strong>Start All</strong> will restart ${nonIndSolo.length} pilot(s) currently in a solo race:<br><em>${names}</em>`,
      'Start All',
      'Start All (Ignore Solo Racers)',
      'Cancel'
    );
    if (choice === 2) return;  // Cancel — leave buttons in their current state
    if (choice === 1) {
      // Ignore solo racers — pass excluded node IDs to the broadcast
      window._mnExcludeNodes = nonIndSolo.map(n => n.nodeId);
    } else {
      window._mnExcludeNodes = [];
    }
  } else {
    window._mnExcludeNodes = [];
  }

  // Disable button immediately and lock out double-presses
  ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) { b.disabled = true; b.classList.add('active'); } });

  // Signal clients to flash their Start button during countdown (excluding solo racers being ignored)
  try {
    const prearmExclude = (window._mnExcludeNodes && window._mnExcludeNodes.length > 0)
      ? '?exclude=' + window._mnExcludeNodes.join(',')
      : '';
    await fetch('/api/multinode/race/prearm' + prearmExclude, { method: 'POST' });
  } catch (_) {}

  // iOS/Safari: create + resume + prime the AudioContext INSIDE the user
  // gesture so the start beep 5 s later isn't silent (see startRace() for
  // the full rationale).  Match the solo-race prime block exactly.
  try {
    if (typeof AudioContext !== 'undefined') {
      if (!beepAudioContext) beepAudioContext = new AudioContext();
      if (beepAudioContext.state === 'suspended') await beepAudioContext.resume();
      if (beepAudioContext.state === 'running') {
        const silentOsc  = beepAudioContext.createOscillator();
        const silentGain = beepAudioContext.createGain();
        silentGain.gain.value = 0;
        silentOsc.connect(silentGain).connect(beepAudioContext.destination);
        silentOsc.start();
        silentOsc.stop(beepAudioContext.currentTime + 0.05);
      }
    }
  } catch (_) {}

  // Enable stop buttons before countdown so the director can cancel
  ['mnStopRaceBtn', 'mnStopRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = false; });

  // Set race-running flag before the countdown so heartbeats arriving during
  // the countdown don't trigger the "solo race in progress" label.
  mnRaceRunning = true;
  _raceCountdownAborted = false;

  // Countdown announcement — identical sequence to single-pilot startRace()
  const completed = await _raceCountdown("Arm your quads");
  if (!completed) {
    // Director cancelled during countdown — restore button states
    mnRaceRunning = false;
    ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) { b.disabled = false; b.classList.remove('active'); } });
    ['mnStopRaceBtn',  'mnStopRaceBtnMain' ].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = true; });
    mnRenderRaceTab(mnCurrentNodes);
    return;
  }

  // The GO cue and the master's clock are NOT fired here any more.
  //
  // The fleet start is scheduled ~1.2 s ahead so every unit begins on one
  // instant (§8).  Starting the host's display when the POST goes out put it
  // that full margin ahead of every client it had just told to wait — measured
  // at 1.18 s of disagreement between the two browsers.  Both now hang off the
  // raceState "started" event, which the firmware emits at the real start.
  try {
    const excludeParam = (window._mnExcludeNodes && window._mnExcludeNodes.length > 0)
      ? '?exclude=' + window._mnExcludeNodes.join(',')
      : '';
    await fetch('/api/multinode/race/start' + excludeParam, { method: 'POST' });
  }
  catch (e) { console.error('[MULTINODE] Start race failed:', e); }
  ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) b.classList.remove('active'); });
  ['mnStopRaceBtn',  'mnStopRaceBtnMain' ].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = false; });
  mnRenderRaceTab(mnCurrentNodes);
}

async function mnStopRace() {
  _raceCountdownAborted = true;
  try {
    await fetch('/api/multinode/race/stop', { method: 'POST' });
    mnRaceRunning = false;
    // Mark the moment of stop.  mnRenderRaceTab suppresses the
    // "Solo race in progress" label for ~5 s after this so the last couple
    // of clients (whose stop POST + heartbeat haven't round-tripped yet)
    // don't briefly flash as solo racers.  After the window expires, any
    // client that *really* is still running will resurface as solo.
    window.__mnLastRaceStopAt = Date.now();
    // Optimistically mark non-excluded clients as stopped so the solo-race label doesn't
    // flash between this render and the next heartbeat confirming running=false.
    mnCurrentNodes = mnCurrentNodes.map(n =>
      (n.isMaster || n.excludedFromCurrentRace) ? n : { ...n, running: false }
    );
    _mnStopTimer();
    ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = false; });
    ['mnStopRaceBtn',  'mnStopRaceBtnMain' ].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = true;  });
    mnRenderRaceTab(mnCurrentNodes);
    mnUpdateRaceDataButtons();
  } catch (e) { console.error('[MULTINODE] Stop race failed:', e); }
}

async function mnClearRace() {
  if (!confirm('Clear all race data for all pilots?')) return;
  mnImportedNodes = null;  // leave import view so live polling resumes
  try {
    await fetch('/api/multinode/clearLaps', { method: 'POST' });
    mnRaceRunning = false;
    window._mnExcludeNodes = [];  // clear pending excludes so previously-ignored pilots reappear
    _mnStopTimer();
    const timerEl = document.getElementById('mn-race-timer');
    if (timerEl) timerEl.textContent = _mnFormatRaceTimer(0);
    ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = false; });
    ['mnStopRaceBtn',  'mnStopRaceBtnMain' ].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = true;  });
    clearLaps();           // clear master's own local laps
    await mnRefreshNodes(); // re-render with empty node data from server (excludedFromCurrentRace now false)
    mnUpdateRaceDataButtons();
  } catch (e) { console.error('[MULTINODE] Clear race failed:', e); }
}

// ── Master race download / import ────────────────────────────────────────────

function mnUpdateRaceDataButtons() {
  const div = document.getElementById('mn-race-data-buttons');
  if (!div) return;
  const hasData = (mnCurrentNodes || []).some(n => (n.laps || []).length > 0)
               || (window.lapTimes || []).length > 0;
  // 'flex' — see the note in rvUpdateRaceDataButtons(); .race-actions is a row.
  div.style.display = (!mnRaceRunning && hasData) ? 'flex' : 'none';
  const mnPill = document.getElementById('mnRaceStatePill');
  if (mnPill) mnPill.hidden = !mnRaceRunning;
}

function downloadMnRaceData() {
  const master = _mnMasterEntry();
  const allNodes = [];

  // Master node — convert laps to consistent { lapNumber, lapTimeMs } shape
  if ((master.laps || []).length > 0) {
    allNodes.push({
      nodeId:     0,
      isMaster:   true,
      pilotName:  master.pilotName || 'Master',
      pilotColor: master.pilotColor || 0x0080FF,
      laps:       master.laps.map(l => ({ lapNumber: l.lapNumber, lapTimeMs: l.lapTimeMs || 0 })),
    });
  }

  // Client nodes
  (mnCurrentNodes || []).filter(n => !n.isMaster && (n.laps || []).length > 0).forEach(n => {
    allNodes.push({
      nodeId:     n.nodeId,
      isMaster:   false,
      pilotName:  n.pilotName || ('Node ' + _slotLetter(n.nodeId)),
      pilotColor: n.pilotColor || 0x0080FF,
      quitEarly:  n.quitEarly || false,
      laps:       (n.laps || []).map(l => ({ lapNumber: l.lapNumber, lapTimeMs: l.lapTimeMs || 0 })),
    });
  });

  if (allNodes.length === 0) {
    alert('No race data to download.');
    return;
  }

  const ts = Math.floor(Date.now() / 1000);
  const blob = new Blob([JSON.stringify({ type: 'MultiRace', timestamp: ts, nodes: allNodes }, null, 2)],
                        { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `MultiRace-${ts}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

async function importMnRace(input) {
  const file = input?.files?.[0];
  if (!file) return;
  input.value = '';

  let json;
  try {
    json = JSON.parse(await file.text());
  } catch (e) {
    alert('Invalid JSON file — could not parse.');
    return;
  }

  // Detect wrong file type
  if (Array.isArray(json?.races) || (Array.isArray(json) && json[0]?.lapTimes !== undefined)) {
    alert('This is a single-pilot race file. Import it from the Race History tab instead.');
    return;
  }
  if (json?.type !== 'MultiRace' || !Array.isArray(json?.nodes)) {
    alert('File format not recognised. Expected a MultiRace file downloaded from master mode.');
    return;
  }

  const nodes = json.nodes;

  // Restore master's own laps into window.lapTimes (stored in seconds)
  const masterNode = nodes.find(n => n.isMaster || n.nodeId === 0);
  window.lapTimes = masterNode ? (masterNode.laps || []).map(l => (l.lapTimeMs || 0) / 1000) : [];

  // Build mnCurrentNodes with computed stats so the leaderboard renders correctly
  mnCurrentNodes = nodes.map(n => {
    const laps     = n.laps || [];
    const realLaps = laps.filter(l => l.lapNumber > 0);
    const lapCount = realLaps.length;
    const totalMs  = realLaps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
    const avgMs    = lapCount > 0 ? Math.round(totalMs / lapCount) : 0;
    const fastestMs = lapCount > 0 ? Math.min(...realLaps.map(l => l.lapTimeMs || Infinity)) : Infinity;
    return {
      ...n,
      online:                  true,
      running:                 false,
      independent:             false,
      skipEnabled:             n.skipEnabled || false,
      excludedFromCurrentRace: false,
      lapCount, totalMs, avgMs, fastestMs,
    };
  });

  mnRaceRunning = false;
  mnImportedNodes = mnCurrentNodes;  // freeze race tab against live polling
  _mnStopTimer();
  const timerEl = document.getElementById('mn-race-timer');
  if (timerEl) timerEl.textContent = _mnFormatRaceTimer(0);

  mnRenderRaceTab(mnCurrentNodes);
  mnUpdateRaceDataButtons();
  alert(`Race imported: ${nodes.length} pilot${nodes.length !== 1 ? 's' : ''}.`);
  document.getElementById('nav-link-race').click();
}

// Stop polling whenever the user navigates away from the Race tab
document.addEventListener('click', (e) => {
  if (e.target.closest('.tablinks') && !e.target.closest('#nav-link-race')) {
    mnStopPolling();
  }
});

// Also listen for multiNodeLap SSE events on master (real-time node lap updates)
if (typeof window._mnSSEListener === 'undefined') {
  window._mnSSEListener = true;
  document.addEventListener('mnNodeLapReceived', () => {
    if (mnPollingInterval) mnRefreshNodes();
  });
}

// ════════════════════════════════════════════════════════════════════

document.addEventListener('DOMContentLoaded', () => {
  // Ensure only the Race tab is visible on first load (prevents panels stacking)
  const tabIds = ['race', 'history', 'calib', 'ota', 'config'];

  tabIds.forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    el.style.display = (id === 'race') ? '' : 'none';
  });

  // Ensure nav "active" state is correct on first load
  document.querySelectorAll('.nav-links .tablinks').forEach(a => a.classList.remove('active'));
  const raceLink = document.getElementById('nav-link-race');
  if (raceLink) raceLink.classList.add('active');

  // Detect node mode and show the correct Race tab view immediately.
  // This fires early; the config-load path below is the primary trigger once config is ready.
  // Dev Mode is now a firmware setting; loaded from /config in openSettingsModal.
  // Keep localStorage fallback for first load before settings are opened.
  const _storedDevMode = localStorage.getItem('mnDevMode');
  if (_storedDevMode === '1') {
    mnDevMode = true;
    const _dt = document.getElementById('devModeToggle');
    const _dl = document.getElementById('devModeLabel');
    if (_dt) _dt.checked = true;
    if (_dl) _dl.textContent = 'On';
    const _pnd = document.getElementById('pilotNameDisplay');
    if (_pnd) _pnd.style.cursor = 'pointer';
    // Both Dev-Mode-gated surfaces, applied here too.  The /config load below
    // is the authority and calls these as well — but if that fetch fails, this
    // path would otherwise leave Dev Mode "on" with both surfaces still hidden.
    applyAddLapButtonUI();
    applySystemMonitorUI();
  }

  (async () => {
    try {
      setSplashStatus('Connecting to timer\u2026');
      const r = await fetchWithRetry('/api/mode', {}, 3, 1500);
      const data = await r.json();
      mnNodeMode        = data.nodeMode       || 0;
      if (typeof updateApplyMultiNodeButtonState === 'function') updateApplyMultiNodeButtonState();
      mnMyNodeId        = data.myNodeId       || 0;
      mnMasterConnected = data.masterConnected || false;
      if (data.nodeMode !== 2) mnStatusSSID = data.ssid || '';
      if (data.ssid) mnMyOwnSSID = data.ssid;
      if (typeof audioAnnouncer !== 'undefined') audioAnnouncer.sdAvailable = !!data.sdAvailable;
      if (data.devMode !== undefined) {
        mnDevMode = !!data.devMode;
        const _dt = document.getElementById('devModeToggle');
        const _dl = document.getElementById('devModeLabel');
        const _pnd = document.getElementById('pilotNameDisplay');
        if (_dt) _dt.checked = mnDevMode;
        if (_dl) _dl.textContent = mnDevMode ? 'On' : 'Off';
        if (_pnd) _pnd.style.cursor = mnDevMode ? 'pointer' : 'default';
        localStorage.setItem('mnDevMode', mnDevMode ? '1' : '0');
      }
      // Restore race state before rendering
      if (data.timerRunning) {
        if (data.nodeMode === 1) {
          mnRaceRunning = true;
          if (!mnRaceTimerIntervalId) _mnStartTimer(data.raceElapsedMs || 0);
        } else {
          startRaceDisplayOnly(data.raceElapsedMs || 0);
        }
      }
      // Always restore lap data regardless of run state — laps persist after race stop.
      if (data.nodeMode === 1) {
        // Master: pre-fetch all node+lap data before first render.
        try { await mnRefreshNodes(); } catch (_) {}
      } else {
        try {
          const lr = await fetchWithRetry('/api/laps/current', {}, 3, 1500);
          const ld = await lr.json();
          if (ld.laps && ld.laps.length > 0) {
            _restoreInProgressLaps(ld.laps);
            // This is the PAGE LOAD path; the copy near the top of the file is
            // the SSE-reconnect path.  Both restore laps, so both have to
            // re-anchor the current-lap clock — fixing only one left a plain
            // browser refresh still showing the whole race as the current lap.
            setLapTimerFromLastLap(ld.laps);
          }
        } catch (_) {}
      }
      onRaceTabOpen();
      _pageInitDone = true;
      if (data.nodeMode === 2) {
        if (!mnStatusSSID) {
          try {
            const r2 = await fetch('/config');
            if (r2.ok) { const cfg = await r2.json(); mnStatusSSID = cfg.masterSSID || ''; }
          } catch (_) {}
          mnUpdateRaceStatusBar();
        }
        mnStartClientPoll();
      }
      hideSplash();
    } catch (e) {
      console.warn('[Race] /api/mode fetch failed:', e);
      hideSplash();
    }
  })();

  // Close the SSE connection explicitly before page reload/navigation.
  // Windows OS buffers SSE data at the TCP layer after the tab is gone, so the
  // Close SSE as early as possible during navigation so Chrome doesn't abort
  // the connection mid-flight and log a browser-level network error.
  // beforeunload fires before Chrome tears down the network layer;
  // pagehide is a belt-and-suspenders fallback for browsers that skip beforeunload.
  const _closeSSE = () => { if (eventSource) { eventSource.close(); eventSource = null; } };
  window.addEventListener('beforeunload', _closeSSE);
  window.addEventListener('pagehide', _closeSSE);

  // Keep paused scanner overlays correct on resize/rotation
  window.addEventListener('resize', () => {
    clearTimeout(window.__rssiResizeT);
    window.__rssiResizeT = setTimeout(() => {
      rescalePausedScannerFrameToCanvas();
    }, 100);
  });

});