#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <Arduino.h>

// Abstract transport interface for sending events to clients
// Supports multiple simultaneous transports (WiFi, USB, etc.)
class TransportInterface {
   public:
    virtual ~TransportInterface() {}
    
    // Send lap time event to all connected clients
    virtual void sendLapEvent(uint32_t lapTimeMs, uint8_t peakRssi = 0) = 0;
    
    // Send RSSI value to all connected clients (if streaming enabled)
    virtual void sendRssiEvent(uint8_t rssi) = 0;
    
    // Send race state event (started/stopped)
    virtual void sendRaceStateEvent(const char* state) = 0;
    
    // Check if transport is ready/connected
    virtual bool isConnected() = 0;
    
    // Update transport (process incoming data, etc.)
    virtual void update(uint32_t currentTimeMs) = 0;
};

// Transport manager - manages multiple transports and broadcasts to all
class TransportManager {
   public:
    TransportManager() : transportCount(0) {}
    
    // Register a transport
    void addTransport(TransportInterface* transport) {
        if (transportCount < MAX_TRANSPORTS) {
            transports[transportCount++] = transport;
        }
    }
    
    // Broadcast lap event to all transports
    void broadcastLapEvent(uint32_t lapTimeMs, uint8_t peakRssi = 0) {
        for (uint8_t i = 0; i < transportCount; i++) {
            if (transports[i] && transports[i]->isConnected()) {
                transports[i]->sendLapEvent(lapTimeMs, peakRssi);
            }
        }
    }
    
    // Broadcast RSSI event to all transports
    void broadcastRssiEvent(uint8_t rssi) {
        for (uint8_t i = 0; i < transportCount; i++) {
            if (transports[i] && transports[i]->isConnected()) {
                transports[i]->sendRssiEvent(rssi);
            }
        }
    }
    
    // Broadcast race state event to all transports
    void broadcastRaceStateEvent(const char* state) {
        for (uint8_t i = 0; i < transportCount; i++) {
            if (transports[i] && transports[i]->isConnected()) {
                transports[i]->sendRaceStateEvent(state);
            }
        }
    }
    
    // Update all transports
    void updateAll(uint32_t currentTimeMs) {
        for (uint8_t i = 0; i < transportCount; i++) {
            if (transports[i]) {
                transports[i]->update(currentTimeMs);
            }
        }
    }
    
   private:
    static const uint8_t MAX_TRANSPORTS = 4;  // WiFi + USB + future transports
    TransportInterface* transports[MAX_TRANSPORTS];
    uint8_t transportCount;
};

#endif  // TRANSPORT_H
