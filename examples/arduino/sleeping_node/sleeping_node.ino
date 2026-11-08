// Copyright (c) Johan Böhlin
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Arduino.h>
#include <Ieee802154NetworkNode.h>

const uint64_t SLEEP_TIME_US = 1000LL * 1000LL * 15LL; // 15s

// Encyption key used for our own packet encryption (GCM).
// The key should be the same for both the host and the node.
const char gcm_encryption_key[] = "0123456789ABCDEF"; // Must be exact 16 bytes long. \0 does not count.

// Used to validate the integrity of the messages.
// The secret should be the same for both the host and the node.
const char gcm_encryption_secret[] = "01234567"; // Must be exact 8 bytes long. \0 does not count.

// Shared between node and host. Keep this in you own shared .h file.
struct __attribute__((packed)) ApplicationMessage {
  double temperature;
};

Ieee802154NetworkNode _ieee802154_node({
    .gcm_encryption_key = gcm_encryption_key,
    .gcm_encryption_secret = gcm_encryption_secret,
    .firmware_version = 0xabcd, // Get from build version
    .pan_id = 0x1234,
});

void setup() {
  Serial.begin(115200);

  char buffer[64];
  sprintf(buffer, "This device IEEE802.15.4 MAC: 0x%llx", _ieee802154_node.deviceMacAddress());
  Serial.println(buffer);

  ApplicationMessage message = {
      .temperature = 25.2,
  };
  auto result = _ieee802154_node.sendMessage((const uint8_t *)&message, sizeof(ApplicationMessage));
  if (result) {
    Serial.println("Message sent successfully (attempts: " + String(result.attempts) +
                   ", ACK RSSI: " + String(result.ack_rssi) + ", LQI: " + String(result.ack_lqi) + ")");
    if (result.timestamp) {
      Serial.println(" -- Received timestamp: " + String(*result.timestamp));
    }
    if (result.payload) {
      Serial.println(" -- Received payload (" + String(result.payload->size()) + " bytes)");
    }
  } else {
    Serial.println("Failed to send message (attempts: " + String(result.attempts) +
                   ", errors: " + String(result.errors) + ", last_error: " + String(result.last_error) + ")");
  }

  esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
  esp_sleep_config_gpio_isolate();
  esp_sleep_cpu_retention_init();
  esp_deep_sleep_try_to_start();
}

void loop() {
  // Will never end up here.
}
