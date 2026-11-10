#include "reader.h"
#include "khdays/assets/kh2_bar.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <fstream>
#include <iterator>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace rc {
namespace {
Bytes read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Could not open " + utf8(file));
    return {std::istreambuf_iterator<char>(in), {}};
}

std::uint32_t u32(std::span<const std::uint8_t> data, size_t p) {
    return data[p] | (std::uint32_t(data[p + 1]) << 8) | (std::uint32_t(data[p + 2]) << 16)
           | (std::uint32_t(data[p + 3]) << 24);
}

std::uint16_t u16(std::span<const std::uint8_t> data, size_t p) {
    return static_cast<std::uint16_t>(data[p] | (data[p + 1] << 8));
}

struct Temp {
    fs::path dir;
    Temp() {
        const auto root = fs::temp_directory_path();
        const auto base = std::to_string(SDL_GetPerformanceCounter());
        for (int i = 0; i < 20; ++i) {
            dir = root / ("kh2-rc-viewer-" + base + "-" + std::to_string(i));
            if (fs::create_directory(dir)) return;
        }
        throw std::runtime_error("Could not create a temporary folder");
    }
    ~Temp() {
        std::error_code ec;
        for (const auto* file : {"script.bdx", "script.txt", "sys.bar", "sys.xml"})
            fs::remove(dir / file, ec);
        fs::remove(dir, ec);
    }
};

void run(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* proc = SDL_CreateProcess(argv.data(), true);
    if (!proc) throw std::runtime_error(SDL_GetError());
    int code = -1;
    void* output = SDL_ReadProcess(proc, nullptr, &code);
    SDL_free(output);
    SDL_DestroyProcess(proc);
    if (code != 0)
        throw std::runtime_error("OpenKH exited with code " + std::to_string(code));
}

void run_decoder(const char* tool, std::initializer_list<std::string> arguments) {
    const char* base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(SDL_GetError());
    const auto decoder = path_of(base) / "Decoder";
    const auto runtime = decoder / "runtime" / "dotnet.exe";
    const auto command = decoder / (std::string("OpenKh.Command.") + tool + ".dll");
    if (!fs::is_regular_file(runtime) || !fs::is_regular_file(command))
        throw std::runtime_error("Decoder files are missing. Extract the full KH2 RC Viewer ZIP.");
    std::vector<std::string> args{utf8(runtime), "--fx-version", "10.0.9", utf8(command)};
    args.insert(args.end(), arguments);
    run(args);
}

void write_input(const fs::path& path, std::span<const std::uint8_t> data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.write(reinterpret_cast<const char*>(data.data()), data.size()))
        throw std::runtime_error("Could not write the decoder input.");
}

std::string trim(std::string s) {
    const auto start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return {};
    return s.substr(start, s.find_last_not_of(" \t\r") - start + 1);
}

