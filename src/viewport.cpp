#include "viewport.h"
#include "attached_frame.h"
#include "secondary/vecmath.h"

#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace rc {
namespace {
constexpr kh2::Mat4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr size_t sora_weapon_joint = 178;
using Vec = std::array<float, 3>;

Vec skin(const khdays::assets::NeutralVertex& v, const std::vector<kh2::Mat4>& palette) {
    if (palette.empty()) return v.position;
    Vec point{};
    float total = 0;
    for (size_t j = 0; j < 4; ++j) {
        if (v.weights[j] <= 0 || v.joints[j] >= palette.size()) continue;
        total += v.weights[j];
        const auto& m = palette[v.joints[j]];
        for (size_t k = 0; k < 3; ++k)
            point[k] += v.weights[j] * (m[k] * v.position[0] + m[k + 4] * v.position[1] +
                                       m[k + 8] * v.position[2] + m[k + 12]);
    }
    return total > 0 ? point : v.position;
}

kh2::Mat4 multiply(const kh2::Mat4& a, const kh2::Mat4& b) {
    kh2::Mat4 out{};
    for (size_t col = 0; col < 4; ++col)
        for (size_t row = 0; row < 4; ++row)
            for (size_t k = 0; k < 4; ++k) out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    return out;
}

kh2::Mat4 placement(const Vec& offset, float yaw) {
    const float radians = yaw * .01745329252F;
    const float c = std::cos(radians), s = std::sin(radians);
    return {c,0,-s,0, 0,1,0,0, s,0,c,0, offset[0],offset[1],offset[2],1};
}

Vec transform(const kh2::Mat4& m, const Vec& point) {
    Vec out{};
    for (size_t i = 0; i < 3; ++i)
        out[i] = m[i] * point[0] + m[i + 4] * point[1] + m[i + 8] * point[2] + m[i + 12];
    return out;
}
}

void Viewport::Actor::load(const Object* next) {
    for (const auto& [name, t] : textures) glDeleteTextures(1, &t.id);
    textures.clear();
    object = next;
    if (!object) return;
    for (const auto& [name, picture] : object->model.textures) {
        if (picture.width <= 0 || picture.height <= 0 || picture.rgba.empty()) continue;
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, picture.width, picture.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, picture.rgba.data());
        textures.emplace(name, Texture{id, picture.width, picture.height});
    }
}

void Viewport::load_sora(const Object* object) { sora_.load(object); fitted_segments_.clear(); }
void Viewport::load_weapon(const Object* object) { weapon_.load(object); fitted_segments_.clear(); }

void Viewport::load(const Object* object) {
    fitted_segments_.clear();
    if (target_.object != object) target_.load(object);
    if (!object) return;
    const auto& model = object->model.model;
    Vec low{}, high{};
    low.fill(std::numeric_limits<float>::max());
    high.fill(-low[0]);
    for (const auto& mesh : model.meshes) {
        for (const auto& vertex : mesh.vertices) {
            const auto p = skin(vertex, model.palette);
            for (size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], p[k]);
                high[k] = std::max(high[k], p[k]);
            }
        }
    }
    for (size_t k = 0; k < 3; ++k) center_[k] = (low[k] + high[k]) * .5F;
    radius_ = std::max(.25F, std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]) * .5F);
    reset();
}

void Viewport::rotate(float dx, float dy) {
    yaw_ = std::fmod(yaw_ + dx * .42F, 360.0F);
    pitch_ = std::clamp(pitch_ + dy * .42F, -85.0F, 85.0F);
}

void Viewport::zoom(float delta) {
    distance_ = std::clamp(distance_ * std::pow(.88F, delta), radius_ * .35F, radius_ * 20);
    scene_zoom_ = std::clamp(scene_zoom_ * std::pow(.88F, delta), .35F, 4.0F);
}

void Viewport::reset() { yaw_ = 20; pitch_ = 9; distance_ = radius_ * 2.8F; }

void Viewport::frame_scene() {
    yaw_ = 65;
    pitch_ = 13;
    scene_zoom_ = 1;
}

