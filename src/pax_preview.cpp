#include "pax_preview.h"
#include "khdays/assets/kh2_bar.h"

#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace rc {
namespace {
constexpr kh2::Mat4 identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
constexpr kh2::Mat4 flip_y{1,0,0,0, 0,-1,0,0, 0,0,1,0, 0,0,0,1};
constexpr kh2::Mat4 camera_axes{1,0,0,0, 0,-1,0,0, 0,0,-1,0, 0,0,0,1};

kh2::Mat4 multiply(const kh2::Mat4& a, const kh2::Mat4& b) {
    kh2::Mat4 out{};
    for (size_t col = 0; col < 4; ++col)
        for (size_t row = 0; row < 4; ++row)
            for (size_t k = 0; k < 4; ++k) out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    return out;
}

std::uint16_t u16(const std::uint8_t* p) {
    return p[0] | (std::uint16_t(p[1]) << 8);
}

std::uint32_t u32(const std::uint8_t* p) {
    return u16(p) | (std::uint32_t(u16(p + 2)) << 16);
}

float real(const std::uint8_t* p) { return std::bit_cast<float>(u32(p)); }

kh2::Mat4 local_matrix(const std::uint8_t* p) {
    // The PAX caster copies translation, rotation and scale from +1c, +28 and +34.
    const float x = real(p + 0x28), y = real(p + 0x2c), z = real(p + 0x30);
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
    const float cz = std::cos(z), sz = std::sin(z);
    kh2::Mat4 matrix{cz*cy, sz*cy, -sy, 0,
                     cz*sy*sx-sz*cx, sz*sy*sx+cz*cx, cy*sx, 0,
                     cz*sy*cx+sz*sx, sz*sy*cx-cz*sx, cy*cx, 0,
                     real(p+0x1c), real(p+0x20), real(p+0x24), 1};
    for (size_t col = 0; col < 3; ++col)
        for (size_t row = 0; row < 3; ++row)
            matrix[col * 4 + row] *= real(p + 0x34 + col * 4);
    return matrix;
}

Bytes read_bank(const GameData& game, const std::string& model) {
    for (auto stem : {model, model.ends_with("_LV99") ? model.substr(0, model.size() - 5) : std::string{}}) {
        if (stem.empty()) continue;
        for (const char* lang : {"us", "fm", "jp"}) {
            const auto file = fs::path("obj") / (stem + ".a." + lang);
            if (!game.exists(file)) continue;
            const auto bytes = game.read(file);
            for (const auto& entry : kh2::bar_entries(bytes.data(), bytes.size()))
                if (entry.type == 18 && entry.size >= 16)
                    return {bytes.begin() + entry.offset, bytes.begin() + entry.offset + entry.size};
        }
    }
    return {};
}

void blend(std::uint8_t alpha) {
    // Convert the GS (A - B) * C + D equation into OpenGL's common blend modes.
    using BlendEquation = void (APIENTRY*)(GLenum);
    static const auto equation = reinterpret_cast<BlendEquation>(SDL_GL_GetProcAddress("glBlendEquation"));
    constexpr GLenum add = 0x8006, reverse_subtract = 0x800b;
    if (equation) equation(add);
    const auto a = alpha & 3, b = (alpha >> 2) & 3, c = (alpha >> 4) & 3, d = (alpha >> 6) & 3;
    const GLenum factor = c == 0 ? GL_SRC_ALPHA : c == 1 ? GL_DST_ALPHA : GL_ONE;
    const GLenum inverse = c == 0 ? GL_ONE_MINUS_SRC_ALPHA : c == 1 ? GL_ONE_MINUS_DST_ALPHA : GL_ZERO;
    if (a == 0 && b == 2 && d == 1) glBlendFunc(factor, GL_ONE);
    else if (a == 2 && b == 0 && d == 1 && equation) {
        equation(reverse_subtract);
        glBlendFunc(factor, GL_ONE);
    } else if (a == 2 && b == 1 && d == 1) glBlendFunc(GL_ZERO, inverse);
    else if (a == 0 && b == 2 && d == 2) glBlendFunc(factor, GL_ZERO);
    else glBlendFunc(factor, inverse);
}
}

