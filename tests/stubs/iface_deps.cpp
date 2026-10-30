#include "aatped.h"
int g_cpuCount = 8;
int computeScore(int cpuId, const ThreadInfo& ti){(void)cpuId;(void)ti; return 50;}
Decision decidePlacement(const ThreadInfo& ti, const Config& cfg){(void)ti;(void)cfg; Decision d; return d;}
float readCpuUtilizationSchedstat(int cpuId){(void)cpuId; return 50.0f;}
#include <mutex>
aatpe_ml::LearningModel g_mlModel;
std::mutex g_mlMutex;
