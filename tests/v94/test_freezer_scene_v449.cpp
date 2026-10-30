#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#include "c4_freezer.h"
#include "aatpe_managers.h"

using namespace aatpe;
using namespace aatpe::c4freezer;

static std::atomic<int> s_fgCounter{0};
namespace {
std::string g_fgA = "com.tencent.longgg.hellogame.0123456789";
std::string g_fgB = "short";
}
ForegroundManager& ForegroundManager::instance() {
    static ForegroundManager inst;
    return inst;
}
std::string ForegroundManager::getPackage() const {
    int c = s_fgCounter.fetch_add(1, std::memory_order_relaxed);
    return (c & 1) ? g_fgA : g_fgB;
}
void ForegroundManager::setPackage(const std::string&) {}
time_t ForegroundManager::startTime() const { return 0; }

std::string aatpeExec(const std::string&, const char*) { return ""; }

std::string readFile(const char*) { return ""; }
bool writeFileAtomic(const char*, const std::string&) { return true; }

const char* STATE_DIR = "/tmp/aatpe_c4freezer_scene_state";

namespace aatpe { namespace gamelist {
bool isManualGame(const std::string&) { return true; }
}}

bool isPackageInGameList(const std::string&) { return false; }

namespace aatpe {
bool isGameEngineThread(const char*, const char*) { return false; }
}

static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

int main() {
    printf("=== R2-M1 c4FreezerStateJson vs c4FreezerTick concurrency ===\n");

    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled for the race test");

    std::atomic<bool> stop{false};

    std::thread ticker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            c4FreezerTick();
        }
    });

    std::thread reader([&]() {
        std::string sink;
        while (!stop.load(std::memory_order_relaxed)) {
            sink = c4FreezerStateJson();
            if (sink.empty()) { printf("  [WARN] empty json\n"); }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true, std::memory_order_relaxed);
    ticker.join();
    reader.join();

    std::string j = c4FreezerStateJson();
    printf("  snapshot: %s\n", j.c_str());
    CHECK(j.find("\"enabled\":true") != std::string::npos, "JSON reports enabled:true");
    CHECK(j.find("\"target\":") != std::string::npos, "JSON has target field");
    CHECK(j.find("\"frozen\":") != std::string::npos, "JSON has frozen field");
    CHECK(j.find("\"cgDir\":") != std::string::npos, "JSON has cgDir field");
    CHECK(j.find("\"lastAction\":") != std::string::npos, "JSON has lastAction field");
    CHECK(j.front() == '{' && j.back() == '}', "JSON is a closed object");

    c4FreezerTick();
    CHECK(true, "tick post-race did not crash");

    printf("=== R2-M1 result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
