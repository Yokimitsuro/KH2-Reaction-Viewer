#pragma once

#include "cache.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rc {
class GameData {
public:
    explicit GameData(const fs::path& selected, const fs::path& cache_folder = {});
    const fs::path& folder() const { return folder_; }
    bool packed() const { return !packages_.empty(); }
    bool exists(const fs::path& file) const;
    Bytes read(const fs::path& file) const;
    Bytes read_remastered(const fs::path& file, std::string_view name) const;
    std::vector<fs::path> models() const;
    fs::path motion_file(const fs::path& model) const;
    const CacheStore& cache() const { return cache_; }

private:
    struct Package { fs::path path; std::string fingerprint; };
    struct PackageEntry {
        std::size_t package;
        std::uint64_t offset;
        std::uint32_t length;
    };

    static Digest digest(const fs::path& name);
    Bytes read_payload(std::ifstream& package, const std::array<std::uint8_t, 16>& header,
                       std::uint64_t offset, std::uint64_t limit, std::uint32_t unpacked_size,
                       std::int32_t encoding, const std::string& identity) const;
    void add_package(const fs::path& index);
    void read_object_table();

    fs::path folder_;
    std::vector<Package> packages_;
    std::array<std::uint8_t, 256> archive_table_{};
    std::map<Digest, PackageEntry> entries_;
    std::map<fs::path, fs::path> motion_files_;
    CacheStore cache_;
};
}
