// Transport manager for WiFi/USB connectivity
let transportManager = null;
let currentConnectionMode = 'auto'; // 'auto', 'wifi', 'usb'
let usbConnected = false;
let eventSource = null;
let eventSourceReconnectTimer = null;
let eventSourceReconnectAttempts = 0;
const MAX_RECONNECT_ATTEMPTS = 10;
const RECONNECT_DELAY_MS = 2000;
let connectionStatusUpdateInterval = null;
let stagedConfig = {};      
let stagedDirty = false;    
let settingsLoading = false;         // true while we are populating UI from device config
let baselineConfig = {};             // last config loaded from device (for "same value" comparisons)
let scannerPaused = false;
const SCANNER_BUFFER_WHILE_PAUSED = false;

// --- Wizard loop control (prevents stale timers/fetches blocking restart) ---
let wizardRecordingTimerId = null;
let wizardAbortController = null;

const CALIBRATION_PAGE_SIZE = 500;  // number of RSSI points per page when fetching calibration data

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
var frequency = 0;
var announcerRate = 1.0;

var lapNo = -1;
var lapTimes = [];
var maxLaps = 0;

// Track data for current race
var currentTrackId = 0;
var currentTrackName = '';
var currentTotalDistance = 0;
var currentDistanceRemaining = 0;
var distancePollingInterval = null;

// Per-lap distance tracking
var currentLapDistance = 0.0;       // Distance travelled in current lap (meters)
var currentLapStartTime = 0;        // When current lap started (ms)
var lastCompletedLapTime = 0;       // Duration of previous lap (ms)
var trackLapLength = 0.0;           // Length of one lap (meters)

var timerInterval;
var lapTimerStartMs = 0;            // Start time for current lap timer
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
var rssiSeries = new TimeSeries();
var rssiCrossingSeries = new TimeSeries();
var maxRssiValue = enterRssi + 10;
var minRssiValue = exitRssi - 10;

