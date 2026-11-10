#include "game_formats.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rc::format {
namespace {
enum class Arg { None, Ssub, Word, Imm32, Float32, Data, Work, Rel16, Rel32 };
enum class Flow { Next, Fork, Jump, Stop };

struct Opcode {
    std::string_view name;
    size_t words = 1;
    Flow flow = Flow::Next;
    std::array<Arg, 2> args{};
};

// VM layout from khdays-port's tools/mods/kh2_bdx.py, read from the PC interpreter.
Opcode decode_opcode(std::uint16_t word) {
    const unsigned op = word & 0xf, sub = (word >> 4) & 3, arg = word >> 6;
    constexpr std::string_view addresses[] = {"push.frame-address", "push.work-address",
                                              "push.indirect-address", "push.code-address"};
    constexpr std::string_view loads[] = {"push.frame", "push.work", "push.indirect", "push.code"};
    constexpr std::string_view pops[] = {"pop.frame", "pop.work", "pop.indirect", "pop.code"};
    constexpr std::string_view copies[] = {"copy.frame", "copy.work", "copy.indirect", "copy.code"};
    constexpr Arg bases[] = {Arg::Word, Arg::Work, Arg::Word, Arg::Data};
    constexpr std::string_view unary[2][12] = {
        {"itof", "", "neg", "not", "eqz", "abs", "ltz", "lez", "eqz", "nez", "gez", "gtz"},
        {"", "ftoi", "fneg", "", "", "fabs", "fltz", "flez", "feqz", "fnez", "fgez", "fgtz"},
    };
    constexpr std::string_view binary[2][12] = {
        {"add", "sub", "mul", "div", "mod", "and", "or", "xor", "shl", "sar", "land", "lor"},
        {"fadd", "fsub", "fmul", "fdiv", "fmod"},
    };
    constexpr std::string_view jumps[] = {"jmp", "jz", "jnz"};
    constexpr std::string_view misc[] = {"yield", "exit", "ret", "drop", "", "dup",
                                          "sin", "cos", "deg2rad", "rad2deg"};
    switch (op) {
    case 0:
        if (sub < 2) return {sub ? "push.float" : "push", 3, Flow::Next, {sub ? Arg::Float32 : Arg::Imm32}};
        if (arg < 4) return {(sub == 2 ? addresses : loads)[arg], 2, Flow::Next, {bases[arg]}};
        break;
    case 1:
        if (arg < 4) return {pops[arg], 2, Flow::Next, {bases[arg]}};
        break;
    case 2:
        if (arg < 4) return {copies[arg], 3, Flow::Next, {Arg::Word, bases[arg]}};
        break;
    case 3: return {"load", 2, Flow::Next, {Arg::Word}};
    case 4: return {"store", 1, Flow::Next, {Arg::Ssub}};
    case 5:
    case 6:
        if (sub < 2 && arg < 12) return {(op == 5 ? unary : binary)[sub][arg]};
        break;
    case 7:
        if (arg < 3) return {jumps[arg], 2, arg ? Flow::Fork : Flow::Jump, {Arg::Rel16}};
        break;
    case 8: return {"call", 2, Flow::Fork, {Arg::Ssub, Arg::Rel16}};
    case 9:
        if (arg < std::size(misc)) return {misc[arg], 1, arg == 1 || arg == 2 ? Flow::Stop : Flow::Next};
        break;
    case 10: return {"syscall", 2, Flow::Next, {Arg::Ssub, Arg::Word}};
    case 11: return {"call.far", 3, Flow::Fork, {Arg::Ssub, Arg::Rel32}};
    }
    return {};
}

struct CallShape {
    unsigned key;
    unsigned inputs;
    bool returns;
};

constexpr CallShape call_shapes[] = {
#include "bdx_calls.inc"
};

std::string_view call_description(unsigned key) {
    // Named only where the port traced the game function (docs/KH2_ENEMIES.md).
    switch (key) {
    case 0x00000003: return "Read the tick length";
    case 0x00010006: return "Register an actor state";
    case 0x00010008: return "Enter the named actor state";
    case 0x00010009: return "Queue the named actor state";
    case 0x0001000b: return "Start an actor motion";
    case 0x0001000d: return "Queue an actor motion";
    case 0x0001000e: return "Check whether the motion ended";
    case 0x00010012: return "Attach an actor to another actor";
    case 0x0001001d: return "Play a captured motion on an actor";
    case 0x00010059: return "Move the actor along a direction";
    case 0x00010065: return "Check whether the actor plays a captured motion";
    case 0x0001006f: return "Steer an actor along a direction";
    case 0x00010070: return "Start a script thread on an actor";
    case 0x00010093: return "Read the actor's position";
    case 0x00010094: return "Set the actor's position";
    case 0x00010097: return "Check a motion trigger";
    case 0x0001009d: return "Start a named camera sequence";
    case 0x000100a5: return "Offer a reaction command";
    case 0x000100ee: return "Read the current captured motion";
    case 0x00020014: return "Start a script thread";
    default: return {};
    }
}

std::string trigger_name(std::uint32_t id) {
    switch (id) {
    case 0: return "OBJ_INIT";
    case 1: return "OBJ_MAIN";
    case 6: return "OBJ_DAMAGE";
    case 11: return "REACTION_CALLBACK";
    case 13: return "OBJ_FALL";
    case 14: return "OBJ_LAND";
    case 15: return "OBJ_ATTACK";
    case 16: return "OBJ_ANMATR_EFFECT";
    case 17: return "OBJ_ANMATR_CALLBACK";
    default: return "TR" + std::to_string(id);
    }
}

constexpr std::array<std::string_view, 256> glyphs = {
#include "message_glyphs.inc"
};

// The count of parameter bytes after each system-text control opcode.
constexpr std::array<unsigned, 32> control_bytes{
    0, 0, 0, 0, 1, 6, 1, 4, 3, 1, 1, 1, 1, 0, 1, 5,
    0, 4, 2, 4, 2, 2, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1,
};

std::string message_text(Data text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const auto code = text[i++];
        if (!code) return out;
        if (code < control_bytes.size()) {
            const auto skip = control_bytes[code];
            if (skip > text.size() - i) break;
            if (code >= 0x19) out += '?';
            else out += glyphs[code];
            i += skip;
        } else if (code == 0x73) {
            out += '\t';
        } else if (!glyphs[code].empty()) {
            out += glyphs[code];
        } else {
            out += '?';
        }
    }
    return {}; // Drop unterminated text.
}