void PaxPreview::load_game(const GameData* game) {
    restart();
    for (const auto& [key, id] : textures_) glDeleteTextures(1, &id);
    textures_.clear();
    banks_.clear();
    common_ = {};
    game_ = game;
    if (!game_ || !game_->exists("effect/texcommon.dpd")) return;
    try {
        const auto bytes = game_->read("effect/texcommon.dpd");
        common_ = kh2::read_effect_block(bytes.data(), bytes.size(), 0);
    } catch (const std::runtime_error&) { /* Standalone banks can still play. */ }
}

void PaxPreview::restart() {
    active_.clear();
    visible_ = 0;
}

const PaxPreview::Element* PaxPreview::element(const std::string& model, int id) {
    if (!game_) return nullptr;
    if (banks_.size() >= 12 && !banks_.contains(model)) {
        restart();
        for (const auto& [key, texture] : textures_) glDeleteTextures(1, &texture);
        textures_.clear();
        banks_.clear();
    }
    auto [it, fresh] = banks_.try_emplace(model);
    auto& bank = it->second;
    if (fresh) {
        try {
            const auto bytes = read_bank(*game_, model);
            if (bytes.size() < 16 || std::memcmp(bytes.data(), "PAX_", 4) != 0) return nullptr;
            const size_t dir = u32(bytes.data() + 12), count = u32(bytes.data() + 8);
            if (dir < 16 || dir > bytes.size() - 16 || count > (dir - 16) / 80) return nullptr;
            const size_t links = u32(bytes.data() + dir + 12);
            if (links > (bytes.size() - dir - 16) / 32) return nullptr;
            const size_t blocks_at = dir + 16 + links * 32;
            if (blocks_at > bytes.size() - 4) return nullptr;
            const size_t block_count = u32(bytes.data() + blocks_at);
            if (block_count > (bytes.size() - blocks_at - 4) / 4) return nullptr;
            auto decoded = kh2::read_effect(bytes.data(), bytes.size());
            std::map<size_t, std::shared_ptr<const kh2::EffectBlock>> blocks;
            for (size_t i = 0; i < decoded.size() && i < block_count; ++i)
                blocks.emplace(u32(bytes.data() + blocks_at + 4 + 4 * i),
                               std::make_shared<const kh2::EffectBlock>(std::move(decoded[i])));
            std::map<std::uint32_t, std::pair<std::shared_ptr<const kh2::EffectBlock>, size_t>> emitters;
            for (size_t i = 0; i < links; ++i) {
                const auto* row = bytes.data() + dir + 16 + i * 32;
                const auto block = blocks.find(u32(row));
                const size_t emitter = u32(row + 4);
                if (block != blocks.end() && emitter < block->second->emitters.size())
                    emitters.try_emplace(u32(row + 8), block->second, emitter);
            }
            for (size_t i = 0; i < count; ++i) {
                const auto* row = bytes.data() + 16 + i * 80;
                const auto emitter = emitters.find(u16(row));
                if (emitter == emitters.end()) continue;
                const auto local = local_matrix(row);
                if (!std::ranges::all_of(local, [](float x) { return std::isfinite(x); })) continue;
                bank.try_emplace(u16(row + 2), Element{emitter->second.first, emitter->second.second,
                                                     u16(row + 6), local});
            }
        } catch (const std::runtime_error&) { /* A bad bank must not stop animation playback. */ }
    }
    const auto found = bank.find(id);
    return found == bank.end() ? nullptr : &found->second;
}

void PaxPreview::begin(float near_clip, float far_clip, float aspect, float slope) {
    visible_ = 0;
    for (auto& [key, fx] : active_) fx.visited = false;
    kh2::Mat4 view{};
    glGetFloatv(GL_MODELVIEW_MATRIX, view.data());
    for (size_t i = 12; i < 15; ++i) view[i] *= 100;
    camera_.view = multiply(multiply(camera_axes, view), flip_y);
    const float n = near_clip * 100, f = far_clip * 100;
    camera_.projection = {1/(slope*aspect),0,0,0, 0,-1/slope,0,0, 0,0,f/(f-n),1, 0,0,-n*f/(f-n),0};
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glEnable(GL_BLEND);
    glDepthFunc(GL_LEQUAL);
}

