#include "reader.h"
#include "game_formats.h"
#include "khdays/assets/kh2_bar.h"

#include <algorithm>
#include <bit>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace rc {
namespace {
using format::u16;
using format::u32;

std::string trim(std::string s) {
    const auto start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return {};
    return s.substr(start, s.find_last_not_of(" \t\r") - start + 1);
}

void annotate(Script& script) {
    std::istringstream input(script.raw);
    std::string line, previous, before_previous;
    const std::regex branch(R"(^jz\s+(\w+)$)"), push(R"(^push\s+(\d{1,5})$)");
    bool callback = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto code = trim(line.substr(0, line.find(';')));
        if (code == "REACTION_CALLBACK:") {
            callback = true;
            script.annotated += "; Entry point for reaction commands\n";
        }
        std::smatch to, number;
        if (callback && previous == "sub" && std::regex_match(code, to, branch) &&
            std::regex_match(before_previous, number, push)) {
            const int id = std::stoi(number[1]);
            script.dispatch.emplace(id, to[1].str());
            script.annotated += "; Possible branch for command " + std::to_string(id) + " -> " + to[1].str() + "\n";
        }
        if (callback && code == "ret") callback = false;
        if ((code.starts_with("call ") || code.starts_with("call.far ")))
            if (const auto target = code.find_last_of(' '); target != std::string::npos && code[target + 1] == 'L')
                script.annotated += "; Calls subroutine " + code.substr(target + 1) + ".\n";
        script.annotated += line + "\n";
        if (!code.empty()) { before_previous = previous; previous = code; }
    }
}

std::vector<ReactionClip> read_reaction_clips(const Bytes& data) {
    std::vector<ReactionClip> clips;
    struct Variant { const char* body; const char* weapon; PlayerRig rig; int joints; };
    constexpr Variant variants[] = {
        {"0010", "0011", PlayerRig::Sora, 228},
        {"0140", nullptr, PlayerRig::Roxas, 229},
        {"0200", nullptr, PlayerRig::LionSora, 133},
    };
    const auto entries = kh2::bar_entries(data.data(), data.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& pack = entries[i];
        if (pack.type != 20 || !pack.size || !kh2::is_bar(data.data(), data.size(), pack.offset)) continue;
        const auto tracks = kh2::bar_entries(data.data(), pack.offset + pack.size, pack.offset, true);
        const auto read_track = [&](const char* tag) -> std::unique_ptr<kh2::Motion> {
            if (!tag) return {};
            for (const auto& track : tracks) {
                if (track.type != 17 || track.name != tag || !track.size ||
                    !kh2::is_bar(data.data(), pack.offset + pack.size, track.offset)) continue;
                for (const auto& part : kh2::bar_entries(data.data(), track.offset + track.size, track.offset, true)) {
                    if (part.type != 9 || part.size <= 0x90) continue;
                    try {
                        return std::make_unique<kh2::Motion>(data.data(), part.offset + part.size, part.offset + 0x90);
                    } catch (const std::runtime_error&) { /* Skip unsupported motion encodings. */ }
                }
            }
            return {};
        };
        for (const auto& variant : variants) {
            auto body = read_track(variant.body);
            if (!body || body->model_joints() != variant.joints) continue;
            clips.push_back({static_cast<int>(i), pack.name, variant.body, std::move(body),
                             read_track(variant.weapon), variant.rig});
            break;
        }
    }
    return clips;
}

}

std::string utf8(const fs::path& path) {
    const auto s = path.u8string();
    return {s.begin(), s.end()};
}

fs::path path_of(const char* value) {
    return fs::path(reinterpret_cast<const char8_t*>(value));
}

std::map<int, Command> read_commands(const GameData& game) {
    const Bytes bytes = game.read("03system.bin");
    for (const auto& entry : kh2::bar_entries(bytes.data(), bytes.size())) {
        if (entry.name != "cmd" || entry.type != 2) continue;
        const auto data = std::span(bytes).subspan(entry.offset, entry.size);
        if (data.size() < 8 || u32(data, 0) != 2 || u32(data, 4) > (data.size() - 8) / 48)
            throw std::runtime_error("Invalid CMD table in 03system.bin");
        std::map<int, Command> out;
        for (size_t i = 0; i < u32(data, 4); ++i) {
            const size_t p = 8 + 48 * i;
            const int id = u16(data, p);
            out.emplace(id, Command{id, u16(data, p + 2), std::bit_cast<std::int16_t>(u16(data, p + 4)),
                                     std::bit_cast<std::int32_t>(u32(data, p + 8)), u32(data, p + 12),
                                     std::bit_cast<float>(u32(data, p + 16))});
        }
        return out;
    }
    throw std::runtime_error("CMD table not found in 03system.bin");
}