struct Operand {
    Arg kind;
    std::int32_t value;
};

std::array<Operand, 2> operands(Data words, size_t address, const Opcode& op) {
    std::array<Operand, 2> result{};
    size_t at = (address + 1) * 2;
    for (size_t i = 0; i < op.args.size(); ++i) {
        const auto kind = op.args[i];
        auto& value = result[i];
        value.kind = kind;
        switch (kind) {
        case Arg::None: break;
        case Arg::Ssub:
            value.value = u16(words, address * 2) >> 6;
            break;
        case Arg::Rel16:
            value.value = std::bit_cast<std::int16_t>(u16(words, at));
            at += 2;
            break;
        case Arg::Data:
        case Arg::Work:
        case Arg::Word:
            value.value = u16(words, at);
            at += 2;
            break;
        case Arg::Imm32:
        case Arg::Float32:
        case Arg::Rel32:
            value.value = std::bit_cast<std::int32_t>(u32(words, at));
            at += 4;
            break;
        }
        if (kind == Arg::Rel16 || kind == Arg::Rel32) {
            const auto target = std::int64_t(address) + std::int64_t(op.words) + value.value;
            value.value = target >= 0 && target <= std::numeric_limits<std::int32_t>::max()
                ? static_cast<std::int32_t>(target) : -1;
        }
    }
    return result;
}

