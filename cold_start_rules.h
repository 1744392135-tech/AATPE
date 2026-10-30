
#ifndef AATPE_COLD_START_RULES_H
#define AATPE_COLD_START_RULES_H

#include <string>

namespace aatpe {

void coldStartOnForegroundSwitch(const std::string& pkg);

void coldStartTick();

void coldStartSetThermalFreeze(bool f);
void coldStartSetIoPressure(bool f);
bool coldStartDisplayedPresent(const std::string& dumpsysOut);

#ifdef AATPE_HOST_TEST
void coldStartSetNowForTest(long long (*fn)());
void coldStartSetJankForTest(bool (*fn)());
void coldStartSetDisplayedForTest(std::string (*fn)());
long long coldStartRejectedCountForTest();
void coldStartSetProcIdentityForTest(const std::string& pkg, int pid,
                                     unsigned long long start_time);
bool coldStartIsColdForTest();
#endif

bool coldStartActive();
std::string coldStartPkg();

}

#endif
