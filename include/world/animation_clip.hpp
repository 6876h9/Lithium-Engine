#pragma once

#include "core/math.hpp"
#include <string>
#include <vector>
#include <cmath>

// ---- Keyframe types ----

struct PosKey {
    float time;
    Vector3 value;
};

struct RotKey {
    float time;
    Vector4 value; // quaternion: x, y, z, w
};

struct ScaleKey {
    float time;
    Vector3 value;
};

// ---- Per-bone animation channel ----

struct BoneChannel {
    int bone_index = -1;
    std::vector<PosKey>   pos_keys;
    std::vector<RotKey>   rot_keys;
    std::vector<ScaleKey> scale_keys;

    Vector3 sample_position(float time) const {
        if (pos_keys.empty()) return {0.0f, 0.0f, 0.0f};
        if (pos_keys.size() == 1) return pos_keys[0].value;
        for (size_t i = 0; i + 1 < pos_keys.size(); ++i) {
            if (time <= pos_keys[i + 1].time) {
                float t = (time - pos_keys[i].time) / (pos_keys[i + 1].time - pos_keys[i].time);
                t = std::max(0.0f, std::min(1.0f, t));
                const auto& a = pos_keys[i].value;
                const auto& b = pos_keys[i + 1].value;
                return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
            }
        }
        return pos_keys.back().value;
    }

    // Quaternion slerp
    Vector4 sample_rotation(float time) const {
        if (rot_keys.empty()) return {0.0f, 0.0f, 0.0f, 1.0f};
        if (rot_keys.size() == 1) return rot_keys[0].value;
        for (size_t i = 0; i + 1 < rot_keys.size(); ++i) {
            if (time <= rot_keys[i + 1].time) {
                float t = (time - rot_keys[i].time) / (rot_keys[i + 1].time - rot_keys[i].time);
                t = std::max(0.0f, std::min(1.0f, t));
                Vector4 a = rot_keys[i].value;
                Vector4 b = rot_keys[i + 1].value;
                // Ensure shortest path
                float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
                if (dot < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; dot = -dot; }
                if (dot > 0.9995f) {
                    // Linear fallback when very close
                    return { a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t };
                }
                float theta = std::acos(dot);
                float sin_theta = std::sin(theta);
                float wa = std::sin((1.0f - t) * theta) / sin_theta;
                float wb = std::sin(t * theta) / sin_theta;
                return { wa*a.x + wb*b.x, wa*a.y + wb*b.y, wa*a.z + wb*b.z, wa*a.w + wb*b.w };
            }
        }
        return rot_keys.back().value;
    }

    Vector3 sample_scale(float time) const {
        if (scale_keys.empty()) return {1.0f, 1.0f, 1.0f};
        if (scale_keys.size() == 1) return scale_keys[0].value;
        for (size_t i = 0; i + 1 < scale_keys.size(); ++i) {
            if (time <= scale_keys[i + 1].time) {
                float t = (time - scale_keys[i].time) / (scale_keys[i + 1].time - scale_keys[i].time);
                t = std::max(0.0f, std::min(1.0f, t));
                const auto& a = scale_keys[i].value;
                const auto& b = scale_keys[i + 1].value;
                return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
            }
        }
        return scale_keys.back().value;
    }
};

// ---- Animation Clip ----

struct AnimationClip {
    std::string name;
    float duration = 0.0f;       // In ticks
    float ticks_per_second = 24.0f;
    std::vector<BoneChannel> channels;

    float get_duration_seconds() const {
        return (ticks_per_second > 0.0f) ? duration / ticks_per_second : 0.0f;
    }

    // Channel targeting a given bone, or null if the clip does not animate it.
    const BoneChannel* find_channel(int bone_index) const {
        for (const auto& channel : channels) {
            if (channel.bone_index == bone_index) return &channel;
        }
        return nullptr;
    }
};
