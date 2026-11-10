#include "catalog.h"
#include "viewport.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace rc;
using ImGui::TextUnformatted;

std::string lowercase(std::string s) {
    for (auto& c : s) if ('A' <= c && c <= 'Z') c += 'a' - 'A';
    return s;
}

std::string label(int id, const std::map<int, Command>& commands, const std::map<int, std::string>& names) {
    if (auto cmd = commands.find(id); cmd != commands.end()) {
        if (auto text = names.find(cmd->second.text & 0x7fff); text != names.end() && !text->second.empty())
            return text->second;
    }
    return "Command " + std::to_string(id);
}

std::string range_label(const Window& window) {
    return std::to_string(window.start) +
           (window.end == 65535 ? " to end" : "-" + std::to_string(window.end));
}

std::string actor_title(const std::string& id) {
    if (const char* name = object_name(id)) return std::string(name) + " · " + id;
    return id;
}

struct Selection {
    fs::path pending;
    std::mutex mutex;
    static void SDLCALL chosen(void* userdata, const char* const* list, int) {
        if (!list || !list[0]) return;
        auto& self = *static_cast<Selection*>(userdata);
        std::lock_guard lock(self.mutex);
        self.pending = path_of(list[0]);
    }
    fs::path consume() {
        std::lock_guard lock(mutex);
        return std::exchange(pending, {});
    }
};

struct App {
    struct ChainStep { int command, clip, target; float duration; const char* title; };
    enum class Browse { Reactions, Chains, Models };
    std::unique_ptr<GameData> game;
    fs::path current, data_folder;
    std::vector<fs::path> objects;
    std::map<int, Command> commands;
    std::map<int, std::string> names;
    std::vector<ReactionGroup> reactions;
    std::shared_ptr<Object> object;
    std::list<std::pair<fs::path, std::shared_ptr<Object>>> recent_objects;
    std::unique_ptr<Object> sora;
    std::unique_ptr<Object> weapon;
    Viewport view;
    Selection choose_game;
    std::string error, message_note;
    char filter[120]{};
    Browse browse = Browse::Reactions;
    int reaction = -1, reaction_target = 0;
    bool details = false, sora_only = false, reaction_preview = false;
    bool select_reactions_tab = false, select_chains_tab = false;
    std::vector<ChainStep> chain;
    std::vector<SceneSegment> chain_camera;
    const char* chain_title = "Axel's counterattack";
    float chain_tick = 0;
    int chain_phase = 0;
    int cmd = -1, slot = -1;
    float tick = 0, speed = 1;
    bool playing = false, loop = true;
    bool follow_camera = true;
    bool scene_active = false, show_scene_tab = false;
    bool show_bdx_tab = false;
    int scene_clip = -1, scene_target = -1;
    float scene_tick = 0, scene_delay = 0, scene_yaw = 0;
    std::array<float, 3> scene_offset{};
    ImVec2 view_at{}, view_size{};

    void open_root(const fs::path& path) {
        auto selected = std::make_unique<GameData>(path, data_folder.empty() ? fs::path{} : data_folder / "cache");
        auto entries = read_commands(*selected);
        objects = selected->models();
        game = std::move(selected);
        commands = std::move(entries);
        load_names();
        current.clear();
        view.load(nullptr);
        view.load_sora(nullptr);
        view.load_weapon(nullptr);
        sora.reset();
        weapon.reset();
        object.reset();
        recent_objects.clear();
        cmd = slot = -1;
        reaction = -1;
        reaction_target = 0;
        browse = Browse::Reactions;
        chain.clear();
        details = false;
        reaction_preview = select_chains_tab = false;
        select_reactions_tab = true;
        scene_active = show_scene_tab = false;
        playing = false;
        error.clear();
        reactions = load_reactions(*game, commands, names);
        if (!reactions.empty()) select_reaction(0);
    }

    void remember_game() const {
        if (data_folder.empty() || !game) return;
        std::ofstream file(data_folder / "last-game.txt", std::ios::binary | std::ios::trunc);
        if (file) file << utf8(game->folder()) << '\n';
    }

    void reopen_game() {
        if (data_folder.empty()) return;
        std::ifstream file(data_folder / "last-game.txt", std::ios::binary);
        std::string line;
        if (!std::getline(file, line) || line.empty()) return;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (fs::is_directory(path_of(line.c_str()))) open_root(path_of(line.c_str()));
    }

    void load_names() {
        names.clear();
        message_note.clear();
        if (!game) return;
        try {
            names = read_names(*game, "us");
        } catch (const std::exception& problem) { message_note = problem.what(); }
    }

    void open_object(const fs::path& path) {
        const auto found = std::find_if(recent_objects.begin(), recent_objects.end(), [&](const auto& entry) {
            return entry.first == path;
        });
        if (found == recent_objects.end()) {
            recent_objects.emplace_front(path, std::make_shared<Object>(read_object(*game, path)));
            if (recent_objects.size() > 6) recent_objects.pop_back();
        } else {
            recent_objects.splice(recent_objects.begin(), recent_objects, found);
        }
        view.load(recent_objects.front().second.get());
        object = recent_objects.front().second;
        current = path;
        scene_active = show_scene_tab = false;
        chain.clear();
        sora_only = false;
        scene_clip = scene_target = -1;
        slot = cmd = -1;
        if (object->motions) {
            for (size_t i = 0; i < object->motions->slots(); ++i) {
                if (object->motions->motion(i)) { set_motion(static_cast<int>(i), 0); break; }
            }
        }
        if (const auto ids = object->command_ids(); !ids.empty()) set_command(*ids.begin());
        error.clear();
    }

    const kh2::Motion* motion() const {
        return object && object->motions && slot >= 0 ? object->motions->motion(slot) : nullptr;
    }

    const kh2::Motion* scene_motion() const {
        return scene_clip >= 0 && object ? object->reaction_clips[scene_clip].motion.get() : nullptr;
    }

    const kh2::Motion* target_motion() const {
        return scene_target >= 0 && object && object->motions ? object->motions->motion(scene_target) : nullptr;
    }

    const kh2::Motion* weapon_motion() const {
        return scene_clip >= 0 && object ? object->reaction_clips[scene_clip].weapon.get() : nullptr;
    }

    const kh2::Motion* sora_landing() const {
        if (!sora || !sora->motions) return nullptr;
        // LAND (motion ID 5) uses the player MSET's four battle/weapon slots.
        for (const int id : {20, 21, 23, 22}) {
            const auto* motion = sora->motions->motion(id);
            if (motion && motion->model_joints() == int(sora->model.joints.size())) return motion;
        }
        return nullptr;
    }

