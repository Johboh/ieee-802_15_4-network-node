#include "Ieee802154NetworkNode.h"
#include <Ieee802154NetworkShared.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>

Ieee802154NetworkNode::Ieee802154NetworkNode(Configuration configuration)
    : _ieee802154({
          .channel = 0,
          .pan_id = configuration.pan_id,
      }),
      _nvs_storage("Ieee802154"), _configuration(configuration),
      _gcm_encryption(configuration.gcm_encryption_key, configuration.gcm_encryption_secret, false) {}

bool Ieee802154NetworkNode::sendMessage(std::vector<uint8_t> message) {
  return sendMessage(message.data(), message.size());
}

bool Ieee802154NetworkNode::sendMessage(uint8_t *message, uint8_t message_size) {
  std::scoped_lock lock(_send_mutex);

  if (!_nvs_initialized) {
    initializeNvs();
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
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Got disovery response with channel %d and host 0x%llx",
               response->channel, _host_address);
    } else {
      ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Got unknown message %d while waiting for device discovery respose",
               message_id);
    }
  });

  // Discover on all channels.
  // Reduce number of retries.
  _ieee802154.setNumberOfDataFramesRetries(10);
  auto encrypted = _gcm_encryption.encrypt(&discovery_request, sizeof(Ieee802154NetworkShared::DiscoveryRequestV1));
  uint8_t channel = 11;
  for (; channel <= 26; ++channel) {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Discovering on channel %d...", channel);
    _ieee802154.setChannel(channel);
    // Broadcast never emit ACKs.
    _ieee802154.transmit(__UINT64_MAX__, encrypted.data(), encrypted.size());
    /*if (r) {
      // Got ack.
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Got ACK for device request (in loop)");
      break;
    }*/
    // Also check if we got a message that we want.
    auto bits = xEventGroupWaitBits(event_group, 1, pdFALSE, pdFALSE, 1);
    auto found_host = ((bits & 1) != 0);
    if (found_host) {
      ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Got device response for device request (in loop)");
      break;
    }
  }

  if (channel > 26) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Sent device discovery on all channels without successful ACK");
  }

  // Wait for data with timeout.
  auto bits = xEventGroupWaitBits(event_group, 1, pdTRUE, pdFALSE, (1000 / portTICK_PERIOD_MS));
  auto found_host = ((bits & 1) != 0);

  if (!found_host) {
    ESP_LOGW(Ieee802154NetworkNodeLog::TAG, "Never received device discovery response");
  } else {
    ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Host found during device discovery");
  }

  _ieee802154.receive({}); // Stop receiving.

  return found_host;
}
bool Ieee802154NetworkNode::requestData() {
  ESP_LOGI(Ieee802154NetworkNodeLog::TAG, "Requesting data");

  return false;
}

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