#include "NvsStorage.h"

NvsStorage::NvsStorage(std::string namespace_name) : _namespace_name(namespace_name) {}

bool NvsStorage::eraseKey(const char *key) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(_namespace_name.c_str(), NVS_READWRITE, &my_handle);
  if (err == ESP_OK) {
    nvs_erase_key(my_handle, key);
  }
  return err == ESP_OK;
}