# FPVGate Desktop App Changelog

All notable changes to the FPVRaceOne Electron desktop application will be documented in this file.

## [1.3.3] - 2024-12-10

### Added
- **Application Menu** - Native menu bar with keyboard shortcuts
  - File menu: Open OSD (Ctrl+O), Refresh Connection (Ctrl+R), Exit (Alt+F4)
  - View menu: Toggle DevTools (F12), Toggle Fullscreen (F11)
  - Help menu: Documentation link, About dialog
- **OSD Window Support** - Native OSD overlay window
  - Open via menu (Ctrl+O) or IPC call
  - Transparent, frameless, always-on-top window
  - Perfect for streaming overlays
- **About Dialog** - Shows version and feature summary

### Changed
- **Version Bump** - Updated from v1.0.0 to v1.3.3 to match main project
- **Package Description** - Updated to reflect new features

### Inherited from Web Interface (v1.3.3)
Since the Electron app loads the web interface from the `data/` folder, it automatically inherits all web interface updates:

- **Modern Configuration UI** - Full-screen overlay modal with 6 organized sections
  - Lap & Announcer Settings
  - Pilot Info
  - LED Setup
  - WiFi & Connection
  - System Settings
  - Diagnostics
- **SSE Keepalive** - Automatic connection keepalive (WiFi mode only)
- **Mobile-Responsive** - Works on all screen sizes

### Inherited from Web Interface (v1.3.2)
- **WiFi Status Display** - Real-time WiFi connection status (WiFi mode only)
  - Shows AP mode with client count
  - Shows Station mode with signal strength
  - Note: Only functional in WiFi mode, gracefully fails in USB mode
- **Marshalling Mode** - Edit saved race data after completion
  - Add or remove laps from completed races
  - Real-time recalculation of race statistics
- **LED Settings Persistence** - All LED configuration persists across reboots
- **Improved Race History** - Individual race files with index for better performance

### Inherited from Web Interface (v1.3.1)
- **Enhanced Calibration Wizard** - Simplified 3-peak marking system
  - Only mark the 3 highest peaks (one per lap)
  - Automatic threshold calculation
  - Visual smoothing with 15-point moving average

### USB Mode Limitations
The following features require WiFi connection and won't work in USB mode:
- **WiFi Status Display** - Will show error or be hidden (API endpoint unavailable)
- **Firmware OTA Updates** - Requires WiFi, `/update` endpoint not accessible
- **SSE Keepalive** - Not applicable to USB serial communication

These limitations are expected and documented. All other features work identically in both USB and WiFi modes.

## [1.0.0] - 2024-12-04

### Initial Release
- **USB Serial CDC** - Direct USB connection for low-latency communication
  - Zero-latency local connection
  - Automatic USB/WiFi detection and switching
  - Auto-detection of ESP32 devices
- **Native Desktop App** - Windows, macOS, and Linux support
  - Native USB connectivity via node-serialport
  - Full feature parity with web interface
  - Bundled with all dependencies
- **WiFi Support** - Fallback to WiFi when USB unavailable
- **COM Port Selection** - Manual port selection for multiple devices
- **Transport Abstraction** - Unified API for WiFi and USB
  - Single codebase works over both protocols
  - Automatic transport selection

### Features from Web Interface (v1.3.0)
- **iOS/Safari Audio Support** - Full audio on all platforms
- **Mobile-Responsive Interface** - Optimized for all screen sizes
- **Vibration Feedback** - Race start vibration
- **OSD Overlay System** - Live race display for streaming
- **Cross-Device Race Storage** - SD card storage for shared access
- **Enhanced Race Management** - Tagging, naming, detailed analysis
- **Calibration Wizard** - Interactive RSSI threshold setup
- **Multi-Voice Support** - 4 ElevenLabs voices + PiperTTS
- **LED Presets** - 10 customizable LED effects
- **Self-Test Diagnostics** - Hardware and software verification
- **Config Import/Export** - Full configuration backup/restore
- **Race Import/Export** - Share race data between devices