    const ReactionPair* scene_pair() const {
        if (scene_clip < 0) return nullptr;
        for (const auto& pair : known_pairs(utf8(current.stem())))
            if (pair.package_slot == object->reaction_clips[scene_clip].package_slot && pair.target_slot == scene_target)
                return &pair;
        return nullptr;
    }

    const ReactionTarget* selected_target() const {
        if (reaction < 0) return nullptr;
        return &reactions[reaction].targets[reaction_target];
    }

    std::string target_title() const {
        if (const auto* link = selected_target(); link && link->model == current) return link->name;
        const auto id = utf8(current.stem());
        return object_name(id) ? object_name(id) : id;
    }

    void select_reaction(int index, int target = 0) {
        const auto& link = reactions[index].targets[target];
        if (current != link.model) open_object(link.model);
        reaction = index;
        reaction_target = target;
        reaction_preview = false;
        chain.clear();
        scene_active = playing = false;
        view.load(object.get());
        set_command(link.command);
    }

    void start_scene(int clip, int target) {
        if (!sora) {
            auto model = std::make_unique<Object>(read_object(*game, fs::path("obj") / "P_EX100.mdlx", false));
            view.load_sora(model.get());
            sora = std::move(model);
            const auto path = fs::path("obj") / "W_EX010.mdlx";
            if (game->exists(path)) {
                try {
                    weapon = std::make_unique<Object>(read_object(*game, path, false));
                    view.load_weapon(weapon.get());
                } catch (const std::runtime_error&) { /* The body preview remains available. */ }
            }
        }
        if (!sora->player || object->reaction_clips[clip].motion->model_joints() != int(sora->model.joints.size()))
            throw std::runtime_error("This reaction track does not match the Sora skeleton in this extraction.");
        scene_clip = clip;
        scene_target = target;
        chain.clear();
        sora_only = false;
        scene_tick = scene_delay = scene_yaw = 0;
        scene_offset = {};
        if (!scene_pair()) {
            scene_offset[2] = 1.5F;
            scene_yaw = 180.0F;
        }
        scene_active = true;
        playing = false;
        view.frame_scene();
        error.clear();
    }

    void play_pair(const ReactionPair& pair) {
        for (size_t i = 0; i < object->reaction_clips.size(); ++i) {
            if (object->reaction_clips[i].package_slot != pair.package_slot) continue;
            const auto* target = object->motions->motion(pair.target_slot);
            if (!target || target->model_joints() != int(object->model.joints.size())) break;
            cmd = pair.command;
            start_scene(static_cast<int>(i), pair.target_slot);
            if (pair.command == 444) scene_offset[2] = 4.8F;
            show_scene_tab = true;
            return;
        }
        throw std::runtime_error("This extraction is missing one of the expected tracks.");
    }

    void play_selected_reaction() {
        const auto* link = selected_target();
        if (!link) return;
        if (const auto* pair = pair_for(*link)) {
            play_pair(*pair);
        } else if (link->package_slot >= 0) {
            const auto clip = std::find_if(object->reaction_clips.begin(), object->reaction_clips.end(),
                                           [&](const auto& entry) { return entry.package_slot == link->package_slot; });
            if (clip == object->reaction_clips.end())
                throw std::runtime_error("The linked Sora track is missing in this copy of the game.");
            start_scene(static_cast<int>(clip - object->reaction_clips.begin()), -1);
            sora_only = true;
        } else if (!motion() || !std::any_of(object->windows.begin(), object->windows.end(),
                       [&](const auto& w) { return int(w.slot) == slot && contains_command(w, link->command); })) {
            return;
        }
        reaction_preview = true;
        playing = true;
    }

    bool has_model(const char* filename) const {
        return game && game->exists(fs::path("obj") / filename);
    }

    ChainStep chain_step(int id, int package_slot, int target_slot, const char* title = nullptr,
                         bool target_underlay = false) const {
        const auto& clips = object->reaction_clips;
        const auto clip = std::find_if(clips.begin(), clips.end(), [&](const auto& c) { return c.package_slot == package_slot; });
        const auto* target = target_slot >= 0 && object->motions ? object->motions->motion(target_slot) : nullptr;
        if (clip == clips.end() || clip->motion->length_ticks() <= 0 ||
            (target_slot >= 0 && (!target || target->model_joints() != int(object->model.joints.size()))))
            throw std::runtime_error("A clip required for this chain is missing.");
        const float duration = target && !target_underlay
            ? std::max(clip->motion->length_ticks(), target->length_ticks()) : clip->motion->length_ticks();
        return {id, static_cast<int>(clip - clips.begin()), target_slot, duration, title};
    }

    void start_chain(std::vector<ChainStep> steps, const char* title) {
        reaction = -1;
        start_scene(steps.front().clip, steps.front().target);
        chain = std::move(steps);
        chain_camera.clear();
        for (const auto& step : chain) {
            const auto& clip = object->reaction_clips[step.clip];
            chain_camera.push_back({clip.motion.get(), step.target < 0 ? nullptr : object->motions->motion(step.target),
                                    clip.weapon.get(), step.duration, step.target >= 0, false});
        }
        chain_title = title;
        sora_only = chain.front().target < 0;
        chain_tick = 0;
        loop = false;
        details = false;
        select_chains_tab = true;
        seek_chain();
    }

    void open_axel_chain() {
        const auto data = fs::path("obj") / "B_EX110_LV99.mdlx";
        const auto normal = fs::path("obj") / "B_EX110.mdlx";
        const auto model = game->exists(data) ? data : normal;
        if (current != model) open_object(model);
        const auto pairs = known_pairs(utf8(current.stem()));
        std::vector<ChainStep> steps;
        for (int id : {444, 490, 443}) {
            const auto pair = std::find_if(pairs.begin(), pairs.end(), [id](const auto& p) { return p.command == id; });
            if (pair == pairs.end()) throw std::runtime_error("This Axel variant has no chain mapping.");
            // During the opener Axel's idle is an underlay. The length is set by Sora's action.
            steps.push_back(chain_step(id, pair->package_slot, pair->target_slot, nullptr, id == 444));
        }
        start_chain(std::move(steps), "Axel's counterattack");
        view.align_axel_steps(chain_camera, preview_pose());
    }

    void open_thruster_chain() {
        const auto model = fs::path("obj") / "M_EX660.mdlx";
        if (current != model) open_object(model);
        // The callback steers Sora toward a live enemy. Preview the enemy's idle flight across both steps.
        start_chain({chain_step(269, 252, 0, nullptr, true), chain_step(285, 253, 0, nullptr, true)},
                    "Rapid Thruster's aerial combo");
        scene_offset = {0, .55F, 1.35F};
        scene_yaw = 180;
        const float rate = std::min(1.0F, chain_camera[0].target_motion->length_ticks() /
                                         (chain[0].duration + chain[1].duration));
        chain_camera[0].target_rate = chain_camera[1].target_rate = rate;
        chain_camera[1].target_start = chain[0].duration * rate;
    }

