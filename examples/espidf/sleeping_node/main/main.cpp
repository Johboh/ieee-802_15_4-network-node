#include <Ieee802154NetworkNode.h>
#include <cstring>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LOG_TAG "c6-node"

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

extern "C" {
void app_main();
}

void app_main(void) {
  ESP_LOGI(LOG_TAG, "This device IEEE802.15.4 MAC: 0x%llx", _ieee802154_node.deviceMacAddress());

  ApplicationMessage message = {
      .temperature = 25.2,
  };
  _ieee802154_node.sendMessage((uint8_t *)&message, sizeof(ApplicationMessage));

  esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
  esp_sleep_config_gpio_isolate();
  esp_sleep_cpu_retention_init();
  esp_deep_sleep_try_to_start();

  // Will never reach here.
  while (1) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    fflush(stdout);
  }
}
