#ifndef AATPE_STATE_TTL_H
#define AATPE_STATE_TTL_H

#include <ctime>
#include <string>

void stateTtlSweep(time_t nowSec, int ttlHours);

void placementAvgRecordGain(float gainPercent, int confidence);

void writePlacementAvgStats();

bool stateTtlIsExpired(time_t mtime, time_t nowSec, int ttlHours);

#endif
