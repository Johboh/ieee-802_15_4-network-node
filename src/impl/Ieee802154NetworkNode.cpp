// Copyright (c) Johan Böhlin
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ieee802154NetworkNode.h"
#include <Ieee802154NetworkShared.h>
#include <WiFiHelper.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <map>
#include <nvs_flash.h>
#include <string>

// Keep track of next sequence number during sleep and esp_restart()
// Using RTC_NOINIT_ATTR will keep the value during both sleep and esp_restart(), but we cannot initalize it.
// We want to initialize it with a random value, so we not start with 0 on every power on.
#define SEQUENCE_NUMBER_IS_SET 0x3fbd73e2
RTC_NOINIT_ATTR uint8_t _Ieee802154NetworkNode_next_sequence_number;
RTC_NOINIT_ATTR uint32_t _Ieee802154NetworkNode_next_sequence_number_is_set;

#define REQUESTED_DATA_MESSAGE_ANY BIT0

Ieee802154NetworkNode::Ieee802154NetworkNode(Configuration configuration)
    : _ota_helper({
          .web_ota = {.enabled = false},
          .arduino_ota = {.enabled = false},
          .rollback_strategy = OtaHelper::RollbackStrategy::MANUAL,
      }),
      _ieee802154(
          {.channel = 0,
           .pan_id = configuration.pan_id,
           .initial_sequence_number = (_Ieee802154NetworkNode_next_sequence_number_is_set == SEQUENCE_NUMBER_IS_SET
                                           ? _Ieee802154NetworkNode_next_sequence_number
                                           : (uint8_t)esp_random()),
           .tx_power = configuration.tx_power}),
      _nvs_storage("Ieee802154"), _configuration(configuration),
      _gcm_encryption(configuration.gcm_encryption_key, configuration.gcm_encryption_secret, false) {
  esp_log_level_set(OtaHelperLog::TAG, ESP_LOG_ERROR);
  esp_log_level_set(WiFiHelperLog::TAG, ESP_LOG_ERROR);
  _Ieee802154NetworkNode_next_sequence_number = _ieee802154.nextSequenceNumber();
  _Ieee802154NetworkNode_next_sequence_number_is_set = SEQUENCE_NUMBER_IS_SET;
}

Ieee802154NetworkNode::SendMessageResult Ieee802154NetworkNode::sendMessage(const std::vector<uint8_t> &message) {
  return sendMessage(message.data(), message.size());
}