std::string xml_text(std::string s) {
    for (const auto& [key, value] : std::initializer_list<std::pair<std::string, std::string>>{
            {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
        for (size_t pos = s.find(key); pos != std::string::npos; pos = s.find(key, pos + value.size()))
            s.replace(pos, key.size(), value);
    }
    return s;
}

void annotate(Script& script) {
    const std::map<std::string, std::string> syscalls = {
        {"trap_sysobj_motion_start", "Starts an animation."},
        {"trap_sysobj_motion_push", "Queues an animation."},
        {"trap_sysobj_motion_id", "Reads the current animation ID."},
        {"trap_obj_motion_capture", "Loads a captured reaction motion for an actor."},
        {"trap_obj_motion_speed", "Changes the animation speed."},
        {"trap_obj_motion_check_range", "Checks an animation range."},
        {"trap_obj_hook", "Runs an object hook script."},
        {"trap_obj_attach", "Attaches the object to another actor."},
        {"trap_obj_act_start", "Starts the named action on the object."},
        {"trap_sysobj_motion_change", "Switches the active animation."},
        {"trap_damage_is_reaction", "Checks whether damage is part of a reaction."},
        {"trap_damage_exec_reaction", "Executes the reaction associated with the damage."},
    };
    std::istringstream input(script.raw);
    std::string line, previous, before_previous;
    const std::regex branch(R"(^beqz\s+(\w+)$)"), push(R"(^push\s+(\d{1,5})$)");
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
        if (code.starts_with("jal "))
            if (const auto target = code.find_last_of(' '); target != std::string::npos && code[target + 1] == 'L')
                script.annotated += "; Calls subroutine " + code.substr(target + 1) + ".\n";
        if (code.starts_with("syscall")) {
            const auto at = line.find("trap_");
            if (at != std::string::npos) {
                const auto end = line.find_first_of(" \t", at);
                if (const auto found = syscalls.find(line.substr(at, end - at)); found != syscalls.end())
                    script.annotated += "; " + found->second + "\n";
            }
        }
        script.annotated += line + "\n";
        if (!code.empty()) { before_previous = previous; previous = code; }
    }
}

std::vector<ReactionClip> read_reaction_clips(const Bytes& data) {
    std::vector<ReactionClip> clips;
    const auto entries = kh2::bar_entries(data.data(), data.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& pack = entries[i];
        if (pack.type != 20 || !pack.size || !kh2::is_bar(data.data(), data.size(), pack.offset)) continue;
        ReactionClip clip{static_cast<int>(i), pack.name, "0010", {}, {}};
        for (const auto& track : kh2::bar_entries(data.data(), pack.offset + pack.size, pack.offset, true)) {
            // For the Sora variant, 0010 is the body and 0011 is the primary weapon.
            if (track.type != 17 || (track.name != "0010" && track.name != "0011") || !track.size ||
                !kh2::is_bar(data.data(), pack.offset + pack.size, track.offset)) continue;
            auto& selected = track.name == "0010" ? clip.motion : clip.weapon;
            if (selected) continue;
            for (const auto& part : kh2::bar_entries(data.data(), track.offset + track.size, track.offset, true)) {
                if (part.type != 9 || part.size <= 0x90) continue;
                try {
                    selected = std::make_unique<kh2::Motion>(data.data(), part.offset + part.size, part.offset + 0x90);
                } catch (const std::runtime_error&) { /* Skip unsupported motion encodings. */ }
                break;
            }
        }
        if (clip.motion) clips.push_back(std::move(clip));
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
    const auto& cache = game.cache();
    const auto key = cache.enabled() ? cache_key("messages-v1-", bytes) : std::string{};
    auto data = cache.read(key);
    if (data.empty()) {
        const Temp temp;
        write_input(temp.dir / "sys.bar", bytes);
        run_decoder("MsgTool", {"-i", utf8(temp.dir / "sys.bar"), "-e", "eusys"});
        data = read(temp.dir / "sys.xml");
        cache.write(key, data);
    }
    const std::string xml(data.begin(), data.end());
    const std::regex message(R"rx(<message id="(\d{1,9})"\s*>\s*<text>([^<]*)</text>)rx");
    std::map<int, std::string> out;
    for (auto i = std::sregex_iterator(xml.begin(), xml.end(), message); i != std::sregex_iterator(); ++i)
        out.emplace(std::stoi((*i)[1]), xml_text((*i)[2]));
    return out;
}

Object read_object(const GameData& game, const fs::path& path, bool decode_scripts) {
    const auto bytes = game.read(path);
    const auto entries = kh2::bar_entries(bytes.data(), bytes.size());
    Object object;
    object.model = game.packed() ? kh2::import_mdlx(bytes.data(), bytes.size(), utf8(path.stem()))
                                 : kh2::import_mdlx(game.folder() / path);
    if (object.model.model.meshes.empty()) throw std::runtime_error("This MDLX has no visible mesh");
    if (const auto mset = game.motion_file(path); !mset.empty()) {
        const auto data = game.read(mset);
        object.motions = std::make_unique<kh2::MotionSet>(data.data(), data.size());
        object.player = std::make_unique<kh2::MotionPlayer>(kh2::read_motion_skeleton(bytes.data(), bytes.size()));
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
            const auto key = cache.enabled() ? cache_key("bdx-v1-", input) : std::string{};
            auto text = cache.read(key);
            if (text.empty()) {
                const Temp temp;
                const auto src = temp.dir / "script.bdx", dst = temp.dir / "script.txt";
                write_input(src, input);
                run_decoder("Bdxio", {"decode", utf8(src), utf8(dst)});
                text = read(dst);
                cache.write(key, text);
            }
            script.raw.assign(text.begin(), text.end());
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
        if (op != std::string_view::npos && (line.substr(op).starts_with("b ") || line.substr(op).starts_with("ret")))
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

std::span<const ReactionPair> known_pairs(const std::string& object) {
    // BDX capture indices and the target's action routines in the corresponding Axel MSET.
    static constexpr ReactionPair axel[] = {
        {443, 255, 242, "Clear Light", "Axel is driven down in the finisher"},
        {444, 252, 0, "Burst Frontier", "Axel holds position as Sora strikes"},
        {490, 254, 241, "Overtaker", "Axel clashes with Sora in the air"},
    };
    if (object == "B_EX110" || object == "B_EX110_LV99") return axel;
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
    return {};
}

const char* object_name(const std::string& id) {
    if (id == "P_EX100") return "Sora";
    if (id == "P_EX100_KH1F") return "Sora (Limit Form)";
    if (id == "B_EX110") return "Axel";
    if (id == "B_EX110_LV99") return "Axel (Data)";
    if (id == "B_EX130") return "Xaldin";
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
