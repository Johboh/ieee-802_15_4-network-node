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
  };

  Ieee802154NetworkNode(Configuration configuration);

public:
  /**
   * Send a message to the host.
   * If no host can be found or there is channel mismatch, will go into discovery mode and try to find the host.
   * If a timestamp or payload was received during the send, the pendingTimestamp() and pendingPayload() functions will
   * return this. In case of a firmware update, this function will never return and instead firmware will commence and
   * the device will restart on update complete.
   *
   * @param message the message to send. Maximum message size is 74 bytes.
   * @return true if message was delivered successfully.
   */
  bool sendMessage(std::vector<uint8_t> message);
  /**
   * Send a message to the host.
   * If no host can be found or there is channel mismatch, will go into discovery mode and try to find the host.
   * If a timestamp or payload was received during the send, the pendingTimestamp() and pendingPayload() functions will
   * return this. In case of a firmware update, this function will never return and instead firmware will commence and
   * the device will restart on update complete.
   *
   * @param message the message to send.
   * @param message_size maxium message size is 74 bytes.
   * @return true if message was delivered successfully.
   */
  bool sendMessage(uint8_t *message, uint8_t message_size);

  /**
   * If set, get the pending timestamp. Will clear any pending timestamp upon access.
   */
  std::optional<uint64_t> pendingTimestamp();
  /**
   * If set, get the pending payload. Will clear any pending payload upon access.
   */
  std::optional<std::vector<uint8_t>> pendingPayload();

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
   * Forget any previous stored channel and host MAC address.
   */
  void forget();

  /**
   * @brief Get the device mac address for this device. This would be the source address in the 802.15.4 frame (or the
   * destination address for a sender).
   */
  uint64_t deviceMacAddress();

private:
  struct FirmwareUpdate {
    char wifi_ssid[32] = {0};     // WiFi SSID that node should connect to.
    char wifi_password[32] = {0}; // WiFi password that the node should connect to.
    char url[74] = {0};           // url where to find firmware binary.
    char md5[32] = {0};           // MD5 hash of firmware. Does not include trailing \0
  };

  void initializeNvs();
  void teardown();
  bool sendApplicationMessage(uint8_t *message, uint8_t message_size);
  bool performDiscovery();
  bool requestData();
  bool performFirmwareUpdateViaWifi(FirmwareUpdate &firmware_update);

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
  uint64_t _host_address;
  std::mutex _send_mutex;
  bool _nvs_initialized = false;
  OnFirmwareUpdateComplete _on_firmware_update_complete;

  // Pending states
private:
  std::optional<uint64_t> _pending_timestamp;
  std::optional<std::vector<uint8_t>> _pending_payload;
};