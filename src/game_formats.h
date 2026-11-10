#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace rc::format {
using Data = std::span<const std::uint8_t>;

inline std::uint16_t u16(Data data, size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

inline std::uint32_t u32(Data data, size_t offset) {
    return std::uint32_t(u16(data, offset)) | (std::uint32_t(u16(data, offset + 2)) << 16);
}

std::string disassemble_bdx(Data data);
std::map<int, std::string> read_message_table(Data data);
}
