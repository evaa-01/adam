#pragma once

#include <cstdint>
#include <vector>

namespace adam::launcher_art {
    // The launcher background (a picture from the game) as RGBA8 pixels, decoded from the stored ROM.
    // False if no ROM is stored or it doesn't contain the expected image.
    bool load_background(std::vector<char>& rgba, uint32_t& width, uint32_t& height);
}