std::map<int, std::string> read_names(const GameData& game, const std::string& lang) {
    const auto input = fs::path("msg") / lang / "sys.bar";
    if (!game.exists(input)) return {};
    const auto bytes = game.read(input);
    for (const auto& entry : kh2::bar_entries(bytes.data(), bytes.size())) {
        if (entry.type == 2)
            return format::read_message_table(std::span(bytes).subspan(entry.offset, entry.size));
    }
    return {};
}

Object read_object(const GameData& game, const fs::path& path, bool decode_scripts) {
    const auto bytes = game.read(path);
    const auto entries = kh2::bar_entries(bytes.data(), bytes.size());
    Object object;
    try {
        object.model = game.packed() ? kh2::import_mdlx(bytes.data(), bytes.size(), utf8(path.stem()))
                                     : kh2::import_mdlx(game.folder() / path);
        if (object.model.model.meshes.empty()) object.model_status = "This MDLX has no visible mesh.";
    } catch (const std::runtime_error& error) {
        // Some reaction sources are script objects or use map geometry. Their
        // motion packages and scripts can still be previewed independently.
        object.model_status = error.what();
    }
    if (const auto mset = game.motion_file(path); !mset.empty()) {
        const auto data = game.read(mset);
        object.motions = std::make_unique<kh2::MotionSet>(data.data(), data.size());
        if (object.model_status.empty()) {
            try {
                object.player = std::make_unique<kh2::MotionPlayer>(kh2::read_motion_skeleton(bytes.data(), bytes.size()));
            } catch (const std::runtime_error&) { /* Static mesh and script remain available. */ }
        }
        object.reaction_clips = read_reaction_clips(data);
        for (size_t slot = 0; slot < object.motions->slots(); ++slot) {
            if (const auto* attrs = object.motions->attributes(slot)) {
                for (const auto& r : attrs->ranges)
                    if (r.id == 6 || r.id == 7 || r.id == 14 || r.id == 20 || r.id == 25 || r.id == 26)
                        object.windows.push_back({slot, r.start, r.end, r.id, r.params});
            }
        }
    }
    for (const auto& entry : entries) {
        if (!decode_scripts || (entry.type & 0xffff) != 3 || !entry.size) continue;
        Script script;
        script.name = entry.name;
        try {
            const auto input = std::span(bytes).subspan(entry.offset, entry.size);
            const auto& cache = game.cache();
            const auto key = cache.enabled() ? cache_key("bdx-v3-", input) : std::string{};
            const auto text = cache.read(key);
            if (text.empty()) {
                script.raw = format::disassemble_bdx(input);
                const auto* first = reinterpret_cast<const std::uint8_t*>(script.raw.data());
                cache.write(key, {first, script.raw.size()});
            } else {
                script.raw.assign(text.begin(), text.end());
            }
            annotate(script);
        } catch (const std::exception& error) { script.status = error.what(); }
        object.scripts.push_back(std::move(script));
    }
    return object;
}

const char* type_name(int type) {
    switch (type) {
        case 6: return "Enable conditional reaction";
        case 7: return "Disable conditional reaction";
        case 14: return "Allow reaction";
        case 20: return "Reaction on this object";
        case 25: return "Reaction on another object";
        case 26: return "AnmatrCommand 2";
        default: return "Trigger";
    }
}

const char* param_name(int type, size_t i) {
    if (i == 0 && (type == 6 || type == 7)) return "possible condition ID";
    if (i == 0 && (type == 20 || type == 25 || type == 26)) return "command ID";
    if (i == 1 && (type == 6 || type == 7 || type == 20)) return "collision group";
    return "parameter";
}

bool has_command_id(const Window& window) {
    return (window.type == 20 || window.type == 25 || window.type == 26) && !window.args.empty();
}

bool contains_command(const Window& window, int id) {
    return has_command_id(window) && window.args.front() == id;
}

