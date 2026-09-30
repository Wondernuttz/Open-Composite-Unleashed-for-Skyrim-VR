#pragma once

#include <atomic>
#include <cstdint>

// Independent of the MV/depth bridge publication protocol. A mask lease covers
// both its COM lifetime and every clear/replay/copy queued against its texture.
// No caller waits for another thread: a missed owned draw poisons this real
// frame so it cannot silently become a partially protected synthetic frame.
namespace DapaMaskBridge {
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free,
    "Mask handoff must not introduce a blocking atomic implementation");
inline constexpr std::uint8_t Format = 2;
enum class AccessMode { Read, Write, FrameStart };

template<class Bridge> bool Supported(const Bridge* bridge) noexcept
{
    // Constant after the containing bridge header has been published.
    return bridge && bridge->_padPreFP[0] == Format;
}

inline std::uint32_t Load(std::uint32_t& word) noexcept
{
    return std::atomic_ref<std::uint32_t>(word).load(std::memory_order_acquire);
}

template<class Bridge> class Access {
public:
    Access() noexcept = default;
    Access(Bridge* value, AccessMode requested) noexcept { TryAcquire(value, requested); }
    bool TryAcquire(Bridge* value, AccessMode requested) noexcept
    {
        if (acquired) return false;
        bridge = value; mode = requested;
        if (!Supported(bridge)) return false;
        // A reset must not absorb a producer conflict that occurs while reset
        // owns the gate. Record the baseline before attempting acquisition.
        entryConflicts = Load(bridge->maskConflictSerial);
        std::uint32_t expected = 0;
        acquired = std::atomic_ref<std::uint32_t>(bridge->maskAccessGate)
            .compare_exchange_strong(expected, 1, std::memory_order_acquire,
                std::memory_order_relaxed);
        if (!acquired) {
            if (mode != AccessMode::Read)
                std::atomic_ref<std::uint32_t>(bridge->maskConflictSerial)
                    .fetch_add(1, std::memory_order_acq_rel);
            return false;
        }
        frame = bridge->maskFrameGeneration;
        return true;
    }

    ~Access() noexcept
    {
        if (acquired)
            std::atomic_ref<std::uint32_t>(bridge->maskAccessGate)
                .store(0, std::memory_order_release);
    }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
    explicit operator bool() const noexcept { return acquired; }
    std::uint32_t Frame() const noexcept { return frame; }
    std::uint32_t ConflictSerial() const noexcept
    {
        return bridge ? Load(bridge->maskConflictSerial) : 0;
    }
    bool Clean() const noexcept
    {
        return acquired && frame != 0 && bridge->maskFrameGeneration == frame &&
            ConflictSerial() == bridge->maskFrameConflictBaseline;
    }
    bool Valid() const noexcept
    {
        return Clean() && bridge->preFPDepthCaptured && bridge->preFPDepthTexture;
    }
    std::uint64_t Texture() const noexcept
    {
        return Valid() ? bridge->preFPDepthTexture : 0;
    }
    void Poison() noexcept
    {
        if (acquired)
            std::atomic_ref<std::uint32_t>(bridge->maskConflictSerial)
                .fetch_add(1, std::memory_order_acq_rel);
    }
    bool Publish(std::uint64_t texture) noexcept
    {
        if (mode != AccessMode::Write || !Clean() || !texture) return false;
        bridge->preFPDepthTexture = texture;
        bridge->preFPDepthCaptured = 1;
        return true;
    }
    bool StartFrame() noexcept
    {
        if (!acquired || mode != AccessMode::FrameStart) return false;
        bridge->preFPDepthCaptured = 0;
        bridge->preFPDepthTexture = 0;
        frame = ++bridge->maskFrameGeneration;
        if (!frame) frame = ++bridge->maskFrameGeneration;
        bridge->maskFrameConflictBaseline = entryConflicts;
        return Clean();
    }
private:
    Bridge* bridge = nullptr;
    AccessMode mode = AccessMode::Read;
    bool acquired = false;
    std::uint32_t frame = 0, entryConflicts = 0;
};

template<class Bridge> bool BeginFrame(Bridge* bridge) noexcept
{
    Access<Bridge> access(bridge, AccessMode::FrameStart);
    return access.StartFrame();
}
} // namespace DapaMaskBridge