Viewport::StagePose Viewport::clip_pose(const SceneSegment& segment, float tick, const ScenePose& scene) const {
    // Scripted pairs already carry their relative movement in the skeletal tracks.
    StagePose stage{sora_.pose(segment.sora_motion, tick, segment.apply_root), {}, {}, segment.show_target, false};
    const float landing_tick = segment.landing ? tick - segment.sora_motion->length_ticks() : 0;
    if (landing_tick > 0 && !stage.sora.joints.empty()) {
        constexpr float descent_ticks = 12;
        const auto touchdown = sora_.pose(segment.landing, 0, false);
        auto landing = landing_tick <= descent_ticks ? touchdown
            : sora_.pose(segment.landing, landing_tick - descent_ticks, false);
        // Plant the landing at the end of the reaction, keeping the authored ground height.
        for (const size_t axis : {12, 14})
            landing.placement[axis] = stage.sora.joints.front()[axis] - touchdown.joints.front()[axis];
        const float remaining = 1 - std::min(1.0F, landing_tick / descent_ticks);
        sora_.blend(stage.sora, landing, 1 - remaining * remaining);
        stage.sora = std::move(landing);
    }
    if (stage.show_target) {
        const float target_tick = segment.target_start + tick * segment.target_rate - scene.target_delay;
        stage.target = target_.pose(segment.target_motion, target_tick, segment.apply_root);
        auto target_offset = scene.target_offset;
        target_offset[2] += segment.target_depth;
        stage.target.placement = multiply(placement(target_offset, scene.target_yaw), stage.target.placement);
    }
    auto offset = segment.offset;
    const float settle = std::clamp(tick / 48, 0.0F, 1.0F);
    offset[1] *= 1 - settle * settle * (3 - 2 * settle);
    const auto stage_origin = placement(offset, segment.yaw);
    stage.sora.placement = multiply(stage_origin, stage.sora.placement);
    stage.target.placement = multiply(stage_origin, stage.target.placement);
    if (weapon_.object && weapon_.object->player && segment.weapon_motion &&
        segment.weapon_motion->model_joints() == int(weapon_.object->model.joints.size())) {
        const auto& joints = stage.sora.joints;
        if (sora_weapon_joint < joints.size()) {
            stage.weapon = weapon_.pose(segment.weapon_motion, tick, false);
            stage.show_weapon = true;
        }
    }
    return stage;
}

Viewport::StagePose Viewport::stage_pose(const SceneSegment& segment, float tick, const ScenePose& scene,
                                         const SceneSegment* previous) const {
    auto stage = clip_pose(segment, tick, scene);
    if (previous && tick < segment.blend_ticks) {
        auto from = clip_pose(*previous, previous->duration, scene);
        const float t = tick / segment.blend_ticks;
        const float weight = t * t * (3 - 2 * t);
        if (!segment.blend_target_only) {
            sora_.blend(from.sora, stage.sora, weight);
            if (stage.show_weapon && from.show_weapon) weapon_.blend(from.weapon, stage.weapon, weight);
        }
        if (stage.show_target && from.show_target) {
            // Let the incoming clip move the boss even while its pose blends across the cut.
            if (segment.blend_target_only && tick > 0 && !stage.target.joints.empty()) {
                const auto start = clip_pose(segment, 0, scene).target;
                const auto start_root = multiply(start.placement, start.joints.front());
                const auto current_root = multiply(stage.target.placement, stage.target.joints.front());
                const auto advance = multiply(current_root, khdays::secondary::inverse_affine(start_root));
                from.target.placement = multiply(advance, from.target.placement);
            }
            target_.blend(from.target, stage.target, weight);
        }
    }
    if (stage.show_weapon)
        stage.weapon.placement = multiply(stage.sora.placement, stage.sora.joints[sora_weapon_joint]);
    return stage;
}