    void open_elemental_chain(bool fire) {
        const auto model = fs::path("obj") / (fire ? "B_AL100_1ST.mdlx" : "B_AL100_2ND.mdlx");
        const auto alternate = fs::path("obj") / (fire ? "B_AL100_FIRE.mdlx" : "B_AL100_ICE.mdlx");
        const auto selected = game->exists(model) ? model : alternate;
        if (current != selected) open_object(selected);
        // Both bosses use slot 241 for rc_swing and slot 242 for rc_swing_end.
        start_chain({chain_step(fire ? 505 : 506, 252, 241, fire ? "Firagun · Swing" : "Blizzagun · Swing"),
                     chain_step(fire ? 661 : 662, 253, 242, fire ? "Firagun · Finish" : "Blizzagun · Finish")},
                    fire ? "Firagun combo" : "Blizzagun combo");
        chain_camera.back().blend_ticks = 6;
        chain_camera.back().blend_target_only = true;
        if (!fire) chain_camera.back().landing = sora_landing();
    }

    void seek_chain() {
        float local = chain_tick;
        chain_phase = 0;
        while (chain_phase + 1 < static_cast<int>(chain.size()) && local >= chain[chain_phase].duration)
            local -= chain[chain_phase++].duration;
        const auto& step = chain[chain_phase];
        cmd = step.command;
        scene_clip = step.clip;
        scene_target = step.target;
        sora_only = step.target < 0;
        scene_tick = std::clamp(local, 0.0F, step.duration);
    }

    ScenePose preview_pose() const {
        const auto* sora = scene_motion();
        const auto* target = target_motion();
        const float duration = std::max({0.0F, sora ? sora->length_ticks() : 0.0F,
                                        target ? target->length_ticks() + scene_delay : 0.0F});
        const auto segments = chain.empty() ? std::span<const SceneSegment>{} : std::span<const SceneSegment>(chain_camera);
        SceneSegment segment = chain.empty()
            ? SceneSegment{sora, target, weapon_motion(), duration, !sora_only, !scene_pair()}
            : chain_camera[chain_phase];
        if (chain.empty() && cmd == 662 && !segment.apply_root) segment.landing = sora_landing();
        return {segment, scene_tick, scene_delay, scene_offset,
                scene_yaw, segments, chain_tick, follow_camera, chain_phase > 0 && !chain.empty()
                    ? &chain_camera[chain_phase - 1] : nullptr};
    }

    void set_motion(int id, float start) {
        slot = id;
        tick = motion() ? std::clamp(start, 0.0F, motion()->length_ticks()) : 0;
        playing = false;
    }

    void set_command(int id) {
        cmd = id;
        for (const auto& range : object->windows) {
            if (!contains_command(range, id) || !object->motions->motion(range.slot)) continue;
            set_motion(static_cast<int>(range.slot), static_cast<float>(range.start));
            break;
        }
    }

    void refresh() {
        try {
            if (const auto p = choose_game.consume(); !p.empty()) {
                open_root(p);
                remember_game();
            }
        } catch (const std::exception& problem) { error = problem.what(); }
    }

    void top(SDL_Window* window) {
        ImGui::TextColored({.54F,.78F,1,1}, "KH2 Reaction Viewer"); ImGui::SameLine();
        ImGui::TextDisabled("0.7  /  Kingdom Hearts II Final Mix");
        if (ImGui::Button("Open game data"))
            SDL_ShowOpenFolderDialog(Selection::chosen, &choose_game, window,
                                     game ? utf8(game->folder()).c_str() : nullptr, false);
        ImGui::SameLine();
        if (game) ImGui::TextDisabled("%zu reaction names · %s", reactions.size(),
                                      game->packed() ? "PC packages" : "Extracted files");
        const auto source = game ? utf8(game->folder()) : "Choose Image/dt or a KH2 extraction.";
        const auto note = !error.empty() ? error : !message_note.empty() ? message_note : source;
        ImGui::TextUnformatted(note.c_str());
    }

    void models_panel() {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##objects", "Search by name or model ID", filter, sizeof(filter));
        ImGui::Separator();
        const auto query = lowercase(filter);
        if (ImGui::BeginChild("files")) {
            bool matched = false;
            for (const auto& file : objects) {
                const auto name = utf8(file.stem());
                const auto title = actor_title(name);
                if (lowercase(title).find(query) == std::string::npos) continue;
                matched = true;
                if (ImGui::Selectable(title.c_str(), file == current)) {
                    try { open_object(file); reaction = -1; }
                    catch (const std::exception& problem) { error = problem.what(); }
                }
            }
            if (!matched && game && game->packed() && !query.empty() &&
                query.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos) {
                auto name = query;
                for (auto& ch : name) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                const auto file = fs::path("obj") / (name + ".mdlx");
                if (game->exists(file) && ImGui::Selectable(actor_title(name).c_str(), file == current)) {
                    try { open_object(file); reaction = -1; }
                    catch (const std::exception& problem) { error = problem.what(); }
                }
            }
        }
        ImGui::EndChild();
    }

