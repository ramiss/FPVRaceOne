#include "nodemode.h"

// Firmware version strings (for RotorHazard identification).
// NOTE: handleReadCommand() copies these into a 32-byte response buffer and
// stops at 31 characters, so each string INCLUDING its prefix must fit in 31.
// "FIRMWARE_VERSION: " is 18, leaving 13 for the value -- keep it short or the
// version silently loses its tail on the wire.
const char *firmwareVersionString = "FIRMWARE_VERSION: FPVRaceOne1.0";
const char *firmwareBuildDateString = "FIRMWARE_BUILDDATE: " __DATE__;
const char *firmwareBuildTimeString = "FIRMWARE_BUILDTIME: " __TIME__;
const char *firmwareProcTypeString = "FIRMWARE_PROCTYPE: ESP32-C6";

NodeMode::NodeMode() {
    // Constructor
}

void NodeMode::begin(LapTimer* timer, Config* config) {
    _timer = timer;
    _config = config;
    
    // Load initial settings from config
    _settings.vtxFreq = config->getFrequency();
    _settings.enterAtLevel = config->getEnterRssi();
    _settings.exitAtLevel = config->getExitRssi();
    
    // Initialize lap counter
    _lastPass.lap = 0;
    _lastPass.timestamp = 0;
    _lastPass.rssiPeak = 0;
    
    // Start race automatically in node mode (RotorHazard expects node to be always active)
    _timer->start();
}

void NodeMode::process() {
    // Handle serial communication
    handleSerialInput();
    
    // Check for new laps and update state
    if (_timer->isLapAvailable()) {
        uint32_t lapTime = _timer->getLapTime();
        uint8_t rssiPeak = _timer->getRssi();
        
        // Update internal state for RotorHazard
        _lastPass.timestamp = millis();
        _lastPass.rssiPeak = rssiPeak;
        _lastPass.lap++;
    }
}

void NodeMode::handleSerialInput() {
    int iterCount = 0;
    
    // Process all available bytes (USB CDC can have multiple bytes queued)
    while (Serial.available() > 0) {
        uint8_t inByte = Serial.read();
        
        if (_currentCommand == 0) {
            // New command byte
            _currentCommand = inByte;
            
            // Validate command before processing
            if (!isValidCommand(_currentCommand)) {
                // Invalid command (likely baud rate mismatch or garbage) - ignore silently
                _currentCommand = 0;
                continue;
            }
            
            if (_currentCommand >= 0x51) {
                // Write command - expect payload + checksum
                _expectedPayloadSize = getPayloadSize(_currentCommand);
                _payloadIndex = 0;
            } else {
                // Read command - handle immediately
                handleReadCommand(_currentCommand);
                _currentCommand = 0;
            }
        } else {
            // Collecting payload bytes for write command
            _payloadBuffer[_payloadIndex++] = inByte;
            
            if (_payloadIndex >= _expectedPayloadSize + 1) {  // +1 for checksum
                // Complete message received - verify checksum
                uint8_t checksum = 0;
                for (uint8_t i = 0; i < _expectedPayloadSize; i++) {
                    checksum += _payloadBuffer[i];  // SUM, not XOR (RotorHazard uses sum)
                }
                
                if (_payloadBuffer[_expectedPayloadSize] == (checksum & 0xFF)) {
                    // Valid message - handle it
                    handleWriteCommand(_currentCommand, _payloadBuffer, _expectedPayloadSize);
                }
                
                // Reset state
                _currentCommand = 0;
                _payloadIndex = 0;
            }
        }
        
        // Prevent blocking (process up to 100 bytes per call)
        if (++iterCount > 100) return;
    }
}

