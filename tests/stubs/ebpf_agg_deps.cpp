#include "aatped.h"

std::atomic<unsigned int> g_debugSampleCounter{0};

std::atomic<uint64_t> g_ebpfEventsTotal{0};std::atomic<uint64_t> g_ebpfEventsFork{0};
std::atomic<uint64_t> g_ebpfEventsExit{0};
std::atomic<uint64_t> g_ebpfEventsSwitch{0};

std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

std::string g_foregroundApp;

void snapshotConfig(Config& out) { out = Config(); }

void markThreadScoreDirty(int , long long ) {}
std::string aatpeDetectForegroundApp(bool ) { return "unknown"; }
ThreadRole classifyThread(int , int , const std::string&,
                          const std::string&, int& outPriority, std::string& outSubtype,
                          int* outConfidence, unsigned long long ) {
    outPriority = aatpe::thread_classify::kDefaultPriority;
    outSubtype.clear();
    if (outConfidence) *outConfidence = 0;
    return ROLE_S5_BACKGROUND;
}
std::string readFile(const char* ) { return std::string(); }

void cleanupThreadScoringState(int ) {}

#include "aatpe_recovery.h"
namespace aatpe_recovery { void recoveryErase(int ) {} }
