#pragma once

#include "reader.h"

namespace rc {
struct ReactionTarget {
    int command;
    fs::path model;
    std::string name;
    int package_slot, offer_slot, offer_tick;
    bool script_reference;
};

struct ReactionGroup {
    std::string name;
    std::vector<ReactionTarget> targets;
};

const ReactionPair* pair_for(const ReactionTarget& target);
bool has_preview(const ReactionTarget& target);
std::vector<ReactionGroup> load_reactions(const GameData& game, const std::map<int, Command>& commands,
                                          const std::map<int, std::string>& names);
}
