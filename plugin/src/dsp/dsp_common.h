#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace gasfree {

inline constexpr double C4_FREQ = 261.6255653005986;
inline constexpr double PI = 3.14159265358979323846;
inline constexpr double TWO_PI = 6.28318530717958647692;
inline constexpr double LN2 = 0.69314718055994530942;
inline constexpr double DB_TO_LN = 0.11512925464970228420; // ln(10)/20

inline float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline double clampd(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float fastTanh(float x) noexcept {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline double fastTanhD(double x) noexcept {
    if (x < -3.0) return -1.0;
    if (x > 3.0) return 1.0;
    double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

inline double polyblep(double t, double dt) noexcept {
    if (dt <= 0.0) return 0.0;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

inline double blepSaw(double t, double dt) noexcept {
    return 2.0 * t - 1.0 - polyblep(t, dt);
}

inline double blepPulse(double t, double dt, double pw) noexcept {
    double t2 = t - pw;
    if (t2 < 0.0) t2 += 1.0;
    return (t < pw ? 1.0 : -1.0) + polyblep(t, dt) - polyblep(t2, dt);
}

// Fast deterministic PRNG (xorshift32) - RT safe, no locks
struct FastRng {
    uint32_t state = 0x12345678u;

    void seed(uint32_t s) noexcept { state = s ? s : 0x12345678u; }

    uint32_t nextU32() noexcept {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    // [0, 1)
    double next01() noexcept {
        return static_cast<double>(nextU32()) * (1.0 / 4294967296.0);
    }

    // [-1, 1)
    double nextBipolar() noexcept {
        return next01() * 2.0 - 1.0;
    }
};

struct Scale {
    std::vector<double> degrees; // sorted octave fractions in [0, period)
    double period = 1.0;

    bool valid() const noexcept { return !degrees.empty() && period > 0.0; }
};

inline double quantizePitch(double x, const Scale& scale) noexcept {
    const double p = scale.period;
    const auto& d = scale.degrees;
    const double o = std::floor(x / p);
    const double r = x - o * p;
    double best = d[0] + p;
    double bd = std::abs(r - best);
    for (double deg : d) {
        const double dd = std::abs(r - deg);
        if (dd < bd) {
            bd = dd;
            best = deg;
        }
    }
    return o * p + best;
}

// Wait-free Single-Producer Single-Consumer ring buffer (Capacity must be power of two)
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");

public:
    bool push(T&& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        buf_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = std::move(buf_[tail]);
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> buf_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace gasfree