Ieee802154NetworkNode::SendMessageResult Ieee802154NetworkNode::sendMessage(const uint8_t *message,
                                                                            uint8_t message_size) {
  if (message == nullptr && message_size > 0) {
    ESP_LOGE(Ieee802154NetworkNodeLog::TAG, "Message is null but message_size > 0");
    return SendMessageResult::Failure;
  }
  if (message_size > 74) {
    ESP_LOGE(Ieee802154NetworkNodeLog::TAG, "Message size %d exceeds maximum allowed size of 74 bytes", message_size);
    return SendMessageResult::Failure;
  }

  std::scoped_lock lock(_send_mutex);

  if (!_nvs_initialized) {
    initializeNvs();
    _ota_helper.cancelRollback();
    _nvs_initialized = true;
  }

  bool initialize_nvs = false;
  if (!_ieee802154.initialize(initialize_nvs)) {
    ESP_LOGE(Ieee802154NetworkNodeLog::TAG, "Failed to initialize 802.15.4 radio");
    return SendMessageResult::Failure;
  }

  // Read channel and host address from NVS.
  uint8_t channel = 0;
  bool read_ok = _nvs_storage.readFromNVS(NVS_KEY_CHANNEL, channel);
  if (read_ok) {
    _channel = channel;
    _ieee802154.setChannel(_channel);
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
      return SendMessageResult::Failure;
    }
  }

  std::optional<uint64_t> pending_timestamp;
  std::optional<std::vector<uint8_t>> pending_payload;

  // Try sending application message once.
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "First attempt of sending message...");
  auto tx_result = sendApplicationMessage(message, message_size);
  if (tx_result == Ieee802154::TransmitResult::Success || tx_result == Ieee802154::TransmitResult::SuccessWithPending) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "First attempt of sending message OK (attempts: %u, ACK RSSI: %d, LQI: %u)",
             tx_result.attempts, tx_result.ack_rssi, tx_result.ack_lqi);
    bool req_ok = true;
    if (tx_result == Ieee802154::TransmitResult::SuccessWithPending) {
      req_ok = requestData(pending_timestamp, pending_payload);
      if (!req_ok) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Data request failed");
      }
    }
    teardown();
    return SendMessageResult(req_ok ? SendMessageResult::Success : SendMessageResult::Failure, tx_result,
                             pending_timestamp, std::move(pending_payload));
  } else {
    // Not good. Wait and try again.
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG,
             "First attempt of sending message failed (attempts: %u, errors: %u, last_error: %d)", tx_result.attempts,
             tx_result.errors, tx_result.last_error);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Second attempt of sending message...");
    tx_result = sendApplicationMessage(message, message_size);
  }

  if (tx_result == Ieee802154::TransmitResult::Success || tx_result == Ieee802154::TransmitResult::SuccessWithPending) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG,
             "Second attempt of sending message OK (attempts: %u, ACK RSSI: %d, LQI: %u)", tx_result.attempts,
             tx_result.ack_rssi, tx_result.ack_lqi);
    bool req_ok = true;
    if (tx_result == Ieee802154::TransmitResult::SuccessWithPending) {
      req_ok = requestData(pending_timestamp, pending_payload);
      if (!req_ok) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Data request failed");
      }
    }
    teardown();
    return SendMessageResult(req_ok ? SendMessageResult::Success : SendMessageResult::Failure, tx_result,
                             pending_timestamp, std::move(pending_payload));
  } else {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG,
             "Second attempt of sending message failed (attempts: %u, errors: %u, last_error: %d)", tx_result.attempts,
             tx_result.errors, tx_result.last_error);

    // Not good. Assume faulty host or channel.
    // Go into discovery mode.
    auto r = performDiscovery();
    if (!r) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Device discovery failed");
      teardown();
      return SendMessageResult(SendMessageResult::Failure, tx_result, pending_timestamp, std::move(pending_payload));
    } else {
      // Discovery OK, try sending message.
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Third attempt of sending message (after discovery)...");
      tx_result = sendApplicationMessage(message, message_size);
    }
  }

  bool success =
      (tx_result == Ieee802154::TransmitResult::Success || tx_result == Ieee802154::TransmitResult::SuccessWithPending);
  if (success) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Third attempt of sending message OK (attempts: %u, ACK RSSI: %d, LQI: %u)",
             tx_result.attempts, tx_result.ack_rssi, tx_result.ack_lqi);
  } else {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG,
             "Third attempt of sending message failed (attempts: %u, errors: %u, last_error: %d)", tx_result.attempts,
             tx_result.errors, tx_result.last_error);
  }
  if (success && tx_result == Ieee802154::TransmitResult::SuccessWithPending) {
    if (!requestData(pending_timestamp, pending_payload)) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Data request failed");
      success = false;
    }
  }

  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "End of sendMessage: %d", success);
  teardown();
  return SendMessageResult(success ? SendMessageResult::Success : SendMessageResult::Failure, tx_result,
                           pending_timestamp, std::move(pending_payload));
}

