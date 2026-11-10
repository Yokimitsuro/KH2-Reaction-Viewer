#include "game_data.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <bit>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace rc {
namespace {
std::uint32_t u32(const std::uint8_t* data) {
    return data[0] | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

std::string generic_utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

void read_exact(std::ifstream& file, std::span<std::uint8_t> data) {
    if (data.empty()) return;
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Truncated game file.");
}

Bytes read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open " + generic_utf8(path));
    Bytes data(static_cast<size_t>(fs::file_size(path)));
    read_exact(stream, data);
    return data;
}

std::string entry_name(const std::uint8_t* start, std::string_view extension = {}) {
    const auto* end = std::find(start, start + 32, std::uint8_t(0));
    std::string name(start, end);
    if (!extension.empty() && name.ends_with(extension)) name.resize(name.size() - extension.size());
    if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        return {};
    return name;
}

// PC original-asset cipher adapted from OpenKH EgsEncryption (Apache-2.0).
// Every block uses the XOR of the same 11 round keys.
void decode_prefix(Bytes& data, const std::array<std::uint8_t, 16>& seed) {
    constexpr std::array<std::uint8_t, 256> substitution{
        0x7E,0x88,0x97,0x55,0x0B,0x06,0xF1,0x08,0xEB,0xBB,0x14,0x1C,0xD8,0x7A,0xEC,0x41,
        0x34,0xB2,0xA3,0x46,0xEF,0x6B,0xFE,0xE1,0xCF,0x53,0xA5,0x05,0x12,0xD2,0x8E,0x52,
        0x4A,0x80,0xE9,0x81,0xB0,0xF0,0xB4,0x9C,0xFF,0x0F,0x15,0x13,0xDA,0x73,0x4E,0x77,
        0xBE,0xD7,0x30,0xE5,0xF6,0x5A,0x11,0x37,0x67,0xBC,0x83,0x6F,0x27,0x76,0xD0,0xCD,
        0x69,0x0D,0x2E,0x51,0x42,0x90,0xB8,0xB6,0x4C,0xAD,0xCE,0x5B,0x1A,0x1F,0xF5,0xAF,
        0x01,0xF8,0x5E,0x3A,0x6E,0x68,0x8B,0xE8,0x9F,0xC9,0xD9,0x26,0x92,0x29,0xC8,0x33,
        0x98,0x32,0x54,0xD4,0x44,0x25,0x66,0xAC,0x5F,0x99,0x21,0xE4,0x8F,0x1D,0xC2,0xD5,
        0xA4,0x62,0xF9,0x02,0x61,0xDE,0x59,0xE7,0x07,0x9A,0xFA,0x2F,0x95,0x3F,0x86,0xD3,
        0x78,0xA7,0x75,0xED,0xD6,0x2D,0x64,0x87,0xBD,0xC7,0xC1,0xAA,0xF2,0x8C,0x17,0xCB,
        0x31,0x8A,0xC3,0xCC,0x04,0xEE,0x6A,0xAB,0x5C,0x22,0x70,0xCA,0x9E,0x71,0x6D,0x85,
        0x45,0x5D,0xB9,0xA9,0xA6,0x10,0x47,0xFB,0x82,0x7D,0x84,0x7B,0xC6,0xE2,0x38,0xFC,
        0x2B,0x0E,0x20,0x9D,0xC5,0xF3,0x39,0xA8,0xA0,0x65,0x58,0x43,0x7C,0xE3,0x36,0x18,
        0x72,0x49,0x79,0xAE,0xD1,0x74,0x40,0xC4,0x91,0x4F,0x24,0x63,0xBF,0xBA,0x23,0x96,
        0x50,0xB3,0x57,0xDF,0x1E,0x03,0x48,0x7F,0x35,0x4D,0x3E,0xE6,0xA1,0xDD,0x09,0x3C,
        0x3D,0x3B,0x56,0x8D,0x93,0x2A,0x9B,0x4B,0x0C,0x28,0xB1,0xE0,0x60,0x89,0x19,0xDB,
        0x2C,0xF7,0x6C,0xB5,0x1B,0x94,0xC0,0xDC,0xEA,0xB7,0x0A,0xF4,0x16,0xFD,0xA2,0x00,
    };
    constexpr std::array<std::uint8_t, 10> round_codes{1,2,4,8,16,32,64,128,27,54};
    auto key = seed;
    for (size_t i = 0; i < key.size(); ++i)
        if (key[i] == 0) key[i] = static_cast<std::uint8_t>(i);
    auto mask = key;
    for (const auto round : round_codes) {
        key[0] ^= substitution[key[13]] ^ round;
        key[1] ^= substitution[key[14]];
        key[2] ^= substitution[key[15]];
        key[3] ^= substitution[key[12]];
        for (size_t i = 4; i < key.size(); ++i) key[i] ^= key[i - 4];
        for (size_t i = 0; i < key.size(); ++i) mask[i] ^= key[i];
    }
    for (size_t i = 0; i < std::min<size_t>(256, data.size()); ++i) data[i] ^= mask[i % mask.size()];
}
}

