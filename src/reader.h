#pragma once

#include "game_data.h"
#include "khdays/assets/kh2_mdlx.h"
#include "khdays/assets/kh2_motion.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <span>

namespace rc {
namespace kh2 = khdays::assets::kh2;

struct Command {
    int id, execute, argument, text;
    unsigned flags;
    float range;
};

struct Window {
    size_t slot;
    int start, end, type;
    std::vector<std::uint16_t> args;
};

struct Script {
    std::string name, raw, annotated, status;
    std::map<int, std::string> dispatch;
};

enum class PlayerRig { Sora, Roxas, LionSora };
const char* player_name(PlayerRig rig);
const char* player_model(PlayerRig rig);

enum class AllyRig { None, JackSparrow, Riku };
const char* ally_name(AllyRig rig);
const char* ally_model(AllyRig rig);
const char* ally_weapon_model(AllyRig rig);

struct ReactionClip {
    int package_slot;
    std::string package, track;
    std::shared_ptr<const kh2::Motion> motion, weapon;
    PlayerRig rig = PlayerRig::Sora;
    AllyRig ally_rig = AllyRig::None;
    std::shared_ptr<const kh2::Motion> ally, ally_weapon;
};

struct ReactionPair {
    int command, package_slot, target_slot;
    const char* name;
    const char* target_action;
    bool follow_target_motion = false;
    int target_continuation = -1;
    const char* prop_model = nullptr;
    int prop_slot = -1;
    int opening_effect = -1;
};

struct Object {
    khdays::assets::ImportedModel model;
    std::vector<kh2::MdlxMeshInfo> mesh_info;
    std::string model_status;
    std::unique_ptr<kh2::MotionSet> motions;
    std::unique_ptr<kh2::MotionPlayer> player;
    std::vector<Window> windows;
    std::vector<Script> scripts;
    std::vector<ReactionClip> reaction_clips;
    std::set<int> command_ids() const;
};

std::string utf8(const fs::path& path);
fs::path path_of(const char* utf8);
std::map<int, Command> read_commands(const GameData& game);
std::map<int, std::string> read_names(const GameData& game, const std::string& lang);
Object read_object(const GameData& game, const fs::path& path, bool decode_scripts = true);
const char* type_name(int type);
const char* param_name(int type, size_t index);
bool has_command_id(const Window& window);
bool contains_command(const Window& window, int id);
std::string branch_excerpt(const Script& script, const std::string& branch);
std::span<const ReactionPair> known_pairs(const std::string& object);
const char* object_name(const std::string& id);
}
