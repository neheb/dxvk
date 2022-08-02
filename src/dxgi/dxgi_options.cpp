#include "dxgi_options.h"

#include <unordered_map>

namespace dxvk {

  static int32_t parsePciId(const std::string& str) {
    if (str.size() != 4)
      return -1;
    
    int32_t id = 0;

    for (char c : str) {
      id *= 16;

      if (c >= '0' && c <= '9')
        id += c - '0';
      else if (c >= 'A' && c <= 'F')
        id += c - 'A' + 10;
      else if (c >= 'a' && c <= 'f')
        id += c - 'a' + 10;
      else
        return -1;
    }

    return id;
  }

  
  DxgiOptions::DxgiOptions(const Config& config) {
    // Fetch these as a string representing a hexadecimal number and parse it.
    this->customVendorId = parsePciId(config.getOption<std::string>("dxgi.customVendorId"));
    this->customDeviceId = parsePciId(config.getOption<std::string>("dxgi.customDeviceId"));
    this->customDeviceDesc = config.getOption<std::string>("dxgi.customDeviceDesc", "");

    // Emulate a UMA device
    this->emulateUMA = config.getOption<bool>("dxgi.emulateUMA", false);
    
    // Interpret the memory limits as Megabytes
    this->maxDeviceMemory = VkDeviceSize(config.getOption<int32_t>("dxgi.maxDeviceMemory", 0)) << 20;
    this->maxSharedMemory = VkDeviceSize(config.getOption<int32_t>("dxgi.maxSharedMemory", 0)) << 20;

    // Force nvapiHack to be disabled if NvAPI is enabled in environment
    if (env::getEnvVar("DXVK_ENABLE_NVAPI") == "1")
      this->nvapiHack = false;
    else
      this->nvapiHack = config.getOption<bool>("dxgi.nvapiHack", true);
  }
  
}
