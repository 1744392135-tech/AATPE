#pragma once

#include "aatped_types.h"

namespace aatpe_priority {

bool aatpeSetThreadPriority(int tid, ThreadRole role, int level);

bool aatpeRestoreThreadPriority(int tid);

bool uclampAvailable();

int uclampSupportState();

#ifdef AATPE_HOST_TEST
void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime);
#endif

}
