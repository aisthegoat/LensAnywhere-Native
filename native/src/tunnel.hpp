#pragma once

#include <cstdint>
#include <vector>

namespace Tunnel {
    void StartBackend();
    void ShutdownBackend();
    bool IsServerReady();
    void SearchLens(std::vector<uint8_t> png_bytes);
}
