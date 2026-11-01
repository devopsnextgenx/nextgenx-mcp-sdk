#include "cppmcp/client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;

    cppmcp::ClientCache cache(16);
    std::string key(reinterpret_cast<const char*>(data), size);
    const auto ttl = std::chrono::milliseconds(data[0]);
    cache.put(key, nlohmann::json::binary(std::vector<std::uint8_t>(data, data + size)), ttl);
    (void)cache.get(key);
    cache.erase(key);
    cache.clear();
    return 0;
}
