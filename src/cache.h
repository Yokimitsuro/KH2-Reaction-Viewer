#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rc {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 16>;

Digest md5(std::span<const std::uint8_t> data);
std::string cache_key(std::string_view prefix, std::span<const std::uint8_t> data);

class CacheStore {
public:
    explicit CacheStore(fs::path folder = {});
    bool enabled() const { return !folder_.empty(); }
    Bytes read(const std::string& key, size_t expected_size = 0) const;
    void write(const std::string& key, std::span<const std::uint8_t> bytes) const;

private:
    fs::path folder_;
    mutable std::uintmax_t size_ = 0;
    void make_room(std::uintmax_t bytes) const;
};
}
