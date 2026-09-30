#include "DrvOpenXR/DapaMaskBridge.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

struct Bridge {
    std::uint64_t preFPDepthTexture = 0;
    std::uint8_t preFPDepthCaptured = 0;
    std::uint8_t _padPreFP[3]{DapaMaskBridge::Format, 0, 0};
    std::uint32_t maskAccessGate = 0, maskFrameGeneration = 0;
    std::uint32_t maskConflictSerial = 0, maskFrameConflictBaseline = 0;
};
using Access = DapaMaskBridge::Access<Bridge>;
using Mode = DapaMaskBridge::AccessMode;
static unsigned checks = 0;
static void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    Bridge bridge;
    Check(DapaMaskBridge::Supported(&bridge), "format 2 advertises negotiated protection");
    {
        Access read(&bridge, Mode::Read);
        Check(read && !read.Clean() && !read.Valid(), "no prior frame cannot expose a mask");
    }
    Check(DapaMaskBridge::BeginFrame(&bridge), "first real frame starts");
    {
        Access read(&bridge, Mode::Read);
        Check(read.Clean() && !read.Valid() && read.Texture() == 0,
            "frame with no owned draws remains clean and has no stale mask");
    }
    {
        Access write;
        Check(write.TryAcquire(&bridge, Mode::Write), "producer can acquire after scene admission");
        Check(write.Publish(0x1234), "producer publishes under lease");
        Access competingRead(&bridge, Mode::Read);
        Check(!competingRead && write.Clean(), "reader contention skips without poisoning producer");
    }
    {
        Access read(&bridge, Mode::Read);
        Check(read.Valid() && read.Texture() == 0x1234, "consumer obtains published mask under lease");
        std::atomic<bool> couldReplace{true};
        std::thread writer([&] {
            Access write(&bridge, Mode::Write);
            couldReplace.store(bool(write));
            if (write) bridge.preFPDepthTexture = 0x9876;
        });
        writer.join();
        Check(!couldReplace && bridge.preFPDepthTexture == 0x1234,
            "concurrent producer cannot replace a texture held by consumer");
        Check(!read.Clean() && !read.Valid(),
            "missed owned draw poisons copied mask instead of accepting partial coverage");
    }
    {
        Access write(&bridge, Mode::Write);
        Check(write && !write.Clean() && !write.Publish(0x9876),
            "later successful producer does not silently repair a partially captured frame");
    }
    Check(DapaMaskBridge::BeginFrame(&bridge), "next complete real frame recovers immediately");
    {
        Access read(&bridge, Mode::Read);
        Check(read.Clean() && !read.Valid() && bridge.preFPDepthTexture == 0,
            "new no-draw frame expires prior mask");
    }
    {
        Access write(&bridge, Mode::Write);
        const auto previousFrame = write.Frame();
        Check(write.Publish(0x4444), "next frame can publish normally");
        Check(!DapaMaskBridge::BeginFrame(&bridge), "overlapping frame start does not wait");
        Check(write.Frame() == previousFrame && !write.Clean() && !write.Publish(0x5555),
            "failed frame start invalidates in-flight producer and cannot advance beneath it");
    }
    Check(DapaMaskBridge::BeginFrame(&bridge), "frame after overlapping boundary recovers");
    {
        Access reset(&bridge, Mode::FrameStart);
        Access write(&bridge, Mode::Write);
        Check(!write, "producer cannot enter held reset");
        Check(!reset.StartFrame() && !reset.Clean(),
            "reset baseline cannot absorb owned draw missed during reset");
    }
    Check(DapaMaskBridge::BeginFrame(&bridge), "poisoned reset recovers at next boundary");
    {
        Access write(&bridge, Mode::Write);
        write.Poison();
        Check(!write.Publish(0x1234), "GPU mask failure cannot expose earlier partial coverage");
    }
    Bridge old;
    old._padPreFP[0] = 1;
    old.preFPDepthCaptured = 1;
    old.preFPDepthTexture = 0xDEADBEEF;
    Check(!DapaMaskBridge::Supported(&old) && !DapaMaskBridge::BeginFrame(&old),
        "old raw-pointer protocol is never promoted to protected format");
    {
        Access read(&old, Mode::Read), write(&old, Mode::Write);
        Check(!read && !write && !read.Texture() && old.maskConflictSerial == 0,
            "mixed versions never dereference legacy pointer or mutate new metadata");
    }
    bridge.maskFrameGeneration = UINT32_MAX;
    Check(DapaMaskBridge::BeginFrame(&bridge) && bridge.maskFrameGeneration == 1,
        "frame wrap keeps zero reserved for uninitialized state");

    // Exercise actual ownership rather than only metadata: readers dereference
    // immutable heap objects while producers replace and delete previous ones.
    // Every object's destruction must occur after its consumer lease ends.
    struct Texture { std::uint64_t serial, inverse; };
    Bridge concurrent;
    Texture* current = nullptr;
    std::atomic<bool> start{false}, done{false};
    std::atomic<unsigned> reads{0}, replacements{0}, violations{0};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t serial = 1; serial <= 30000; ++serial) {
            if (!DapaMaskBridge::BeginFrame(&concurrent)) continue;
            Access write(&concurrent, Mode::Write);
            if (!write || !write.Clean()) continue;
            auto* next = new Texture{serial, ~serial};
            delete current;
            current = next;
            if (write.Publish(reinterpret_cast<std::uint64_t>(next))) ++replacements;
        }
        done.store(true, std::memory_order_release);
    });
    std::vector<std::thread> consumers;
    for (int index = 0; index < 3; ++index) consumers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!done.load(std::memory_order_acquire)) {
            Access read(&concurrent, Mode::Read);
            if (!read || !read.Valid()) continue;
            auto* texture = reinterpret_cast<const Texture*>(read.Texture());
            if (texture) {
                const auto serial = texture->serial;
                std::this_thread::yield();
                if (texture->serial != serial || texture->inverse != ~serial) ++violations;
                ++reads;
            }
        }
    });
    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& consumer : consumers) consumer.join();
    delete current;
    Check(violations == 0, "consumer-held heap object survives concurrent replace/delete attempts");
    Check(replacements != 0 && reads != 0, "stress exercised replacement and reader access");
    std::printf("PASS: %u mask lease checks; %u replacements, %u retained reads, %u lifetime violations\n",
        checks, replacements.load(), reads.load(), violations.load());
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