void write_instruction(std::ostream& out, const Opcode& op, const std::array<Operand, 2>& args) {
    out << ' ' << op.name;
    for (size_t i = 0; i < args.size() && args[i].kind != Arg::None; ++i) {
        out << (i ? ", " : " ");
        const auto [kind, value] = args[i];
        if (kind == Arg::Rel16 || kind == Arg::Rel32 || kind == Arg::Data) out << 'L' << value;
        else if (kind == Arg::Work) out << 'W' << value;
        else if (kind == Arg::Float32) out << std::bit_cast<float>(value);
        else out << value;
    }
    if (op.name == "syscall") {
        const auto key = (static_cast<unsigned>(args[0].value) << 16) | static_cast<std::uint16_t>(args[1].value);
        const auto meaning = call_description(key);
        const auto* shape = std::ranges::lower_bound(call_shapes, key, {}, &CallShape::key);
        if (!meaning.empty()) out << " ; " << meaning;
        if (shape != std::end(call_shapes) && shape->key == key)
            out << (meaning.empty() ? " ; " : " ") << '(' << shape->inputs << " in, " << int(shape->returns) << " out)";
    }
    out << '\n';
}
}

std::map<int, std::string> read_message_table(Data data) {
    if (data.size() < 8 || u32(data, 0) != 1 || u32(data, 4) > (data.size() - 8) / 8)
        throw std::runtime_error("Invalid KH2 message table.");
    const size_t count = u32(data, 4);
    const size_t header_size = 8 + count * 8;
    std::map<int, std::string> result;
    for (size_t i = 0; i < count; ++i) {
        const size_t record = 8 + i * 8;
        const auto id = u32(data, record);
        const size_t offset = u32(data, record + 4);
        if (id > std::numeric_limits<int>::max() || offset < header_size || offset >= data.size()) continue;
        auto text = message_text(data.subspan(offset));
        if (!text.empty()) result.try_emplace(static_cast<int>(id), std::move(text));
    }
    return result;
}