Ieee802154::TransmitResult Ieee802154NetworkNode::sendApplicationMessage(const uint8_t *message, uint8_t message_size) {
  auto wire_message_size = sizeof(Ieee802154NetworkShared::MessageV1) + message_size;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[wire_message_size]);
  if (!buffer) {
    ESP_LOGE(Ieee802154NetworkNodeLog::TAG, "Failed to allocate memory for application message");
    return Ieee802154::TransmitResult::Failure;
  }
  Ieee802154NetworkShared::MessageV1 *wire_message =
      reinterpret_cast<Ieee802154NetworkShared::MessageV1 *>(buffer.get());
  wire_message->id = Ieee802154NetworkShared::MESSAGE_ID_MESSAGE;
  wire_message->firmware_version = _configuration.firmware_version;
  if (message != nullptr && message_size > 0) {
    memcpy(wire_message->payload, message, message_size);
  }

  auto encrypted = _gcm_encryption.encrypt(wire_message, wire_message_size);

  return _ieee802154.transmit(_host_address, encrypted.data(), encrypted.size(), _configuration.data_frame_retries);
}

bool Ieee802154NetworkNode::performDiscovery() {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "In device discovery");

  Ieee802154NetworkShared::DiscoveryRequestV1 discovery_request;
  std::vector<DiscoveredHost> discovered_hosts;
  EventGroupHandle_t event_group = xEventGroupCreate();

  _ieee802154.receive([&](Ieee802154::Message message) {
    auto decrypted = _gcm_encryption.decrypt(message.payload);
    if (decrypted.empty()) {
      return;
    }
    uint8_t message_id = decrypted.data()[0];
    if (message_id == Ieee802154NetworkShared::MESSAGE_ID_DISCOVERY_RESPONSE_V1) {
      if (decrypted.size() < sizeof(Ieee802154NetworkShared::DiscoveryResponseV1)) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Discovery response packet too small: %d bytes", decrypted.size());
        return;
      }
      Ieee802154NetworkShared::DiscoveryResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::DiscoveryResponseV1 *>(decrypted.data());

      DiscoveredHost host = {
          .mac_address = message.source_address,
          .channel = response->channel,
          .rssi = message.rssi,
          .lqi = message.lqi,
      };
      discovered_hosts.push_back(host);
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG,
               " -- Got discovery response from 0x%llx on channel %d with RSSI %d, LQI %u", host.mac_address,
               host.channel, host.rssi, host.lqi);
    } else {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Got unknown message %d while waiting for device discovery response",
               message_id);
    }
  });

  auto encrypted = _gcm_encryption.encrypt(&discovery_request, sizeof(Ieee802154NetworkShared::DiscoveryRequestV1));

  // Try each channel multiple times to gather all possible hosts.
  for (uint8_t channel = 26; channel >= 11; --channel) {
    _ieee802154.setChannel(channel);
    for (uint8_t attempt = 1; attempt <= 4; ++attempt) {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Broadcasting discovery on channel %d, attempt %d...", channel,
               attempt);
      auto bcast_result =
          _ieee802154.broadcast(encrypted.data(), encrypted.size(), _configuration.discovery_broadcast_retries);
      if (!bcast_result) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG,
                 " -- Broadcast failed on channel %d (attempts: %u, errors: %u, last_error: %d)", channel,
                 bcast_result.attempts, bcast_result.errors, bcast_result.last_error);
      }

      // Wait a short time to collect responses. This clears the event bits after reading.
      xEventGroupWaitBits(event_group, REQUESTED_DATA_MESSAGE_ANY, pdTRUE, pdFALSE, (30 / portTICK_PERIOD_MS));
    }
  }

  _ieee802154.receive({}); // Stop receiving.

  if (discovered_hosts.empty()) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Never received any device discovery response");
    vEventGroupDelete(event_group);
    return false;
  }

  // Group responses by MAC address and keep only the best response for each host.
  // Prioritize LQI (link quality / packet error rate) as the primary metric,
  // using RSSI as a tie-breaker when LQI values are equal (e.g. saturated at 255).
  // This avoids favoring cross-channel responses or noisy high-interference channels.
  std::map<uint64_t, DiscoveredHost> best_per_host;
  for (const auto &host : discovered_hosts) {
    auto it = best_per_host.find(host.mac_address);
    if (it == best_per_host.end() || host.lqi > it->second.lqi ||
        (host.lqi == it->second.lqi && host.rssi > it->second.rssi)) {
      best_per_host[host.mac_address] = host;
    }
  }

  // Find the overall best host among deduplicated entries (prioritizing LQI, then RSSI)
  auto best_host_it = std::max_element(best_per_host.begin(), best_per_host.end(), [](const auto &a, const auto &b) {
    if (a.second.lqi != b.second.lqi) {
      return a.second.lqi < b.second.lqi;
    }
    return a.second.rssi < b.second.rssi;
  });

  _channel = best_host_it->second.channel;
  _ieee802154.setChannel(_channel);
  _nvs_storage.writeToNVS(NVS_KEY_CHANNEL, _channel);
  _host_address = best_host_it->second.mac_address;
  _nvs_storage.writeToNVS(NVS_KEY_HOST, best_host_it->second.mac_address);

  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Best host found: 0x%llx on channel %d with RSSI %d, LQI %u",
           best_host_it->second.mac_address, best_host_it->second.channel, best_host_it->second.rssi,
           best_host_it->second.lqi);

  vEventGroupDelete(event_group);
  return true;
}
bool Ieee802154NetworkNode::requestData(std::optional<uint64_t> &pending_timestamp,
                                        std::optional<std::vector<uint8_t>> &pending_payload) {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Requesting data");

  auto result = _ieee802154.dataRequest(_host_address, _configuration.data_request_retries);
  EventGroupHandle_t event_group = xEventGroupCreate();

  if (result == Ieee802154::DataRequestResult::Failure) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Failed to request data (attempts: %u, errors: %u, last_error: %d)",
             result.attempts, result.errors, result.last_error);
    vEventGroupDelete(event_group);
    return false;
  }

  if (result == Ieee802154::DataRequestResult::NoDataAvailable) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- No data available (attempts: %u, ACK RSSI: %d, LQI: %u)",
             result.attempts, result.ack_rssi, result.ack_lqi);
    vEventGroupDelete(event_group);
    return true;
  }

  // We have data. Wait for it.
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Data available, waiting (attempts: %u, ACK RSSI: %d, LQI: %u)",
           result.attempts, result.ack_rssi, result.ack_lqi);
  uint32_t firmware_url_identifier = 0;
  uint32_t firmware_checksum_identifier = 0;
  uint32_t firmware_credentials_identifier = 0;
  std::optional<FirmwareUpdate> _pending_firmware;
  bool rediscover_requested = false;
  _ieee802154.receive([&](Ieee802154::Message message) {
    auto decrypted = _gcm_encryption.decrypt(message.payload);
    if (decrypted.empty()) {
      return;
    }
    uint8_t message_id = decrypted.data()[0];
    switch (message_id) {
    case Ieee802154NetworkShared::MESSAGE_ID_FORGET_HOST_RESPONSE_V1: {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got forgetHostResponseV1");
      forget();
      rediscover_requested = true;
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_TIMESTAMP_RESPONSE_V1: {
      if (decrypted.size() < sizeof(Ieee802154NetworkShared::PendingTimestampResponseV1)) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Timestamp response packet too small: %d bytes", decrypted.size());
        return;
      }
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingTimestampResponseV1 (RSSI: %d, LQI: %u)", message.rssi,
               message.lqi);
      Ieee802154NetworkShared::PendingTimestampResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingTimestampResponseV1 *>(decrypted.data());
      auto timestamp = response->timestamp;
      pending_timestamp = timestamp;
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_PAYLOAD_RESPONSE_V1: {
      if (decrypted.size() < sizeof(Ieee802154NetworkShared::PendingPayloadResponseV1)) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Payload response packet too small: %d bytes", decrypted.size());
        return;
      }
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingPayloadResponseV1 (RSSI: %d, LQI: %u)", message.rssi,
               message.lqi);
      pending_payload = std::vector<uint8_t>(
          decrypted.begin() + sizeof(Ieee802154NetworkShared::PendingPayloadResponseV1), decrypted.end());
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_FIRMWARE_WIFI_CREDENTIALS_RESPONSE_V1: {
      if (decrypted.size() < sizeof(Ieee802154NetworkShared::PendingFirmwareWifiCredentialsResponseV1)) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Firmware wifi credentials packet too small: %d bytes",
                 decrypted.size());
        return;
      }
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingFirmwareWifiCredentialsResponseV1 (RSSI: %d, LQI: %u)",
               message.rssi, message.lqi);
      Ieee802154NetworkShared::PendingFirmwareWifiCredentialsResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingFirmwareWifiCredentialsResponseV1 *>(decrypted.data());
      if (!_pending_firmware) {
        FirmwareUpdate empty_firmware_update;
        _pending_firmware = empty_firmware_update;
      }
      memcpy(_pending_firmware->wifi_ssid, response->wifi_ssid, sizeof(response->wifi_ssid));
      _pending_firmware->wifi_ssid[sizeof(response->wifi_ssid)] = '\0';
      memcpy(_pending_firmware->wifi_password, response->wifi_password, sizeof(response->wifi_password));
      _pending_firmware->wifi_password[sizeof(response->wifi_password)] = '\0';
      firmware_credentials_identifier = response->identifier;
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_FIRMWARE_CHECKSUM_RESPONSE_V1: {
      if (decrypted.size() < sizeof(Ieee802154NetworkShared::PendingFirmwareChecksumResponseV1)) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Firmware checksum packet too small: %d bytes", decrypted.size());
        return;
      }
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingFirmwareChecksumResponseV1 (RSSI: %d, LQI: %u)",
               message.rssi, message.lqi);
      Ieee802154NetworkShared::PendingFirmwareChecksumResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingFirmwareChecksumResponseV1 *>(decrypted.data());
      if (!_pending_firmware) {
        FirmwareUpdate empty_firmware_update;
        _pending_firmware = empty_firmware_update;
      }
      memcpy(_pending_firmware->md5, response->md5, sizeof(response->md5));
      _pending_firmware->md5[sizeof(response->md5)] = '\0';
      firmware_checksum_identifier = response->identifier;
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      break;
    }

    case Ieee802154NetworkShared::MESSAGE_ID_PENDING_FIRMWARE_URL_RESPONSE_V1: {
      if (decrypted.size() < sizeof(Ieee802154NetworkShared::PendingFirmwareUrlResponseV1)) {
        ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Firmware url packet too small: %d bytes", decrypted.size());
        return;
      }
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Got PendingFirmwareUrlResponseV1 (RSSI: %d, LQI: %u)", message.rssi,
               message.lqi);
      Ieee802154NetworkShared::PendingFirmwareUrlResponseV1 *response =
          reinterpret_cast<Ieee802154NetworkShared::PendingFirmwareUrlResponseV1 *>(decrypted.data());
      if (!_pending_firmware) {
        FirmwareUpdate empty_firmware_update;
        _pending_firmware = empty_firmware_update;
      }
      memcpy(_pending_firmware->url, response->url, sizeof(response->url));
      _pending_firmware->url[sizeof(response->url)] = '\0';
      firmware_url_identifier = response->identifier;
      xEventGroupSetBits(event_group, REQUESTED_DATA_MESSAGE_ANY);
      break;
    }

    default:
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Got unhandled message %d", message_id);
      break;
    }
  });

  // Wait until no more messages has been received within a period.
  while (1) {
    auto bits =
        xEventGroupWaitBits(event_group, REQUESTED_DATA_MESSAGE_ANY, pdTRUE, pdFALSE, (1000 / portTICK_PERIOD_MS));
    auto got_any_message = ((bits & REQUESTED_DATA_MESSAGE_ANY) != 0);
    if (!got_any_message || rediscover_requested) {
      break;
    }
  }
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Data wait complete");

  _ieee802154.receive({}); // Stop receiving.

  if (rediscover_requested) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Performing discovery after forget host request");
    bool discovery_ok = performDiscovery();
    vEventGroupDelete(event_group);
    return discovery_ok;
  }

  // If we now have a complete firmware update, lets go and update the firmware.
  if (_pending_firmware) {
    if (firmware_credentials_identifier != firmware_url_identifier ||
        firmware_credentials_identifier != firmware_checksum_identifier) {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Got firmware update but identifiers does not match in messages.");
    } else if (strlen(_pending_firmware->wifi_ssid) > 0 && strlen(_pending_firmware->wifi_password) > 0 &&
               strlen(_pending_firmware->url) > 0) {

      bool restart = false;
      auto successful = performFirmwareUpdateViaWifi(*_pending_firmware);

      if (_on_firmware_update_complete) {
        restart = _on_firmware_update_complete(successful);
      } else {
        restart = successful;
      }

      if (restart) {
        ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Restarting...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
      }

      vEventGroupDelete(event_group);
      return successful;
    } else {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG,
               " -- Got firmware update but with missing fields. Unable to perform firmware update.");
    }
  }

  vEventGroupDelete(event_group);
  return true;
}

