#include "cache.h"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace rc {
namespace {
constexpr std::uintmax_t max_size = 256ULL * 1024 * 1024;
constexpr std::uintmax_t max_entry = 24ULL * 1024 * 1024;

bool is_cache_file(const fs::path& path) {
    if (path.extension() != ".bin") return false;
    const auto stem = path.stem().wstring();
    return stem.starts_with(L"asset-v1-") || stem.starts_with(L"asset-v2-") || stem.starts_with(L"bdx-v1-") ||
           stem.starts_with(L"bdx-v2-") || stem.starts_with(L"bdx-v3-") || stem.starts_with(L"messages-v1-");
}
}

Digest md5(std::span<const std::uint8_t> data) {
    Digest hash{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0,
                                  const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                                  hash.data(), static_cast<ULONG>(hash.size()))))
        throw std::runtime_error("Could not calculate a data hash.");
    return hash;
}

std::string cache_key(std::string_view prefix, std::span<const std::uint8_t> data) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result(prefix);
    for (const auto byte : md5(data)) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

CacheStore::CacheStore(fs::path folder) : folder_(std::move(folder)) {
    if (folder_.empty()) return;
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (ec) { folder_.clear(); return; }
    for (const auto& entry : fs::directory_iterator(folder_, ec)) {
        if (!entry.is_regular_file(ec) || !is_cache_file(entry.path())) continue;
        const auto size = entry.file_size(ec);
        if (!ec) size_ += size;
    }
}

Bytes CacheStore::read(const std::string& key, size_t expected_size) const {
    if (!enabled()) return {};
    const auto file = folder_ / (key + ".bin");
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size == 0 || size > max_entry || (expected_size && size != expected_size)) return {};
    Bytes bytes(static_cast<size_t>(size));
    std::ifstream stream(file, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
}

void CacheStore::make_room(std::uintmax_t bytes) const {
    if (size_ + bytes <= max_size) return;
    using File = std::pair<fs::file_time_type, fs::path>;
    std::vector<File> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder_, ec)) {
        if (!entry.is_regular_file(ec) || !is_cache_file(entry.path())) continue;
        const auto stamp = entry.last_write_time(ec);
        if (!ec) files.emplace_back(stamp, entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& [stamp, file] : files) {
        const auto bytes_on_disk = fs::file_size(file, ec);
        if (!ec && fs::remove(file, ec)) size_ -= std::min(size_, bytes_on_disk);
        if (size_ + bytes <= max_size) break;
    }
}

void CacheStore::write(const std::string& key, std::span<const std::uint8_t> bytes) const {
    if (!enabled() || bytes.empty() || bytes.size() > max_entry) return;
    const auto path = folder_ / (key + ".bin");
    std::error_code ec;
    const auto existing = fs::file_size(path, ec);
    if (!ec && existing == bytes.size()) return;
    make_room(bytes.size());
    if (size_ + bytes.size() > max_size) return;
    const auto temp = folder_ / (key + "-" + std::to_string(GetCurrentProcessId()) + ".tmp");
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (stream) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) { fs::remove(temp, ec); return; }
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (existing != static_cast<std::uintmax_t>(-1)) size_ -= std::min(size_, existing);
        size_ += bytes.size();
    }
    else fs::remove(temp, ec);
}
}
