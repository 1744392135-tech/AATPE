#include "aatped.h"
#include "aatpe_state_ttl.h"

const char* STATE_DIR = "/tmp/aatpe_ttl_d_state";

std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;

std::unordered_map<int, ThreadSettleState> g_threadSettle;
std::mutex g_settleMutex;

std::unordered_map<int, ThreadInfo> g_threads;
std::mutex g_threadsMutex;

std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
std::mutex g_scoreCacheMutex;

void stateCacheNoteRemove(const char*) {}
bool atomicWriteFile(const char*, long long) { return true; }
bool atomicWriteFile(const char*, const std::string&) { return true; }
