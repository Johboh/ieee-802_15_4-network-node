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
  uint64_t magic = 0x56322abc24;
  double temperature;
  uint64_t magic2 = 0xdeadbeefaabbccdd;
};

Ieee802154NetworkNode _ieee802154_node({.gcm_encryption_key = gcm_encryption_key,
                                        .gcm_encryption_secret = gcm_encryption_secret,
                                        .pan_id = 0x1234});

void setup() {
  ApplicationMessage message = {
      .temperature = 25.2,
  };
  _ieee802154_node.sendMessage((uint8_t *)&message, sizeof(ApplicationMessage));

  esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
  esp_sleep_config_gpio_isolate();
  esp_sleep_cpu_retention_init();
  esp_deep_sleep_try_to_start();
}

void loop() {
  // Will never end up here.
}
