#include "webhook.h"
#include "debug.h"
#include <HTTPClient.h>
#include <WiFi.h>

WebhookManager::WebhookManager() : webhookCount(0), enabled(true), queueHead(0), queueTail(0), queueCount(0), lastWebhookMs(0) {
    memset(webhookIPs, 0, sizeof(webhookIPs));
    memset(requestQueue, 0, sizeof(requestQueue));
}

bool WebhookManager::addWebhook(const char* ip) {
    if (!ip || strlen(ip) == 0) {
        DEBUG("Invalid webhook IP\n");
        return false;
    }
    
    // Check if already exists
    for (uint8_t i = 0; i < webhookCount; i++) {
        if (strcmp(webhookIPs[i], ip) == 0) {
            DEBUG("Webhook IP already exists: %s\n", ip);
            return false;
        }
    }
    
    // Check max limit
    if (webhookCount >= MAX_WEBHOOKS) {
        DEBUG("Max webhooks reached (%d)\n", MAX_WEBHOOKS);
        return false;
    }
    
    // Copy IP to fixed buffer
    strncpy(webhookIPs[webhookCount], ip, 15);
    webhookIPs[webhookCount][15] = '\0';  // Ensure null termination
    webhookCount++;
    DEBUG("Webhook added: %s (total: %d)\n", ip, webhookCount);
    return true;
}

bool WebhookManager::removeWebhook(const char* ip) {
    for (uint8_t i = 0; i < webhookCount; i++) {
        if (strcmp(webhookIPs[i], ip) == 0) {
            // Shift remaining IPs down
            for (uint8_t j = i; j < webhookCount - 1; j++) {
                strcpy(webhookIPs[j], webhookIPs[j + 1]);
            }
            webhookCount--;
            memset(webhookIPs[webhookCount], 0, 16);  // Clear last slot
            DEBUG("Webhook removed: %s (remaining: %d)\n", ip, webhookCount);
            return true;
        }
    }
    DEBUG("Webhook not found: %s\n", ip);
    return false;
}

void WebhookManager::clearWebhooks() {
    webhookCount = 0;
    memset(webhookIPs, 0, sizeof(webhookIPs));
    DEBUG("All webhooks cleared\n");
}

uint8_t WebhookManager::getWebhookCount() const {
    return webhookCount;
}

const char* WebhookManager::getWebhookIP(uint8_t index) const {
    if (index < webhookCount) {
        return webhookIPs[index];
    }
    return nullptr;
}

void WebhookManager::setEnabled(bool en) {
    enabled = en;
    DEBUG("Webhooks %s\n", enabled ? "enabled" : "disabled");
}

bool WebhookManager::isEnabled() const {
    return enabled;
}

void WebhookManager::triggerLap() {
    if (!enabled) return;
    queueRequest("/Lap");
}

void WebhookManager::triggerGhostLap() {
    if (!enabled) return;
    queueRequest("/GhostLap");
}

void WebhookManager::triggerRaceStart() {
    if (!enabled) return;
    queueRequest("/RaceStart");
}

void WebhookManager::triggerRaceStop() {
    if (!enabled) return;
    queueRequest("/RaceStop");
}

void WebhookManager::triggerOff() {
    if (!enabled) return;
    queueRequest("/off");
}

void WebhookManager::triggerFlash() {
    if (!enabled) return;
    queueRequest("/flash");
}

void WebhookManager::queueRequest(const char* endpoint) {
    // Don't queue webhooks if WiFi isn't ready yet
    if (WiFi.status() != WL_CONNECTED) {
        DEBUG("Webhook skipped (WiFi not ready): %s\n", endpoint);
        return;
    }
    
    if (queueCount >= WEBHOOK_QUEUE_SIZE) {
        DEBUG("Webhook queue full, dropping request: %s\n", endpoint);
        return;
    }
    
    strncpy(requestQueue[queueTail].endpoint, endpoint, 31);
    requestQueue[queueTail].endpoint[31] = '\0';
    requestQueue[queueTail].timestamp = millis();
    
    queueTail = (queueTail + 1) % WEBHOOK_QUEUE_SIZE;
    queueCount++;
    
    DEBUG("Webhook queued: %s (queue: %d/%d)\n", endpoint, queueCount, WEBHOOK_QUEUE_SIZE);
}

void WebhookManager::process() {
    if (!enabled || queueCount == 0 || webhookCount == 0) {
        return;
    }
    
    // Rate limit: don't send more than once every 50ms to avoid overwhelming network
    uint32_t now = millis();
    if (now - lastWebhookMs < 50) {
        return;
    }
    
    processNextRequest();
    lastWebhookMs = now;
}

void WebhookManager::processNextRequest() {
    if (queueCount == 0) return;
    
    const char* endpoint = requestQueue[queueHead].endpoint;
    DEBUG("Processing webhook: %s\n", endpoint);
    
    sendToAll(endpoint);
    
    // Remove from queue
    queueHead = (queueHead + 1) % WEBHOOK_QUEUE_SIZE;
    queueCount--;
}

void WebhookManager::sendToAll(const char* endpoint) {
    for (uint8_t i = 0; i < webhookCount; i++) {
        sendWebhook(webhookIPs[i], endpoint);
    }
}

void WebhookManager::sendWebhook(const char* ip, const char* endpoint) {
    // Create HTTPClient on stack (safer than static)
    HTTPClient http;
    
    // Build URL in fixed buffer to avoid String allocation
    char url[64];
    snprintf(url, sizeof(url), "http://%s%s", ip, endpoint);
    
    // setTimeout() bounds only the READ phase; HTTPClient's _connectTimeout is
    // separate and defaults to 5000 ms.  Without the second call a webhook
    // target that goes offline costs 5 s inside parallelTask on EVERY lap,
    // not the WEBHOOK_TIMEOUT_MS this line appears to promise.
    http.setTimeout(WEBHOOK_TIMEOUT_MS);
    http.setConnectTimeout(WEBHOOK_TIMEOUT_MS);
    
    // Check WiFi is connected before attempting HTTP
    if (WiFi.status() != WL_CONNECTED) {
        DEBUG("Webhook skipped - WiFi not connected\n");
        return;
    }
    
    if (!http.begin(url)) {
        DEBUG("Webhook failed to begin: %s\n", url);
        return;
    }
    
    int httpCode = http.POST("");  // Empty POST body
    
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_ACCEPTED) {
            DEBUG("Webhook OK: %s (code: %d)\n", url, httpCode);
        } else {
            DEBUG("Webhook code %d: %s\n", httpCode, url);
        }
    } else {
        DEBUG("Webhook failed: %s\n", url);
    }
    
    http.end();
}
