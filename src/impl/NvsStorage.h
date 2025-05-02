#pragma once

#include <esp_log.h>
#include <nvs.h>
#include <string>

namespace NvsStorageLog {
const char TAG[] = "NvsStorage";
};

class NvsStorage {
public:
  NvsStorage(std::string namespace_name);

public:
  template <typename T> bool readFromNVS(const char *key, T &value) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(_namespace_name.c_str(), NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
      size_t required_size = sizeof(T);
      err = nvs_get_blob(my_handle, key, &value, &required_size);
      nvs_close(my_handle);
      if (err == ESP_OK) {
        return true;
      } else {
        ESP_LOGI(NvsStorageLog::TAG, "No value found for key: %s", key);
      }
    } else {
      ESP_LOGE(NvsStorageLog::TAG, "Error %s when opening NVS.", esp_err_to_name(err));
    }
    return false;
  }

  template <typename T> bool writeToNVS(const char *key, const T &value) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(_namespace_name.c_str(), NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
      err = nvs_set_blob(my_handle, key, &value, sizeof(T));
      if (err == ESP_OK) {
        err = nvs_commit(my_handle);
        nvs_close(my_handle);
        if (err == ESP_OK) {
          return true;
        }
      }
      ESP_LOGE(NvsStorageLog::TAG, "Error %s committing to NVS.", esp_err_to_name(err));
    } else {
      ESP_LOGE(NvsStorageLog::TAG, "Error %s when opening NVS.", esp_err_to_name(err));
    }
    return false;
  }

  bool eraseKey(const char *key);

private:
  std::string _namespace_name;
};
