#include <atomic>
#include <cstdio>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

static std::atomic<bool> g_renderTargetRefreshTaskPending{false};
static std::queue<std::function<void()>> tasks;
static unsigned startupReads = 0, menuReads = 0, resourceReads = 0, failures = 0;
static int liveResource = 1, publishedResource = 0;
static bool liveMenu = true, publishedMenu = false, queueWhileExecuting = false;
void QueueBridgeMaintenance();
void ObserveInitialBridgeMenuState() { ++startupReads; }
void RefreshMenuActivityFromGameState()
{
    if (startupReads != menuReads + 1) ++failures;
    ++menuReads;
    publishedMenu = liveMenu;
    if (queueWhileExecuting) QueueBridgeMaintenance();
}
void RefreshBridgeRenderTargets()
{
    ++resourceReads;
    publishedResource = liveResource;
}
namespace SKSE {
struct TaskInterface {
    void AddTask(std::function<void()> work) { tasks.push(std::move(work)); }
};
TaskInterface* GetTaskInterface() { static TaskInterface instance; return &instance; }
}
#include "BridgeMaintenanceProduction.inl"

static void Check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
static void RunTask()
{
    auto work = std::move(tasks.front()); tasks.pop(); work();
}
int main()
{
    for (int tick = 0; tick < 1000; ++tick) QueueBridgeMaintenance();
    Check(tasks.size() == 1 && startupReads == 0 && menuReads == 0 && resourceReads == 0,
        "stalled game thread queues one task without reading engine state");
    liveResource = 2; liveMenu = false; queueWhileExecuting = true;
    RunTask();
    Check(publishedResource == 2 && !publishedMenu && tasks.empty(),
        "task samples current state and cannot requeue itself while executing");
    Check(startupReads == 1 && menuReads == 1 && resourceReads == 1 && !g_renderTargetRefreshTaskPending,
        "both maintenance functions execute once and release pending state");
    queueWhileExecuting = false;
    QueueBridgeMaintenance();
    Check(tasks.size() == 1, "next scheduler tick can refresh without a one-second delay");
    liveResource = 3; liveMenu = true; RunTask();
    Check(publishedResource == 3 && publishedMenu, "next task observes new resources and real menu");

    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i)
        callers.emplace_back([] { for (int n = 0; n < 1000; ++n) QueueBridgeMaintenance(); });
    for (auto& caller : callers) caller.join();
    Check(tasks.size() == 1, "concurrent queue requests coalesce into one task");
    RunTask();
    Check(tasks.empty() && !g_renderTargetRefreshTaskPending,
        "coalesced task drains cleanly");
    std::printf("Production bridge maintenance queue: %u failures\n", failures);
    return failures ? 1 : 0;
}