void Viewport::align_axel_steps(std::span<SceneSegment> segments, const ScenePose& scene) const {
    // Axel's captured reactions start in separate scene coordinates. Register the
    // shared stage to the previous take, preserving the spacing inside each pair.
    const auto anchors = [&](const SceneSegment& segment, float tick) {
        const auto stage = clip_pose(segment, tick, scene);
        const auto anchor = [](const ActorPose& actor) {
            const auto& root = actor.joints.empty() ? identity : actor.joints.front();
            return transform(actor.placement, {root[12], root[13], root[14]});
        };
        return std::array{anchor(stage.sora), anchor(stage.target)};
    };
    const auto approach = anchors(segments[1], 0);
    const auto opener_end = anchors(segments[0], segments[0].duration);
    segments[0].target_depth = approach[1][2] - opener_end[1][2];
    const auto heading = [](const std::array<Vec, 2>& actors) {
        return std::atan2(actors[0][0] - actors[1][0], actors[0][2] - actors[1][2]);
    };
    for (size_t i = 1; i < segments.size(); ++i) {
        auto& next = segments[i];
        const auto& previous = segments[i - 1];
        const auto from = anchors(previous, previous.duration);
        next.yaw = (heading(from) - heading(anchors(next, 0))) * 57.2957795F;
        const auto to = anchors(next, 0);
        for (const size_t axis : {0, 2})
            next.offset[axis] = ((from[0][axis] + from[1][axis]) - (to[0][axis] + to[1][axis])) * .5F;
        // Clear Light opens in the air. Match the outgoing height and return
        // to the clip's ground plane before Axel hits the ground.
        if (i == 2)
            next.offset[1] = ((from[0][1] + from[1][1]) - (to[0][1] + to[1][1])) * .5F;
        next.blend_ticks = 16;
    }
}

void Viewport::fit_scene(std::span<const SceneSegment> segments, const ScenePose& scene) {
    if (std::ranges::equal(segments, fitted_segments_) && scene.target_offset == fitted_offset_ &&
        scene.target_yaw == fitted_yaw_ && scene.target_delay == fitted_delay_) return;
    fitted_segments_.assign(segments.begin(), segments.end());
    fitted_offset_ = scene.target_offset;
    fitted_yaw_ = scene.target_yaw;
    fitted_delay_ = scene.target_delay;
    camera_path_.clear();

    struct Sample { float tick; Vec low, high, center; };
    std::vector<Sample> samples;
    Vec minimum{}, maximum{};
    minimum.fill(std::numeric_limits<float>::max());
    maximum.fill(-minimum[0]);
    float elapsed = 0;
    const SceneSegment* previous = nullptr;
    for (const auto& segment : segments) {
        const int intervals = static_cast<int>(std::clamp(std::ceil(segment.duration / 4), 1.0F, 180.0F));
        for (int i = 0; i <= intervals; ++i) {
            const float tick = segment.duration * i / intervals;
            const auto stage = stage_pose(segment, tick, scene, previous);
            Sample sample{elapsed + tick, {}, {}, {}};
            sample.low.fill(std::numeric_limits<float>::max());
            sample.high.fill(-sample.low[0]);
            sora_.bounds(stage.sora, sample.low, sample.high);
            if (stage.show_target) target_.bounds(stage.target, sample.low, sample.high);
            if (stage.show_weapon) weapon_.bounds(stage.weapon, sample.low, sample.high);
            if (sample.low[0] > sample.high[0] || sample.low[1] > sample.high[1] || sample.low[2] > sample.high[2]) continue;
            for (size_t axis = 0; axis < 3; ++axis) {
                sample.center[axis] = (sample.low[axis] + sample.high[axis]) * .5F;
                minimum[axis] = std::min(minimum[axis], sample.low[axis]);
                maximum[axis] = std::max(maximum[axis], sample.high[axis]);
            }
            samples.push_back(sample);
        }
        elapsed += segment.duration;
        previous = &segment;
    }
    fixed_radius_ = .8F;
    if (samples.empty()) {
        fixed_center_ = {0, .8F, 0};
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) fixed_center_[axis] = (minimum[axis] + maximum[axis]) * .5F;
    fixed_radius_ = std::max(.8F, std::hypot(maximum[0]-minimum[0], maximum[1]-minimum[1], maximum[2]-minimum[2]) * .5F);

    // Smooth across the whole timeline, including step boundaries.
    const float smoothing = segments.size() > 1 ? 36.0F : 20.0F;
    for (const auto& sample : samples) {
        Vec center{}, farthest{};
        float total = 0;
        for (const auto& neighbor : samples) {
            const float u = (sample.tick - neighbor.tick) / smoothing;
            const float kernel = std::max(0.0F, 1 - u * u);
            const float weight = kernel * kernel;
            if (weight == 0) continue;
            for (size_t axis = 0; axis < 3; ++axis) center[axis] += neighbor.center[axis] * weight;
            total += weight;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            center[axis] /= total;
            farthest[axis] = std::max(std::abs(sample.low[axis] - center[axis]), std::abs(sample.high[axis] - center[axis]));
        }
        const float log_radius = std::log(std::max(.8F, std::hypot(farthest[0], farthest[1], farthest[2])));
        if (!camera_path_.empty() && camera_path_.back().tick == sample.tick)
            camera_path_.back().log_radius = std::max(camera_path_.back().log_radius, log_radius);
        else camera_path_.push_back({sample.tick, center, log_radius});
    }
    // A look-ahead pass eases out before wide action. A second pass holds
    // the wider framing long enough to prevent rapid zooming back in.
    const float widen_per_tick = segments.size() > 1 ? .010F : .018F;
    const float tighten_per_tick = segments.size() > 1 ? .007F : .012F;
    for (size_t i = camera_path_.size() - 1; i > 0; --i) {
        const auto& next = camera_path_[i];
        auto& previous = camera_path_[i - 1];
        previous.log_radius = std::max(previous.log_radius, next.log_radius - widen_per_tick * (next.tick - previous.tick));
    }
    for (size_t i = 1; i < camera_path_.size(); ++i) {
        const auto& previous = camera_path_[i - 1];
        auto& next = camera_path_[i];
        next.log_radius = std::max(next.log_radius, previous.log_radius - tighten_per_tick * (next.tick - previous.tick));
    }
}