    void catalog() {
        if (!ImGui::BeginTabBar("browse")) return;
        const auto tab = [&](const char* label, Browse mode, int flags = 0) {
            if (!ImGui::BeginTabItem(label, nullptr, flags)) return false;
            if (browse != mode) { browse = mode; filter[0] = '\0'; }
            return true;
        };
        const int reaction_flags = select_reactions_tab ? ImGuiTabItemFlags_SetSelected : 0;
        select_reactions_tab = false;
        if (tab("Reactions", Browse::Reactions, reaction_flags)) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##reactions", "Find a reaction or enemy", filter, sizeof(filter));
            const auto query = lowercase(filter);
            if (ImGui::BeginChild("reaction-list")) {
                for (size_t i = 0; i < reactions.size(); ++i) {
                    const auto& group = reactions[i];
                    const auto match = std::find_if(group.targets.begin(), group.targets.end(), [&](const auto& target) {
                        return lowercase(target.name).find(query) != std::string::npos ||
                               lowercase(utf8(target.model.stem())).find(query) != std::string::npos ||
                               std::to_string(target.command) == query;
                    });
                    const bool title_matches = lowercase(group.name).find(query) != std::string::npos;
                    if (!title_matches && match == group.targets.end()) continue;
                    ImGui::PushID(static_cast<int>(i));
                    const auto& target = match == group.targets.end() ? group.targets.front() : *match;
                    if (ImGui::Selectable("##reaction-row", reaction == static_cast<int>(i), 0, {0, 40})) {
                        try { select_reaction(static_cast<int>(i), title_matches ? 0 : int(match - group.targets.begin())); }
                        catch (const std::exception& problem) { error = problem.what(); }
                    }
                    const auto at = ImGui::GetItemRectMin();
                    auto* draw = ImGui::GetWindowDrawList();
                    draw->AddText({at.x + 7, at.y + 5}, ImGui::GetColorU32(ImGuiCol_Text), group.name.c_str());
                    auto subtitle = target.name;
                    const bool more_targets = std::any_of(group.targets.begin(), group.targets.end(),
                                                         [&](const auto& link) { return link.name != target.name; });
                    if (more_targets) subtitle += "  + other targets";
                    draw->AddText({at.x + 7, at.y + 23}, IM_COL32(155, 182, 209, 255), subtitle.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        const int chain_flags = select_chains_tab ? ImGuiTabItemFlags_SetSelected : 0;
        select_chains_tab = false;
        if (tab("Chains", Browse::Chains, chain_flags)) {
            if ((has_model("B_EX110_LV99.mdlx") || has_model("B_EX110.mdlx")) &&
                ImGui::Selectable("Axel's counterattack", !chain.empty() && std::string_view(chain_title) == "Axel's counterattack", 0, {0, 30})) {
                try { open_axel_chain(); }
                catch (const std::exception& problem) { error = problem.what(); }
            }
            if (has_model("B_EX110.mdlx") || has_model("B_EX110_LV99.mdlx"))
                ImGui::TextDisabled("3 steps · Sora and Axel");
            if (has_model("M_EX660.mdlx")) {
                if (ImGui::Selectable("Rapid Thruster's aerial combo", !chain.empty() && std::string_view(chain_title) == "Rapid Thruster's aerial combo", 0, {0, 30})) {
                    try { open_thruster_chain(); }
                    catch (const std::exception& problem) { error = problem.what(); }
                }
                ImGui::TextDisabled("2 steps · Sora and Rapid Thruster");
            }
            for (bool fire : {true, false}) {
                if (!has_model(fire ? "B_AL100_1ST.mdlx" : "B_AL100_2ND.mdlx") &&
                    !has_model(fire ? "B_AL100_FIRE.mdlx" : "B_AL100_ICE.mdlx")) continue;
                const char* title = fire ? "Firagun combo" : "Blizzagun combo";
                if (ImGui::Selectable(title, !chain.empty() && std::string_view(chain_title) == title, 0, {0, 30})) {
                    try { open_elemental_chain(fire); }
                    catch (const std::exception& problem) { error = problem.what(); }
                }
                ImGui::TextDisabled("2 steps · Sora and %s", fire ? "Volcanic Lord" : "Blizzard Lord");
            }
            ImGui::EndTabItem();
        }
        if (tab("Models", Browse::Models)) {
            models_panel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void commands_panel() {
        const auto ids = object->command_ids();
        if (ids.empty()) ImGui::TextWrapped("No command references found in this object's ANB ranges or decoded BDX.");
        if (ImGui::BeginChild("ids", {0, std::min(162.0F, ImGui::GetTextLineHeightWithSpacing() * std::max<size_t>(ids.size(), 1) + 8)}, ImGuiChildFlags_Borders)) {
            for (const int id : ids) {
                const auto title = std::to_string(id) + " · " + label(id, commands, names) + "##id" + std::to_string(id);
                if (ImGui::Selectable(title.c_str(), id == cmd)) set_command(id);
            }
        }
        ImGui::EndChild();
        if (cmd < 0) return;
        ImGui::Spacing();
        ImGui::TextColored({.54F,.78F,1,1}, "%s", label(cmd, commands, names).c_str());
        if (const auto found = commands.find(cmd); found != commands.end()) {
            const auto& c = found->second;
            ImGui::Text("ID %d (0x%04X) · Execute %d · Argument %d", cmd, cmd, c.execute, c.argument);
            ImGui::TextDisabled("CMD · text %d · flags 0x%08X · range %.1f", c.text, c.flags, c.range);
        } else ImGui::TextDisabled("No CMD entry for this value");
        for (const auto& pair : known_pairs(utf8(current.stem()))) {
            if (pair.command != cmd) continue;
            if (ImGui::Button("Preview both characters")) {
                try { play_pair(pair); }
                catch (const std::exception& problem) { error = problem.what(); }
            }
        }
        ImGui::SeparatorText("ANB · activation ranges");
        int count = 0;
        for (const auto& w : object->windows) {
            if (!contains_command(w, cmd)) continue;
            ImGui::PushID(count++);
            const auto title = object->motions->name(w.slot) + " / slot " + std::to_string(w.slot) +
                               " · " + range_label(w);
            const bool active = int(w.slot) == slot && tick >= w.start && tick <= w.end;
            if (ImGui::Selectable(title.c_str(), active)) set_motion(int(w.slot), float(w.start));
            ImGui::TextDisabled("%s [%d]", type_name(w.type), w.type);
            for (size_t i = 0; i < w.args.size(); ++i)
                ImGui::BulletText("%s = %u", param_name(w.type, i), unsigned(w.args[i]));
            ImGui::PopID();
        }
        if (!count) ImGui::TextDisabled("No range for this ID in the main MSET tracks.");
        ImGui::SeparatorText("BDX · script references");
        count = 0;
        for (const auto& script : object->scripts) {
            if (auto branch = script.dispatch.find(cmd); branch != script.dispatch.end()) {
                const std::string title = script.name + " · REACTION_CALLBACK -> " + branch->second;
                if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    const auto code = branch_excerpt(script, branch->second);
                    if (!code.empty()) TextUnformatted(code.c_str());
                    ImGui::TreePop();
                }
                ++count;
            }
        }
        if (count) ImGui::TextWrapped("Possible branches found by matching direct ID comparisons.");
        else ImGui::TextDisabled("No direct comparison for this ID found.");
    }

    void motions_panel() {
        if (!object->motions) { ImGui::TextWrapped("No MSET found next to this MDLX."); return; }
        if (ImGui::BeginChild("slots", {0, 160}, ImGuiChildFlags_Borders)) {
            for (size_t i = 0; i < object->motions->slots(); ++i) {
                if (!object->motions->motion(i)) continue;
                const auto name = std::to_string(i) + " · " + object->motions->name(i);
                if (ImGui::Selectable(name.c_str(), int(i) == slot)) set_motion(int(i), 0);
            }
        }
        ImGui::EndChild();
        ImGui::SeparatorText("Reaction ranges for this clip");
        for (const auto& w : object->windows) {
            if (static_cast<int>(w.slot) != slot) continue;
            ImGui::PushID(&w);
            if (ImGui::SmallButton("Jump to start")) set_motion(slot, static_cast<float>(w.start));
            ImGui::SameLine();
            ImGui::Text("%s · %s", range_label(w).c_str(), type_name(w.type));
            for (size_t i = 0; i < w.args.size(); ++i)
                ImGui::BulletText("%s = %u", param_name(w.type, i), unsigned(w.args[i]));
            ImGui::PopID();
        }
    }

    void scripts_panel(bool raw) {
        if (object->scripts.empty()) { ImGui::TextDisabled("This MDLX has no BDX entries."); return; }
        if (!ImGui::BeginTabBar(raw ? "raw-scripts" : "script-blocks")) return;
        for (const auto& script : object->scripts) {
            ImGui::PushID(&script);
            if (ImGui::BeginTabItem(script.name.c_str())) {
                if (!script.status.empty()) ImGui::TextWrapped("%s", script.status.c_str());
                else {
                    const auto& code = raw ? script.raw : script.annotated;
                    if (!raw) ImGui::TextWrapped("English notes explain recognized calls and preserve the original BDX instructions.");
                    if (ImGui::SmallButton("Copy script")) SDL_SetClipboardText(code.c_str());
                    ImGui::InputTextMultiline("##code", const_cast<char*>(code.c_str()), code.size() + 1,
                                              {-1, -1}, ImGuiInputTextFlags_ReadOnly);
                }
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }

    void scene_panel() {
        const auto id = utf8(current.stem());
        const auto pairs = known_pairs(id);
        if (!pairs.empty()) {
            ImGui::SeparatorText("Reaction pairs traced in BDX");
            for (const auto& pair : pairs) {
                const auto name = names.empty() ? std::string(pair.name) : label(pair.command, commands, names);
                if (ImGui::Selectable((name + " · " + std::to_string(pair.command)).c_str(), scene_active && scene_pair() == &pair)) {
                    try { play_pair(pair); }
                    catch (const std::exception& problem) { error = problem.what(); }
                }
            }
        }
        if (scene_active) {
            ImGui::SeparatorText("Scene tracks");
            const auto& player = object->reaction_clips[scene_clip];
            const auto* pair = scene_pair();
            if (pair) ImGui::Text("Sora performs %s", pair->name);
            ImGui::Text("Sora · %s / %s", player.package.c_str(), player.track.c_str());
            if (pair) ImGui::TextWrapped("%s", pair->target_action);
            ImGui::Text("%s · %s / slot %d", object_name(id) ? object_name(id) : "Target",
                        scene_target < 0 ? "Static pose" : object->motions->name(scene_target).c_str(), scene_target);
            if (pair) ImGui::TextWrapped("Original clips linked through BDX. Preview placement is adjustable.");
            else ImGui::TextWrapped("Manually paired clips.");
            if (ImGui::Button("Return to single model")) {
                scene_active = playing = false;
                view.load(object.get());
            }
        }
        if (ImGui::CollapsingHeader("Choose clips manually", pairs.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            const auto label = scene_clip < 0 ? "Choose Sora track" : object->reaction_clips[scene_clip].package.c_str();
            if (ImGui::BeginCombo("Sora", label)) {
                for (size_t i = 0; i < object->reaction_clips.size(); ++i) {
                    const auto& clip = object->reaction_clips[i];
                    if (clip.motion->model_joints() != 228) continue;
                    const auto name = clip.package + " · package " + std::to_string(clip.package_slot);
                    if (ImGui::Selectable(name.c_str(), int(i) == scene_clip)) {
                        try { start_scene(int(i), scene_target >= 0 ? scene_target : slot); }
                        catch (const std::exception& problem) { error = problem.what(); }
                    }
                }
                ImGui::EndCombo();
            }
            if (scene_active && ImGui::BeginCombo("Target", scene_target < 0 ? "Static pose" : object->motions->name(scene_target).c_str())) {
                if (ImGui::Selectable("Static pose", scene_target < 0)) { scene_target = -1; scene_tick = 0; }
                for (size_t i = 0; i < object->motions->slots(); ++i) {
                    const auto* motion = object->motions->motion(i);
                    if (!motion || motion->model_joints() != int(object->model.joints.size())) continue;
                    const auto name = object->motions->name(i) + " · slot " + std::to_string(i);
                    if (ImGui::Selectable(name.c_str(), int(i) == scene_target)) {
                        scene_target = int(i);
                        scene_tick = 0;
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (scene_active && ImGui::CollapsingHeader("Adjust placement and timing")) {
            ImGui::DragFloat3("Target offset (m)", scene_offset.data(), .01F, -30, 30);
            ImGui::SliderFloat("Target rotation", &scene_yaw, -180, 180, "%.0f deg");
            ImGui::SliderFloat("Target delay", &scene_delay, -120, 120, "%.0f ticks");
        }
    }

    void reaction_overview() {
        const auto& group = reactions[reaction];
        const auto* link = selected_target();
        ImGui::TextDisabled("REACTION COMMAND");
        ImGui::PushFont(nullptr, 25);
        TextUnformatted(group.name.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::SeparatorText("Target");
        const auto list_targets = [&] {
            std::map<std::string, int> variants;
            for (size_t i = 0; i < group.targets.size(); ++i) {
                const auto& target = group.targets[i];
                ImGui::PushID(static_cast<int>(i));
                std::string title = target.name;
                if (target.command == 505 || target.command == 506) title += " · Swing";
                else if (target.command == 661 || target.command == 662) title += " · Finish";
                else if (std::count_if(group.targets.begin(), group.targets.end(), [&](const auto& other) { return other.name == target.name; }) > 1)
                    title += " · Variant " + std::to_string(++variants[target.name]);
                if (ImGui::Selectable(title.c_str(), static_cast<int>(i) == reaction_target)) {
                    try { select_reaction(reaction, static_cast<int>(i)); }
                    catch (const std::exception& problem) { error = problem.what(); }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s · Command %d", utf8(target.model.stem()).c_str(), target.command);
                ImGui::PopID();
            }
        };
        if (group.targets.size() <= 4) {
            list_targets();
        } else {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##reaction-target", link->name.c_str())) {
                list_targets();
                ImGui::EndCombo();
            }
        }
        link = selected_target();
        ImGui::Spacing();
        const auto* pair = pair_for(*link);
        const bool target_clip = object && motion() && std::any_of(object->windows.begin(), object->windows.end(),
             [&](const auto& w) { return int(w.slot) == slot && contains_command(w, link->command); });
        const char* action = pair ? "Play reaction with both characters" :
            link->package_slot >= 0 ? "Play Sora's reaction animation" : target_clip ? "Play linked target animation" : nullptr;
        if (action) {
            if (ImGui::Button(action, {-1, 34})) {
                try { play_selected_reaction(); }
                catch (const std::exception& problem) { error = problem.what(); }
            }
            if (pair) ImGui::TextWrapped("Both actors use linked game clips. The scene uses preview placement.");
            else if (link->package_slot >= 0) {
                ImGui::TextWrapped("Sora's original clip is linked. The target's synchronized animation is unavailable.");
                if (sora_only && ImGui::SmallButton("Show target model")) {
                    scene_active = sora_only = playing = false;
                    reaction_preview = false;
                    view.load(object.get());
                }
            }
        } else {
            ImGui::TextWrapped("%s", link->script_reference
                ? "The target's script references this command. A reaction clip has not been matched."
                : "The animation data references this command. A playable clip has not been matched.");
        }
        if (pair && utf8(link->model.stem()).starts_with("B_EX110") && ImGui::SmallButton("Play Axel's full chain")) {
            try { open_axel_chain(); playing = true; }
            catch (const std::exception& problem) { error = problem.what(); }
            return;
        }
        if (link->model.stem() == "M_EX660" && (link->command == 269 || link->command == 285) &&
            ImGui::SmallButton("Play Rapid Thruster's full chain")) {
            try { open_thruster_chain(); playing = true; }
            catch (const std::exception& problem) { error = problem.what(); }
            return;
        }
        if ((link->command == 505 || link->command == 661 || link->command == 506 || link->command == 662) &&
            utf8(link->model.stem()).starts_with("B_AL100") && ImGui::SmallButton("Play full chain")) {
            try { open_elemental_chain(link->command == 505 || link->command == 661); playing = true; }
            catch (const std::exception& problem) { error = problem.what(); }
            return;
        }
        ImGui::Spacing();
        if (ImGui::SmallButton(details ? "Hide technical details" : "Show technical details")) details = !details;
    }

    void chain_overview() {
        ImGui::TextDisabled("REACTION CHAIN");
        ImGui::PushFont(nullptr, 25);
        TextUnformatted(chain_title);
        ImGui::PopFont();
        const auto actors = "Target · " + target_title();
        ImGui::TextColored({.54F,.78F,1,1}, "%s", actors.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Play full chain", {-1, 34})) {
            chain_tick = 0;
            seek_chain();
            playing = true;
        }
        ImGui::SeparatorText("Steps");
        float start = 0;
        for (size_t i = 0; i < chain.size(); ++i) {
            const auto& step = chain[i];
            const auto title = std::to_string(i + 1) + "  " + (step.title ? step.title : label(step.command, commands, names));
            if (ImGui::Selectable(title.c_str(), static_cast<int>(i) == chain_phase, 0, {0, 27})) {
                chain_tick = start;
                seek_chain();
                playing = false;
            }
            start += step.duration;
        }
        ImGui::Spacing();
        ImGui::TextWrapped("The original clips play in order. The timeline marks the start of each step.");
        if (current.stem() == "M_EX660")
            ImGui::TextWrapped("Rapid Thruster uses its idle flight animation. Its position and Sora's approach are approximated for this preview.");
        if (chain_camera.back().landing)
            ImGui::TextWrapped("Sora blends into his standard landing while Blizzard Lord finishes its reaction.");
        if (ImGui::SmallButton(details ? "Hide technical details" : "Show technical details")) details = !details;
    }

    void inspector() {
        if (!object) {
            ImGui::TextWrapped("Open your game's Image/dt folder. Then pick a reaction, chain, or model from the library.");
            return;
        }
        if (!chain.empty()) {
            chain_overview();
            if (!details) return;
        } else if (reaction >= 0) {
            reaction_overview();
            if (!details || !chain.empty()) return;
        }
        ImGui::Spacing();
        if (!ImGui::BeginTabBar("inspector")) return;
        if (ImGui::BeginTabItem("Commands")) { commands_panel(); ImGui::EndTabItem(); }
        if (!object->reaction_clips.empty()) {
            const int flags = show_scene_tab ? ImGuiTabItemFlags_SetSelected : 0;
            show_scene_tab = false;
            if (ImGui::BeginTabItem("Duo Preview", nullptr, flags)) { scene_panel(); ImGui::EndTabItem(); }
        }
        if (ImGui::BeginTabItem("Animations")) { motions_panel(); ImGui::EndTabItem(); }
        const int bdx_flags = show_bdx_tab ? ImGuiTabItemFlags_SetSelected : 0;
        show_bdx_tab = false;
        if (ImGui::BeginTabItem("BDX Notes", nullptr, bdx_flags)) { scripts_panel(false); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Raw BDX")) { scripts_panel(true); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    void timeline() {
        if (reaction >= 0 && !scene_active && !reaction_preview && !details) {
            ImGui::TextDisabled("Use the preview button to play the linked reaction.");
            return;
        }
        const auto* m = scene_active ? scene_motion() : motion();
        if (!m) { ImGui::TextDisabled("Select a motion to play it."); return; }
        const bool in_chain = !chain.empty();
        float& tick = in_chain ? chain_tick : scene_active ? scene_tick : this->tick;
        const auto* other = scene_active ? target_motion() : nullptr;
        float end = std::max({0.0F, m->length_ticks(), other ? other->length_ticks() + scene_delay : 0.0F});
        if (in_chain) {
            end = 0;
            for (const auto& step : chain) end += step.duration;
        }
        tick = std::clamp(tick, 0.0F, end);
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            if (tick >= end) tick = 0;
            playing = !playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("-1")) { tick = std::max(0.0F, tick - 1); playing = false; }
        ImGui::SameLine();
        if (ImGui::Button("+1")) { tick = std::min(end, tick + 1); playing = false; }
        ImGui::SameLine(); ImGui::Checkbox("Loop", &loop);
        ImGui::SameLine(); ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("##speed", &speed, .02F, .1F, 4, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset camera")) {
            if (scene_active) view.frame_scene();
            else view.reset();
        }
        ImGui::SetNextItemWidth(-1);
        if (in_chain || reaction >= 0) {
            float seconds = tick / 60;
            if (ImGui::SliderFloat("##time", &seconds, 0, end / 60, "%.2f s")) {
                tick = seconds * 60;
                playing = false;
            }
        } else if (ImGui::SliderFloat("##tick", &tick, 0, end, "Tick %.0f")) playing = false;
        const auto at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        auto* draw = ImGui::GetWindowDrawList();
        const auto lane = [&](float start, float stop, bool first) {
            const float x1 = at.x + std::clamp(start / end, 0.0F, 1.0F) * width;
            const float x2 = at.x + std::clamp(stop / end, 0.0F, 1.0F) * width;
            const float y = at.y + (first ? 0 : 6);
            draw->AddRectFilled({x1, y}, {x2, y + (first ? 5 : 4)},
                                first ? IM_COL32(91, 211, 180, 255) : IM_COL32(103, 145, 217, 255));
        };
        draw->AddRectFilled(at, {at.x + width, at.y + 10}, IM_COL32(39, 53, 75, 255));
        if (end > 0) {
            if (in_chain) {
                float begin = 0;
                for (size_t i = 0; i < chain.size(); ++i) {
                    lane(begin, begin + chain[i].duration, true);
                    const float x = at.x + begin / end * width;
                    draw->AddLine({x, at.y}, {x, at.y + 10}, IM_COL32(34, 54, 72, 255), 2.0F);
                    begin += chain[i].duration;
                }
            } else if (scene_active) {
                lane(0, m->length_ticks(), true);
                if (other) lane(scene_delay, scene_delay + other->length_ticks(), false);
            } else {
                for (const auto& w : object->windows)
                    if (int(w.slot) == slot) lane(w.start, w.end, contains_command(w, cmd));
            }
            const float cursor = at.x + tick / end * width;
            draw->AddLine({cursor, at.y}, {cursor, at.y + 10}, IM_COL32(240, 246, 255, 255));
        }
        ImGui::Dummy({width, 10});
        if (in_chain) {
            const auto title = chain[chain_phase].title ? chain[chain_phase].title : label(cmd, commands, names);
            ImGui::TextDisabled("Step %d of %zu · %s · %.2f s", chain_phase + 1, chain.size(), title.c_str(), end / 60);
        } else if (scene_active) {
            const auto actors = sora_only ? std::string("Sora") : "Sora + " + target_title();
            ImGui::TextDisabled("%s · %.0f ticks · %.2f s", actors.c_str(), end, end / 60);
        } else {
            ImGui::TextDisabled("%s · slot %d · %.0f ticks · %.2f s", object->motions->name(slot).c_str(), slot, end, end / 60);
            if (m->model_joints() != int(object->model.joints.size()))
                ImGui::TextColored({1,.66F,.34F,1}, "Skeleton mismatch. Showing the bind pose.");
        }
        if (playing && end > 0) {
            tick += ImGui::GetIO().DeltaTime * 60 * speed;
            if (tick >= end) {
                if (loop) tick = std::fmod(tick, end);
                else { tick = end; playing = false; }
            }
        }
        if (in_chain) seek_chain();
    }

    void frame(SDL_Window* window) {
        refresh();
        const auto size = ImGui::GetIO().DisplaySize;
        const float top_h = 88, track_h = 113, left = std::clamp(size.x * .21F, 225.0F, 320.0F);
        const float right = std::clamp(size.x * .32F, 350.0F, 550.0F), mid = size.x - left - right;
        const float body_h = size.y - top_h;
        const auto panel = [](const char* name, float x, float y, float w, float h, int extra = 0) {
            ImGui::SetNextWindowPos({x, y});
            ImGui::SetNextWindowSize({w, h});
            ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | extra);
        };
        panel("header", 0, 0, size.x, top_h);
        top(window); ImGui::End();
        panel("catalog", 0, top_h, left, body_h);
        catalog(); ImGui::End();
        panel("inspect", left + mid, top_h, right, body_h);
        inspector(); ImGui::End();
        panel("timeline", left, size.y - track_h, mid, track_h);
        timeline(); ImGui::End();
        view_at = {left, top_h};
        view_size = {mid, body_h - track_h};
        panel("preview", view_at.x, view_at.y, view_size.x, view_size.y,
              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (scene_active) {
            if (sora_only) TextUnformatted("Sora · Reaction animation");
            else ImGui::Text("Sora + %s", target_title().c_str());
        } else if (object) ImGui::Text("%s", target_title().c_str());
        else ImGui::TextUnformatted("3D Preview");
        ImGui::TextDisabled("Drag to orbit · scroll to zoom");
        if (scene_active) {
            ImGui::Checkbox("Follow action", &follow_camera);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Track the action with smooth framing. Turn off for a fixed camera.");
        }
        auto area = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##orbit", {std::max(1.0F, area.x), std::max(1.0F, area.y)});
        auto& io = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) view.rotate(io.MouseDelta.x, io.MouseDelta.y);
        if (ImGui::IsItemHovered() && io.MouseWheel) view.zoom(io.MouseWheel);
        ImGui::End();
    }
};

int inspect(const fs::path& root, const std::string& name, const fs::path& log, const fs::path& cache) {
    const GameData game(root, cache);
    const auto cmds = read_commands(game);
    const auto names = read_names(game, "us");
    auto object = read_object(game, fs::path("obj") / path_of((name + ".mdlx").c_str()));
    std::ofstream out(log);
    if (!out) throw std::runtime_error("Could not open the report.");
    out << name << ' ' << object.model.model.meshes.size() << " meshes, " << object.model.joints.size() << " bones, "
        << object.model.textures.size() << " textures, " << object.windows.size() << " ranges, "
        << (object.motions ? object.motions->slots() : 0) << " slots\n";
    for (int id : object.command_ids()) out << "RC " << id << ' ' << label(id, cmds, names) << '\n';
    for (const auto& w : object.windows) {
        out << "ANB " << object.motions->name(w.slot) << ' ' << w.slot << " · " << w.start << '-' << w.end
            << " · type " << w.type;
        for (const auto arg : w.args) out << ' ' << arg;
        out << '\n';
    }
    for (const auto& script : object.scripts) {
        out << "BDX " << script.name << ' ' << script.raw.size() << " bytes " << script.status << '\n';
        for (const auto& [id, branch] : script.dispatch) out << "  REACTION_CALLBACK " << id << " -> " << branch << '\n';
    }
    constexpr kh2::Mat4 unit{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    int poses = 0;
    if (object.motions && object.player) {
        for (size_t slot = 0; slot < object.motions->slots(); ++slot) {
            const auto* m = object.motions->motion(slot);
            if (!m || m->model_joints() != int(object.model.joints.size())) continue;
            object.player->evaluate(*m, 0, unit, nullptr);
            const auto first = object.player->joints();
            object.player->evaluate(*m, m->length_ticks() * .61F, unit, nullptr);
            const auto& next = object.player->joints();
            if (next != first) ++poses;
            for (const auto& matrix : next)
                if (!std::all_of(matrix.begin(), matrix.end(), [](float f){return std::isfinite(f);}))
                    throw std::runtime_error("Non-finite animation matrix");
        }
    }
    out << "Clips with verified pose change " << poses << '\n';
    return poses > 0 && !object.command_ids().empty() ? 0 : 1;
}
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--inspect") {
        try { return inspect(path_of(argv[2]), argv[3], path_of(argv[4]), argc > 5 ? path_of(argv[5]) : fs::path{}); }
        catch (const std::exception& error) { std::ofstream(path_of(argv[4])) << error.what(); return 1; }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    const bool saved_preview = argc == 3 && std::string_view(argv[1]) == "--saved-preview";
    const auto capture = saved_preview ? path_of(argv[2]) : argc > 3 ? path_of(argv[3]) : fs::path{};
    auto* window = SDL_CreateWindow("KH2 RC Viewer", 1480, 880, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                     (capture.empty() ? 0 : SDL_WINDOW_HIDDEN));
    auto context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "KH2 RC Viewer", SDL_GetError(), window);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 1024, 620);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.FontSizeBase = 16;
    style.WindowPadding = {12, 10};
    style.FramePadding = {10, 5};
    style.ItemSpacing = {7, 6};
    style.FrameRounding = 5;
    style.TabRounding = 5;
    style.ScrollbarRounding = 5;
    style.WindowBorderSize = 1;
    style.Colors[ImGuiCol_WindowBg] = {.068F, .086F, .122F, 1};
    style.Colors[ImGuiCol_Border] = {.16F, .20F, .28F, 1};
    style.Colors[ImGuiCol_Header] = {.11F, .25F, .39F, 1};
    style.Colors[ImGuiCol_Button] = {.12F, .29F, .46F, 1};
    wchar_t windows[260]{};
    const auto len = GetWindowsDirectoryW(windows, 260);
    const auto font = len > 0 && len < 260 ? fs::path(windows) / "Fonts" / "segoeui.ttf" : fs::path{};
    if (!fs::is_regular_file(font) || !io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), 16))
        io.Fonts->AddFontDefaultVector();
    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 120");
    App app;
    if (const char* base = SDL_GetBasePath(); base && (capture.empty() || saved_preview))
        app.data_folder = path_of(base) / "ViewerData";
    if (argc > 1 && !saved_preview) {
        try {
            app.open_root(path_of(argv[1]));
            if (argc > 2) {
                app.open_object(fs::path("obj") / path_of((std::string(argv[2]) + ".mdlx").c_str()));
                app.reaction = -1;
            }
            if (app.object && !capture.empty() && argc > 4) {
                const std::string option = argv[4];
                if (option == "--chain") {
                    const auto model = app.current.stem();
                    if (model == "M_EX660") app.open_thruster_chain();
                    else if (model == "B_AL100_1ST" || model == "B_AL100_FIRE") app.open_elemental_chain(true);
                    else if (model == "B_AL100_2ND" || model == "B_AL100_ICE") app.open_elemental_chain(false);
                    else app.open_axel_chain();
                    if (argc > 5) {
                        app.chain_tick = std::max(0.0F, std::stof(argv[5]));
                        app.seek_chain();
                    }
                    if (argc > 6 && std::string_view(argv[6]) == "--fixed-camera") app.follow_camera = false;
                } else if (option == "--reaction" && argc > 5) {
                    const int id = std::stoi(argv[5]);
                    const auto model = app.current;
                    bool found = false;
                    for (size_t i = 0; i < app.reactions.size() && !found; ++i) {
                        const auto& targets = app.reactions[i].targets;
                        for (size_t j = 0; j < targets.size(); ++j) {
                            if (targets[j].command != id || targets[j].model != model) continue;
                            app.select_reaction(static_cast<int>(i), static_cast<int>(j));
                            found = true;
                            break;
                        }
                    }
                    if (!found) throw std::runtime_error("No catalog entry for this reaction and model.");
                    if (argc > 6) {
                        app.play_selected_reaction();
                        app.playing = false;
                        app.scene_tick = std::max(0.0F, std::stof(argv[6]));
                    }
                } else if (option == "--bdx") {
                    app.show_bdx_tab = true;
                    if (argc > 5 && app.motion())
                        app.tick = std::clamp(std::stof(argv[5]), 0.0F, app.motion()->length_ticks());
                } else if (option == "--pair" && argc > 5) {
                    const int id = std::stoi(argv[5]);
                    const auto pairs = known_pairs(utf8(app.current.stem()));
                    const auto pair = std::find_if(pairs.begin(), pairs.end(), [id](const auto& p) { return p.command == id; });
                    if (pair == pairs.end()) throw std::runtime_error("No documented pair for this command.");
                    app.play_pair(*pair);
                    if (argc > 6) app.scene_tick = std::clamp(std::stof(argv[6]), 0.0F, app.scene_motion()->length_ticks());
                } else if (option == "--tracks" && argc > 6) {
                    const int package_slot = std::stoi(argv[5]), target_slot = std::stoi(argv[6]);
                    const auto& clips = app.object->reaction_clips;
                    const auto clip = std::find_if(clips.begin(), clips.end(), [package_slot](const auto& entry) {
                        return entry.package_slot == package_slot;
                    });
                    if (clip == clips.end() || !app.object->motions->motion(target_slot))
                        throw std::runtime_error("Invalid reaction package or target slot.");
                    app.start_scene(static_cast<int>(clip - clips.begin()), target_slot);
                    app.show_scene_tab = true;
                    if (argc > 7) app.scene_tick = std::clamp(std::stof(argv[7]), 0.0F, app.scene_motion()->length_ticks());
                } else {
                    const int id = std::stoi(option);
                    if (!app.object->command_ids().contains(id)) throw std::runtime_error("This object does not reference that command.");
                    app.set_command(id);
                    if (argc > 5 && app.motion())
                        app.tick = std::clamp(std::stof(argv[5]), 0.0F, app.motion()->length_ticks());
                }
            }
        } catch (const std::exception& error) { app.error = error.what(); }
    } else {
        try { app.reopen_game(); }
        catch (const std::exception& error) { app.error = error.what(); }
    }
    bool running = true;
    int frames = 0;
    int result = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame(window);
        ImGui::Render();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(.045F, .06F, .09F, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const auto scale = io.DisplayFramebufferScale;
        const auto scene = app.preview_pose();
        app.view.draw(int(app.view_at.x * scale.x),
                      h - int((app.view_at.y + app.view_size.y) * scale.y),
                      int(app.view_size.x * scale.x), int(app.view_size.y * scale.y), app.slot, app.tick,
                      app.scene_active ? &scene : nullptr);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!capture.empty() && ++frames == 3) {
            auto* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
            if (surface) {
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);
                SDL_FlipSurface(surface, SDL_FLIP_VERTICAL);
                result = SDL_SaveBMP(surface, utf8(capture).c_str()) && app.object && app.error.empty() ? 0 : 1;
                SDL_DestroySurface(surface);
            } else result = 1;
            running = false;
        }
        SDL_GL_SwapWindow(window);
    }
    app.view.load(nullptr);
    app.view.load_sora(nullptr);
    app.view.load_weapon(nullptr);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
