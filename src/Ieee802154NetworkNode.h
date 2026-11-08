// Copyright (c) Johan Böhlin
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "impl/NvsStorage.h"
#include <GCMEncryption.h>
#include <Ieee802154.h>
#include <OtaHelper.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace Ieee802154NetworkNodeLog {
const char TAG[] = "802.15.4 Node";
} // namespace Ieee802154NetworkNodeLog

class Ieee802154NetworkNode {
public:
  static constexpr uint16_t DEFAULT_PAN_ID = 0x9191;

  struct PayloadMessage {
    Ieee802154NetworkNode &host;
    std::vector<uint8_t> payload;
  };

  struct TimestampMessage {
    Ieee802154NetworkNode &host;
    uint64_t timestamp; // unix timestamp in seconds, UTC.
  };

  struct Configuration {
    /**
     * Encyption key used for the GCM packet encryption. Must be exact 16 bytes long. \0 does not count.
     *
     */
    const char *gcm_encryption_key;
    /**
     * Secret used for the GCM packet encryption to validate the integrity of the messages. We expect the decrypted
     * payload to contain this string. Must be exact 8 bytes long. \0 does not count.
     */
    const char *gcm_encryption_secret;
    /**
     * This is the firmware version of the node. This is important so the host knows when to indicate when there is a
     * new firmware, and only send a firmware update payload if so. Incremental.
     */
    const uint32_t firmware_version;
    /**
     * Private Area Network Identifier. Should be same between host and node.
     */
    uint16_t pan_id = DEFAULT_PAN_ID;
    /**
     * @brief Transmit power in dB.
     * Unknown allowed range.
     */
    int8_t tx_power = 20;
    /**
     * @brief How many times to try resending a data frame on failure (no ACK, busy CCA, etc.).
     * Defaults to 30.
     */
    uint8_t data_frame_retries = 30;
    /**
     * @brief How many times to try resending a data request frame on failure (no ACK, busy CCA, etc.).
     * Defaults to 30.
     */
    uint8_t data_request_retries = 30;
    /**
     * @brief How many times to try resending a discovery broadcast frame on failure (busy CCA, etc.).
     * Defaults to 3 (since discovery already sends multiple broadcast attempts per channel, a low retry count avoids
     * stalling on heavily Wi-Fi-congested channels).
     */
    uint8_t discovery_broadcast_retries = 3;
  };

  Ieee802154NetworkNode(Configuration configuration);

public:
  /**
   * Result of a sendMessage() operation, including transmit status (attempts, errors, last error, ACK RSSI, ACK LQI)
   * and any pending timestamp or payload received from the host.
   * Converts implicitly to bool for easy `if (result)` checks.
   */
  struct SendMessageResult : Ieee802154::TransmitStatus {
    enum class Status {
      Success,
      Failure,
    };
    static constexpr Status Success = Status::Success;
    static constexpr Status Failure = Status::Failure;

    Status status = Status::Failure;
    std::optional<uint64_t> timestamp;
    std::optional<std::vector<uint8_t>> payload;

    constexpr SendMessageResult() = default;
    constexpr SendMessageResult(Status status) : Ieee802154::TransmitStatus(), status(status) {}
    constexpr SendMessageResult(Status status, const Ieee802154::TransmitStatus &tx_status)
        : Ieee802154::TransmitStatus(tx_status), status(status) {}
    SendMessageResult(Status status, const Ieee802154::TransmitStatus &tx_status, std::optional<uint64_t> timestamp,
                      std::optional<std::vector<uint8_t>> payload)
        : Ieee802154::TransmitStatus(tx_status), status(status), timestamp(timestamp), payload(std::move(payload)) {}

    constexpr operator Status() const { return status; }
    constexpr operator bool() const { return status == Status::Success; }
  };