Viewport::CameraKey Viewport::follow_view(float tick) const {
    if (camera_path_.empty()) return {tick, fixed_center_, std::log(fixed_radius_)};
    const auto next = std::lower_bound(camera_path_.begin(), camera_path_.end(), tick,
                                       [](const auto& key, float time) { return key.tick < time; });
    if (next == camera_path_.begin()) return *next;
    if (next == camera_path_.end()) return camera_path_.back();
    const auto& previous = *std::prev(next);
    const float blend = (tick - previous.tick) / (next->tick - previous.tick);
    Vec center{};
    for (size_t i = 0; i < 3; ++i) center[i] = std::lerp(previous.center[i], next->center[i], blend);
    return {tick, center, std::lerp(previous.log_radius, next->log_radius, blend)};
}

void Viewport::draw(int x, int y, int w, int h, int slot, float tick, const ScenePose* scene) {
    if (w <= 0 || h <= 0) return;
    glEnable(GL_SCISSOR_TEST);
    glViewport(x, y, w, h);
    glScissor(x, y, w, h);
    glClearColor(.075F, .102F, .148F, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!target_.object) { glDisable(GL_SCISSOR_TEST); return; }
    Vec center{center_[0], center_[1], center_[2]};
    float distance = distance_, radius = radius_;
    StagePose stage{};
    const bool duo = scene && sora_.object;
    if (duo) {
        fit_scene(scene->chain.empty() ? std::span(&scene->segment, 1) : scene->chain, *scene);
        const float clock = scene->chain.empty() ? scene->tick : scene->chain_tick;
        if (scene->follow) {
            const auto camera = follow_view(clock);
            center = camera.center;
            radius = std::exp(camera.log_radius);
        } else {
            center = fixed_center_;
            radius = fixed_radius_;
        }
        const float aspect = static_cast<float>(w) / h;
        distance = radius * (1 + 2.0F / std::min(1.0F, aspect)) * scene_zoom_;
        stage = stage_pose(scene->segment, scene->tick, *scene, scene->previous);
    } else {
        const auto* motions = target_.object->motions.get();
        stage.target = target_.pose(motions && slot >= 0 ? motions->motion(slot) : nullptr, tick, false);
        stage.show_target = true;
    }
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    const double near_clip = radius * .015, top = near_clip * .43, right = top * w / h;
    glFrustum(-right, right, -top, top, near_clip, radius * 40.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -distance);
    glRotatef(pitch_, 1, 0, 0);
    glRotatef(yaw_, 0, 1, 0);
    glTranslatef(-center[0], -center[1], -center[2]);

    glColor3f(.21F, .28F, .37F);
    glBegin(GL_LINES);
    for (int i = -10; i <= 10; ++i) {
        const float step = duo ? .75F : radius * .4F, end = 10 * step, at = i * step;
        glVertex3f(at, 0, -end); glVertex3f(at, 0, end);
        glVertex3f(-end, 0, at); glVertex3f(end, 0, at);
    }
    glEnd();

    if (duo) sora_.draw(stage.sora);
    if (stage.show_weapon) weapon_.draw(stage.weapon);
    if (stage.show_target) target_.draw(stage.target);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
}

