#pragma once

#include "reader.h"

namespace rc {
struct SceneSegment {
    const kh2::Motion* sora_motion;
    const kh2::Motion* target_motion;
    const kh2::Motion* weapon_motion;
    float duration;
    bool show_target;
    bool apply_root = true;
    float target_start = 0;
    float target_rate = 1;
    float target_depth = 0;
    std::array<float, 3> offset{};
    float yaw = 0;
    float blend_ticks = 0;
    bool blend_target_only = false;
    const kh2::Motion* landing = nullptr;
    bool operator==(const SceneSegment&) const = default;
};

struct ScenePose {
    SceneSegment segment;
    float tick, target_delay;
    std::array<float, 3> target_offset;
    float target_yaw;
    std::span<const SceneSegment> chain;
    float chain_tick;
    bool follow;
    const SceneSegment* previous = nullptr;
};

class Viewport {
public:
    void load(const Object* object);
    void load_sora(const Object* object);
    void load_weapon(const Object* object);
    void draw(int x, int y, int w, int h, int slot, float tick, const ScenePose* scene);
    void rotate(float dx, float dy);
    void zoom(float delta);
    void reset();
    void frame_scene();
    float context_distance() const;
    void align_axel_steps(std::span<SceneSegment> segments, const ScenePose& scene) const;

private:
    struct Texture { unsigned id; int width, height; };
    struct ActorPose {
        kh2::Mat4 placement;
        std::vector<kh2::Mat4> palette;
        std::vector<kh2::Mat4> joints;
    };
    struct Actor {
        const Object* object = nullptr;
        std::map<std::string, Texture> textures;
        void load(const Object* next);
        ActorPose pose(const kh2::Motion* motion, float tick, bool root_motion) const;
        void blend(const ActorPose& from, ActorPose& to, float weight) const;
        void bounds(const ActorPose& pose, std::array<float, 3>& low, std::array<float, 3>& high) const;
        void draw(const ActorPose& pose) const;
    };
    struct StagePose {
        ActorPose sora, target, weapon;
        bool show_target, show_weapon;
    };
    struct CameraKey {
        float tick;
        std::array<float, 3> center;
        float log_radius;
    };

    StagePose clip_pose(const SceneSegment& segment, float tick, const ScenePose& scene) const;
    StagePose stage_pose(const SceneSegment& segment, float tick, const ScenePose& scene,
                         const SceneSegment* previous) const;
    void fit_scene(std::span<const SceneSegment> segments, const ScenePose& scene);
    CameraKey follow_view(float tick) const;

    Actor target_, sora_, weapon_;
    float center_[3]{0, .8F, 0}, radius_ = 1.5F, distance_ = 4;
    float yaw_ = 145, pitch_ = 9;
    float scene_zoom_ = 1;
    std::vector<SceneSegment> fitted_segments_;
    std::vector<CameraKey> camera_path_;
    std::array<float, 3> fitted_offset_{}, fixed_center_{};
    float fitted_yaw_ = 0, fitted_delay_ = 0;
    float fixed_radius_ = .8F;
};
}