bool Ieee802154NetworkNode::performFirmwareUpdateViaWifi(FirmwareUpdate &firmware_update) {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Performing firmware update");
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- SSID: %s", firmware_update.wifi_ssid);
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Password (length): %d", strlen(firmware_update.wifi_password));
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- MD5 checksum: %s", firmware_update.md5);
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- URL: %s", firmware_update.url);

  char hostname[32]; // Max allowed is 32
  snprintf(hostname, sizeof(hostname), "ieee802154node_%llx", deviceMacAddress());
  WiFiHelper _wifi_helper(hostname);

  // Turn of 802.15.4
  teardown();

  if (!_wifi_helper.connectToAp(firmware_update.wifi_ssid, firmware_update.wifi_password, false, 10000)) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Unable to connect to WiFi. Firmware update aborted.");
    _wifi_helper.disconnect();
    return false;
  }

  // Start OTA.
  auto url = std::string(firmware_update.url);
  auto md5str = std::string(firmware_update.md5);
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Starting firmware update from %s", firmware_update.url);
  if (!_ota_helper.updateFrom(url, OtaHelper::FlashMode::FIRMWARE, md5str)) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, " -- Failed to download firmware update.");
    _wifi_helper.disconnect();
    return false;
  }

  // Successful update.
  _wifi_helper.disconnect();
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, " -- Firmware update successful.");
  return true;
}

uint64_t Ieee802154NetworkNode::deviceMacAddress() const { return _ieee802154.deviceMacAddress(); }

void Ieee802154NetworkNode::teardown() {
  // Store in RTC memory
  _Ieee802154NetworkNode_next_sequence_number = _ieee802154.nextSequenceNumber();
  _ieee802154.teardown();
}

void Ieee802154NetworkNode::forget() {
  _host_address = 0;
  _channel = 0;
  _nvs_storage.eraseKey(NVS_KEY_HOST);
  _nvs_storage.eraseKey(NVS_KEY_CHANNEL);
}

void Ieee802154NetworkNode::setChannel(uint8_t channel) {
  if (!_nvs_initialized) {
    initializeNvs();
    _nvs_initialized = true;
  }
  _channel = std::clamp(channel, (uint8_t)11, (uint8_t)26);
  _ieee802154.setChannel(_channel);
  _nvs_storage.writeToNVS(NVS_KEY_CHANNEL, _channel);
}

uint8_t Ieee802154NetworkNode::channel() const { return _channel; }

uint64_t Ieee802154NetworkNode::hostMacAddress() const { return _host_address; }

void Ieee802154NetworkNode::initializeNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}
