#pragma once

#include "renderer/gl_loader.hpp"
#include <array>
#include <chrono>
#include <cstddef>

// Per-pass CPU and GPU timings for one frame.
//
// GPU timing uses GL_TIME_ELAPSED queries, which are asynchronous: asking for a
// result the same frame it was issued forces a full pipeline stall and would make
// the profiler itself the slowest thing in the frame. Results are therefore read
// from a ring of query sets a few frames old, which is always ready by the time it
// is asked for. That means GPU numbers lag the displayed frame slightly - correct
// for spotting which pass is expensive, not for matching a single frame exactly.
class RenderProfiler {
public:
    enum Pass {
        Shadow,
        Geometry,
        Lighting,
        SSAO,
        Bloom,
        GodRays,
        Resolve,
        PassCount
    };

    static const char* pass_name(int p) {
        static const char* names[PassCount] = {
            "Shadow", "Geometry", "Lighting", "SSAO", "Bloom", "God Rays", "Resolve"
        };
        return (p >= 0 && p < PassCount) ? names[p] : "?";
    }

    void initialize() {
        if (initialized_) return;
        for (auto& slot : slots_) {
            glGenQueries(PassCount, slot.queries.data());
            slot.issued.fill(false);
        }
        initialized_ = true;
    }

    void shutdown() {
        if (!initialized_) return;
        for (auto& slot : slots_) glDeleteQueries(PassCount, slot.queries.data());
        initialized_ = false;
    }

    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // Harvests whatever the oldest in-flight slot has finished, then claims that
    // slot for the frame about to be recorded.
    void begin_frame() {
        draw_calls = 0;
        triangles = 0;
        if (!enabled_ || !initialized_) return;

        current_ = (current_ + 1) % kSlots;
        Slot& slot = slots_[current_];
        for (int p = 0; p < PassCount; ++p) {
            if (!slot.issued[p]) continue;
            GLuint ready = 0;
            glGetQueryObjectuiv(slot.queries[p], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(slot.queries[p], GL_QUERY_RESULT, &ns);
                gpu_ms[p] = static_cast<float>(ns) / 1.0e6f;
            }
            slot.issued[p] = false;
        }
        cpu_ms.fill(0.0f);
    }

    void begin_pass(int p) {
        if (!enabled_ || p < 0 || p >= PassCount) return;
        pass_start_[p] = Clock::now();
        if (!initialized_ || active_ != -1) return;   // GL_TIME_ELAPSED cannot nest
        glBeginQuery(GL_TIME_ELAPSED, slots_[current_].queries[p]);
        active_ = p;
    }

    void end_pass(int p) {
        if (!enabled_ || p < 0 || p >= PassCount) return;
        std::chrono::duration<float, std::milli> d = Clock::now() - pass_start_[p];
        cpu_ms[p] += d.count();
        if (active_ == p) {
            glEndQuery(GL_TIME_ELAPSED);
            slots_[current_].issued[p] = true;
            active_ = -1;
        }
    }

    float total_cpu_ms() const {
        float t = 0.0f;
        for (float v : cpu_ms) t += v;
        return t;
    }
    float total_gpu_ms() const {
        float t = 0.0f;
        for (float v : gpu_ms) t += v;
        return t;
    }

    // Reset each frame by begin_frame(), incremented by the draw submission paths.
    int draw_calls = 0;
    int triangles = 0;

    std::array<float, PassCount> cpu_ms{};
    std::array<float, PassCount> gpu_ms{};

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int kSlots = 3;   // deep enough that a result is always ready

    struct Slot {
        std::array<GLuint, PassCount> queries{};
        std::array<bool, PassCount> issued{};
    };

    std::array<Slot, kSlots> slots_{};
    std::array<Clock::time_point, PassCount> pass_start_{};
    int current_ = 0;
    int active_ = -1;
    bool enabled_ = true;
    bool initialized_ = false;
};

// Times a pass for as long as it is in scope, so an early return cannot leave a
// query open - which would desynchronise every later pass in the frame.
struct ScopedGpuPass {
    ScopedGpuPass(RenderProfiler& p, int pass) : prof(p), id(pass) { prof.begin_pass(id); }
    ~ScopedGpuPass() { prof.end_pass(id); }
    RenderProfiler& prof;
    int id;
};
