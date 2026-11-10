#pragma once

#include "game_data.h"
#include "khdays/assets/kh2_effect.h"
#include "khdays/assets/kh2_motion.h"

#include <functional>
#include <map>
#include <tuple>

namespace rc {
namespace kh2 = khdays::assets::kh2;

class PaxPreview {
public:
    // The anchor receives an effect's bone ID and a tick on the actor's track.
    using Anchor = std::function<kh2::Mat4(int, float)>;

    void load_game(const GameData* game);
    void restart();
    void begin(float near_clip, float far_clip, float aspect, float slope);
    void draw(const kh2::Motion* motion, const std::string& bank, int lane, float tick,
              const Anchor& anchor, int opening_effect = -1);
    void end();
    int visible() const { return visible_; }

private:
    struct Element {
        std::shared_ptr<const kh2::EffectBlock> block;
        std::size_t emitter;
        int bone;
        kh2::Mat4 local;
    };
    struct Active {
        std::unique_ptr<kh2::EffectInstance> instance;
        int tick = -1;
        bool failed = false, visited = false;
        kh2::EffectCamera camera;
        std::vector<kh2::EffectDraw> draws;
    };

    const Element* element(const std::string& model, int id);
    void draw_cue(const kh2::Motion* motion, const std::string& bank, int lane, int cue,
                  int id, int start, float tick, const Anchor& anchor);
    unsigned texture(const kh2::EffectBlock& block, std::uint64_t tex0);
    void submit(const kh2::EffectBlock& block, const std::vector<kh2::EffectDraw>& draws);

    const GameData* game_ = nullptr;
    std::map<std::string, std::map<int, Element>> banks_;
    std::map<std::tuple<const kh2::Motion*, int, int>, Active> active_;
    std::map<std::pair<const kh2::EffectBlock*, std::uint64_t>, unsigned> textures_;
    kh2::EffectBlock common_;
    kh2::EffectCamera camera_;
    int visible_ = 0;
};
}
