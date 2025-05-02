#pragma once

#include "impl/NvsStorage.h"
#include <GCMEncryption.h>
#include <Ieee802154.h>
#include <cstdint>
#include <mutex>

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
  bool sendMessage(std::vector<uint8_t> message);
  bool sendMessage(uint8_t *message, uint8_t message_size);
  void teardown();
  void forget();

  /**
   * @brief Get the device mac address for this device. This would be the source address in the 802.15.4 frame (or the
   * destination address for a sender).
   */
  uint64_t deviceMacAddress();

private:
  void initializeNvs();
  bool sendApplicationMessage(uint8_t *message, uint8_t message_size);
  bool performDiscovery();
  bool requestData();

private:
  static constexpr char NVS_KEY_HOST[] = "host";
  static constexpr char NVS_KEY_CHANNEL[] = "channel";

private:
  Ieee802154 _ieee802154;
  NvsStorage _nvs_storage;
  Configuration _configuration;
  GCMEncryption _gcm_encryption;

private:
  uint64_t _host_address;
  std::mutex _send_mutex;
  bool _nvs_initialized = false;
};