#include "Ieee802154NetworkNode.h"
#include <Ieee802154NetworkShared.h>
#include <WiFiHelper.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <format>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <string>

Ieee802154NetworkNode::Ieee802154NetworkNode(Configuration configuration)
    : _ota_helper({
          .web_ota = {.enabled = false},
          .arduino_ota = {.enabled = false},
          .rollback_strategy = OtaHelper::RollbackStrategy::MANUAL,
      }),
      _ieee802154({.channel = 0, .pan_id = configuration.pan_id}), _nvs_storage("Ieee802154"),
      _configuration(configuration),
      _gcm_encryption(configuration.gcm_encryption_key, configuration.gcm_encryption_secret, false) {
  esp_log_level_set(OtaHelperLog::TAG, ESP_LOG_ERROR);
  esp_log_level_set(WiFiHelperLog::TAG, ESP_LOG_ERROR);
}

bool Ieee802154NetworkNode::sendMessage(std::vector<uint8_t> message) {
  return sendMessage(message.data(), message.size());
}

bool Ieee802154NetworkNode::sendMessage(uint8_t *message, uint8_t message_size) {
  std::scoped_lock lock(_send_mutex);

  if (!_nvs_initialized) {
    initializeNvs();
    _ota_helper.cancelRollback();
    _nvs_initialized = true;
  }

  bool initialize_nvs = false;
  _ieee802154.initialize(initialize_nvs);

  // Read channel and host address from NVS.
  uint8_t channel = 0;
  bool read_ok = _nvs_storage.readFromNVS(NVS_KEY_CHANNEL, channel);
  if (read_ok) {
    _ieee802154.setChannel(channel);
    read_ok = _nvs_storage.readFromNVS(NVS_KEY_HOST, _host_address);
  }
  if (read_ok) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Read channel %d and host 0x%llx from NVS", channel, _host_address);
  } else {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Failed to read channel and host from NVS");
  }

  // If we failed to load from NVS, go directly to disovery.
  if (!read_ok) {
    auto r = performDiscovery();
    if (!r) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Device discovery failed");
      teardown();
      return r;
    }
  }

  // Try sending application message once.
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "First attempt of sending message...");
  auto r = sendApplicationMessage(message, message_size);
  if (r) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "First attempt of sending message OK");

    r = requestData();
    if (!r) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Data request failed");
    }
    teardown();
    return r;
  } else {
    // Not good. Wait and try again.
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "First attempt of sending message failed");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Second attempt of sending message...");
    r = sendApplicationMessage(message, message_size);
  }

  if (r) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Second attempt of sending message OK");

    r = requestData();
    if (!r) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Data request failed");
    }
    teardown();
    return r;
  } else {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Second attempt of sending message failed");

    // Not good. Assume faulty host or channel.
    // Go into discovery mode.
    r = performDiscovery();
    if (!r) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Device discovery failed");
      teardown();
      return r;
    } else {
      // Discovery OK, try sending message.
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Third attempt of sending message (after discovery)...");
      r = sendApplicationMessage(message, message_size);
    }
  }

  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "End of sendMessage: %d", r);
  teardown();
  return r;
}

bool Ieee802154NetworkNode::sendApplicationMessage(uint8_t *message, uint8_t message_size) {
  auto wire_message_size = sizeof(Ieee802154NetworkShared::MessageV1) + message_size;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[wire_message_size]);
  Ieee802154NetworkShared::MessageV1 *wire_message =
      reinterpret_cast<Ieee802154NetworkShared::MessageV1 *>(buffer.get());
  wire_message->id = Ieee802154NetworkShared::MESSAGE_ID_MESSAGE;
  memcpy(wire_message->payload, message, message_size);

  auto encrypted = _gcm_encryption.encrypt(wire_message, wire_message_size);

  return _ieee802154.transmit(_host_address, encrypted.data(), encrypted.size());
}

