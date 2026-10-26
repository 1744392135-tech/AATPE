// ttl_deps.cpp — minimal host stubs for linking aatpe_state_ttl.o into the
// hermetic D-002 TTL regression test. Provides exactly the extern globals that
// aatpe_state_ttl.cpp references (verified via `nm -u`), nothing more.
// Includes the same umbrella header the production TU uses so all the
// HistoryEntry / ThreadScoreEntry / ThreadSettleState / ThreadInfo types are
// complete.
#include "aatped.h"
#include "aatpe_state_ttl.h"

// Redirected state dir for the test.
const char* STATE_DIR = "/tmp/aatpe_ttl_d_state";

// g_history / g_threadSettle / g_scoreCache / g_threads + their mutexes.
std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;

std::unordered_map<int, ThreadSettleState> g_threadSettle;
std::mutex g_settleMutex;

std::unordered_map<int, ThreadInfo> g_threads;
std::mutex g_threadsMutex;

std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
std::mutex g_scoreCacheMutex;

// Filesystem helpers referenced by stateTtlSweep / writePlacementAvgStats.
void stateCacheNoteRemove(const char*) {}
bool atomicWriteFile(const char*, long long) { return true; }
bool atomicWriteFile(const char*, const std::string&) { return true; }
