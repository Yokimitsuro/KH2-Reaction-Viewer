#include "game_data.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "stb_image.h"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

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

// From the port's installer/collection_archive.py. Read the S-box from the
// player's executable and check its digest before using it.
std::array<std::uint8_t, 256> load_archive_table(const fs::path& folder) {
    fs::path exe;
    auto ancestor = folder;
    for (int depth = 0; depth < 4 && exe.empty(); ++depth) {
        for (const auto* name : {"KINGDOM HEARTS II FINAL MIX.exe", "KH2FM.exe"}) {
            const auto candidate = ancestor / name;
            if (fs::is_regular_file(candidate)) { exe = candidate; break; }
        }
        ancestor = ancestor.parent_path();
    }
    if (exe.empty())
        throw std::runtime_error("Select the KH2 installation folder with KINGDOM HEARTS II FINAL MIX.exe and Image/dt.");
    auto data = read_file(exe);
    constexpr size_t table_size = 256, known_offset = 0x710940;
    constexpr std::array<std::uint8_t, 32> expected{
        0x75,0x9b,0x2f,0x22,0xf7,0xe0,0xbf,0xd2,0x57,0x06,0x3a,0x01,0xf3,0xde,0x0c,0x08,
        0x35,0x90,0x52,0x17,0xaa,0x92,0x6d,0xf6,0xde,0xf1,0x34,0xa1,0x43,0x13,0x67,0x2d,
    };
    const auto matches = [&](size_t offset) {
        std::array<std::uint8_t, 32> hash{};
        return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data.data() + offset,
                                        table_size, hash.data(), static_cast<ULONG>(hash.size()))) && hash == expected;
    };
    size_t offset = known_offset;
    if (data.size() < known_offset + table_size || !matches(offset)) {
        for (offset = 0; offset + table_size <= data.size(); offset += 16)
            if (matches(offset)) break;
        if (offset + table_size > data.size())
            throw std::runtime_error("The installed KH2 executable has no recognized archive table.");
    }
    std::array<std::uint8_t, table_size> table{};
    std::copy_n(data.data() + offset, table.size(), table.begin());
    return table;
}

void decode_prefix(Bytes& data, std::array<std::uint8_t, 16> header,
                   const std::array<std::uint8_t, 256>& table) {
    constexpr std::array<std::uint32_t, 11> round_codes{0,1,2,4,8,16,32,64,128,27,54};
    for (size_t i = 0; i < header.size(); ++i)
        if (!header[i]) header[i] = static_cast<std::uint8_t>(i);
    std::array<std::uint32_t, 44> words{};
    for (size_t i = 0; i < 4; ++i) words[i] = u32(header.data() + 4 * i);
    for (size_t i = 4; i < words.size(); ++i) {
        auto next = words[i - 1];
        if (i % 4 == 0) {
            next = std::rotr(next, 8);
            std::uint32_t substituted = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                substituted |= std::uint32_t(table[(next >> shift) & 0xff]) << shift;
            next = substituted ^ round_codes[i / 4];
        }
        words[i] = words[i - 4] ^ next;
    }
    std::array<std::uint32_t, 4> mask{};
    for (size_t i = 0; i < words.size(); ++i) mask[i % 4] ^= words[i];
    const size_t length = std::min<size_t>(0x100, data.size() - data.size() % 16);
    for (size_t i = 0; i < length; ++i)
        data[i] ^= static_cast<std::uint8_t>(mask[(i / 4) % 4] >> ((i % 4) * 8));
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
        archive_table_ = load_archive_table(path);
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
    const bool compressed = unpacked_size > 16 && encoding > 0;
    const auto stored_size = compressed ? static_cast<std::uint32_t>(encoding) : unpacked_size;
    constexpr std::uint32_t asset_limit = 128 * 1024 * 1024;
    if ((unpacked_size > 16 && encoding < -2) || header_size > entry.length || stored_size > entry.length - header_size ||
        stored_size > asset_limit || unpacked_size > asset_limit) throw invalid();
    std::string key;
    if (!source.fingerprint.empty()) {
        const auto identity = source.fingerprint + "|" + std::to_string(entry.offset) + "|" + std::to_string(entry.length);
        key = cache_key("asset-v2-", {reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()});
        if (auto cached = cache_.read(key, unpacked_size); !cached.empty()) return cached;
    }

    package.seekg(static_cast<std::streamoff>(entry.offset + header_size));
    Bytes data(stored_size);
    read_exact(package, data);
    if (unpacked_size > 16 && (compressed || encoding == -1)) decode_prefix(data, header, archive_table_);
    if (!compressed) {
        if (!key.empty()) cache_.write(key, data);
        return data;
    }
    // The archive stores the byte count without its alignment padding.
    Bytes output(unpacked_size + 15);
    const int decoded = stbi_zlib_decode_buffer(reinterpret_cast<char*>(output.data()),
        static_cast<int>(output.size()), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    if (decoded < static_cast<int>(unpacked_size)) throw invalid();
    output.resize(unpacked_size);
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
