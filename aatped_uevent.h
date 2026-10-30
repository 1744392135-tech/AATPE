#ifndef AATPED_UEVENT_H
#define AATPED_UEVENT_H

#include <atomic>

extern std::atomic<bool> g_ueventFallbackActive;
extern std::atomic<int> g_ueventEventCount;
void checkUeventFallback(void);
void pollSysfsEvents(void);

#endif
