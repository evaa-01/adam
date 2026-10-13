// The game's named full-screen images from the player's stored ROM (nothing from the ROM is in the exe), for the launcher
// background and the bezels (bezels.cpp), each with its palette, found through the game's own asset tables (see
// scripts\export_named_images.py):
//   images:   base ROM 0x1D9000, table 0x800D6AD0 (12-byte entries {offset, packed size, unpacked size}), Yay0 files
//             that unpack to a 16-byte header (u16 width, u16 height, 4 bytes, 8-char name) + CI8 pixels
//   palettes: base ROM 0x60CB80, table 0x800DD820, 256 raw RGBA16 colours
//   pairs:    0x800E2080, {u16 image, u16 palette} per image number
// Any mismatch (other ROM, damaged file) leaves that image empty; the callers then keep their default look.
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "librecomp/game.hpp"

#include "launcher_art.h"

namespace {
    constexpr uint32_t BOOT_VRAM = 0x80096400;
    constexpr uint32_t BOOT_ROM = 0x1000;
    constexpr uint32_t IMAGE_ROM = 0x1D9000, IMAGE_TABLE = 0x800D6AD0;
    constexpr uint32_t PALETTE_ROM = 0x60CB80, PALETTE_TABLE = 0x800DD820;
    constexpr uint32_t PAIRS = 0x800E2080;

    struct Rom {
        std::vector<uint8_t> bytes;
        bool has(size_t at, size_t len) const { return at <= bytes.size() && len <= bytes.size() - at; }
        uint32_t u32(size_t at) const {
            return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) | (uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
        }
        uint16_t u16(size_t at) const { return uint16_t((bytes[at] << 8) | bytes[at + 1]); }
    };

    uint32_t boot_rom(uint32_t vram) { return vram - BOOT_VRAM + BOOT_ROM; }

    // Unpacks the Yay0 file at rom[at] (like scripts\yay0.py). Empty on any inconsistency.
    std::vector<uint8_t> yay0(const Rom& rom, size_t at) {
        if (!rom.has(at, 16) || std::memcmp(&rom.bytes[at], "Yay0", 4) != 0) {
            return {};
        }
        uint32_t size = rom.u32(at + 4);
        size_t mask_pos = at + 16, link_pos = at + rom.u32(at + 8), chunk_pos = at + rom.u32(at + 12);
        if (size > 0x400000) {
            return {};
        }
        std::vector<uint8_t> out;
        out.reserve(size);
        uint32_t mask = 0;
        int bits = 0;
        while (out.size() < size) {
            if (bits == 0) {
                if (!rom.has(mask_pos, 4)) return {};
                mask = rom.u32(mask_pos);
                mask_pos += 4;
                bits = 32;
            }
            if (mask & 0x80000000u) {
                if (!rom.has(chunk_pos, 1)) return {};
                out.push_back(rom.bytes[chunk_pos++]);
            }
            else {
                if (!rom.has(link_pos, 2)) return {};
                uint16_t link = rom.u16(link_pos);
                link_pos += 2;
                size_t count = link >> 12;
                size_t distance = (link & 0xFFFu) + 1;
                if (count == 0) {
                    if (!rom.has(chunk_pos, 1)) return {};
                    count = rom.bytes[chunk_pos++] + 0x12u;
                }
                else {
                    count += 2;
                }
                if (distance > out.size()) return {};
                size_t src = out.size() - distance;
                for (size_t i = 0; i < count; i++) {
                    out.push_back(out[src + i]);
                }
            }
            mask <<= 1;
            bits--;
        }
        out.resize(size);
        return out;
    }

    // Asset table entry -> unpacked file.
    std::vector<uint8_t> load_asset(const Rom& rom, uint32_t base, uint32_t table, uint32_t index) {
        size_t entry = boot_rom(table) + size_t(index) * 12;
        if (!rom.has(entry, 12)) {
            return {};
        }
        size_t at = size_t(base) + rom.u32(entry);
        uint32_t packed = rom.u32(entry + 4), size = rom.u32(entry + 8);
        if (packed != 0) {
            return yay0(rom, at);
        }
        if (!rom.has(at, size)) {
            return {};
        }
        return { rom.bytes.begin() + at, rom.bytes.begin() + at + size };
    }
}

namespace {
    std::vector<uint8_t> read_stored_rom() {
        std::ifstream file{ recomp::get_config_path() / "adam.z64", std::ios::binary };
        if (!file) {
            return {};
        }
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    // One image + its palette -> RGBA8. False on any mismatch.
    bool decode(const Rom& rom, uint32_t index, const std::string& name, std::vector<char>& rgba, uint32_t& width, uint32_t& height) {
        size_t pair = boot_rom(PAIRS) + size_t(index) * 4;
        if (!rom.has(pair, 4) || rom.u16(pair) != index || name.size() != 8) {
            return false;
        }
        std::vector<uint8_t> image = load_asset(rom, IMAGE_ROM, IMAGE_TABLE, index);
        std::vector<uint8_t> palette = load_asset(rom, PALETTE_ROM, PALETTE_TABLE, rom.u16(pair + 2));
        if (image.size() < 16 || palette.size() < 512 || std::memcmp(&image[8], name.data(), 8) != 0) {
            return false;
        }
        width = (uint32_t(image[0]) << 8) | image[1];
        height = (uint32_t(image[2]) << 8) | image[3];
        if (image.size() != 16 + size_t(width) * height) {
            return false;
        }

        rgba.resize(size_t(width) * height * 4);
        for (size_t i = 0; i < size_t(width) * height; i++) {
            const uint8_t* c = &palette[size_t(image[16 + i]) * 2];
            uint16_t colour = uint16_t((c[0] << 8) | c[1]); // RGBA 5551
            rgba[i * 4 + 0] = char(((colour >> 11) & 31) * 255 / 31);
            rgba[i * 4 + 1] = char(((colour >> 6) & 31) * 255 / 31);
            rgba[i * 4 + 2] = char(((colour >> 1) & 31) * 255 / 31);
            rgba[i * 4 + 3] = char(255);
        }
        return true;
    }
}

std::vector<adam::launcher_art::Picture> adam::launcher_art::load_images(const std::vector<std::pair<uint32_t, std::string>>& wanted) {
    Rom rom{ read_stored_rom() };
    std::vector<Picture> out;
    for (const auto& [index, name] : wanted) {
        Picture picture{ .index = index, .name = name };
        if (rom.bytes.empty() || !decode(rom, index, name, picture.rgba, picture.width, picture.height)) {
            picture.rgba.clear();
        }
        out.push_back(std::move(picture));
    }
    return out;
}