var audioEnabled = false;
var speakObjsQueue = [];
var lapFormat = 'full'; // 'full', 'laptime', 'timeonly'
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
    // WiFi-only mode
    console.log('[Init] Initializing WiFi-only mode');
    setupWiFiEvents();
    updateConnectionStatus('WiFi', true);
    return;
  }
  
  // Try USB first in auto/usb mode
  if (currentConnectionMode === 'auto' || currentConnectionMode === 'usb') {
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
        
        // Auto-connect to first FPVGate device in auto mode
        if (currentConnectionMode === 'auto') {
          // Try to find by manufacturer first
          let fpvgatePort = ports.find(p => 
            p.manufacturer && (p.manufacturer.includes('Espressif') || p.manufacturer.includes('Silicon Labs'))
          );
          
          // If not found, look for COM12 specifically (common FPVGate port on Windows)
          if (!fpvgatePort) {
            fpvgatePort = ports.find(p => p.path === 'COM12');
          }
          
          console.log('[Init] FPVRaceOne port found:', fpvgatePort);
          if (fpvgatePort) {
            await connectUSB(fpvgatePort.path);
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
    setupWiFiEvents();
    updateConnectionStatus('WiFi', true);
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

// Watchdog: if the SSE connection appears open but no keepalive arrives in 35s,
// the connection is stale (TCP alive but server silent). Force a reconnect.
function startKeepaliveWatchdog() {
  lastKeepaliveMs = Date.now();
  if (keepaliveWatchdogTimer) clearInterval(keepaliveWatchdogTimer);
  keepaliveWatchdogTimer = setInterval(() => {
    if (!eventSource || eventSource.readyState !== EventSource.OPEN) return;
    if (Date.now() - lastKeepaliveMs > KEEPALIVE_TIMEOUT_MS) {
      console.warn('[Keepalive] No keepalive in 35s — connection is stale, forcing reconnect');
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

  // Re-fetch mode and race state to restore everything after a page reload or reconnect.
  try {
    const r = await fetch('/api/mode');
    if (!r.ok) return;
    const data = await r.json();
    const newMode     = data.nodeMode    || 0;
    const modeChanged = newMode !== mnNodeMode;
    mnNodeMode        = newMode;
    mnMyNodeId        = data.myNodeId        || 0;
    mnMasterConnected = data.masterConnected  || false;
    mnMasterRaceActive = data.masterRaceActive || false;
    if (data.nodeMode !== 2) mnStatusSSID = data.ssid || '';

    const timerRunning  = data.timerRunning  || false;
    const raceElapsedMs = data.raceElapsedMs || 0;

    if (modeChanged || newMode === 1) {
      // Restore master race state before polling kicks in so mnRenderRaceTab sees it
      if (newMode === 1 && timerRunning) {
        mnRaceRunning = true;
        if (!mnRaceTimerIntervalId) _mnStartTimer(raceElapsedMs);
      }
      onRaceTabOpen();
      if (newMode === 1 && !mnPollingInterval) mnInitTab();
      if (newMode === 2) mnStartClientPoll();
    }

    // Restore single-pilot and client timer display
    if (timerRunning && (newMode === 0 || newMode === 2)) {
      startRaceDisplayOnly(raceElapsedMs);
    }

    // Restore in-progress laps: single mode shows lap table; master populates lapTimes[]
    // so _mnMasterEntry() returns the correct history for the race tab grid
    if (timerRunning && (newMode === 0 || newMode === 1)) {
      try {
        const lr = await fetch('/api/laps/current');
        if (lr.ok) {
          const ld = await lr.json();
          if (ld.laps && ld.laps.length > 0) _restoreInProgressLaps(ld.laps);
        }
      } catch (_) {}
    }

    mnUpdateRaceStatusBar();
  } catch (err) {
    console.warn('[Reconnect] /api/mode fetch failed:', err);
  }
}

function setupWiFiEvents() {
  // Clear any pending reconnect timer
  if (eventSourceReconnectTimer) {
    clearTimeout(eventSourceReconnectTimer);
    eventSourceReconnectTimer = null;
  }

  if (eventSource) {
    eventSource.close();
  }

  if (!window.EventSource) return;

  lastKeepaliveMs = Date.now(); // reset watchdog baseline for this new connection attempt
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

        eventSourceReconnectTimer = setTimeout(() => {
          console.log('Attempting EventSource reconnect...');
          setupWiFiEvents();
        }, RECONNECT_DELAY_MS);
      } else {
        showDisconnectedBanner('Connection lost. Please refresh the page.');
        console.error('Max reconnect attempts reached.');
      }
    }
  }, false);

  // Server sends a keepalive ping every 15s. Track it so the watchdog can detect
  // a stale-but-open TCP connection before the browser notices.
  eventSource.addEventListener("keepalive", function () {
    lastKeepaliveMs = Date.now();
  }, false);

  eventSource.addEventListener("rssi", function (e) {
    rssiBuffer.push(e.data);
    if (rssiBuffer.length > 10) rssiBuffer.shift();
  }, false);

  eventSource.addEventListener("lap", function (e) {
    var lap = (parseFloat(e.data) / 1000).toFixed(2);
    addLap(lap);
    console.log("lap:", lap + "s");
  }, false);

  // Sync race button state when the server starts or stops a race
  // (e.g. maxLaps reached on server side, or raced from another client)
  eventSource.addEventListener("raceState", function (e) {
    if (e.data === "started") {
      startRaceButton.disabled = true;
      stopRaceButton.disabled = false;
      addLapButton.disabled = false;
    } else if (e.data === "stopped") {
      stopRaceButton.disabled = true;
      startRaceButton.disabled = false;
      addLapButton.disabled = true;
    }
  }, false);

  // Master pushed updated node list (running state or DNF changed)
  eventSource.addEventListener("multiNodeState", function (e) {
    try {
      const data = JSON.parse(e.data);
      if (Array.isArray(data.nodes)) {
        mnRenderNodes(data.nodes);
        mnRenderRaceTab(data.nodes);
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
        if (data.lap === 1) {
          queueSpeak(`<p>${callsign} entered gate 1</p>`);
        } else {
          queueSpeak(`<p>${callsign} Lap ${data.lap}, ${formatMsSpeak(data.ms)}</p>`);
        }
      }
    } catch (_) {}
  }, false);

  // Master pushed race start/stop to this client node
  eventSource.addEventListener("masterRaceState", function (e) {
    if (e.data === "prearming") {
      // Master countdown in progress — flash Start button yellow but don't start timer
      const btn = document.getElementById('startRaceButton');
      if (btn) btn.classList.add('active');
    } else if (e.data === "started") {
      mnMasterRaceActive = true;
      const btn = document.getElementById('startRaceButton');
      if (btn) btn.classList.remove('active');
      startRaceDisplayOnly();
    } else if (e.data === "stopped") {
      mnMasterRaceActive = false;
      const btn = document.getElementById('startRaceButton');
      if (btn) btn.classList.remove('active');
      stopRaceDisplayOnly();
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
    var lap = (parseFloat(data) / 1000).toFixed(2);
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
  race.style.display = "block";
  calib.style.display = "none";

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

  // Dev mode: click pilot name display on single/client Race view to inject a simulated lap
  const pilotNameDisplay = document.getElementById('pilotNameDisplay');
  if (pilotNameDisplay) {
    pilotNameDisplay.addEventListener('click', () => {
      if (!mnDevMode) return;
      addManualLap();
    });
  }

  // Wire the "hide" link on the Race tab banner
  const hideLink = document.getElementById("raceTabDownloadReminderHide");
  if (hideLink) {
    hideLink.addEventListener("click", (ev) => {
      ev.preventDefault();
      hideRaceDownloadReminder();
      applyRaceHistoryModeUI(); // re-apply to honor the sessionStorage flag immediately
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
    console.log(configData);
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

    stopRaceButton.disabled = true;
    startRaceButton.disabled = false;
    addLapButton.disabled = true;

    clearInterval(timerInterval);
    timer.innerHTML = "00:00:00s";
    clearLaps();

    // Apply voice enabled state from device config (DO NOT auto-save here)
    audioEnabled = !!Number(configData.voiceEnabled);
    console.log('[DEBUG-STARTUP] voiceEnabled from /config:', configData.voiceEnabled, '→ audioEnabled:', audioEnabled);
    console.log('[DEBUG-STARTUP] wifiExtAntenna from /config:', configData.wifiExtAntenna);

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
    selectedVoice = configData.selectedVoice || 'default';

    const lapFormatSelect = document.getElementById('lapFormatSelect');
    const voiceSelect = document.getElementById('voiceSelect');
    if (lapFormatSelect) lapFormatSelect.value = lapFormat;
    if (voiceSelect) voiceSelect.value = selectedVoice;

    // Load and apply theme from device config
    if (configData.theme) {
      const savedTheme = configData.theme;
      document.documentElement.setAttribute('data-theme', savedTheme);
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
  // If nothing staged, do not prompt
  if (!stagedDirty || !stagedConfig || Object.keys(stagedConfig).length === 0) {
    return true;
  }

  return window.confirm(
    'You have unsaved changes.\n\nClose settings without saving?'
  );
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

function setTrackDataSettingsVisible(visible) {
  const navItems = document.querySelectorAll('.settings-nav-item');

  navItems.forEach(item => {
    const onclick = item.getAttribute('onclick') || '';
    if (onclick.includes("switchSettingsSection('tracks')")) {
      item.style.display = visible ? '' : 'none';
    }
  });

  // If Track Data is currently selected and we hide it, switch away
  if (!visible && typeof switchSettingsSection === 'function') {
    switchSettingsSection('system');
  }
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
      rssiValue = parseInt(rssiBuffer.shift());
      if (crossing && rssiValue < exitRssi) {
        crossing = false;
      } else if (!crossing && rssiValue > enterRssi) {
        crossing = true;
      }
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

setInterval(addRssiPoint, 200);

function createRssiChart() {
  rssiChart = new SmoothieChart({
    responsive: true,
    millisPerPixel: 50,
    grid: {
      strokeStyle: "rgba(255,255,255,0.25)",
      sharpLines: true,
      verticalSections: 0,
      borderVisible: false,
    },
    labels: {
      precision: 0,
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
  rssiChart.streamTo(document.getElementById("rssiChart"), 200);
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

  // Show the current tab, and add an "active" class to the button that opened the tab
  document.getElementById(tabName).style.display = "block";

  // Switch between single-pilot and master race view
  if (tabName === "race") onRaceTabOpen();

  // Hook pause button when entering Calibration tab; create chart on first open
  if (tabName === "calib") {
    const btn = document.getElementById('pauseCalibBtn');
    if (btn && !btn.dataset.bound) {
      btn.dataset.bound = "1";
      btn.addEventListener('click', toggleRssiPaused);
      btn.textContent = rssiPaused ? 'Resume' : 'Pause';
    }
    // Create the chart now that the canvas is visible and has real dimensions
    if (!rssiChart) {
      createRssiChart();
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

  // Tracks
  const tracksEnabledToggle = document.getElementById('tracksEnabled');
  const selectedTrackSelect = document.getElementById('selectedTrack');
  const selectedTrackId = selectedTrackSelect ? parseInt(selectedTrackSelect.value, 10) : 0;

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

    // Tracks
    tracksEnabled: (tracksEnabledToggle && tracksEnabledToggle.checked) ? 1 : 0,
    selectedTrackId: Number.isFinite(selectedTrackId) ? selectedTrackId : 0,

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
    wifiExtAntenna: (externalAntennaToggle && externalAntennaToggle.checked) ? 1 : 0,
    wifiTxPower: (() => {
      const el = document.getElementById('wifiTxPowerInput');
      const v = el ? parseInt(el.value, 10) : 21;
      return Number.isFinite(v) ? Math.min(21, Math.max(2, v)) : 21;
    })(),

    // Signal processing
    filterMode: (() => {
      const el = document.getElementById('filterModeSelect');
      return el ? parseInt(el.value, 10) : 0;
    })(),
    besselHz: (() => {
      const el = document.getElementById('besselHzSelect');
      return el ? parseInt(el.value, 10) : 0;
    })(),
    enterHoldSamples: (() => {
      const el = document.getElementById('enterHoldInput');
      const v = el ? parseInt(el.value, 10) : 4;
      return Number.isFinite(v) ? Math.min(20, Math.max(1, v)) : 4;
    })(),
    exitConfirmSamples: (() => {
      const el = document.getElementById('exitConfirmInput');
      const v = el ? parseInt(el.value, 10) : 2;
      return Number.isFinite(v) ? Math.min(10, Math.max(1, v)) : 2;
    })(),

    // Multi-node
    nodeMode: (() => {
      const el = document.getElementById('nodeModeSelect');
      return el ? parseInt(el.value, 10) : 0;
    })(),
    masterSSID: (document.getElementById('masterSSIDInput')?.value || ''),
    mnSkipMasterStart: document.getElementById('mnSkipMasterStartToggle')?.checked ? 1 : 0,
    devMode: document.getElementById('devModeToggle')?.checked ? 1 : 0,

  };

  return cfg;
}


function autoSaveConfig() {
  // Stage ONLY: compute snapshot and stage each key vs baseline
  const snap = buildConfigSnapshotFromUI();
  Object.keys(snap).forEach(k => stageConfig(k, snap[k]));
}

function saveRSSIThresholds() {
  if (calibOverviewMode) exitCalibrationOverviewModeByUserAction();
  // Explicitly stage current RSSI values before saving (enter/exit may have changed via the calibration wizard)
  const enterEl = document.getElementById('enter');
  const exitEl  = document.getElementById('exit');
  if (enterEl) stageConfig('enterRssi', parseInt(enterEl.value || 0));
  if (exitEl)  stageConfig('exitRssi',  parseInt(exitEl.value  || 0));
  saveConfig();
}

async function saveConfig() {
  // Commit staged config to device (single write)
  if (!stagedDirty) {
    console.log('[Config] No staged changes to save.');
    return;
  }

  // Send only staged (delta) fields — avoids accidentally overwriting fields like voiceEnabled
  // that have their own save path. Falls back to full snapshot only if staged is somehow empty.
  const payload = stagedConfig && Object.keys(stagedConfig).length ? stagedConfig : buildConfigSnapshotFromUI();

  try {
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
  } catch (err) {
    console.error('[Config] Save failed:', err);
    // Keep stagedDirty=true so user can try saving again
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

  // RSSI sensitivity (your current missing one)
  wire('rssiSensitivity', 'change');

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

  // Multi-node
  wire('masterSSIDInput', 'input');

  // NOTE:
  // Your minLap/alarm/maxLaps/announcerRate/etc already call autoSaveConfig()
  // inside their updateX() functions, so they are covered.
}


/* Don't use this one because it saves to flash immediately on every change.
// Debounced auto-save to prevent excessive API calls
let saveTimeout = null;
function autoSaveConfig() {
  clearTimeout(saveTimeout);
  saveTimeout = setTimeout(() => {
    saveConfig();
  }, 1000); // Wait 1 second after last change before saving
}


async function saveConfig() {
  // Get pilot settings
  const colorInput = document.getElementById('pilotColor');
  const themeSelect = document.getElementById('themeSelect');
  const voiceSelect = document.getElementById('voiceSelect');
  const lapFormatSelect = document.getElementById('lapFormatSelect');
  
  // Convert hex color to integer
  let pilotColorInt = 0x0080FF; // default
  if (colorInput && colorInput.value) {
    pilotColorInt = parseInt(colorInput.value.replace('#', ''), 16);
  }
  
  // Save all settings to device
  const rssiSensitivitySelect = document.getElementById('rssiSensitivity');
  const configData = {
    freq: frequency,
    minLap: parseInt(minLapInput.value * 10),
    alarm: parseInt(alarmThreshold.value * 10),
    anType: announcerSelect.selectedIndex,
    anRate: parseInt(announcerRate * 10),
    enterRssi: enterRssi,
    exitRssi: exitRssi,
    maxLaps: maxLaps,
    rssiSens: rssiSensitivitySelect ? parseInt(rssiSensitivitySelect.value) : 1,
    name: pilotNameInput.value,
    pilotColor: pilotColorInt,
    theme: themeSelect ? themeSelect.value : 'oceanic',
    selectedVoice: voiceSelect ? voiceSelect.value : 'default',
    lapFormat: lapFormatSelect ? lapFormatSelect.value : 'full',
    ssid: ssidInput ? ssidInput.value : '',
    pwd: pwdInput ? pwdInput.value : '',
  };
  
  if (usbConnected && transportManager) {
    const response = await transportManager.sendCommand('config', 'POST', configData);
    console.log("/config:", response);
  } else {
    fetch("/config", {
      method: "POST",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json",
      },
      body: JSON.stringify(configData),
    })
      .then((response) => response.json())
      .then((response) => console.log("/config:" + JSON.stringify(response)));
  }
}
*/

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
    const r = await fetch('/api/version');
    if (!r.ok) return;
    const data = await r.json();
    if (data && data.firmwareVersion) {
      const v = data.firmwareVersion;
      const footer = document.getElementById('firmwareVersion');
      if (footer) {
        const built = footer.textContent.match(/Boot ID:\s+\S+/);
        footer.textContent = `FPVRaceOne Personal Lap Timer v${v}` + (built ? `  \u2022  ${built[0]}` : '');
      }
      const badge = document.getElementById('updateVersionDisplay');
      if (badge) badge.textContent = `v${v}`;
    }
  } catch (e) {
    console.warn('[UI] Failed to load firmware version:', e);
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
  announcerRate = parseFloat(value);
  $(obj).parent().find("span").text(announcerRate.toFixed(1));
  audioAnnouncer.setRate(announcerRate);
  // Auto-save when changed
  autoSaveConfig();
}

function updateMinLap(obj, value) {
  $(obj)
    .parent()
    .find("span")
    .text(parseFloat(value).toFixed(1) + "s");
  // Auto-save when changed
  autoSaveConfig();
}

function updateAlarmThreshold(obj, value) {
  $(obj)
    .parent()
    .find("span")
    .text(parseFloat(value).toFixed(1) + "v");
  // Auto-save when changed
  autoSaveConfig();
}

function updateMaxLaps(obj, value) {
  maxLaps = parseInt(value);
  let displayText;
  if (maxLaps === 0) {
    displayText = "Inf.";
  } else if (maxLaps === 1) {
    displayText = "1";
  } else {
    displayText = maxLaps;
  }
  $(obj)
    .parent()
    .find("span")
    .text(displayText);
  // Auto-save when changed
  autoSaveConfig();
}

// function getAnnouncerVoices() {
//   $().articulate("getVoices", "#voiceSelect", "System Default Announcer Voice");
// }

// Shared AudioContext for beeps (reused to avoid iOS issues)
var beepAudioContext = null;

function beep(duration, frequency, type) {
  // Create or reuse AudioContext
  if (!beepAudioContext) {
    beepAudioContext = new AudioContext();
  }
  
  // iOS/Safari: ensure AudioContext is running
  if (beepAudioContext.state === 'suspended') {
    beepAudioContext.resume().then(() => {
      playBeepTone(duration, frequency, type);
    }).catch(err => {
      console.warn('[Beep] AudioContext resume failed:', err);
    });
  } else {
    playBeepTone(duration, frequency, type);
  }
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

// Silently restore in-progress laps after page reload — no TTS, no state side-effects
function _restoreInProgressLaps(laps) {
  lapNo = -1;
  lapTimes = [];
  const table = document.getElementById('lapTable');
  if (!table) return;
  while (table.rows.length > 0) table.deleteRow(0);
  let cumSec = 0;
  laps.forEach((l, idx) => {
    const lapSec = (l.lapTimeMs / 1000).toFixed(2);
    const newLap = parseFloat(lapSec);
    lapTimes.push(newLap);
    lapNo = idx;
    cumSec += newLap;
    const row = table.insertRow();
    row.setAttribute('data-lap-index', idx);
    const c1 = row.insertCell(0); c1.innerHTML = lapNo + 1;
    const c2 = row.insertCell(1);
    c2.innerHTML = lapNo === 0 ? `Gate 1: ${lapSec}s` : `${lapSec}s`;
    const gap = idx > 0 ? (newLap - lapTimes[idx - 1]).toFixed(2) : null;
    const c3 = row.insertCell(2);
    c3.innerHTML = gap !== null ? (parseFloat(gap) > 0 ? '+' + gap : gap) + 's' : '-';
    const c4 = row.insertCell(3); c4.innerHTML = cumSec.toFixed(2) + 's';
  });
  highlightFastestLap();
  updateLapCounter();
}

function addLap(lapStr) {
  const pilotName = pilotNameInput.value;
  
  const newLap = parseFloat(lapStr);
  lapNo += 1;
  lapTimes.push(newLap);
  
  // Track lap timing for distance estimation
  lastCompletedLapTime = newLap * 1000; // Convert to milliseconds
  currentLapStartTime = Date.now();     // Reset lap start time
  lapTimerStartMs = Date.now();         // Reset lap timer
  currentLapDistance = 0.0;             // Reset distance counter
  
  // Calculate total time so far
  const totalTime = lapTimes.reduce((sum, time) => sum + time, 0).toFixed(2);
  
  // Calculate gap from previous lap (for regular laps only, not gate 1)
  let gap = "";
  if (lapNo > 1) {
    gap = (newLap - lapTimes[lapTimes.length - 2]).toFixed(2);
    if (gap > 0) gap = "+" + gap;
  }
  
  const table = document.getElementById("lapTable");
  const row = table.insertRow();
  row.setAttribute('data-lap-index', lapTimes.length - 1);
  
  const cell1 = row.insertCell(0);  // Lap No
  const cell2 = row.insertCell(1);  // Lap Time
  const cell3 = row.insertCell(2);  // Gap
  const cell4 = row.insertCell(3);  // Total Time
  
  cell1.innerHTML = lapNo;
  
  if (lapNo == 0) {
    cell2.innerHTML = "Gate 1: " + lapStr + "s";
    cell3.innerHTML = "-";
  } else {
    cell2.innerHTML = lapStr + "s";
    cell3.innerHTML = gap ? gap + "s" : "-";
  }
  
  cell4.innerHTML = totalTime + "s";
  
  // Highlight fastest lap
  highlightFastestLap();

  const lapSpeak = formatMsSpeak(Math.round(parseFloat(lapStr) * 1000));
  switch (announcerSelect.options[announcerSelect.selectedIndex].value) {
    case "beep":
      beep(100, 330, "square");
      break;
    case "1lap":
      if (lapNo == 0) {
        queueSpeak(`<p>${pilotName} entered gate 1</p>`);
      } else {
        let text;
        switch (lapFormat) {
          case 'full':
            text = `<p>${pilotName} Lap ${lapNo}, ${lapSpeak}</p>`;
            break;
          case 'laptime':
            text = `<p>${pilotName} Lap ${lapNo}, ${lapSpeak}</p>`;
            break;
          case 'timeonly':
            text = `<p>${pilotName} ${lapSpeak}</p>`;
            break;
          default:
            text = `<p>${pilotName} Lap ${lapNo}, ${lapSpeak}</p>`;
        }
        queueSpeak(text);
      }
      break;
    case "2lap":
      if (lapNo == 0) {
        queueSpeak(`<p>${pilotName} entered gate 1</p>`);
      } else if (last2lapStr != "") {
        const text2 = "<p>" + pilotName + " 2 laps " + formatMsSpeak(Math.round(parseFloat(last2lapStr) * 1000)) + "</p>";
        queueSpeak(text2);
      }
      break;
    case "3lap":
      if (lapNo == 0) {
        queueSpeak(`<p>${pilotName} entered gate 1</p>`);
      } else if (last3lapStr != "") {
        const text3 = "<p>" + pilotName + " 3 laps " + formatMsSpeak(Math.round(parseFloat(last3lapStr) * 1000)) + "</p>";
        queueSpeak(text3);
      }
      break;
    default:
      break;
  }
  
  // Update lap counter
  updateLapCounter();

  // Update lap analysis
  updateAnalysisView();

  // Auto-stop race if max laps reached (excluding hole shot, and if maxLaps > 0)
  if (maxLaps > 0 && lapNo > 0 && lapNo >= maxLaps) {
    setTimeout(function() {
      if (!stopRaceButton.disabled) {
        stopRace();
        queueSpeak('<p>Race complete</p>');
      }
    }, 500); // Small delay to allow lap announcement
  }
}

function startTimer() {
  const _timerStart = Date.now();
  timerInterval = setInterval(function () {
    const elapsed = Date.now() - _timerStart;
    const minutes = Math.floor(elapsed / 60000);
    const seconds = Math.floor((elapsed % 60000) / 1000);
    const millis  = Math.floor((elapsed % 1000) / 10);
    let m = minutes < 10 ? "0" + minutes : minutes;
    let s = seconds < 10 ? "0" + seconds : seconds;
    let ms = millis  < 10 ? "0" + millis  : millis;
    timer.innerHTML = `${m}:${s}:${ms}s`;
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

  clearInterval(timerInterval);
  const _timerStart = Date.now() - offsetMs;
  timerInterval = setInterval(function () {
    const elapsed = Date.now() - _timerStart;
    const minutes = Math.floor(elapsed / 60000);
    const seconds = Math.floor((elapsed % 60000) / 1000);
    const millis  = Math.floor((elapsed % 1000) / 10);
    const m  = minutes < 10 ? '0' + minutes : minutes;
    const s  = seconds < 10 ? '0' + seconds : seconds;
    const ms = millis   < 10 ? '0' + millis  : millis;
    timer.innerHTML = `${m}:${s}:${ms}s`;
  }, 10);

  currentLapStartTime  = Date.now();
  lapTimerStartMs      = Date.now();
  lastCompletedLapTime = 0;
  currentLapDistance   = 0.0;
  startDistancePolling();
}

// Stop the visual race display without sending /timer/stop to the server.
// Used when the master remotely stops the race on this client node.
function stopRaceDisplayOnly() {
  clearInterval(timerInterval);
  timer.innerHTML = '00:00:00s';
  stopRaceButton.disabled  = true;
  startRaceButton.disabled = false;
  startRaceButton.classList.remove('active');
  addLapButton.disabled    = true;
  stopDistancePolling();
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

function hideRaceDownloadReminder() {
  const banner = document.getElementById("raceTabDownloadReminder");
  if (banner) {
    banner.style.display = "none";
    sessionStorage.setItem("hideRaceDownloadReminder", "1");
  }
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
  const enableBtn = document.getElementById('EnableAudioButton');
  const disableBtn = document.getElementById('DisableAudioButton');
  
  if (audioEnabled) {
    enableBtn.style.backgroundColor = '#f39c12';
    enableBtn.style.opacity = '1';
    disableBtn.style.backgroundColor = '';
    disableBtn.style.opacity = '0.5';
  } else {
    enableBtn.style.backgroundColor = '';
    enableBtn.style.opacity = '0.5';
    disableBtn.style.backgroundColor = '#f39c12';
    disableBtn.style.opacity = '1';
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

  updateLedPresetUI();

  // Keep existing live behavior
  if (preset === 9) {
    const pilotColor = document.getElementById('pilotColor')?.value || '#0080FF';
    const color = pilotColor.substring(1);
    sendLedCommand('color', { color }).catch(err => console.error('Failed to set pilot color:', err));
  } else if (preset === 2) {
    setSolidColor();
  } else if (preset === 6) {
    setFadeColor();
  } else if (preset === 7) {
    setStrobeColor();
  }

  // Stage config
  stageConfig('ledPreset', Number.isFinite(preset) ? preset : 0);
}


function setSolidColor() {
  const colorInput = document.getElementById('ledSolidColor');
  const colorHex = colorInput.value.substring(1);

  sendLedCommand('color', { color: colorHex })
    .then(data => console.log('LED solid color changed:', data))
    .catch(err => console.error('Failed to change LED solid color:', err));

  stageConfig('ledColor', parseInt(colorHex, 16));
}

function setFadeColor() {
  const colorInput = document.getElementById('ledFadeColor');
  const colorHex = colorInput.value.substring(1);

  sendLedCommand('fadecolor', { color: colorHex })
    .then(data => console.log('LED fade color changed:', data))
    .catch(err => console.error('Failed to change LED fade color:', err));

  stageConfig('ledFadeColor', parseInt(colorHex, 16));
}

function setStrobeColor() {
  const colorInput = document.getElementById('ledStrobeColor');
  const colorHex = colorInput.value.substring(1);

  sendLedCommand('strobecolor', { color: colorHex })
    .then(data => console.log('LED strobe color changed:', data))
    .catch(err => console.error('Failed to change LED strobe color:', err));

  stageConfig('ledStrobeColor', parseInt(colorHex, 16));
}

function updateLedBrightness(obj, value) {
  const brightness = parseInt(value, 10);
  $(obj).parent().find('span').text(brightness);

  // Keep existing live behavior
  sendLedCommand('brightness', { brightness })
    .then(data => console.log('LED brightness changed:', data))
    .catch(err => console.error('Failed to change LED brightness:', err));

  // Stage config
  stageConfig('ledBrightness', Number.isFinite(brightness) ? brightness : 128);
}


function updateLedSpeed(obj, value) {
  const speed = parseInt(value, 10);
  $(obj).parent().find('span').text(speed);

  // Keep existing live behavior
  sendLedCommand('speed', { speed })
    .then(data => console.log('LED speed changed:', data))
    .catch(err => console.error('Failed to change LED speed:', err));

  // Stage config
  stageConfig('ledSpeed', Number.isFinite(speed) ? speed : 10);
}

function toggleLedManualOverride(enabled) {
  const enable = enabled ? 1 : 0;

  sendLedCommand('override', { enable })
    .then(data => console.log('LED manual override:', enabled ? 'enabled' : 'disabled', data))
    .catch(err => console.error('Failed to toggle LED manual override:', err));

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
}

function highlightFastestLap() {
  if (lapTimes.length === 0) return;
  
  // Find fastest lap (excluding gate 1 at index 0)
  let fastestTime = Infinity;
  let fastestIndex = -1;
  
  for (let i = 1; i < lapTimes.length; i++) {  // Start from 1 to skip gate 1
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
async function _raceCountdown(armPhrase) {
  const _wait = () => new Promise(r => setTimeout(r, 50));
  const _speechDone = async () => {
    while (audioAnnouncer.isSpeaking() || audioAnnouncer.audioQueue.length > 0) await _wait();
  };

  queueSpeak(`<p>${armPhrase}</p>`);
  await _speechDone();
  queueSpeak("<p>Starting in</p>");
  await _speechDone();

  for (let i = 5; i >= 1; i--) {
    const t0 = Date.now();
    queueSpeak(`<p>${i}</p>`);
    await _speechDone();
    const pad = 1000 - (Date.now() - t0);
    if (pad > 0) await new Promise(r => setTimeout(r, pad));
  }
}

async function startRace() {
  updateLapCounter();
  startRaceButton.disabled = true;
  startRaceButton.classList.add('active');
  
  // iOS/Safari: unlock AudioContext for beeps during user interaction
  if (beepAudioContext && beepAudioContext.state === 'suspended') {
    try {
      await beepAudioContext.resume();
      console.log('[Race] AudioContext resumed for beeps');
    } catch (err) {
      console.warn('[Race] AudioContext resume failed:', err);
    }
  }
  
  await _raceCountdown("Arm your quad");
  
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
  
  // Initialize lap timing for distance estimation
  currentLapStartTime = Date.now();
  lapTimerStartMs = Date.now();
  lastCompletedLapTime = 0;
  currentLapDistance = 0.0;
  
  // Start polling distance if tracks enabled
  startDistancePolling();
}

function stopRace() {
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

  // Stop distance polling
  stopDistancePolling();

  // Show Download/Transfer buttons now that race is stopped
  updateRaceDataButtonsVisibility();

  // Note: Race data remains visible after stopping.
  // Use "Transfer to Race History" button to save it, or "Clear Laps" to remove it.
  // Race data is NOT automatically transferred to Race History anymore.
}

function clearLaps() {
  // Note: Race data is NOT automatically saved to Race History.
  // If you want to save before clearing, use "Transfer to Race History" button first.

  var tableHeaderRowCount = 1;
  var rowCount = lapTable.rows.length;
  for (var i = tableHeaderRowCount; i < rowCount; i++) {
    lapTable.deleteRow(tableHeaderRowCount);
  }
  lapNo = -1;
  lapTimes = [];
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
    buttonsDiv.style.display = (lapTimes.length > 0 && raceIsStopped) ? 'block' : 'none';
  }
}

function downloadCurrentRaceData() {
  if (lapTimes.length === 0) {
    alert('No race data to download');
    return;
  }

  // Create race data object from current race
  const validLaps = lapTimes.slice(1);
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

  let totalRaceDistance = 0;
  if (trackLapLength > 0 && lapTimes.length > 0) {
    totalRaceDistance = trackLapLength * lapTimes.length;
  }

  const raceData = {
    timestamp: Math.floor(Date.now() / 1000),
    lapTimes: lapTimes.map(t => Math.round(t * 1000)),
    fastestLap: Math.round(fastest * 1000),
    medianLap: Math.round(median * 1000),
    best3LapsTotal: Math.round(best3Total * 1000),
    pilotName: pilotNameInput.value || '',
    frequency: frequency,
    band: bandValue,
    channel: channelValue,
    trackId: currentTrackId || 0,
    trackName: currentTrackName || '',
    totalDistance: totalRaceDistance
  };

  // Create download
  const dataStr = JSON.stringify({ races: [raceData] }, null, 2);
  const dataBlob = new Blob([dataStr], { type: 'application/json' });
  const url = URL.createObjectURL(dataBlob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `race-${raceData.timestamp}.json`;
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



// Theme functionality
function changeTheme() {
    const theme = document.getElementById('themeSelect').value;
    if (theme === 'lighter') {
      document.documentElement.removeAttribute('data-theme');
    } else {
      document.documentElement.setAttribute('data-theme', theme);
    }
    updateThemeLogos(theme);
    autoSaveConfig(); // Save to device
  }
  
  function updateThemeLogos(theme) {
    // Light themes list
    const lightThemes = new Set(['lighter','github','onelight','solarizedlight','lightowl']);
    const isLight = lightThemes.has(theme);
    const favicon = document.getElementById('favicon');
    const headerLogo = document.getElementById('headerLogo');
    const logoPath = isLight ? 'logo-black.svg' : 'logo-white.svg';
    if (favicon) {
      favicon.href = logoPath;
      favicon.type = 'image/svg+xml';
    }
    if (headerLogo) headerLogo.src = logoPath;
  }

function loadDarkMode() {
    // Theme is now loaded from device config on page load
    // This function is kept for compatibility but may not be needed
    const themeSelect = document.getElementById('themeSelect');
    if (themeSelect && themeSelect.value) {
      const theme = themeSelect.value;
      if (theme !== 'lighter') {
        document.documentElement.setAttribute('data-theme', theme);
      }
      updateThemeLogos(theme);
    }
  }

// Manual lap addition
function addManualLap() {
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
  
  // Fastest Lap (excluding Gate 1 which is just passing through to start)
  const validLaps = lapTimes.slice(1); // Skip Gate 1
  if (validLaps.length === 0) {
    document.getElementById('statFastest').textContent = '--';
    document.getElementById('statFastestLapNo').textContent = 'Need 1 lap';
  } else {
    const fastest = Math.min(...validLaps);
    const fastestIndex = validLaps.indexOf(fastest) + 1; // +1 to account for skipped Gate 1
    document.getElementById('statFastest').textContent = `${fastest.toFixed(2)}s`;
    document.getElementById('statFastestLapNo').textContent = `Lap ${fastestIndex}`;
  }
  
  // Fastest 3 Consecutive Laps (for RaceGOW format) - skip Gate 1
  if (validLaps.length >= 3) {
    let fastestConsecTime = Infinity;
    let fastestConsecStart = -1;
    
    // Check all consecutive 3-lap windows (starting from Lap 1, not Gate 1)
    for (let i = 0; i <= validLaps.length - 3; i++) {
      const consecTime = validLaps[i] + validLaps[i + 1] + validLaps[i + 2];
      if (consecTime < fastestConsecTime) {
        fastestConsecTime = consecTime;
        fastestConsecStart = i + 1; // +1 to account for skipped Gate 1
      }
    }
    
    document.getElementById('statFastest3Consec').textContent = `${fastestConsecTime.toFixed(2)}s`;
    const lapNums = `L${fastestConsecStart}-L${fastestConsecStart + 1}-L${fastestConsecStart + 2}`;
    document.getElementById('statFastest3ConsecLaps').textContent = lapNums;
  } else {
    document.getElementById('statFastest3Consec').textContent = '--';
    document.getElementById('statFastest3ConsecLaps').textContent = 'Need 3 laps';
  }
  
  // Median Lap (excluding Gate 1)
  if (validLaps.length === 0) {
    document.getElementById('statMedian').textContent = '--';
  } else {
    const sorted = [...validLaps].sort((a, b) => a - b);
    const mid = Math.floor(sorted.length / 2);
    const median = sorted.length % 2 === 0 ? 
      (sorted[mid - 1] + sorted[mid]) / 2 : 
      sorted[mid];
    document.getElementById('statMedian').textContent = `${median.toFixed(2)}s`;
  }
  
  // Best 3 Laps (sum of 3 fastest individual laps) - skip Gate 1
  if (validLaps.length >= 3) {
    const lapsWithIndex = validLaps.map((time, index) => ({ time, index: index + 1 })); // +1 for actual lap number
    lapsWithIndex.sort((a, b) => a.time - b.time);
    const best3 = lapsWithIndex.slice(0, 3);
    const totalTime = best3.reduce((sum, l) => sum + l.time, 0);
    const lapNumbers = best3.map(l => `L${l.index}`).sort().join(', ');
    document.getElementById('statBest3').textContent = `${totalTime.toFixed(2)}s`;
    document.getElementById('statBest3Laps').textContent = lapNumbers;
  } else {
    document.getElementById('statBest3').textContent = '--';
    document.getElementById('statBest3Laps').textContent = 'Need 3 laps';
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
      // Special label for Gate 1
      html += createBarItemWithColor(`Gate 1`, time, maxTime, `${time.toFixed(2)}s`, colorIndex);
    } else {
      html += createBarItemWithColor(`Lap ${lapNumber}`, time, maxTime, `${time.toFixed(2)}s`, colorIndex);
    }
    
  });
  html += '</div>';
  
  if (lapTimes.length > 10) {
    html += `<p style="text-align: center; margin-top: 16px; color: var(--secondary-color); font-size: 14px;">Showing last 10 of ${lapTimes.length} laps</p>`;
  }
  
  document.getElementById('analysisContent').innerHTML = html;
}

function renderFastestRound() {
  if (lapTimes.length < 3) {
    document.getElementById('analysisContent').innerHTML = 
      '<p class="no-data">Complete at least 3 laps to see fastest round</p>';
    return;
  }
  
  // Find best consecutive 3 laps
  let bestTime = Infinity;
  let bestStartIndex = 0;
  
  for (let i = 0; i <= lapTimes.length - 3; i++) {
    const sum = lapTimes[i] + lapTimes[i+1] + lapTimes[i+2];
    if (sum < bestTime) {
      bestTime = sum;
      bestStartIndex = i;
    }
  }
  
  const lap1 = lapTimes[bestStartIndex];
  const lap2 = lapTimes[bestStartIndex + 1];
  const lap3 = lapTimes[bestStartIndex + 2];
  const maxTime = Math.max(lap1, lap2, lap3);
  
  let html = '<div class="analysis-bars">';
  html += createBarItemWithColor(`Lap ${bestStartIndex + 1}`, lap1, maxTime, `${lap1.toFixed(2)}s`, 0);
  html += createBarItemWithColor(`Lap ${bestStartIndex + 2}`, lap2, maxTime, `${lap2.toFixed(2)}s`, 1);
  html += createBarItemWithColor(`Lap ${bestStartIndex + 3}`, lap3, maxTime, `${lap3.toFixed(2)}s`, 2);
  html += '</div>';
  html += `<p style="text-align: center; margin-top: 16px; font-weight: bold; color: var(--primary-color);">Total: ${bestTime.toFixed(2)}s</p>`;
  
  document.getElementById('analysisContent').innerHTML = html;
}

function createBarItemWithColor(label, time, maxTime, displayTime, colorIndex) {
  const percentage = (time / maxTime) * 100;
  const colors = barColors[colorIndex % barColors.length];
  return `
    <div class="bar-item">
      <div class="bar-label">${label}</div>
      <div class="bar-container">
        <div class="bar-fill" style="width: ${percentage}%; background: linear-gradient(90deg, ${colors[0]}, ${colors[1]});">
          <span class="bar-time">${displayTime}</span>
        </div>
      </div>
    </div>
  `;
}

// Race History Functions
let raceHistoryData = [];
let raceHistoryPersistent = true; // false when SD-less RAM-only mode
let ledConnected = false;
let currentDetailRace = null;

function saveCurrentRace() {
  if (lapTimes.length === 0) return;
  
  // Calculate stats (excluding Gate 1)
  const validLaps = lapTimes.slice(1);
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

  // Calculate total race distance: track length per lap × number of laps
  let totalRaceDistance = 0;
  if (trackLapLength > 0 && lapTimes.length > 0) {
    totalRaceDistance = trackLapLength * lapTimes.length;
  }

  const raceData = {
    timestamp: Math.floor(Date.now() / 1000),
    lapTimes: lapTimes.map(t => Math.round(t * 1000)), // Convert to milliseconds
    fastestLap: Math.round(fastest * 1000),
    medianLap: Math.round(median * 1000),
    best3LapsTotal: Math.round(best3Total * 1000),
    pilotName: pilotNameInput.value || '',
    frequency: frequency,
    band: bandValue,
    channel: channelValue,
    trackId: currentTrackId || 0,
    trackName: currentTrackName || '',
    totalDistance: totalRaceDistance
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

  setTrackDataSettingsVisible(raceHistoryPersistent);
  setLEDSettingsVisible(ledConnected);

  setButtonLabel(
    importBtn,
    raceHistoryPersistent ? 'Import Races' : 'Import Race (overrides current data)'
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

    if (raceHistoryPersistent || userHidden) {
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
    const fastestLap = (race.fastestLap / 1000).toFixed(2);
    // Lap count should exclude Gate 1 (first entry)
    const actualLapCount = race.lapTimes.length > 0 ? race.lapTimes.length - 1 : 0;
    // Calculate total race time (sum of all times)
    const totalTime = race.lapTimes.reduce((sum, t) => sum + t, 0) / 1000;
    const name = race.name || '';
    const tag = race.tag || '';
    const pilotName = race.pilotName || race.pilotCallsign || '';
    const freqDisplay = race.frequency ? `${race.band}${race.channel} (${race.frequency}MHz)` : '';
    const trackDisplay = race.trackName ? race.trackName : '';
    const distanceDisplay = race.totalDistance ? `${race.totalDistance.toFixed(1)}m` : '';

    html += `
      <div class="race-item" data-race-index="${index}" onclick="viewRaceDetails(${index})">
        <div class="race-item-buttons">
          ${raceHistoryPersistent ? `<button class="race-item-button" onclick="event.stopPropagation(); openEditModal(${index})">Edit</button>` : ''}
          <button class="race-item-button" onclick="event.stopPropagation(); downloadSingleRace(${race.timestamp})">Download</button>
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
          <div class="race-item-stat">Fastest: <strong>${fastestLap}s</strong></div>
          <div class="race-item-stat">Total Time: <strong>${totalTime.toFixed(2)}s</strong></div>
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
  const date = new Date(race.timestamp * 1000);
  const dateStr = date.toLocaleDateString() + ' ' + date.toLocaleTimeString();

  // Calculate total race time
  const totalTime = race.lapTimes.reduce((sum, t) => sum + t, 0) / 1000;

  document.getElementById('raceDetailsTitle').textContent = `Race - ${dateStr}`;
  document.getElementById('detailFastest').textContent = (race.fastestLap / 1000).toFixed(2) + 's';
  document.getElementById('detailMedian').textContent = (race.medianLap / 1000).toFixed(2) + 's';
  document.getElementById('detailBest3').textContent = (race.best3LapsTotal / 1000).toFixed(2) + 's';
  document.getElementById('detailTotalTime').textContent = totalTime.toFixed(2) + 's';

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
        label: 'Gate 1',
        time: cumulativeTime,
        percentage: percentage
      });
    } else {
      // Regular laps
      events.push({
        type: 'lap',
        label: `Lap ${index}`,
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
    eventDiv.title = `${event.label} - ${event.time.toFixed(2)}s`;
    
    eventDiv.innerHTML = `
      <div class="timeline-flag ${event.type}"></div>
      <div class="timeline-label">${event.label}</div>
      <div class="timeline-time">${event.time.toFixed(2)}s</div>
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
      lapTimeDiv.textContent = `${lapTime.toFixed(2)}s`;
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
      
      console.log(`Playback: ${index === 0 ? 'Gate 1' : 'Lap ' + index} - ${lapTime.toFixed(2)}s`);
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
  const displayLaps = lapTimes.slice(-10);
  const maxTime = Math.max(...displayLaps);
  
  // Get track distance if available
  const trackDistance = currentDetailRace.totalDistance || 0;
  const hasTrackData = trackDistance > 0 && currentDetailRace.lapTimes.length > 0;
  const perLapDistance = hasTrackData ? trackDistance / currentDetailRace.lapTimes.length : 0;
  
  // Calculate total race time
  const totalTime = lapTimes.reduce((sum, t) => sum + t, 0);
  
  let html = '<div class="analysis-bars">';
  displayLaps.forEach((time, index) => {
    const actualIndex = lapTimes.length - displayLaps.length + index;
    let label;
        
    // First entry is Gate 1 (start), not a lap
    if (actualIndex === 0) {
      label = 'Gate 1';
    } else {
      label = `Lap ${actualIndex}`;
    }
    
    // Add distance info if available: "Lap x - y/z m"
    if (hasTrackData && actualIndex > 0) {
      label = `${time.toFixed(2)}s\n${label} - ${perLapDistance.toFixed(0)}m`;
    } else if (hasTrackData && actualIndex === 0) {
      label = `${time.toFixed(2)}s\nGate 1 (Start)`;
    } else if (actualIndex === 0) {
      label = 'Gate 1';
    }
    
    const displayTime = hasTrackData ? '' : `${time.toFixed(2)}s`; // Don't show time in bar if it's in label
    html += createBarItemWithColor(label, time, maxTime, displayTime, index);
  });
  html += '</div>';
  html += `<p style="text-align: center; margin-top: 16px; font-weight: bold; color: var(--primary-color);">Total Race Time: ${totalTime.toFixed(2)}s</p>`;
  
  document.getElementById('detailContent').innerHTML = html;
}

function renderDetailFastestRound() {
  if (!currentDetailRace) return;
  
  const lapTimes = currentDetailRace.lapTimes.map(t => t / 1000);
  
  if (lapTimes.length < 3) {
    document.getElementById('detailContent').innerHTML = 
      '<p class="no-data">Not enough laps for fastest round</p>';
    return;
  }
  
  let bestTime = Infinity;
  let bestStartIndex = 0;
  
  for (let i = 0; i <= lapTimes.length - 3; i++) {
    const sum = lapTimes[i] + lapTimes[i+1] + lapTimes[i+2];
    if (sum < bestTime) {
      bestTime = sum;
      bestStartIndex = i;
    }
  }
  
  const lap1 = lapTimes[bestStartIndex];
  const lap2 = lapTimes[bestStartIndex + 1];
  const lap3 = lapTimes[bestStartIndex + 2];
  const maxTime = Math.max(lap1, lap2, lap3);
  
  let html = '<div class="analysis-bars">';
  html += createBarItemWithColor(`Lap ${bestStartIndex + 1}`, lap1, maxTime, `${lap1.toFixed(2)}s`, 0);
  html += createBarItemWithColor(`Lap ${bestStartIndex + 2}`, lap2, maxTime, `${lap2.toFixed(2)}s`, 1);
  html += createBarItemWithColor(`Lap ${bestStartIndex + 3}`, lap3, maxTime, `${lap3.toFixed(2)}s`, 2);
  html += '</div>';
  html += `<p style="text-align: center; margin-top: 16px; font-weight: bold; color: var(--primary-color);">Total: ${bestTime.toFixed(2)}s</p>`;
  
  document.getElementById('detailContent').innerHTML = html;
}

function downloadRaces() {
  const a = document.createElement('a');
  a.href = '/api/races/download';
  a.download = 'races.json';   // browser-enforced
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
}

function downloadSingleRace(timestamp) {
    const a = document.createElement('a');
    a.href = '/races/downloadOne?timestamp=' + timestamp, '_blank';
    a.download = 'races.json';   // browser-enforced
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
}

let editingRaceIndex = null;

function openEditModal(index) {
  editingRaceIndex = index;
  const race = raceHistoryData[index];
  
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
    const lapLabel = index === 0 ? 'Gate 1' : `Lap ${index}`;
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

  // Normalize expected shape: { races:[...] } or [...] (array)
  const racesArray = Array.isArray(json) ? json : (Array.isArray(json?.races) ? json.races : null);
  if (!racesArray) {
    alert('Race file format not recognized. Expected {"races":[...]} or an array.');
    return;
  }

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
        lapTimes: Array.isArray(r.lapTimes) ? r.lapTimes : []
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

// OSD Overlay Function
function openOSD() {
  const osdUrl = window.location.origin + '/osd.html';
  
  // Open OSD in new tab
  window.open(osdUrl, '_blank');
  
  // Copy URL to clipboard
  navigator.clipboard.writeText(osdUrl)
    .then(() => {
      // Show temporary success message
      const button = event.target;
      const originalText = button.textContent;
      button.textContent = '✓ URL Copied!';
      button.style.backgroundColor = '#27ae60';
      
      setTimeout(() => {
        button.textContent = originalText;
        button.style.backgroundColor = '';
      }, 2000);
    })
    .catch(err => {
      console.error('Failed to copy URL:', err);
      alert('OSD opened, but failed to copy URL. URL: ' + osdUrl);
    });
}

// Config Download/Import Functions
function downloadConfig() {
  fetch('/config')
    .then(response => response.json())
    .then(config => {
      // Add all client-side settings
      const fullConfig = {
        ...config,
        // Theme
        theme: localStorage.getItem('theme') || 'oceanic',
        // Audio settings
        audioEnabled: audioEnabled,
        lapFormat: localStorage.getItem('lapFormat') || 'full',
        selectedVoice: localStorage.getItem('selectedVoice') || 'default',
        ttsEngine: localStorage.getItem('ttsEngine') || 'piper',
        // Pilot frontend settings
        pilotColor: localStorage.getItem('pilotColor') || '#0080FF',
        // LED settings (get from current UI state)
        ledPreset: parseInt(document.getElementById('ledPreset')?.value || 2),
        ledSolidColor: document.getElementById('ledSolidColor')?.value || '#FF00FF',
        ledFadeColor: document.getElementById('ledFadeColor')?.value || '#0080FF',
        ledStrobeColor: document.getElementById('ledStrobeColor')?.value || '#FFFFFF',
        ledSpeed: parseInt(document.getElementById('ledSpeed')?.value || 5),
        ledManualOverride: document.getElementById('ledManualOverride')?.checked || false,
        // Battery monitoring
        batteryMonitoring: document.getElementById('batteryMonitorToggle')?.checked !== false,
        timestamp: new Date().toISOString()
      };
      
      const dataStr = JSON.stringify(fullConfig, null, 2);
      const dataBlob = new Blob([dataStr], { type: 'application/json' });
      const url = URL.createObjectURL(dataBlob);
      const link = document.createElement('a');
      link.href = url;
      link.download = `fpvgate-config-${new Date().toISOString().slice(0,10)}.json`;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      URL.revokeObjectURL(url);
    })
    .catch(error => {
      console.error('Error downloading config:', error);
      alert('Error downloading configuration');
    });
}

function importConfig(input) {
  const file = input.files[0];
  if (!file) return;
  
  const reader = new FileReader();
  reader.onload = function(e) {
    try {
      const config = JSON.parse(e.target.result);
      
      // Apply backend config
      fetch('/config', {
        method: 'POST',
        headers: {
          'Accept': 'application/json',
          'Content-Type': 'application/json'
        },
        body: JSON.stringify({
          freq: config.freq,
          minLap: config.minLap,
          alarm: config.alarm,
          anType: config.anType,
          anRate: config.anRate,
          enterRssi: config.enterRssi,
          exitRssi: config.exitRssi,
          maxLaps: config.maxLaps,
          name: config.name || '',
          ssid: config.ssid || '',
          pwd: config.pwd || '',
          // LED settings
          ledMode: config.ledMode,
          ledBrightness: config.ledBrightness,
          ledColor: config.ledColor,
          ledPreset: config.ledPreset,
          ledSpeed: config.ledSpeed,
          ledFadeColor: config.ledFadeColor,
          ledStrobeColor: config.ledStrobeColor,
          ledManualOverride: config.ledManualOverride,
          // Track settings
          tracksEnabled: config.tracksEnabled,
          selectedTrackId: config.selectedTrackId,
          // Webhook settings
          webhooksEnabled: config.webhooksEnabled,
          webhookCount: config.webhookCount,
          webhookIPs: config.webhookIPs,
          gateLEDsEnabled: config.gateLEDsEnabled,
          webhookRaceStart: config.webhookRaceStart,
          webhookRaceStop: config.webhookRaceStop,
          webhookLap: config.webhookLap,
          // Operation mode
          opMode: config.opMode
        })
      })
      .then(response => response.json())
      .then(data => {
        console.log('Config imported:', data);
        
        // Apply all client-side settings
        // Theme
        if (config.theme) {
          localStorage.setItem('theme', config.theme);
        }
        // Audio settings
        if (config.lapFormat) {
          localStorage.setItem('lapFormat', config.lapFormat);
        }
        if (config.selectedVoice) {
          localStorage.setItem('selectedVoice', config.selectedVoice);
        }
        if (config.ttsEngine) {
          localStorage.setItem('ttsEngine', config.ttsEngine);
        }
        // Pilot frontend settings
        if (config.pilotColor) {
          localStorage.setItem('pilotColor', config.pilotColor);
        }
        // LED settings
        if (config.ledPreset !== undefined) {
          const ledPresetSelect = document.getElementById('ledPreset');
          if (ledPresetSelect) ledPresetSelect.value = config.ledPreset;
        }
        if (config.ledSolidColor) {
          const ledSolidColor = document.getElementById('ledSolidColor');
          if (ledSolidColor) ledSolidColor.value = config.ledSolidColor;
        }
        if (config.ledFadeColor) {
          const ledFadeColor = document.getElementById('ledFadeColor');
          if (ledFadeColor) ledFadeColor.value = config.ledFadeColor;
        }
        if (config.ledStrobeColor) {
          const ledStrobeColor = document.getElementById('ledStrobeColor');
          if (ledStrobeColor) ledStrobeColor.value = config.ledStrobeColor;
        }
        if (config.ledSpeed !== undefined) {
          const ledSpeed = document.getElementById('ledSpeed');
          if (ledSpeed) ledSpeed.value = config.ledSpeed;
        }
        if (config.ledManualOverride !== undefined) {
          const ledManualOverride = document.getElementById('ledManualOverride');
          if (ledManualOverride) ledManualOverride.checked = config.ledManualOverride;
        }
        // Battery monitoring
        if (config.batteryMonitoring !== undefined) {
          const batteryToggle = document.getElementById('batteryMonitorToggle');
          if (batteryToggle) batteryToggle.checked = config.batteryMonitoring;
        }
        
        alert('Configuration imported successfully! Page will reload.');
        // Reload config to update UI
        setTimeout(() => location.reload(), 500);
      })
      .catch(error => {
        console.error('Error importing config:', error);
        alert('Error importing configuration');
      });
    } catch (error) {
      console.error('Error parsing config JSON:', error);
      alert('Invalid configuration file');
    }
  };
  reader.readAsText(file);
  input.value = ''; // Reset file input
}

// Calibration Wizard
let wizardState = {
  recording: false,
  data: [],
  markers: [], // Array of {index, lap: 1|2|3} - only peaks!
  currentLap: 1,
  chart: null,
  calculatedEnter: 0,
  calculatedExit: 0
};

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
    calculatedEnter: 0,
    calculatedExit: 0
  };

  // Show modal and recording screen
  document.getElementById('calibrationWizardModal').style.display = 'flex';
  document.getElementById('wizardRecording').style.display = 'block';
  document.getElementById('wizardMarking').style.display = 'none';
  document.getElementById('wizardResults').style.display = 'none';

  // Start recording
  fetch('/calibration/start', { method: 'POST', signal: wizardAbortController.signal })
    .then(async (response) => {
      if (!response.ok) {
        const t = await response.text().catch(() => '');
        throw new Error(`POST /calibration/start failed: HTTP ${response.status} ${response.statusText} ${t}`);
      }
      // server returns JSON but don’t *require* parsing
      await response.text().catch(() => '');
      wizardState.recording = true;
      wizardRecordingLoop();
    })
    .catch(error => {
      if (error?.name === 'AbortError') return;
      console.error('Error starting calibration wizard:', error);
      alert('Error starting calibration wizard');
      closeCalibrationWizard();
    });
}


async function fetchCalibrationMeta(signal) {
  const resp = await fetch(`/calibration/data?limit=0`, { signal });
  if (!resp.ok) {
    const t = await resp.text().catch(() => '');
    throw new Error(`GET /calibration/data?limit=0 failed: HTTP ${resp.status} ${resp.statusText} ${t}`);
  }
  return await resp.json(); // { total: N }
}

async function fetchCalibrationPage(offset, limit, signal) {
  const resp = await fetch(`/calibration/data?offset=${offset}&limit=${limit}`, { signal });
  if (!resp.ok) {
    const t = await resp.text().catch(() => '');
    throw new Error(`GET /calibration/data page failed: HTTP ${resp.status} ${resp.statusText} ${t}`);
  }
  return await resp.json(); // { total, offset, limit, count, data:[...] }
}

async function fetchAllCalibrationData(signal) {
  const meta = await fetchCalibrationMeta(signal);
  const total = meta.total || 0;

  const all = [];
  let offset = 0;

  while (offset < total) {
    const page = await fetchCalibrationPage(offset, CALIBRATION_PAGE_SIZE, signal);
    if (Array.isArray(page.data) && page.data.length) {
      all.push(...page.data);
      offset += page.data.length;
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
      document.getElementById('wizardSampleCount').textContent = `Samples: ${meta.total || 0}`;
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

async function stopCalibrationWizard() {
  wizardState.recording = false;

  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }

  try {
    const resp = await fetch('/calibration/stop', { method: 'POST', signal: wizardAbortController?.signal });

    if (!resp.ok) {
      const t = await resp.text().catch(() => '');
      throw new Error(`POST /calibration/stop failed: HTTP ${resp.status} ${resp.statusText} ${t}`);
    }

    // Consume body safely (may be empty in some builds)
    await resp.text().catch(() => '');

    // Fetch recorded data (paged)
    const { total, data } = await fetchAllCalibrationData();
    console.log('Calibration data received:', total, 'samples');
    wizardState.data = data;

    if (!Array.isArray(wizardState.data) || wizardState.data.length < 10) {
      alert('Not enough data recorded. Please try again with at least 3 clear gate passes.');
      closeCalibrationWizard();
      return;
    }

    enterCalibrationOverviewModeFromWizard();

    document.getElementById('wizardRecording').style.display = 'none';
    document.getElementById('wizardMarking').style.display = 'block';

    drawWizardChart();
  } catch (error) {
    console.error('Error stopping calibration wizard:', error);
    alert('Error processing calibration data');
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

// Find up to N prominent peaks that are well separated
function detectTopPeaks(values, desiredCount = 3) {
  const n = values.length;
  if (n < 20) return [];

  const minVal = Math.min(...values);
  const maxVal = Math.max(...values);
  const range = maxVal - minVal;
  if (range <= 0) return [];

  // Candidate must be a local maximum and above a prominence threshold
  const threshold = minVal + range * 0.55; // adjust if needed
  const candidates = [];

  // Local maxima check radius
  const r = 6;
  for (let i = r; i < n - r; i++) {
    const v = values[i];
    if (v < threshold) continue;

    let isMax = true;
    for (let k = 1; k <= r; k++) {
      if (values[i - k] > v || values[i + k] > v) {
        isMax = false;
        break;
      }
    }
    if (isMax) {
      candidates.push({ index: i, value: v });
    }
  }

  if (candidates.length === 0) return [];

  // Sort by peak height desc
  candidates.sort((a, b) => b.value - a.value);

  // Enforce minimum separation between selected peaks
  const minSep = Math.max(40, Math.floor(n * 0.12)); // 12% of series or 40 samples
  const chosen = [];

  for (const c of candidates) {
    if (chosen.length >= desiredCount) break;
    const tooClose = chosen.some(p => Math.abs(p.index - c.index) < minSep);
    if (!tooClose) chosen.push(c);
  }

  // If we still don't have enough, relax separation a bit and try again
  if (chosen.length < desiredCount) {
    const relaxedSep = Math.max(25, Math.floor(n * 0.08));
    for (const c of candidates) {
      if (chosen.length >= desiredCount) break;
      const tooClose = chosen.some(p => Math.abs(p.index - c.index) < relaxedSep);
      if (!tooClose) chosen.push(c);
    }
  }

  // Return indices sorted in time order
  return chosen
    .slice(0, desiredCount)
    .sort((a, b) => a.index - b.index)
    .map(p => p.index);
}

function autoPopulateWizardPeaksIfEmpty(rawRssi, smoothedRssi) {
  // Only run once per wizard run (only when no markers exist)
  if (wizardState.markers.length !== 0) return;

  // We use smoothed data to pick visually obvious peaks, but markers refer to raw indexes
  const peakIdx = detectTopPeaks(smoothedRssi, 3);

  if (peakIdx.length === 3) {
    wizardState.markers = peakIdx.map((idx, i) => ({ index: idx, lap: i + 1 }));
    wizardState.currentLap = 4; // done
    updateWizardStatus('Auto-detected 3 peaks. Undo or click to adjust, then "Calculate Thresholds".');

    // Enable buttons
    document.getElementById('wizardUndoButton').disabled = false;
    document.getElementById('wizardCalculateButton').disabled = false;

    console.log('[Wizard] Auto-peaks:', wizardState.markers.map(m => ({ lap: m.lap, index: m.index, rssi: rawRssi[m.index] })));
  } else {
    // If we couldn't confidently find 3 peaks, keep manual workflow
    updateWizardStatus(`Mark Peak ${wizardState.currentLap}`);
    console.log('[Wizard] Auto-peak detection found', peakIdx.length, 'peaks; leaving manual.');
  }
}


function drawWizardChart() {
  const canvas = document.getElementById('wizardChart');
  const ctx = canvas.getContext('2d');
  
  // Set canvas size
  canvas.width = canvas.offsetWidth;
  canvas.height = 400;
  
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
  
  // Draw peak markers
  wizardState.markers.forEach(marker => {
    const x = padding + (marker.index / (wizardState.data.length - 1)) * chartWidth;
    const rssi = wizardState.data[marker.index].rssi;
    const y = height - padding - ((rssi - minRssi) / rssiRange) * chartHeight;
    
    ctx.fillStyle = '#ff5555';
    ctx.beginPath();
    ctx.arc(x, y, 8, 0, 2 * Math.PI);
    ctx.fill();
    
    // Draw label
    ctx.fillStyle = '#ffffff';
    ctx.font = '14px Arial';
    ctx.textAlign = 'center';
    ctx.fillText(`Peak ${marker.lap}`, x, y - 12);
  });
  
  // Add click listener
  canvas.onclick = function(event) {
    const rect = canvas.getBoundingClientRect();
    const clickX = event.clientX - rect.left;
    
    // Convert click position to data index
    const dataIndex = Math.round(((clickX - padding) / chartWidth) * (wizardState.data.length - 1));
    
    if (dataIndex >= 0 && dataIndex < wizardState.data.length) {
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

function calculateThresholds() {
  if (wizardState.markers.length !== 3) {
    alert('Please mark all 3 peaks before calculating thresholds');
    return;
  }

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  function windowMin(a, b) {
    a = Math.max(0, Math.min(a, wizardState.data.length - 1));
    b = Math.max(0, Math.min(b, wizardState.data.length - 1));
    if (b < a) [a, b] = [b, a];
    let m = Infinity;
    for (let i = a; i <= b; i++) m = Math.min(m, wizardState.data[i].rssi);
    return m;
  }

  function windowMax(a, b) {
    a = Math.max(0, Math.min(a, wizardState.data.length - 1));
    b = Math.max(0, Math.min(b, wizardState.data.length - 1));
    if (b < a) [a, b] = [b, a];
    let m = -Infinity;
    for (let i = a; i <= b; i++) m = Math.max(m, wizardState.data[i].rssi);
    return m;
  }

  // Markers in time order
  const markers = [...wizardState.markers].sort((a, b) => a.index - b.index);

  const allRssiValues = wizardState.data.map(d => d.rssi);
  const globalBaseline = Math.min(...allRssiValues);

  // --- Tunables aligned to your goals ---
  const PEAK_HALF_WINDOW = 20;      // re-find true peak near click
  const EDGE_GUARD = 10;            // exclude immediate peak area
  const VALLEY_SEARCH_SPAN = 180;   // valley window between peaks
  const MIN_AMPLITUDE = 15;         // if peak-valley too small, fall back

  // Enter near peak: 0.75..0.90 is typical. Higher = closer to peak.
  const ENTER_FRAC_HIGH = 0.90;

  // Exit close to enter: fraction of amplitude below enter.
  // Smaller = closer (less hysteresis), but risk chatter/false exit.
  const EXIT_DELTA_FRAC = 0.08;

  // Minimum absolute gap between enter and exit (in RSSI units)
  const MIN_GAP = 10;

  // Keep exit above valley by at least this margin to avoid dip/noise chatter
  const VALLEY_MARGIN = 4;

  // Clamp thresholds into UI range
  const MIN_RSSI = 50;
  const MAX_RSSI = 255;

  // 1) Recompute peaks near each marker
  const peakInfo = markers.map(m => {
    const idx = m.index;
    const peak = windowMax(idx - PEAK_HALF_WINDOW, idx + PEAK_HALF_WINDOW);
    return { idx, peak };
  });

  // 2) Valleys between peaks (and pre/post)
  function valleyBetween(i, j) {
    const left = peakInfo[i].idx;
    const right = peakInfo[j].idx;
    const start = left + EDGE_GUARD;
    const end = right - EDGE_GUARD;
    if (end <= start) return globalBaseline;

    const mid = Math.floor((left + right) / 2);
    const a = Math.max(start, mid - Math.floor(VALLEY_SEARCH_SPAN / 2));
    const b = Math.min(end, mid + Math.floor(VALLEY_SEARCH_SPAN / 2));
    return windowMin(a, b);
  }

  const preValley = windowMin(0, Math.max(0, peakInfo[0].idx - EDGE_GUARD));
  const postValley = windowMin(Math.min(wizardState.data.length - 1, peakInfo[2].idx + EDGE_GUARD), wizardState.data.length - 1);
  const v01 = valleyBetween(0, 1);
  const v12 = valleyBetween(1, 2);

  const localValleys = [
    Math.min(preValley, v01),
    Math.min(v01, v12),
    Math.min(v12, postValley)
  ].map(v => Math.max(v, globalBaseline));

  // 3) Compute per-pass enter/exit candidates
  const enterCandidates = [];
  const exitCandidates = [];

  for (let i = 0; i < 3; i++) {
    const peak = peakInfo[i].peak;
    const valley = localValleys[i];
    const amp = peak - valley;

    if (amp < MIN_AMPLITUDE) {
      // Fallback: use global range but still keep "enter high" and "exit close"
      const globalRange = (Math.max(...allRssiValues) - globalBaseline) || 1;
      const enter = Math.round(globalBaseline + globalRange * 0.80);
      const exit = Math.max(globalBaseline + VALLEY_MARGIN, enter - Math.max(MIN_GAP, Math.round(globalRange * 0.10)));
      enterCandidates.push(enter);
      exitCandidates.push(exit);
      continue;
    }

    // Enter near peak but still tied to local valley+amplitude
    const enter = Math.round(valley + amp * ENTER_FRAC_HIGH);

    // Exit close to enter (small hysteresis), but not allowed to drop into valley noise
    const delta = Math.max(MIN_GAP, Math.round(amp * EXIT_DELTA_FRAC));
    const exit = Math.max(valley + VALLEY_MARGIN, enter - delta);

    enterCandidates.push(enter);
    exitCandidates.push(exit);
  }

  // 4) Choose final thresholds for consistency across all 3 passes
  // Enter: pick the LOWEST of the "high" enters so all three passes can hit it.
  let calculatedEnter = Math.min(...enterCandidates);

  // Exit: keep it as CLOSE as possible to enter, but ensure it will still be hit reliably.
  // Using min(exitCandidates) makes it easiest to drop below on all 3 passes.
  let calculatedExit = Math.min(...exitCandidates);

  // Ensure exit is close to enter (but still below)
  calculatedExit = Math.min(calculatedExit, calculatedEnter - MIN_GAP);

  // Guardrails
  calculatedExit = Math.max(globalBaseline + VALLEY_MARGIN, calculatedExit);
  calculatedEnter = Math.max(calculatedExit + MIN_GAP, calculatedEnter);

  // Clamp
  calculatedEnter = clamp(calculatedEnter, MIN_RSSI, MAX_RSSI);
  calculatedExit = clamp(calculatedExit, MIN_RSSI, MAX_RSSI);

  wizardState.calculatedEnter = calculatedEnter;
  wizardState.calculatedExit = calculatedExit;

  console.log('[Wizard] peaks:', peakInfo.map(p => p.peak));
  console.log('[Wizard] valleys:', localValleys);
  console.log('[Wizard] enterCandidates:', enterCandidates, 'exitCandidates:', exitCandidates);
  console.log('[Wizard] final:', { enter: calculatedEnter, exit: calculatedExit, globalBaseline });

  // Show results
  document.getElementById('wizardMarking').style.display = 'none';
  document.getElementById('wizardResults').style.display = 'block';
  document.getElementById('calculatedEnterRssi').textContent = calculatedEnter;
  document.getElementById('calculatedExitRssi').textContent = calculatedExit;
}

function applyCalculatedThresholds() {
  // Apply to sliders
  if (enterRssiInput) { enterRssiInput.value = wizardState.calculatedEnter; updateEnterRssi(enterRssiInput, wizardState.calculatedEnter); }
  if (exitRssiInput)  { exitRssiInput.value  = wizardState.calculatedExit;  updateExitRssi(exitRssiInput,  wizardState.calculatedExit);  }
  
  // Save to backend
  saveConfig();
  
  // Close wizard
  closeCalibrationWizard();
  
  // Show success message
  //alert('Calibration thresholds applied! You can now fine-tune them manually if needed.');
}

function cancelCalibrationWizard() {
  // stop loop immediately
  wizardState.recording = false;
  if (wizardRecordingTimerId) {
    clearTimeout(wizardRecordingTimerId);
    wizardRecordingTimerId = null;
  }

  // Tell firmware to stop (best effort), then close
  fetch('/calibration/stop', { method: 'POST' })
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

function onFilterModeChange() {
  const sel = document.getElementById('filterModeSelect');
  const row = document.getElementById('besselHzRow');
  if (!sel || !row) return;
  // Show Bessel cutoff selector only when V2 is active
  row.style.display = parseInt(sel.value, 10) === 1 ? '' : 'none';
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

  // Safe to close → clear staged state
  stagedDirty = false;
  stagedConfig = {};
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
  
  // Update nav items
  const navItems = document.querySelectorAll('.settings-nav-item');
  navItems.forEach(item => {
    item.classList.remove('active');
  });
  
  // Add active class to clicked nav item
  event?.target?.closest('.settings-nav-item')?.classList.add('active');
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
  pollDebugLogs();
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
  } else if (line.includes('RX5808 frequency verified properly')) {
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
// Distance Tracking Functions
// ============================================

function startDistancePolling() {
  // Poll distance every 5 seconds during race (less aggressive)
  if (distancePollingInterval) {
    clearInterval(distancePollingInterval);
  }
  
  // Initial fetch
  fetchDistance();
  
  distancePollingInterval = setInterval(() => {
    fetchDistance();
    // Also update the display to refresh the estimated distance
    updateDistanceDisplay();
  }, 5000);
  
  // Also update display more frequently (every 100ms) for smoother estimation
  if (!window.distanceDisplayInterval) {
    window.distanceDisplayInterval = setInterval(() => {
      updateDistanceDisplay();
    }, 100);
  }
}

function stopDistancePolling() {
  if (distancePollingInterval) {
    clearInterval(distancePollingInterval);
    distancePollingInterval = null;
  }
  if (window.distanceDisplayInterval) {
    clearInterval(window.distanceDisplayInterval);
    window.distanceDisplayInterval = null;
  }
}

function fetchDistance() {
  fetch('/timer/distance', {
    signal: AbortSignal.timeout(3000) // 3 second timeout
  })
    .then(response => {
      if (!response.ok) throw new Error('Distance fetch failed');
      return response.json();
    })
    .then(data => {
      currentTrackId = data.trackId || 0;
      currentTrackName = data.trackName || '';
      trackLapLength = data.trackDistance || 0.0;
      currentTotalDistance = data.totalDistance || 0;
      currentDistanceRemaining = data.distanceRemaining || 0;
      updateDistanceDisplay();
    })
    .catch(error => {
      // Silently fail - don't spam console or cause issues
      if (error.name !== 'TimeoutError' && error.name !== 'AbortError') {
        console.warn('Distance fetch error:', error.message);
      }
    });
}

function updateDistanceDisplay() {
  // Update lap counter to include current lap time and per-lap distance
  const lapCounter = document.getElementById('lapCounter');
  if (!lapCounter) return;
  
  // Base lap counter text
  let lapText = '';
  if (maxLaps === 0) {
    lapText = `Lap ${Math.max(0, lapNo)}`;
  } else {
    lapText = `Lap ${Math.max(0, lapNo)} / ${maxLaps}`;
  }
  
  // Calculate current lap time (time since last lap or race start)
  if (lapTimerStartMs > 0) {
    const currentLapMs = Date.now() - lapTimerStartMs;
    const minutes = Math.floor(currentLapMs / 60000);
    const seconds = Math.floor((currentLapMs % 60000) / 1000);
    const centiseconds = Math.floor((currentLapMs % 1000) / 10);
    
    const m = minutes < 10 ? "0" + minutes : minutes;
    const s = seconds < 10 ? "0" + seconds : seconds;
    const ms = centiseconds < 10 ? "0" + centiseconds : centiseconds;
    const lapTimeText = `${m}:${s}:${ms}s`;
    
    lapText += ` - ${lapTimeText}`;
  }
  
  // Add per-lap distance if track is selected and race is running
  if (currentTrackId && trackLapLength > 0) {
    // Estimate distance travelled in current lap using time extrapolation
    let estimatedDistance = 0;
    if (currentLapStartTime > 0 && lastCompletedLapTime > 0) {
      const currentLapElapsed = Date.now() - currentLapStartTime;
      const progress = Math.min(currentLapElapsed / lastCompletedLapTime, 1.0);
      estimatedDistance = progress * trackLapLength;
    }
    
    lapText += ` - ${estimatedDistance.toFixed(1)}/${trackLapLength.toFixed(1)}m`;
  }
  
  lapCounter.textContent = lapText;
}

// ============================================
// Track Management Functions
// ============================================

let currentEditTrackId = null;
let allTracks = [];

function setTracksUI(enabled) {
  const tracksContent = document.getElementById('tracksContent');
  if (tracksContent) {
    tracksContent.style.display = enabled ? 'block' : 'none';
  }
  if (enabled) {
    loadTracks();
  }
}

function toggleTracksEnabled(enabled, opts = {}) {
  const save = (opts.save !== undefined) ? !!opts.save : true;

  setTracksUI(enabled);

  if (!save) return;

  stageConfig('tracksEnabled', enabled ? 1 : 0);
}

function loadTracks() {
  fetch('/tracks')
    .then(response => response.json())
    .then(data => {
      allTracks = data.tracks || [];
      displayTracks();
      updateTrackSelect();
    })
    .catch(error => console.error('Error loading tracks:', error));
}

function displayTracks() {
  const tracksList = document.getElementById('tracksList');
  if (!tracksList) return;
  
  if (allTracks.length === 0) {
    tracksList.innerHTML = '<p style="color: var(--secondary-color); text-align: center;">No tracks created yet</p>';
    return;
  }
  
  let html = '<div style="display: flex; flex-direction: column; gap: 12px;">';
  
  allTracks.forEach(track => {
    html += `
      <div style="padding: 12px; background-color: var(--bg-secondary); border-radius: 8px; border-left: 4px solid var(--accent-color);">
        <div style="display: flex; justify-content: between; align-items: center; margin-bottom: 8px;">
          <div style="flex: 1;">
            <div style="font-weight: bold; font-size: 16px; margin-bottom: 4px;">${track.name}</div>
            <div style="font-size: 14px; color: var(--secondary-color);">
              ${track.distance} meters${track.tags ? ' • ' + track.tags : ''}
            </div>
          </div>
          <div style="display: flex; gap: 8px;">
            <button onclick="editTrack(${track.trackId})" style="padding: 6px 12px; font-size: 14px;">Edit</button>
            <button onclick="deleteTrack(${track.trackId})" style="padding: 6px 12px; font-size: 14px; background-color: var(--danger-color);">Delete</button>
          </div>
        </div>
        ${track.notes ? `<div style="font-size: 13px; color: var(--secondary-color); margin-top: 8px;">${track.notes}</div>` : ''}
      </div>
    `;
  });
  
  html += '</div>';
  tracksList.innerHTML = html;
}

function updateTrackSelect() {
  const selectEl = document.getElementById('selectedTrack');
  if (!selectEl) return;
  
  // Save current selection
  const currentSelection = selectEl.value;
  
  // Clear and repopulate
  selectEl.innerHTML = '<option value="0">None</option>';
  
  allTracks.forEach(track => {
    const option = document.createElement('option');
    option.value = track.trackId;
    option.textContent = `${track.name} (${track.distance}m)`;
    selectEl.appendChild(option);
  });
  
  // Restore selection if it still exists
  if (currentSelection) {
    selectEl.value = currentSelection;
  }
}

function openCreateTrackModal() {
  currentEditTrackId = null;
  document.getElementById('trackModalTitle').textContent = 'Create Track';
  document.getElementById('trackName').value = '';
  document.getElementById('trackDistance').value = '';
  document.getElementById('trackTags').value = '';
  document.getElementById('trackNotes').value = '';
  document.getElementById('saveTrackBtn').textContent = 'Save Track';
  document.getElementById('trackModal').style.display = 'flex';
}

function editTrack(trackId) {
  const track = allTracks.find(t => t.trackId === trackId);
  if (!track) return;
  
  currentEditTrackId = trackId;
  document.getElementById('trackModalTitle').textContent = 'Edit Track';
  document.getElementById('trackName').value = track.name;
  document.getElementById('trackDistance').value = track.distance;
  document.getElementById('trackTags').value = track.tags || '';
  document.getElementById('trackNotes').value = track.notes || '';
  document.getElementById('saveTrackBtn').textContent = 'Update Track';
  document.getElementById('trackModal').style.display = 'flex';
}

function closeTrackModal() {
  document.getElementById('trackModal').style.display = 'none';
  currentEditTrackId = null;
}

function saveTrack() {
  const name = document.getElementById('trackName').value.trim();
  const distance = parseFloat(document.getElementById('trackDistance').value);
  const tags = document.getElementById('trackTags').value.trim();
  const notes = document.getElementById('trackNotes').value.trim();
  
  if (!name) {
    alert('Please enter a track name');
    return;
  }
  
  if (!distance || distance <= 0) {
    alert('Please enter a valid distance');
    return;
  }
  
  const trackData = {
    trackId: currentEditTrackId || Math.floor(Date.now() / 1000),
    name: name,
    distance: distance,
    tags: tags,
    notes: notes
  };
  
  const endpoint = currentEditTrackId ? '/tracks/update' : '/tracks/create';
  
  fetch(endpoint, {
    method: 'POST',
    headers: {
      'Accept': 'application/json',
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(trackData)
  })
  .then(response => response.json())
  .then(data => {
    if (data.status === 'OK') {
      closeTrackModal();
      loadTracks();
    } else {
      alert('Error saving track');
    }
  })
  .catch(error => {
    console.error('Error saving track:', error);
    alert('Error saving track');
  });
}

function deleteTrack(trackId) {
  if (!confirm('Are you sure you want to delete this track?')) {
    return;
  }
  
  fetch('/tracks/delete', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: 'trackId=' + trackId
  })
  .then(response => response.json())
  .then(data => {
    if (data.status === 'OK') {
      loadTracks();
    } else {
      alert('Error deleting track');
    }
  })
  .catch(error => {
    console.error('Error deleting track:', error);
    alert('Error deleting track');
  });
}

function selectTrack() {
  const selectEl = document.getElementById('selectedTrack');
  const trackId = parseInt(selectEl.value, 10) || 0;

  // Existing UI/runtime behavior
  if (trackId === 0) {
    trackLapLength = 0;
    currentTrackId = 0;
    currentTrackName = '';
  } else {
    const selectedTrack = allTracks.find(t => t.trackId === trackId);
    if (selectedTrack) {
      trackLapLength = selectedTrack.distance;
      currentTrackId = selectedTrack.trackId;
      currentTrackName = selectedTrack.name;
      console.log(`Track selected: ${selectedTrack.name}, length: ${trackLapLength}m`);
    }
  }

  // Keep existing backend behavior
  fetch('/tracks/select', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'trackId=' + trackId
  })
  .then(response => response.json())
  .catch(err => console.error('Error selecting track:', err));

  // Stage config
  stageConfig('selectedTrackId', trackId);
}

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

  // If you have UI sections to show/hide, keep doing it here...

  // Stage-only
  autoSaveConfig();
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
        <button onclick="removeWebhook('${ip}')" style="padding: 6px 10px; font-size: 14px; background-color: var(--danger-color);">Remove</button>
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

// Load tracks when settings modal opens
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
          $(ledBrightnessInput).parent().find('span').text(config.ledBrightness);
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
        // Sync voice enabled state from device so it's never out of date
        if (config.voiceEnabled !== undefined) {
          audioEnabled = !!config.voiceEnabled;
          updateVoiceButtons();
        }
        
        // Theme setting
        const themeSelect = document.getElementById('themeSelect');
        if (themeSelect && config.theme) {
          themeSelect.value = config.theme;
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
        
        // Tracks
        const tracksEnabled = config.tracksEnabled === 1;
        const tracksCheckbox = document.getElementById('tracksEnabled');
        if (tracksCheckbox) {
          tracksCheckbox.checked = tracksEnabled;
          toggleTracksEnabled(tracksEnabled, { save: false });
        }
        
        // Set selected track
        if (config.selectedTrackId) {
          const selectEl = document.getElementById('selectedTrack');
          if (selectEl) {
            selectEl.value = config.selectedTrackId;
          }
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

        // Signal processing mode
        const filterModeSelect = document.getElementById('filterModeSelect');
        if (filterModeSelect && config.filterMode !== undefined) {
          filterModeSelect.value = config.filterMode;
        }
        const besselHzSelect = document.getElementById('besselHzSelect');
        if (besselHzSelect && config.besselHz !== undefined) {
          besselHzSelect.value = config.besselHz;
        }
        onFilterModeChange();

        // Detection parameters
        const enterHoldInput = document.getElementById('enterHoldInput');
        if (enterHoldInput && config.enterHoldSamples !== undefined) {
          enterHoldInput.value = config.enterHoldSamples;
        }
        const exitConfirmInput = document.getElementById('exitConfirmInput');
        if (exitConfirmInput && config.exitConfirmSamples !== undefined) {
          exitConfirmInput.value = config.exitConfirmSamples;
        }

        // Multi-node settings
        const nodeModeSelect = document.getElementById('nodeModeSelect');
        if (nodeModeSelect && config.nodeMode !== undefined) {
          nodeModeSelect.value = String(config.nodeMode);
          onNodeModeChange();
        }
        if (config.nodeMode !== undefined) {
          mnNodeMode = config.nodeMode;
          onRaceTabOpen();  // switch Race tab to master view if needed
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
        }

        const devModeToggle = document.getElementById('devModeToggle');
        const devModeLabel  = document.getElementById('devModeLabel');
        if (devModeToggle && config.devMode !== undefined) {
          devModeToggle.checked = !!config.devMode;
          mnDevMode = !!config.devMode;
          if (devModeLabel) devModeLabel.textContent = config.devMode ? 'On' : 'Off';
          const _pnd = document.getElementById('pilotNameDisplay');
          if (_pnd) _pnd.style.cursor = config.devMode ? 'pointer' : 'default';
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
let mnCurrentNodes       = [];     // latest node list from multiNodeState SSE / polling
let mnRaceTimerIntervalId = null;
let mnRaceStartMs         = 0;
let mnMasterConnected     = false; // client: true when registered with master
let mnClientPollInterval  = null;  // client: timer for periodic /api/mode polls
let mnDevMode             = false; // dev mode: click pilot name to simulate a lap
let mnStatusSSID          = '';    // SSID string for the race status bar

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

  if (mode === 2) {
    // Switching TO client — restore any previously entered SSID
    if (clientFields) clientFields.style.display = '';
    if (ssidInput && _savedMasterSSID) ssidInput.value = _savedMasterSSID;
  } else {
    // Switching AWAY from client — save the current SSID value before hiding
    if (ssidInput && ssidInput.value.trim()) _savedMasterSSID = ssidInput.value.trim();
    if (clientFields) clientFields.style.display = 'none';
  }

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
      await new Promise(r => setTimeout(r, 300));
      window.location.href = targetURL;
      return;
    } catch (_) {
      dots = (dots + 1) % 4;
      statusLine.textContent = 'Waiting for device' + '.'.repeat(dots + 1);
      await new Promise(r => setTimeout(r, 500));
    }
  }
  // Hard cap — redirect anyway
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
    resultsEl.innerHTML = '';
    nets.forEach(n => {
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;align-items:center;gap:8px;margin-bottom:4px;';
      row.innerHTML = `<span style="flex:1;">${n.ssid}</span>
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
  }, 2000);
}

function mnStopClientPoll() {
  if (mnClientPollInterval) { clearInterval(mnClientPollInterval); mnClientPollInterval = null; }
}

async function mnRefreshNodes() {
  try {
    const r = await fetch('/api/multinode/nodes');
    if (!r.ok) return;
    const data = await r.json();
    const nodes = data.nodes || [];
    mnRenderNodes(nodes);
    mnRenderRaceTab(nodes);
  } catch (_) {}
}

// Format milliseconds as a TTS-friendly string ("3 minutes 49 point 4 6" or "49 point 5 0").
function formatMsSpeak(ms) {
  if (!ms || ms <= 0) return '0';
  const m   = Math.floor(ms / 60000);
  const s   = Math.floor((ms % 60000) / 1000);
  const cs  = Math.floor((ms % 1000) / 10);
  const dec = cs.toString().padStart(2, '0').split('').join(' ');
  if (m > 0) return `${m} minute${m !== 1 ? 's' : ''} ${s} point ${dec}`;
  return `${s} point ${dec}`;
}

// Format milliseconds as M:SS.cs (e.g. 1:23.45) for the race view.
function formatMsRace(ms) {
  if (!ms || ms <= 0) return '—';
  const m  = Math.floor(ms / 60000);
  const s  = Math.floor((ms % 60000) / 1000);
  const cs = Math.floor((ms % 1000) / 10);
  const ss = s.toString().padStart(m > 0 ? 2 : 1, '0');
  const cc = cs.toString().padStart(2, '0');
  return m > 0 ? `${m}:${ss}.${cc}` : `${ss}.${cc}`;
}

// Update the status bar above the race clock based on current multi-node mode.
function mnUpdateRaceStatusBar() {
  const bar  = document.getElementById('mn-race-status-bar');
  const text = document.getElementById('mn-race-status-text');
  if (!bar || !text) return;
  if (mnNodeMode === 1) {
    bar.style.display = '';
    text.textContent  = `Multi-Node (Master) — ${mnStatusSSID || 'FPVRaceOne'}`;
  } else if (mnNodeMode === 2) {
    bar.style.display = '';
    if (!mnMasterConnected) {
      text.textContent = `Multi-Node (Client) — Disconnected from ${mnStatusSSID || ''}`.trim();
    } else if (mnMyNodeId > 0) {
      text.textContent = `Multi-Node (Client) ${mnMyNodeId} — Connected to ${mnStatusSSID || ''}`.trim();
    } else {
      text.textContent = 'Multi-Node (Client) — Searching for master node...';
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
    // Start polling so node data loads immediately without visiting Multi-Node tab first
    if (!mnPollingInterval) mnInitTab();
  } else {
    singleView.style.display  = '';
    masterView.style.display  = 'none';
  }
  mnUpdateRaceStatusBar();
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
    // Master's own timer — inject into local lapTimes and update display
    const lapSec = lapMs / 1000;
    if (typeof addLap === 'function') addLap(lapSec.toFixed(2));
    mnRenderRaceTab(mnCurrentNodes);
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
  // For nodeId === 0, addLap() handles TTS.
  // For nodeId !== 0, the multiNodeLap SSE listener handles TTS (avoids double-speak).
}

// Build the master's own pilot entry from local lap data so it appears in the grid.
function _mnMasterEntry() {
  const nameEl      = document.getElementById('pname');
  const colorEl     = document.getElementById('pilotColor');
  const name        = nameEl ? nameEl.value : 'Master';
  const colorHex    = colorEl ? colorEl.value : '#0080FF';
  const colorInt    = parseInt(colorHex.replace('#', ''), 16) || 0x0080FF;
  // lapTimes is seconds (float); convert to the same shape as client laps
  // index 0 = gate 1 (lapNumber 0), index 1 = lap 1 (lapNumber 1), etc.
  const laps = (Array.isArray(window.lapTimes) ? window.lapTimes : [])
    .map((t, i) => ({ lapNumber: i, lapTimeMs: Math.round(t * 1000) }));
  const lapCount  = laps.length;
  const totalMs   = laps.reduce((s, l) => s + l.lapTimeMs, 0);
  const avgMs     = lapCount > 0 ? totalMs / lapCount : 0;
  const fastestMs = lapCount > 0 ? Math.min(...laps.map(l => l.lapTimeMs)) : Infinity;
  return { nodeId: 0, pilotName: name, pilotColor: colorInt,
           online: true, running: false, quitEarly: false, isMaster: true,
           laps, lapCount, totalMs, avgMs, fastestMs };
}

// Render the master race tab: RotorHazard-style summary table + per-pilot lap columns.
// Always shows 8 slots: 1 master + 7 client slots (populated or empty).
function mnRenderRaceTab(nodes) {
  mnCurrentNodes = nodes;

  const masterView = document.getElementById('master-race-view');
  if (!masterView || masterView.style.display === 'none') return;

  const b1 = document.getElementById('mnStartRaceBtnMain'); if (b1) b1.disabled = mnRaceRunning;
  const b2 = document.getElementById('mnStopRaceBtnMain');  if (b2) b2.disabled = !mnRaceRunning;

  const container = document.getElementById('mn-race-container');
  if (!container) return;

  // Build full 8-slot list: master first, then 7 client slots (filled or empty)
  const master = _mnMasterEntry();
  const clients = Array.from({ length: 7 }, (_, i) => {
    const nodeId = i + 1;
    const found  = nodes.find(n => n.nodeId === nodeId);
    if (found) {
      const laps      = Array.isArray(found.laps) ? found.laps.slice().sort((a, b) => a.lapNumber - b.lapNumber) : [];
      const lapCount  = laps.length;
      const totalMs   = laps.reduce((s, l) => s + (l.lapTimeMs || 0), 0);
      const avgMs     = lapCount > 0 ? totalMs / lapCount : 0;
      const fastestMs = lapCount > 0 ? Math.min(...laps.map(l => l.lapTimeMs || Infinity)) : Infinity;
      return { ...found, laps, lapCount, totalMs, avgMs, fastestMs };
    }
    return { nodeId, pilotName: null, online: false, empty: true, lapCount: 0, laps: [], totalMs: 0, avgMs: 0, fastestMs: Infinity };
  });

  const allSlots    = [master, ...clients];
  const activeSlots = allSlots.filter(n => !n.empty);

  // Rank active pilots for summary table: most laps first, then fastest total
  const ranked = [...activeSlots].sort((a, b) => {
    if (b.lapCount !== a.lapCount) return b.lapCount - a.lapCount;
    if (a.lapCount === 0) return 0;
    return a.totalMs - b.totalMs;
  });

  const globalFastestMs = activeSlots.filter(p => p.lapCount > 0)
    .reduce((best, p) => Math.min(best, p.fastestMs), Infinity);

  // ── Summary leaderboard table ──────────────────────────────────
  let html = '<table class="mn-leaderboard"><thead><tr>';
  html += '<th></th><th>Pilot</th><th>Laps</th><th>Total</th><th>Avg</th><th>Fastest</th>';
  html += '</tr></thead><tbody>';

  ranked.forEach((n, i) => {
    const color    = '#' + ((n.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0');
    const callsign = n.pilotName || (n.isMaster ? 'Master' : 'Node ' + n.nodeId);
    const hostTag  = n.isMaster ? ' <span class="mn-card-badge" style="float:right;margin-left:8px;">Host</span>' : '';
    let badge = '';
    if      (n.quitEarly)                           badge = ' <span class="mn-status-dnf">DNF</span>';
    else if (n.running && mnRaceRunning)             badge = ' <span class="mn-status-racing">Racing</span>';
    else if (n.running && !mnRaceRunning && !n.isMaster) badge = ' <span class="mn-status-solo">Solo</span>';
    const isFastPilot = isFinite(globalFastestMs) && n.fastestMs === globalFastestMs;
    const statusDotColor = n.online !== false ? '#4caf50' : '#f44336';
    const editBtn = n.isMaster ? '' :
      `<button class="mn-edit-btn" onclick="mnOpenPilotModal(${n.nodeId})" title="Edit pilot" style="margin-right:5px;vertical-align:middle;">
        <svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zm17.71-10.21a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
      </button>`;
    html += `<tr>
      <td class="mn-lb-rank">${i + 1}</td>
      <td class="mn-lb-pilot">
        ${hostTag}${editBtn}<span style="display:inline-block;width:10px;height:10px;border-radius:50%;background:${statusDotColor};margin-right:6px;vertical-align:middle;" title="${n.online !== false ? 'Connected' : 'Offline'}"></span>${callsign}${badge}
      </td>
      <td class="mn-lb-mono">${n.lapCount}</td>
      <td class="mn-lb-mono">${n.lapCount > 0 ? formatMsRace(n.totalMs)            : '—'}</td>
      <td class="mn-lb-mono">${n.lapCount > 0 ? formatMsRace(Math.round(n.avgMs)) : '—'}</td>
      <td class="mn-lb-mono${isFastPilot ? ' mn-lb-fastest' : ''}">${n.lapCount > 0 ? formatMsRace(n.fastestMs) : '—'}</td>
    </tr>`;
  });
  html += '</tbody></table>';

  // ── Per-pilot lap columns — always 8 slots ─────────────────────
  html += '<div class="mn-pilot-cards">';
  allSlots.forEach(n => {
    const color    = '#' + ((n.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0');
    const callsign = n.pilotName || (n.isMaster ? 'Master' : 'Node ' + n.nodeId);

    if (n.empty) {
      html += `<div class="mn-pilot-card mn-pilot-card-empty">
        <div class="mn-pilot-card-header mn-pilot-card-header-empty">Slot ${n.nodeId}</div>
        <div class="mn-pilot-card-laps mn-card-empty-label">Not connected</div>
      </div>`;
      return;
    }

    const devAttr = mnDevMode ? ` onclick="mnDevTriggerLap(${n.nodeId},${n.lapCount+1},'${callsign.replace(/'/g,"\\'")}');" title="Dev: click to simulate lap" style="background:${color};cursor:pointer;"` : ` style="background:${color};"`;
    const isRacing = n.isMaster ? mnRaceRunning : n.running;
    const cardEditBtn = n.isMaster ? '' : `<button class="mn-edit-btn" onclick="event.stopPropagation();mnOpenPilotModal(${n.nodeId})" title="Edit pilot" style="margin-right:5px;vertical-align:middle;"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zm17.71-10.21a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg></button>`;
    html += `<div class="mn-pilot-card"><div class="mn-pilot-card-header"${devAttr}>${cardEditBtn}${callsign}${n.isMaster ? ' <span class="mn-card-badge" style="background:rgba(0,0,0,0.25);">Host</span>' : ''}${isRacing ? ' <span class="mn-card-badge" style="background:rgba(0,160,0,0.5);">Racing</span>' : ''}${mnDevMode ? ' <span class="mn-card-badge" style="background:rgba(0,0,0,0.3);font-size:9px;">TAP</span>' : ''}</div><div class="mn-pilot-card-laps">`;

    // Solo race in progress (client running outside a master race)
    if (n.running && !mnRaceRunning && !n.isMaster) {
      html += `<div class="mn-card-solo-label">Solo race in progress</div>`;
    } else if (n.lapCount === 0) {
      html += `<div class="mn-card-lap" style="justify-content:center;color:var(--secondary-color);padding:10px;">—</div>`;
    } else {
      let cumMs = n.totalMs;
      for (let i = n.laps.length - 1; i >= 0; i--) {
        const l      = n.laps[i];
        const isBest = l.lapTimeMs === n.fastestMs;
        html += `<div class="mn-card-lap${isBest ? ' mn-card-lap-best' : ''}">
          <span class="mn-card-lap-num">${l.lapNumber}</span>
          <div class="mn-card-lap-times">
            <span class="mn-card-lap-time">${formatMsRace(l.lapTimeMs)}${isBest ? ' ★' : ''}</span>
            <span class="mn-card-lap-cumul">${formatMsRace(cumMs)}</span>
          </div>
        </div>`;
        cumMs -= l.lapTimeMs;
      }
    }
    html += '</div></div>';
  });
  html += '</div>';

  container.innerHTML = html;
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

function mnOpenPilotModal(nodeId) {
  _mnModalNodeId = nodeId;
  const node = mnCurrentNodes.find(n => n.nodeId === nodeId);
  const name     = node ? (node.pilotName || '') : '';
  const colorHex = node ? '#' + ((node.pilotColor || 0x0080FF) >>> 0).toString(16).padStart(6, '0') : '#0080ff';
  document.getElementById('mnPilotModalTitle').textContent = 'Node ' + nodeId + (name ? ' \u2014 ' + name : '');
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
  document.getElementById('mnPilotModal').style.display = 'flex';
  setTimeout(() => document.getElementById('mnPilotModalName').focus(), 50);
}

function mnClosePilotModal() {
  document.getElementById('mnPilotModal').style.display = 'none';
  _mnModalNodeId = null;
}

function mnClosePilotModalBackdrop(evt) {
  if (evt.target === document.getElementById('mnPilotModal')) mnClosePilotModal();
}

async function mnSavePilotModal() {
  if (!_mnModalNodeId) return;
  const nodeId     = _mnModalNodeId;
  const name       = document.getElementById('mnPilotModalName').value.trim();
  const colorHex   = document.getElementById('mnPilotModalColor').value;
  const pilotColor = parseInt(colorHex.replace('#', ''), 16) || 0x0080FF;
  const bandSel    = document.getElementById('mnPilotModalBand');
  const chanSel    = document.getElementById('mnPilotModalChannel');
  const bandIndex  = bandSel  ? parseInt(bandSel.value)  : 0;
  const chanIndex  = chanSel  ? parseInt(chanSel.value) - 1 : 0;  // value is 1-based, store 0-based
  const freq       = (freqLookup[bandIndex] || [])[chanIndex] || 0;
  mnClosePilotModal();
  try {
    const r = await fetch('/api/multinode/editPilot', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ nodeId, pilotName: name, pilotColor, band: bandIndex, chan: chanIndex, freq })
    });
    if (!r.ok) alert('Update failed \u2014 is the client device reachable?');
    mnRefreshNodes();
  } catch (e) { console.error('editPilot failed', e); alert('Update failed'); }
}

async function mnRemoveFromModal() {
  if (!_mnModalNodeId) return;
  const nodeId   = _mnModalNodeId;
  const node     = mnCurrentNodes.find(n => n.nodeId === nodeId);
  const callsign = node ? (node.pilotName || 'Node ' + nodeId) : 'Node ' + nodeId;
  mnClosePilotModal();
  await mnRemoveNode(nodeId, callsign);
}

async function mnRemoveNode(nodeId, callsign) {
  if (!confirm(`Remove "${callsign}" from slot ${nodeId}?\n\nThe pilot can reconnect and will be assigned the next available slot.`)) return;
  try {
    await fetch('/api/multinode/removeNode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ nodeId })
    });
    mnRefreshNodes();
  } catch (e) { console.error('removeNode failed', e); }
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
    const callsign    = n.pilotName || 'Node ' + n.nodeId;
    let runDotCls, runLabel;
    if      (n.quitEarly) { runDotCls = 'mn-run-dot dnf';     runLabel = 'DNF'; }
    else if (n.running)   { runDotCls = 'mn-run-dot running'; runLabel = 'Racing'; }
    else                  { runDotCls = 'mn-run-dot stopped'; runLabel = 'Stopped'; }

    html += `<div class="mn-node-card ${onlineCls}">
      <div class="mn-node-header">
        <span class="mn-node-dot" style="background:${colorHex}"></span>
        <strong>${callsign}</strong>
        <span class="mn-node-status-pill ${n.online ? 'mn-pill-online' : 'mn-pill-offline'}">${n.online ? 'Online' : 'Offline'}</span>
        <span style="margin-left:auto;font-size:12px;color:var(--secondary-color);">Node ${n.nodeId}</span>
        <button class="mn-edit-btn" style="margin-left:6px;" onclick="mnOpenPilotModal(${n.nodeId})" title="Edit pilot">
          <svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zm17.71-10.21a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
        </button>
      </div>
      <div class="mn-node-race-status">
        <span class="${runDotCls}"></span>
        <span class="mn-run-label">${runLabel}</span>
      </div>
      <div style="font-size:13px;">
        ${n.pilotName || 'Node ' + n.nodeId} — Laps: <strong>${n.lapCount || 0}</strong>
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

function _mnFormatRaceTimer(ms) {
  const totalS = Math.floor(ms / 1000);
  const m      = Math.floor(totalS / 60);
  const s      = totalS % 60;
  const cs     = Math.floor((ms % 1000) / 10);
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}:${String(cs).padStart(2, '0')}s`;
}

function _mnStartTimer(offsetMs = 0) {
  mnRaceStartMs = Date.now() - offsetMs;
  mnRaceTimerIntervalId = setInterval(() => {
    const el = document.getElementById('mn-race-timer');
    if (el) el.textContent = _mnFormatRaceTimer(Date.now() - mnRaceStartMs);
  }, 100);
}

function _mnStopTimer() {
  clearInterval(mnRaceTimerIntervalId);
  mnRaceTimerIntervalId = null;
}

async function mnStartRace() {
  // Disable button immediately and lock out double-presses
  ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) { b.disabled = true; b.classList.add('active'); } });

  // Signal clients to flash their Start button during countdown
  try { await fetch('/api/multinode/race/prearm', { method: 'POST' }); } catch (_) {}

  // Unlock AudioContext (iOS/Safari)
  if (typeof beepAudioContext !== 'undefined' && beepAudioContext && beepAudioContext.state === 'suspended') {
    await beepAudioContext.resume().catch(() => {});
  }

  // Set race-running flag before the countdown so heartbeats arriving during
  // the countdown don't trigger the "solo race in progress" label.
  mnRaceRunning = true;

  // Countdown announcement — identical sequence to single-pilot startRace()
  await _raceCountdown("Arm your quads");

  // Beep, start master timer, then broadcast GO to clients — all at the same moment.
  beep(1, 1, "square");
  beep(500, 880, "square");
  if (navigator.vibrate) navigator.vibrate(500);

  _mnStartTimer();
  try { await fetch('/api/multinode/race/start', { method: 'POST' }); }
  catch (e) { console.error('[MULTINODE] Start race failed:', e); }
  ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) b.classList.remove('active'); });
  ['mnStopRaceBtn',  'mnStopRaceBtnMain' ].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = false; });
  mnRenderRaceTab(mnCurrentNodes);
}

async function mnStopRace() {
  try {
    await fetch('/api/multinode/race/stop', { method: 'POST' });
    mnRaceRunning = false;
    _mnStopTimer();
    ['mnStartRaceBtn', 'mnStartRaceBtnMain'].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = false; });
    ['mnStopRaceBtn',  'mnStopRaceBtnMain' ].forEach(id => { const b = document.getElementById(id); if (b) b.disabled = true;  });
    mnRenderRaceTab(mnCurrentNodes);
  } catch (e) { console.error('[MULTINODE] Stop race failed:', e); }
}

async function mnClearRace() {
  if (!confirm('Clear all race data for all pilots?')) return;
  try {
    await fetch('/api/multinode/clearLaps', { method: 'POST' });
    clearLaps();           // clear master's own local laps
    await mnRefreshNodes(); // re-render with empty node data from server
  } catch (e) { console.error('[MULTINODE] Clear race failed:', e); }
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
  }

  fetch('/api/mode').then(r => r.ok ? r.json() : null).then(data => {
    if (!data) return;
    mnNodeMode        = data.nodeMode       || 0;
    mnMyNodeId        = data.myNodeId       || 0;
    mnMasterConnected = data.masterConnected || false;
    if (data.nodeMode !== 2) mnStatusSSID = data.ssid || '';
    if (typeof audioAnnouncer !== 'undefined') audioAnnouncer.sdAvailable = !!data.sdAvailable;
    // Apply devMode from firmware config so race tab renders correctly on first load.
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
    onRaceTabOpen();
    // Client mode: start the connection-state poll (mnInitTab is only called for master)
    if (data.nodeMode === 2) {
      if (!mnStatusSSID) {
        fetch('/config').then(r2 => r2.ok ? r2.json() : null).then(cfg => {
          if (cfg) mnStatusSSID = cfg.masterSSID || '';
          mnUpdateRaceStatusBar();
        }).catch(() => {});
      }
      mnStartClientPoll();
    }
  }).catch(e => console.warn('[Race] /api/mode fetch failed:', e));

  // Keep paused scanner overlays correct on resize/rotation
  window.addEventListener('resize', () => {
    clearTimeout(window.__rssiResizeT);
    window.__rssiResizeT = setTimeout(() => {
      rescalePausedScannerFrameToCanvas();
    }, 100);
  });

});