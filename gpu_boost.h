
#ifndef AATPE_GPU_BOOST_H
#define AATPE_GPU_BOOST_H

#include <string>

namespace aatpe {

void aatpeBoostSet(const char* source, int level);
void aatpeBoostClear(const char* source);

void gpuBoostTick();

bool gpuBoostActive();
void aatpeGpuBoostForceZero(const char* reason);

#ifdef AATPE_GPU_BOOST_TEST
namespace boost_test {
void resetForTest();
void tickForTest(long long nowMs, bool(*writable)(const char*));
std::string nodeForTest();
int consecFailsForTest();
void devfreqResetForTest();
void devfreqSetNodeForTest(const std::string& path);
void devfreqBoostOnForTest();
void devfreqBoostOffForTest();
bool devfreqBoostedForTest();
unsigned long devfreqSavedMinForTest();
std::string devfreqNodeForTest();
int devfreqOffFailsForTest();
std::string devfreqMarkerPathForTest();
bool boostEngagedForTest();
void boostResetHysteresisForTest();
long long boostEngagedMsForTest();
}
#endif

}

#endif
