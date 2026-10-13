#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adam::launcher_art {
    // Any of the game's named full-screen images by image number (with their palettes), decoded from the stored ROM in one
    // read. `name` is checked against the image's own 8-character name. Images that can't be decoded come back with empty rgba.
    struct Picture {
        uint32_t index;
        std::string name;
        uint32_t width = 0, height = 0;
        std::vector<char> rgba;
    };
    std::vector<Picture> load_images(const std::vector<std::pair<uint32_t, std::string>>& wanted);
}
