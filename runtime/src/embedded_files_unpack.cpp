// Unpacks the files built into adam.exe (see embedded_files.h) into the data folder.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "embedded_files.h"

namespace adam {
    const EmbeddedFile* find_embedded_file(const char* path) {
        for (size_t i = 0; i < embedded_file_count; i++) {
            if (strcmp(embedded_files[i].path, path) == 0) {
                return &embedded_files[i];
            }
        }
        return nullptr;
    }

    static bool same_contents(const std::filesystem::path& path, const EmbeddedFile& file) {
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) != file.size || ec) {
            return false;
        }
        std::ifstream in{ path, std::ios::binary };
        std::vector<char> contents{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        return contents.size() == file.size && memcmp(contents.data(), file.data, file.size) == 0;
    }

    bool unpack_embedded_files(const std::filesystem::path& dir) {
        bool ok = true;
        for (size_t i = 0; i < embedded_file_count; i++) {
            const EmbeddedFile& file = embedded_files[i];
            if (!file.unpack) {
                continue;
            }
            std::filesystem::path path = dir / std::filesystem::path{ file.path }.make_preferred();
            if (same_contents(path, file)) {
                continue;
            }
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out{ path, std::ios::binary | std::ios::trunc };
            out.write((const char*)file.data, file.size);
            if (!out) {
                fprintf(stderr, "Could not write %s\n", path.string().c_str());
                ok = false;
            }
        }
        return ok;
    }
}