void NodeMode::handleReadCommand(uint8_t cmd) {
    uint8_t response[32];
    uint8_t len = 0;
    
    switch (cmd) {
        case READ_ADDRESS:
            response[len++] = 0x08;  // Node address
            break;
            
        case READ_FREQUENCY:
            response[len++] = (_settings.vtxFreq >> 8) & 0xFF;
            response[len++] = _settings.vtxFreq & 0xFF;
            break;
            
        case READ_LAP_STATS:
            // Return simple lap stats (not used much, but RotorHazard may query it)
            response[len++] = 0;  // placeholder
            break;
            
        case READ_LAP_PASS_STATS: {
            // Current time (4 bytes, big-endian)
            uint32_t now = millis();
            response[len++] = (now >> 24) & 0xFF;
            response[len++] = (now >> 16) & 0xFF;
            response[len++] = (now >> 8) & 0xFF;
            response[len++] = now & 0xFF;
            
            // Current RSSI (1 byte)
            response[len++] = _timer->getRssi();
            
            // Last pass timestamp (4 bytes, big-endian)
            uint32_t ts = _lastPass.timestamp;
            response[len++] = (ts >> 24) & 0xFF;
            response[len++] = (ts >> 16) & 0xFF;
            response[len++] = (ts >> 8) & 0xFF;
            response[len++] = ts & 0xFF;
            
            // Last pass peak RSSI (1 byte)
            response[len++] = _lastPass.rssiPeak;
            
            // Flags (1 byte) - 0 for now
            response[len++] = 0;
            
            // Lap counter (2 bytes, big-endian)
            response[len++] = (_lastPass.lap >> 8) & 0xFF;
            response[len++] = _lastPass.lap & 0xFF;
            
            // Nadir RSSI (1 byte) - not used in FPVGate
            response[len++] = 0;
            break;
        }
        
        case READ_LAP_EXTREMUMS:
            // Not implemented yet - return zeros
            for (int i = 0; i < 8; i++) {
                response[len++] = 0;
            }
            break;
            
        case READ_RHFEAT_FLAGS:
            response[len++] = (RHFEAT_FLAGS_VALUE >> 8) & 0xFF;
            response[len++] = RHFEAT_FLAGS_VALUE & 0xFF;
            break;
            
        case READ_REVISION_CODE:
            response[len++] = NODE_API_LEVEL;
            break;
            
        case READ_NODE_RSSI_PEAK:
            response[len++] = _lastPass.rssiPeak;
            break;
            
        case READ_NODE_RSSI_NADIR:
            response[len++] = 0;  // Not tracked
            break;
            
        case READ_ENTER_AT_LEVEL:
            response[len++] = _settings.enterAtLevel;
            break;
            
        case READ_EXIT_AT_LEVEL:
            response[len++] = _settings.exitAtLevel;
            break;
            
        case READ_TIME_MILLIS: {
            uint32_t now = millis();
            response[len++] = (now >> 24) & 0xFF;
            response[len++] = (now >> 16) & 0xFF;
            response[len++] = (now >> 8) & 0xFF;
            response[len++] = now & 0xFF;
            break;
        }
        
        case READ_MULTINODE_COUNT:
            response[len++] = 1;  // Single node
            break;
            
        case READ_CURNODE_INDEX:
            response[len++] = _nodeIndex;
            break;
            
        case READ_NODE_SLOTIDX:
            response[len++] = _slotIndex;
            break;
            
        case READ_FW_VERSION: {
            // Send firmware version string
            const char* verStr = firmwareVersionString;
            while (*verStr && len < 31) {
                response[len++] = *verStr++;
            }
            break;
        }
        
        case READ_FW_BUILDDATE: {
            // Send build date string
            const char* dateStr = firmwareBuildDateString;
            while (*dateStr && len < 31) {
                response[len++] = *dateStr++;
            }
            break;
        }
        
        case READ_FW_BUILDTIME: {
            // Send build time string
            const char* timeStr = firmwareBuildTimeString;
            while (*timeStr && len < 31) {
                response[len++] = *timeStr++;
            }
            break;
        }
        
        case READ_FW_PROCTYPE: {
            // Send processor type string
            const char* procStr = firmwareProcTypeString;
            while (*procStr && len < 31) {
                response[len++] = *procStr++;
            }
            break;
        }
        
        default:
            // Unknown command - no response
            return;
    }
    
    if (len > 0) {
        sendResponse(response, len);
    }
}