bool Ieee802154NetworkNode::performDiscovery() {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "In device discovery");

  Ieee802154NetworkShared::DiscoveryRequestV1 discovery_request;

  EventGroupHandle_t event_group = xEventGroupCreate();
  _ieee802154.receive([&](Ieee802154::Message message) {
    auto decrypted = _gcm_encryption.decrypt(message.payload);
    uint8_t message_id = decrypted.data()[0];
    if (message_id == Ieee802154NetworkShared::MESSAGE_ID_DISCOVERY_RESPONSE_V1) {
      Ieee802154NetworkShared::DiscoveryResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::DiscoveryResponseV1 *>(decrypted.data());
      _ieee802154.setChannel(response->channel);
      _nvs_storage.writeToNVS(NVS_KEY_CHANNEL, response->channel);
      _host_address = message.source_address;
      _nvs_storage.writeToNVS(NVS_KEY_HOST, message.source_address);
      xEventGroupSetBits(event_group, 1);
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got disovery response with channel %d and host 0x%llx",
               response->channel, _host_address);
    } else {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Got unknown message %d while waiting for device discovery respose",
               message_id);
    }
  });

  // Discover on all channels.
  // Reduce number of retries.
  _ieee802154.setNumberOfDataFramesRetries(10);
  auto encrypted = _gcm_encryption.encrypt(&discovery_request, sizeof(Ieee802154NetworkShared::DiscoveryRequestV1));
  uint8_t channel = 11;
  for (; channel <= 26; ++channel) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Discovering on channel %d...", channel);
    _ieee802154.setChannel(channel);
    // Broadcast never emit ACKs.
    _ieee802154.broadcast(encrypted.data(), encrypted.size());
    /*if (r) {
      // Got ack.
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Got ACK for device request (in loop)");
      break;
    }*/
    // Also check if we got a message that we want.
    auto bits = xEventGroupWaitBits(event_group, 1, pdFALSE, pdFALSE, 1);
    auto found_host = ((bits & 1) != 0);
    if (found_host) {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got device response for device request (in loop)");
      break;
    }
  }

  if (channel > 26) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Sent device discovery on all channels without successful ACK");
  }

  // Wait for data with timeout.
  auto bits = xEventGroupWaitBits(event_group, 1, pdTRUE, pdFALSE, (1000 / portTICK_PERIOD_MS));
  auto found_host = ((bits & 1) != 0);

  if (!found_host) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Never received device discovery response");
  } else {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Host found during device discovery");
  }

  _ieee802154.receive({}); // Stop receiving.

  return found_host;
}
bool Ieee802154NetworkNode::requestData() {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Requesting data");

  auto result = _ieee802154.dataRequest(_host_address);

  if (result == Ieee802154::DataRequestResult::Failure) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Failed to request data");
    return false;
  }

  if (result == Ieee802154::DataRequestResult::NoDataAvailable) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- No data available.");
    return true;
  }

  // We have data. Wait for it.
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Data available, waiting");
  EventGroupHandle_t event_group = xEventGroupCreate();
  _ieee802154.receive([&](Ieee802154::Message message) {
    auto decrypted = _gcm_encryption.decrypt(message.payload);
    uint8_t message_id = decrypted.data()[0];
    switch (message_id) {
    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_TIMESTAMP_RESPONSE_V1: {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingTimestampResponseV1");
      Ieee802154NetworkShared::PendingTimestampResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingTimestampResponseV1 *>(decrypted.data());
      auto timestamp = response->timestamp;
      _pending_timestamp = timestamp;
      xEventGroupSetBits(event_group, 1);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_PAYLOAD_RESPONSE_V1: {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingPayloadResponseV1");
      _pending_payload = std::vector<uint8_t>(
          decrypted.begin() + sizeof(Ieee802154NetworkShared::PendingPayloadResponseV1), decrypted.end());
      xEventGroupSetBits(event_group, 1);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_FIRMWARE_WIFI_CREDENTIALS_RESPONSE_V1: {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingFirmwareWifiCredentialsResponseV1");
      Ieee802154NetworkShared::PendingFirmwareWifiCredentialsResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingFirmwareWifiCredentialsResponseV1 *>(decrypted.data());
      if (!_pending_firmware) {
        FirmwareUpdate empty_firmware_update;
        _pending_firmware = empty_firmware_update;
      }
      strncpy(_pending_firmware->wifi_ssid, response->wifi_ssid, sizeof(_pending_firmware->wifi_ssid));
      strncpy(_pending_firmware->wifi_password, response->wifi_password, sizeof(_pending_firmware->wifi_password));
      xEventGroupSetBits(event_group, 1);

      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_FIRMWARE_CHECKSUM_RESPONSE_V1: {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingFirmwareChecksumResponseV1");
      Ieee802154NetworkShared::PendingFirmwareChecksumResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingFirmwareChecksumResponseV1 *>(decrypted.data());
      if (!_pending_firmware) {
        FirmwareUpdate empty_firmware_update;
        _pending_firmware = empty_firmware_update;
      }
      strncpy(_pending_firmware->md5, response->md5, sizeof(_pending_firmware->md5));
      xEventGroupSetBits(event_group, 1);

      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_FIRMWARE_URL_RESPONSE_V1: {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingFirmwareUrlResponseV1");
      Ieee802154NetworkShared::PendingFirmwareUrlResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingFirmwareUrlResponseV1 *>(decrypted.data());
      if (!_pending_firmware) {
        FirmwareUpdate empty_firmware_update;
        _pending_firmware = empty_firmware_update;
      }
      strncpy(_pending_firmware->url, response->url, sizeof(_pending_firmware->url));
      xEventGroupSetBits(event_group, 1);
      break;
    }

    default:
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Got unhandled message %d", message_id);
      break;
    }
  });

  // Wait until no more messages has been received within a period.
  while (1) {
    auto bits = xEventGroupWaitBits(event_group, 1, pdTRUE, pdFALSE, (1000 / portTICK_PERIOD_MS));
    auto got_message = ((bits & 1) != 0);
    if (!got_message) {
      break;
    }
  }
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Data wait complete");

  _ieee802154.receive({}); // Stop receiving.

  // If we now have a complete firmware update, lets go and update the firmware.
  if (_pending_firmware) {
    if (strlen(_pending_firmware->wifi_ssid) > 0 && strlen(_pending_firmware->wifi_password) > 0 &&
        strlen(_pending_firmware->url) > 0) {
      // Will never return.
      return performFirmwareUpdate(*_pending_firmware);
    } else {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG,
               " -- Got firmware update but with missing fields. Unable to perform firmware update.");
    }
  }

  return true;
}

bool Ieee802154NetworkNode::performFirmwareUpdate(FirmwareUpdate &firmware_update) {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Performing firmware update");
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- SSID: %s", firmware_update.wifi_ssid);
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Password (length): %d", strlen(firmware_update.wifi_password));
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- MD5 checksum: %s", firmware_update.md5);
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- URL: %s", firmware_update.url);

  std::string hostname = std::format("0x{:016x}", _host_address);
  WiFiHelper _wifi_helper(hostname.c_str());

  // Turn of 802.15.4
  teardown();

  if (!_wifi_helper.connectToAp(firmware_update.wifi_ssid, firmware_update.wifi_password, false, 10000)) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Unable to connect to WiFi. Firmware update aborted.");
    _wifi_helper.disconnect();
    return false;
  }

  // Start OTA.
  auto url = std::string(firmware_update.url);
  auto md5str = std::string(firmware_update.md5, firmware_update.md5 + 32);
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Starting firmwate update from %s", firmware_update.url);
  if (!_ota_helper.updateFrom(url, OtaHelper::FlashMode::FIRMWARE, md5str)) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Failed to download firmware update.");
    _wifi_helper.disconnect();
    return false;
  }

  // Successful update. Restart.
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Firmware update successful, restarting...");
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  esp_restart();

  return true;
}

std::optional<uint64_t> Ieee802154NetworkNode::pendingTimestamp() { return _pending_timestamp; }
std::optional<std::vector<uint8_t>> Ieee802154NetworkNode::pendingPayload() { return _pending_payload; }

uint64_t Ieee802154NetworkNode::deviceMacAddress() { return _ieee802154.deviceMacAddress(); }

void Ieee802154NetworkNode::teardown() { return _ieee802154.teardown(); }

void Ieee802154NetworkNode::forget() {
  _nvs_storage.eraseKey(NVS_KEY_HOST);
  _nvs_storage.eraseKey(NVS_KEY_CHANNEL);
}

void Ieee802154NetworkNode::initializeNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}