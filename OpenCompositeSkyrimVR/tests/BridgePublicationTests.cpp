#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "OpenOVR/Compositor/PublishedBridge.h"

using HANDLE = void*;
constexpr unsigned FILE_MAP_ALL_ACCESS = 1;
constexpr bool FALSE = false;
struct OCRenderTargetBridge {
	static constexpr std::uint32_t MAGIC = 0x4f435254, VERSION = 2;
	std::uint32_t magic = 0, version = 0, byteSize = 0;
	std::uint32_t payload = 0;
};

OCRenderTargetBridge candidate;
bool mappingExists = true, viewSucceeds = true;
std::atomic<unsigned> opens{0}, maps{0}, unmaps{0}, closes{0};
std::atomic<bool> viewAlive{false};
std::mutex pauseMutex;
std::condition_variable pauseCondition;
bool pauseRejection = true, rejectionPaused = false, resumeRejection = false;

HANDLE OpenFileMappingW(unsigned, bool, const wchar_t*)
{
	++opens;
	return mappingExists ? static_cast<void*>(&candidate) : nullptr;
}
void* MapViewOfFile(HANDLE, unsigned, unsigned, unsigned, std::size_t)
{
	++maps;
	if (!viewSucceeds) return nullptr;
	viewAlive = true;
	return &candidate;
}
bool UnmapViewOfFile(const void*) { ++unmaps; viewAlive = false; return true; }
bool CloseHandle(HANDLE) { ++closes; return true; }
void TestLog(const char* message)
{
	if (std::strstr(message, "Invalid magic/version/size") == nullptr) return;
	std::unique_lock lock(pauseMutex);
	if (!pauseRejection) return;
	rejectionPaused = true;
	pauseCondition.notify_all();
	pauseCondition.wait(lock, [] { return resumeRejection; });
}
#define OOVR_LOG(message) TestLog(message)
#include "BridgePublicationProduction.inc"

unsigned checks = 0;
void Check(bool condition, const char* message)
{
	++checks;
	if (!condition) throw std::runtime_error(message);
}
void Retry()
{
	const auto before = opens.load();
	for (unsigned i = 0; i < 179; ++i) OpenRenderTargetBridge();
	Check(opens == before, "failed connection retains 180-call retry cadence");
	OpenRenderTargetBridge();
	Check(opens == before + 1, "connection retries on call 180");
}
int main()
{
	try {
		// Stop the initializer immediately before rejecting/unmapping its view.
		// The old opener had already published that view at this exact point.
		std::thread initializer([] { OpenRenderTargetBridge(); });
		bool paused;
		{
			std::unique_lock lock(pauseMutex);
			paused = pauseCondition.wait_for(lock, std::chrono::seconds(5), [] { return rejectionPaused; });
		}
		std::atomic<bool> candidateExposed{static_cast<bool>(s_pBridge)}, contenderFinished{false};
		std::thread contender([&] {
			OpenRenderTargetBridge(); // Must return while validation is in flight.
			if (s_pBridge) candidateExposed = true;
			contenderFinished = true;
			pauseCondition.notify_all();
		});
		bool returnedWhilePending;
		{
			std::unique_lock lock(pauseMutex);
			returnedWhilePending = pauseCondition.wait_for(lock, std::chrono::seconds(5), [&] { return contenderFinished.load(); });
			resumeRejection = true;
		}
		pauseCondition.notify_all();
		initializer.join();
		contender.join();
		Check(paused, "test reached the real production rejection path");
		Check(!candidateExposed, "reader must never see provisional or rejected mapping");
		Check(returnedWhilePending && opens == 1, "concurrent initialization neither waits nor opens another mapping");
		Check(!s_pBridge && !viewAlive && closes == 1 && unmaps == 1, "invalid view cleaned up privately");
		pauseRejection = false;

		candidate.magic = OCRenderTargetBridge::MAGIC;
		candidate.version = OCRenderTargetBridge::VERSION + 1;
		candidate.byteSize = sizeof(candidate);
		Retry();
		Check(!s_pBridge && !viewAlive && closes == 2 && unmaps == 2, "incompatible version remains unpublished");
		candidate.version = OCRenderTargetBridge::VERSION;
		candidate.byteSize = sizeof(candidate) - 1;
		Retry();
		Check(!s_pBridge && !viewAlive && closes == 3 && unmaps == 3, "undersized mapping remains unpublished");
		candidate.byteSize = sizeof(candidate);

		mappingExists = false;
		Retry();
		Check(!s_pBridge && maps == 3 && closes == 3, "missing mapping neither maps nor closes invalid handle");
		mappingExists = true;
		viewSucceeds = false;
		Retry();
		Check(!s_pBridge && maps == 4 && closes == 4 && unmaps == 3, "map failure closes only its own handle");
		viewSucceeds = true;
		candidate.payload = 314159;
		Retry();
		Check(s_pBridge && s_pBridge->payload == 314159 && viewAlive, "ready writer recovers and publishes valid view");
		Check(closes == 5 && unmaps == 3, "published view survives local handle close");

		std::atomic<unsigned> badReads{0};
		std::vector<std::thread> readers;
		for (unsigned t = 0; t < 8; ++t) readers.emplace_back([&] {
			for (unsigned i = 0; i < 10000; ++i) {
				OpenRenderTargetBridge();
				if (!s_pBridge || s_pBridge->payload != 314159 || !viewAlive) ++badReads;
			}
		});
		for (auto& reader : readers) reader.join();
		Check(badReads == 0, "concurrent connected readers retain one valid mapping");
		Check(opens == 6 && maps == 5 && closes == 5 && unmaps == 3,
		    "connected path never remaps or releases published view");
		std::cout << checks << " production bridge publication checks passed; 80000 concurrent connected reads\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
