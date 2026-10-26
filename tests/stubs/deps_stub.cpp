#include "aatped.h"
#include <mutex>
std::mutex g_cpuMetricsMutex;
SystemState getSystemState(){return SystemState::NORMAL;}
bool isPackageInGameList(const std::string& pkg){(void)pkg;return false;}
float hybridScore(int cpuId, const ThreadInfo& ti){(void)cpuId;(void)ti;return 0.0f;}
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot(){aatpe_ml::FrameTimeOutcome f{}; return f;}