Viewport::ActorPose Viewport::Actor::pose(const kh2::Motion* motion, float tick, bool use_root) const {
    const auto& imported = object->model;
    ActorPose result{identity, imported.model.palette};
    if (motion && object->player && motion->model_joints() == static_cast<int>(imported.joints.size())) {
        tick = std::clamp(tick, 0.0F, std::max(0.0F, motion->length_ticks()));
        if (use_root) {
            result.placement = motion->root_motion(tick);
            for (size_t k = 12; k < 15; ++k) result.placement[k] *= .01F;
        }
        object->player->evaluate(*motion, tick, identity, nullptr);
        const auto& pose = object->player->joints();
        result.joints = pose;
        for (size_t j = 0; j < imported.joints.size() && j < result.joints.size(); ++j) {
            auto& m = result.joints[j];
            for (size_t k = 12; k < 15; ++k) m[k] *= .01F;
            const auto& joint = imported.joints[j];
            if (joint.palette_index < result.palette.size())
                result.palette[joint.palette_index] = multiply(m, joint.inverse_bind);
        }
    }
    return result;
}

void Viewport::Actor::blend(const ActorPose& from, ActorPose& to, float weight) const {
    if (from.joints.empty() || from.joints.size() != to.joints.size()) return;
    const auto& joints = object->model.joints;
    const float weights[] = {1 - weight, weight};
    for (size_t j = 0; j < joints.size() && j < to.joints.size(); ++j) {
        const kh2::Mat4 frames[] = {multiply(from.placement, from.joints[j]), multiply(to.placement, to.joints[j])};
        to.joints[j] = weight <= 0 ? frames[0] : khdays::models::blend_frames(frames, weights, 2);
        const auto& joint = joints[j];
        if (joint.palette_index < to.palette.size())
            to.palette[joint.palette_index] = multiply(to.joints[j], joint.inverse_bind);
    }
    to.placement = identity;
}

void Viewport::Actor::bounds(const ActorPose& pose, Vec& low, Vec& high) const {
    const auto& m = pose.placement;
    for (const auto& mesh : object->model.model.meshes) {
        for (const auto& vertex : mesh.vertices) {
            const auto p = skin(vertex, pose.palette);
            for (size_t i = 0; i < 3; ++i) {
                const float v = m[i] * p[0] + m[i + 4] * p[1] + m[i + 8] * p[2] + m[i + 12];
                if (!std::isfinite(v)) continue;
                low[i] = std::min(low[i], v);
                high[i] = std::max(high[i], v);
            }
        }
    }
}

void Viewport::Actor::draw(const ActorPose& pose) const {
    const auto& imported = object->model;
    glPushMatrix();
    glMultMatrixf(pose.placement.data());
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, .22F);
    for (const auto& mesh : imported.model.meshes) {
        const auto tex = textures.find(mesh.texture_name);
        if (tex != textures.end()) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex->second.id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mesh.repeat[0] ? GL_REPEAT : 0x812F);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mesh.repeat[1] ? GL_REPEAT : 0x812F);
        } else glDisable(GL_TEXTURE_2D);
        glBegin(GL_TRIANGLES);
        for (const auto i : mesh.indices) {
            if (i >= mesh.vertices.size()) continue;
            const auto& v = mesh.vertices[i];
            glColor4ubv(v.color.data());
            if (tex != textures.end())
                glTexCoord2f(v.texcoord[0] / tex->second.width, v.texcoord[1] / tex->second.height);
            glVertex3fv(skin(v, pose.palette).data());
        }
        glEnd();
    }
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();
}
}