  /**
   * Send a message to the host.
   * If no host can be found or there is channel mismatch, will go into discovery mode and try to find the host.
   * If a timestamp or payload was received during the send, it is returned in the timestamp and payload fields of the
   * result. In case of a firmware update, this function will never return and instead firmware will commence and
   * the device will restart on update complete.
   *
   * @param message the message to send. Maximum message size is 74 bytes.
   * @return SendMessageResult indicating Success or Failure, along with attempts, errors, last_error, ack_rssi,
   * ack_lqi, and any pending timestamp or payload.
   */
  SendMessageResult sendMessage(const std::vector<uint8_t> &message);
  /**
   * Send a message to the host.
   * If no host can be found or there is channel mismatch, will go into discovery mode and try to find the host.
   * If a timestamp or payload was received during the send, it is returned in the timestamp and payload fields of the
   * result. In case of a firmware update, this function will never return and instead firmware will commence and
   * the device will restart on update complete.
   *
   * @param message the message to send.
   * @param message_size maxium message size is 74 bytes.
   * @return SendMessageResult indicating Success or Failure, along with attempts, errors, last_error, ack_rssi,
   * ack_lqi, and any pending timestamp or payload.
   */
  SendMessageResult sendMessage(const uint8_t *message, uint8_t message_size);

  /**
   * return true to restart the device (default behavior), or false to not restart the device. Usually you want to
   * restart the device upon firmware update complete. Parameter successful indicates if the firmware update was
   * successful or not.
   */
  typedef std::function<bool(bool successful)> OnFirmwareUpdateComplete;

  /**
   * Set the function to be called upon firmare update completed.
   * If no callback is set, device will restart on sucessful firmware update, but not on failure.
   *
   */
  void setOnFirmwareUpdated(OnFirmwareUpdateComplete on_firmware_update_complete) {
    _on_firmware_update_complete = on_firmware_update_complete;
  }

  /**
   * @brief Change the 802.15.4 channel and persist it to NVS.
   *
   * @param channel channel number between 11 and 26.
   */
  void setChannel(uint8_t channel);

  /**
   * @brief Get the currently configured channel.
   * Returns 0 if no channel has been configured or loaded from NVS yet.
   */
  uint8_t channel() const;

  /**
   * @brief Get the MAC address of the host this node is currently associated with.
   * Returns 0 if not associated/discovered yet.
   */
  uint64_t hostMacAddress() const;

  /**
   * Forget any previous stored channel and host MAC address.
   */
  void forget();

  /**
   * @brief Get the device mac address for this device. This would be the source address in the 802.15.4 frame (or the
   * destination address for a sender).
   */
  uint64_t deviceMacAddress() const;

private:
  struct FirmwareUpdate {
    char wifi_ssid[33] = {0};     // WiFi SSID that node should connect to. Includes trailing \0
    char wifi_password[33] = {0}; // WiFi password that the node should connect to. Includes trailing \0
    char url[75] = {0};           // url where to find firmware binary. Includes trailing \0
    char md5[33] = {0};           // MD5 hash of firmware. Includes trailing \0
  };

  void initializeNvs();
  void teardown();
  Ieee802154::TransmitResult sendApplicationMessage(const uint8_t *message, uint8_t message_size);
  bool performDiscovery();
  bool requestData(std::optional<uint64_t> &pending_timestamp, std::optional<std::vector<uint8_t>> &pending_payload);
  bool performFirmwareUpdateViaWifi(FirmwareUpdate &firmware_update);

  struct DiscoveredHost {
    uint64_t mac_address;
    uint8_t channel;
    int8_t rssi;
    uint8_t lqi;
    bool on_channel = false;
  };

private:
  static constexpr char NVS_KEY_HOST[] = "host";
  static constexpr char NVS_KEY_CHANNEL[] = "channel";

private:
  OtaHelper _ota_helper;
  Ieee802154 _ieee802154;
  NvsStorage _nvs_storage;
  Configuration _configuration;
  GCMEncryption _gcm_encryption;

private:
  uint64_t _host_address = 0;
  uint8_t _channel = 0;
  std::mutex _send_mutex;
  bool _nvs_initialized = false;
  OnFirmwareUpdateComplete _on_firmware_update_complete;
};