std::string disassemble_bdx(Data data) {
    // The 16-byte script name precedes the VM's word-addressed area.
    if (data.size() < 36 || data.size() % 2) throw std::runtime_error("Invalid BDX header.");
    const auto words = data.subspan(16);
    const size_t word_count = words.size() / 2;
    std::map<size_t, std::vector<std::string>> labels;
    std::vector<size_t> pending;
    std::vector<std::pair<std::uint32_t, size_t>> entries;
    size_t start = 6;
    for (;;) {
        if (word_count - start < 4) throw std::runtime_error("Unterminated BDX trigger table.");
        const auto id = u32(words, start * 2);
        const size_t addr = u32(words, start * 2 + 4);
        start += 4;
        if (!id && !addr) break;
        entries.emplace_back(id, addr);
    }
    for (const auto& [id, addr] : entries) {
        if (addr < start || addr >= word_count) continue;
        labels[addr].emplace_back(trigger_name(id));
        pending.push_back(addr);
    }

    // Follow reachable instructions from each trigger. Operand words are
    // marked separately so branches cannot reclassify them as instructions.
    std::vector<Opcode> code(word_count);
    std::vector<bool> occupied(word_count);
    std::set<size_t> addressed;
    std::set<size_t> data_refs;
    do {
        while (!pending.empty()) {
            size_t address = pending.back();
            pending.pop_back();
            while (address < word_count && !occupied[address]) {
                const auto op = decode_opcode(u16(words, address * 2));
                if (op.name.empty() || op.words > word_count - address) break;
                auto begin = occupied.begin() + address;
                if (std::any_of(begin, begin + op.words, [](bool used) { return used; })) break;
                std::fill_n(begin, op.words, true);
                code[address] = op;

                for (const auto& arg : operands(words, address, op)) {
                    const auto target = static_cast<size_t>(arg.value);
                    if (target < start || target >= word_count) continue;
                    if (arg.kind == Arg::Rel16 || arg.kind == Arg::Rel32) {
                        if (addressed.insert(target).second) pending.push_back(target);
                    } else if (arg.kind == Arg::Data) {
                        data_refs.insert(target);
                    }
                }
                if (op.flow == Flow::Stop || op.flow == Flow::Jump) break;
                address += op.words;
            }
        }

        // States and detached threads can enter a routine without a branch.
        // Read only constants from contiguous instructions already decoded.
        std::vector<size_t> previous;
        previous.reserve(13);
        for (size_t at = start; at < word_count;) {
            const auto& op = code[at];
            if (op.name.empty()) {
                previous.clear();
                ++at;
                continue;
            }
            if (op.name == "syscall") {
                const auto args = operands(words, at, op);
                const auto table = args[0].value, number = args[1].value;
                const auto enqueue_routine = [&](size_t count, size_t position) {
                    if (previous.size() < count) return;
                    const auto arg = previous[previous.size() - count + position];
                    if (code[arg].name != "push") return;
                    const auto target = static_cast<size_t>(operands(words, arg, code[arg])[0].value);
                    if (target >= start && target < word_count && addressed.insert(target).second)
                        pending.push_back(target);
                };
                if (table == 1 && number == 6) {
                    enqueue_routine(12, 3);
                    enqueue_routine(12, 10);
                } else if (table == 1 && number == 112) enqueue_routine(3, 1);
                else if (table == 2 && number == 20) enqueue_routine(2, 1);
            }
            previous.push_back(at);
            if (previous.size() > 12) previous.erase(previous.begin());
            at += op.words;
        }
    } while (!pending.empty());

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<float>::max_digits10);
    const auto name_end = std::find(data.begin(), data.begin() + 16, 0);
    out << "; " << std::string(data.begin(), name_end) << '\n'
        << "; Work " << u32(words, 0) << " bytes, frame " << u32(words, 4)
        << " bytes, stack " << u32(words, 8) << " bytes\n"
        << " section .text\n";
    for (size_t address = start; address < word_count;) {
        if (const auto found = labels.find(address); found != labels.end())
            for (const auto& name : found->second) out << name << ":\n";
        if (addressed.contains(address) || data_refs.contains(address)) out << 'L' << address << ":\n";
        if (const auto& op = code[address]; !op.name.empty()) {
            write_instruction(out, op, operands(words, address, op));
            address += op.words;
            continue;
        }
        if (data_refs.contains(address)) {
            const size_t start_byte = address * 2;
            size_t end_byte = start_byte;
            while (end_byte < words.size() && end_byte - start_byte < 1024) {
                const size_t at = end_byte / 2;
                if (occupied[at] || (at != address && (labels.contains(at) || addressed.contains(at) ||
                                                        data_refs.contains(at)))) break;
                if (words[end_byte] < 0x20 || words[end_byte] > 0x7e) break;
                ++end_byte;
            }
            const bool padding = end_byte % 2 == 0;
            if (end_byte > start_byte && end_byte < words.size() && words[end_byte] == 0 &&
                !occupied[end_byte / 2] && !addressed.contains(end_byte / 2) &&
                !labels.contains(end_byte / 2) &&
                (end_byte / 2 == address || !data_refs.contains(end_byte / 2)) &&
                (!padding || words[end_byte + 1] == 0)) {
                const std::string value(words.begin() + start_byte, words.begin() + end_byte);
                out << " db " << std::quoted(value) << ", 0" << (padding ? ", 0\n" : "\n");
                address = end_byte / 2 + 1;
                continue;
            }
        }
        out << " dw 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4)
            << u16(words, address * 2) << std::dec << '\n';
        ++address;
    }
    return out.str();
}
}