GameData::GameData(const fs::path& selected, const fs::path& cache_folder) : cache_(cache_folder) {
    const auto root = fs::absolute(selected).lexically_normal();
    for (const auto& path : {root, root / "original", root / "data" / "kh2"}) {
        if (fs::is_regular_file(path / "03system.bin") && fs::is_directory(path / "obj")) {
            folder_ = path;
            if (exists("00objentry.bin")) read_object_table();
            return;
        }
    }
    for (const auto& path : {root, root / "dt", root / "Image" / "dt"}) {
        if (!fs::is_regular_file(path / "kh2_first.hed")) continue;
        folder_ = path;
        for (const auto* part : {"first", "second", "third", "fourth", "fifth", "sixth"}) {
            const auto index = path / (std::string("kh2_") + part + ".hed");
            if (fs::is_regular_file(index)) add_package(index);
        }
        if (!exists("03system.bin") || !exists("00objentry.bin"))
            throw std::runtime_error("The KH2 packages are missing 03system.bin or 00objentry.bin.");
        read_object_table();
        return;
    }
    throw std::runtime_error("Select the game's Image/dt folder or a KH2 extraction containing 03system.bin and obj.");
}

Digest GameData::digest(const fs::path& name) {
    const auto text = generic_utf8(name);
    return md5({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void GameData::add_package(const fs::path& index) {
    auto package = index;
    package.replace_extension(".pkg");
    if (!fs::is_regular_file(package)) throw std::runtime_error("Missing package " + generic_utf8(package.filename()));
    const auto data = read_file(index);
    constexpr size_t entry_size = 32;
    if (data.empty() || data.size() % entry_size != 0)
        throw std::runtime_error("Invalid package index " + generic_utf8(index.filename()));
    const auto size = fs::file_size(package);
    const size_t package_id = packages_.size();
    for (size_t p = 0; p < data.size(); p += entry_size) {
        const auto* row = data.data() + p;
        const auto offset = u32(row + 16) | (std::uint64_t(u32(row + 20)) << 32);
        const auto length = u32(row + 24);
        if (length < 16 || offset > size || length > size - offset) continue;
        Digest hash{};
        std::copy_n(row, hash.size(), hash.begin());
        entries_.try_emplace(hash, PackageEntry{package_id, offset, length});
    }
    std::string fingerprint;
    if (cache_.enabled()) {
        std::error_code ec;
        const auto package_time = fs::last_write_time(package, ec);
        if (!ec) {
            const auto index_time = fs::last_write_time(index, ec);
            if (!ec) fingerprint = generic_utf8(package) + "|" + std::to_string(size) + "|" +
                std::to_string(package_time.time_since_epoch().count()) + "|" +
                std::to_string(index_time.time_since_epoch().count());
        }
    }
    packages_.push_back({std::move(package), std::move(fingerprint)});
}

bool GameData::exists(const fs::path& file) const {
    return packed() ? entries_.contains(digest(file)) : fs::is_regular_file(folder_ / file);
}

Bytes GameData::read(const fs::path& file) const {
    if (!packed()) return read_file(folder_ / file);
    const auto found = entries_.find(digest(file));
    const auto invalid = [&] { return std::runtime_error("Invalid KH2 package entry for " + generic_utf8(file)); };
    if (found == entries_.end()) throw std::runtime_error("Not found in KH2 packages: " + generic_utf8(file));
    const auto& entry = found->second;
    const auto& source = packages_[entry.package];
    std::ifstream package(source.path, std::ios::binary);
    package.seekg(static_cast<std::streamoff>(entry.offset));
    std::array<std::uint8_t, 16> header{};
    read_exact(package, header);
    const std::uint32_t unpacked_size = u32(header.data());
    const std::uint32_t texture_count = u32(header.data() + 4);
    const auto encoding = std::bit_cast<std::int32_t>(u32(header.data() + 8));
    const std::uint64_t header_size = header.size() + std::uint64_t(texture_count) * 48;
    const auto stored_size = encoding >= 0 ? static_cast<std::uint32_t>(encoding) : unpacked_size;
    constexpr std::uint32_t asset_limit = 128 * 1024 * 1024;
    if (encoding < -2 || header_size > entry.length || stored_size > entry.length - header_size ||
        stored_size > asset_limit || unpacked_size > asset_limit) throw invalid();
    std::string key;
    if (!source.fingerprint.empty()) {
        const auto identity = source.fingerprint + "|" + std::to_string(entry.offset) + "|" + std::to_string(entry.length);
        key = cache_key("asset-v1-", {reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()});
        if (auto cached = cache_.read(key, unpacked_size); !cached.empty()) return cached;
    }

    package.seekg(static_cast<std::streamoff>(entry.offset + header_size));
    Bytes data(stored_size);
    read_exact(package, data);
    if (encoding >= -1) decode_prefix(data, header);
    if (encoding < 0) {
        if (!key.empty()) cache_.write(key, data);
        return data;
    }
    if (data.size() < 2) throw invalid();
    Bytes output(unpacked_size);
    const int decoded = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(output.data()),
        static_cast<int>(output.size()), reinterpret_cast<const char*>(data.data() + 2),
        static_cast<int>(data.size() - 2));
    if (decoded != static_cast<int>(unpacked_size)) throw invalid();
    if (!key.empty()) cache_.write(key, output);
    return output;
}

void GameData::read_object_table() {
    const auto table = read("00objentry.bin");
    constexpr size_t row_size = 96;
    if (table.size() < 8 || u32(table.data() + 4) > (table.size() - 8) / row_size)
        throw std::runtime_error("Invalid KH2 object table.");
    for (size_t i = 0; i < u32(table.data() + 4); ++i) {
        const auto* row = table.data() + 8 + i * row_size;
        const auto model = entry_name(row + 8);
        if (model.empty()) continue;
        const fs::path path = fs::path("obj") / (model + ".mdlx");
        if (!exists(path)) continue;
        const auto motion = entry_name(row + 40, ".mset");
        auto entry = motion_files_.try_emplace(path).first;
        if (!motion.empty() && entry->second.empty())
            entry->second = fs::path("obj") / (motion + ".mset");
    }
}

std::vector<fs::path> GameData::models() const {
    std::vector<fs::path> result;
    if (packed()) {
        for (const auto& [model, motion] : motion_files_) result.push_back(model);
    } else {
        for (const auto& file : fs::directory_iterator(folder_ / "obj")) {
            auto extension = file.path().extension().wstring();
            for (auto& ch : extension) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
            if (file.is_regular_file() && extension == L".mdlx") result.push_back(fs::path("obj") / file.path().filename());
        }
        std::sort(result.begin(), result.end());
    }
    return result;
}

fs::path GameData::motion_file(const fs::path& model) const {
    auto adjacent = model;
    adjacent.replace_extension(".mset");
    if (exists(adjacent)) return adjacent;
    const auto entry = motion_files_.find(model);
    if (entry != motion_files_.end() && !entry->second.empty() && exists(entry->second)) return entry->second;
    return {};
}
}
