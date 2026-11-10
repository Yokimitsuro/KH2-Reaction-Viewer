#include "catalog.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rc {
const ReactionPair* pair_for(const ReactionTarget& target) {
    for (const auto& pair : known_pairs(utf8(target.model.stem())))
        if (pair.command == target.command) return &pair;
    return nullptr;
}

std::vector<ReactionGroup> load_reactions(const GameData& game, const std::map<int, Command>& commands,
                                          const std::map<int, std::string>& names) {
    const auto* base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(SDL_GetError());
    std::ifstream file(path_of(base) / "ReactionCatalog.tsv");
    if (!file) throw std::runtime_error("ReactionCatalog.tsv is missing. Extract the full viewer ZIP.");
    std::map<std::string, std::vector<ReactionTarget>> grouped;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream entry(line);
        int id = -1, slot = -1;
        std::string model, type, display;
        if (!(entry >> id >> model >> type >> slot) || !std::getline(entry >> std::ws, display)) continue;
        const auto command = commands.find(id);
        if (command == commands.end()) continue;
        const auto path = fs::path("obj") / (model + ".mdlx");
        if (!game.exists(path)) continue;
        const auto text = names.find(command->second.text & 0x7fff);
        std::string name = text == names.end() ? std::string{} : text->second;
        if (name.empty() || name == "-") name = "Command " + std::to_string(id);
        grouped[name].push_back({id, path, display, slot, type.find('S') != std::string::npos});
    }
    std::vector<ReactionGroup> groups;
    for (auto& [name, targets] : grouped) {
        std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) {
            const auto rank = [](const auto& target) { return pair_for(target) ? 0 : target.package_slot >= 0 ? 1 : 2; };
            if (rank(a) != rank(b)) return rank(a) < rank(b);
            if (a.name != b.name) return a.name < b.name;
            if (a.model != b.model) return a.model < b.model;
            return a.command < b.command;
        });
        std::set<std::pair<std::string, int>> seen;
        std::erase_if(targets, [&](const auto& target) { return !seen.emplace(target.name, target.command).second; });
        groups.push_back({name, std::move(targets)});
    }
    const auto rank = [](const ReactionGroup& g) {
        if (g.name == "Clear Light") return 0;
        return std::any_of(g.targets.begin(), g.targets.end(), [](const auto& t) { return pair_for(t); }) ? 1 : 2;
    };
    std::stable_sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    return groups;
}
}