void NodeMode::handleWriteCommand(uint8_t cmd, uint8_t* payload, uint8_t len) {
    switch (cmd) {
        case WRITE_FREQUENCY: {
            if (len >= 2) {
                uint16_t freq = (payload[0] << 8) | payload[1];
                
                // Validate frequency range (5645-5945 MHz)
                if (freq >= 5645 && freq <= 5945) {
                    _settings.vtxFreq = freq;
                    _config->setFrequency(freq);
                    // Frequency change will be handled by RX5808 update in main loop
                }
            }
            break;
        }
        
        case WRITE_ENTER_AT_LEVEL: {
            if (len >= 1) {
                _settings.enterAtLevel = payload[0];
                _config->setEnterRssi(payload[0]);
            }
            break;
        }
        
        case WRITE_EXIT_AT_LEVEL: {
            if (len >= 1) {
                _settings.exitAtLevel = payload[0];
                _config->setExitRssi(payload[0]);
            }
            break;
        }
        
        case SEND_STATUS_MESSAGE:
            // Status message received - acknowledge but don't do anything
            break;
            
        case FORCE_END_CROSSING:
            // Force end crossing - not implemented yet
            break;
            
        case WRITE_CURNODE_INDEX:
            if (len >= 1) {
                _nodeIndex = payload[0];
            }
            break;
            
        case JUMP_TO_BOOTLOADER:
            // Jump to bootloader - not implemented (dangerous to restart in node mode)
            break;
            
        default:
            // Unknown write command - ignore
            break;
    }
}

void NodeMode::sendResponse(uint8_t* data, uint8_t len) {
    // Send response data
    Serial.write(data, len);
    Serial.flush();  // Ensure data is sent immediately
}

uint8_t NodeMode::getPayloadSize(uint8_t cmd) {
    switch (cmd) {
        case WRITE_FREQUENCY: return 2;
        case WRITE_ENTER_AT_LEVEL: return 1;
        case WRITE_EXIT_AT_LEVEL: return 1;
        case SEND_STATUS_MESSAGE: return 2;
        case FORCE_END_CROSSING: return 1;
        case WRITE_CURNODE_INDEX: return 1;
        default: return 0;
    }
}

bool NodeMode::isValidCommand(uint8_t cmd) {
    // Validate command is in the valid RotorHazard protocol range
    // This prevents responding to garbage data (e.g., wrong baud rate, boot messages)
    switch (cmd) {
        // Valid READ commands
        case READ_ADDRESS:
        case READ_FREQUENCY:
        case READ_LAP_STATS:
        case READ_LAP_PASS_STATS:
        case READ_LAP_EXTREMUMS:
        case READ_RHFEAT_FLAGS:
        case READ_REVISION_CODE:
        case READ_NODE_RSSI_PEAK:
        case READ_NODE_RSSI_NADIR:
        case READ_ENTER_AT_LEVEL:
        case READ_EXIT_AT_LEVEL:
        case READ_TIME_MILLIS:
        case READ_MULTINODE_COUNT:
        case READ_CURNODE_INDEX:
        case READ_NODE_SLOTIDX:
        case READ_FW_VERSION:
        case READ_FW_BUILDDATE:
        case READ_FW_BUILDTIME:
        case READ_FW_PROCTYPE:
        // Valid WRITE commands
        case WRITE_FREQUENCY:
        case WRITE_ENTER_AT_LEVEL:
        case WRITE_EXIT_AT_LEVEL:
        case SEND_STATUS_MESSAGE:
        case FORCE_END_CROSSING:
        case WRITE_CURNODE_INDEX:
        case JUMP_TO_BOOTLOADER:
            return true;
        default:
            return false;
    }
}