void PaxPreview::draw(const kh2::Motion* motion, const std::string& bank, int lane, float tick,
                      const Anchor& anchor, int opening_effect) {
    if (!motion || bank.empty() || tick < 0) return;
    if (opening_effect >= 0) draw_cue(motion, bank, lane, -1, opening_effect, 0, tick, anchor);
    const auto* attrs = motion->attributes();
    if (!attrs) return;
    for (size_t i = 0; i < attrs->triggers.size(); ++i) {
        const auto& cue = attrs->triggers[i];
        if (cue.id == 1 && cue.params.size() == 1 && cue.frame <= motion->length_ticks())
            draw_cue(motion, bank, lane, static_cast<int>(i), cue.params.front(), cue.frame, tick, anchor);
    }
}

void PaxPreview::draw_cue(const kh2::Motion* motion, const std::string& bank, int lane, int cue,
                          int id, int start, float tick, const Anchor& anchor) {
    if (tick < start || tick - start > 3600) return;
    const auto* info = element(bank, id);
    if (!info) return;
    const auto desired = static_cast<int>(tick - start);
    auto& fx = active_[{motion, lane, cue}];
    fx.visited = true;
    if (fx.failed) return;
    if (fx.instance && fx.instance->done() && fx.draws.empty() && desired >= fx.tick) return;
    auto camera = camera_;
    const auto origin = [&](float time) {
        auto m = anchor(info->bone, time);
        for (size_t i = 12; i < 15; ++i) m[i] *= 100;
        return multiply(multiply(multiply(flip_y, m), flip_y), info->local);
    };
    camera.world = origin(static_cast<float>(start + desired));
    const bool changed = fx.camera.world != camera.world || fx.camera.view != camera.view ||
                         fx.camera.projection != camera.projection;
    try {
        if (!fx.instance || desired < fx.tick || (desired == fx.tick && changed)) {
            fx.instance = std::make_unique<kh2::EffectInstance>(info->block, info->emitter, 0x4000 + id);
            fx.tick = -1;
            fx.draws.clear();
        }
        while (fx.tick < desired && !fx.instance->done()) {
            ++fx.tick;
            const bool last = fx.tick == desired;
            if (last) fx.draws.clear();
            camera.world = origin(static_cast<float>(start + fx.tick));
            fx.instance->tick(camera, last ? &fx.draws : nullptr);
        }
        if (fx.tick != desired) {
            fx.draws.clear();
            fx.tick = desired;
        }
        fx.camera = camera;
        if (std::ranges::any_of(fx.draws, [](const auto& draw) {
                return !draw.screen && !draw.depth_only && !draw.triangles.empty();
            })) ++visible_;
        if (!fx.draws.empty()) submit(*info->block, fx.draws);
    } catch (const std::runtime_error&) {
        fx.failed = true;
        fx.draws.clear();
    }
}

unsigned PaxPreview::texture(const kh2::EffectBlock& block, std::uint64_t tex0) {
    if (!tex0 || ((tex0 >> 26) & 15) > 11 || ((tex0 >> 30) & 15) > 11) return 0;
    const auto key = std::pair{&block, tex0};
    if (const auto found = textures_.find(key); found != textures_.end()) return found->second;
    std::vector<const kh2::EffectPicture*> pictures;
    for (const auto& picture : common_.pictures) pictures.push_back(&picture);
    for (const auto& picture : block.pictures) pictures.push_back(&picture);
    const auto page = kh2::effect_page(pictures, tex0);
    GLuint id = 0;
    if (!page.rgba.empty()) {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, page.width, page.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, page.rgba.data());
    }
    textures_.emplace(key, id);
    return id;
}

void PaxPreview::submit(const kh2::EffectBlock& block, const std::vector<kh2::EffectDraw>& draws) {
    for (const auto& draw : draws) {
        if (draw.screen || draw.triangles.empty()) continue;
        if (draw.depth_test) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(draw.depth_write || draw.depth_only ? GL_TRUE : GL_FALSE);
        const GLboolean colour = draw.depth_only ? GL_FALSE : GL_TRUE;
        glColorMask(colour, colour, colour, colour);
        blend(draw.alpha);
        const auto id = texture(block, draw.tex0);
        if (id) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
        } else glDisable(GL_TEXTURE_2D);
        glBegin(GL_TRIANGLES);
        for (const auto& v : draw.triangles) {
            glColor4f(v.r, v.g, v.b, v.a);
            glTexCoord2f(v.u, v.v);
            glVertex4f(v.x, v.y, 2 * v.z - v.w, v.w);
        }
        glEnd();
    }
}

void PaxPreview::end() {
    std::erase_if(active_, [](const auto& entry) { return !entry.second.visited; });
    blend(0x44);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}
}
