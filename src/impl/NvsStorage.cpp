// Copyright (c) Johan Böhlin
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NvsStorage.h"

NvsStorage::NvsStorage(std::string namespace_name) : _namespace_name(namespace_name) {}

bool NvsStorage::eraseKey(const char *key) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(_namespace_name.c_str(), NVS_READWRITE, &my_handle);
  if (err != ESP_OK) {
    ESP_LOGE(NvsStorageLog::TAG, "Error %s when opening NVS.", esp_err_to_name(err));
    return false;
  }

  err = nvs_erase_key(my_handle, key);
  if (err == ESP_OK) {
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
      ESP_LOGE(NvsStorageLog::TAG, "Error %s committing erasure to NVS.", esp_err_to_name(err));
    }
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  } else {
    ESP_LOGE(NvsStorageLog::TAG, "Error %s erasing key %s from NVS.", esp_err_to_name(err), key);
  }

  nvs_close(my_handle);
  return err == ESP_OK;
}