std::string branch_excerpt(const Script& script, const std::string& branch) {
    const std::string marker = "\n" + branch + ":\n";
    const auto start = script.annotated.find(marker);
    if (start == std::string::npos) return {};
    size_t end = start + marker.size();
    while (end < script.annotated.size()) {
        const auto nl = script.annotated.find('\n', end);
        const auto line = std::string_view(script.annotated).substr(end, nl - end);
        if (line.ends_with(':')) break;
        end = nl == std::string::npos ? script.annotated.size() : nl + 1;
        const auto op = line.find_first_not_of(" \t");
        if (op != std::string_view::npos && (line.substr(op).starts_with("jmp ") || line.substr(op).starts_with("ret")))
            break;
    }
    return script.annotated.substr(start + 1, end - start - 1);
}

std::set<int> Object::command_ids() const {
    std::set<int> ids;
    for (const auto& w : windows)
        if (has_command_id(w)) ids.insert(w.args[0]);
    for (const auto& script : scripts)
        for (const auto& [id, label] : script.dispatch) ids.insert(id);
    return ids;
}

const char* player_name(PlayerRig rig) {
    if (rig == PlayerRig::Roxas) return "Roxas";
    if (rig == PlayerRig::LionSora) return "Lion Sora";
    return "Sora";
}

const char* player_model(PlayerRig rig) {
    if (rig == PlayerRig::Roxas) return "P_EX110.mdlx";
    if (rig == PlayerRig::LionSora) return "P_LK100.mdlx";
    return "P_EX100.mdlx";
}

std::span<const ReactionPair> known_pairs(const std::string& object) {
    // BDX capture indices and the target's action routines in the corresponding Axel MSET.
    static constexpr ReactionPair axel[] = {
        {443, 255, 242, "Clear Light", "Axel is driven down in the finisher"},
        {444, 252, 0, "Burst Frontier", "Axel holds position as Sora strikes"},
        {490, 254, 241, "Overtaker", "Axel clashes with Sora in the air"},
    };
    if (object == "B_EX110" || object == "B_EX110_LV99") return axel;
    // Cerberus's success states and the player motion taken in each step.
    static constexpr ReactionPair cerberus[] = {
        {38, 253, 223, "Evade", "Cerberus reacts to Evade"},
        {39, 254, 224, "Jump!", "Cerberus follows Sora's jump"},
        {41, 255, 225, "Dog Paddle", "Cerberus is stunned by the finisher"},
    };
    if (object == "B_HE020") return cerberus;
    static constexpr ReactionPair fire[] = {
        {505, 252, 241, "Firagun", "Volcanic Lord plays its swing animation"},
        {661, 253, 242, "Firagun", "Volcanic Lord plays its swing-end animation"},
    };
    static constexpr ReactionPair ice[] = {
        {506, 252, 241, "Blizzagun", "Blizzard Lord plays its swing animation"},
        {662, 253, 242, "Blizzagun", "Blizzard Lord plays its swing-end animation"},
    };
    if (object == "B_AL100_1ST" || object == "B_AL100_FIRE") return fire;
    if (object == "B_AL100_2ND" || object == "B_AL100_ICE") return ice;
    static const std::map<std::string_view, std::vector<ReactionPair>> traced = {
#include "traced_pairs.inc"
    };
    const auto found = traced.find(object);
    return found == traced.end() ? std::span<const ReactionPair>{} : std::span<const ReactionPair>{found->second};
}

const char* object_name(const std::string& id) {
    if (id == "P_EX100") return "Sora";
    if (id == "P_EX100_KH1F") return "Sora (Limit Form)";
    if (id == "B_EX110") return "Axel";
    if (id == "B_EX110_LV99") return "Axel (Data)";
    if (id == "B_EX130") return "Xaldin";
    if (id == "B_HE020") return "Cerberus";
    if (id == "B_AL100_1ST" || id == "B_AL100_FIRE") return "Volcanic Lord";
    if (id == "B_AL100_2ND" || id == "B_AL100_ICE") return "Blizzard Lord";
    if (id == "M_EX010") return "Soldier";
    if (id == "M_EX660") return "Rapid Thruster";
    if (id == "M_EX520") return "Hook Bat";
    if (id == "M_EX990") return "Dusk";
    if (id == "F_BB050") return "Shadow Stalker (Chandelier)";
    return nullptr;
}
}
