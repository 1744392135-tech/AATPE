// AATPE - State/cache TTL cleanup (#77)
// Auto-expire sensitive history entries so /dev/aatpe does not grow forever.
#ifndef AATPE_STATE_TTL_H
#define AATPE_STATE_TTL_H

#include <ctime>
#include <string>

// Run one TTL sweep. Called periodically from the daemon loop.
// Cleans:
//   - g_history entries older than ttlHours*3600s
//   - g_threadSettle entries for dead threads (LIVENESS gc, always runs)
//   - g_scoreCache entries idle longer than ttlHours*3600s
//   - state files under STATE_DIR that are stale (e.g. thread_*.json)
//
// D-002 UNIFIED SEMANTICS: ttlHours <= 0 DISABLES all age-based expiration.
// Under that setting history entries, score-cache entries and stale state files
// are ALL retained (no 24h fallback). The liveness GC of g_threadSettle for dead
// threads still runs (it is not a TTL). This matches stateTtlIsExpired(), which
// has always returned false for ttlHours<=0 ("no expiration").
// NOTE (P3-38): g_decisionRing was previously listed here but its cleanup is
// not implemented in stateTtlSweep(); the ring is bounded in size and is
// overwritten on a FIFO basis, so TTL cleanup is not required. The stale-file
// sweep below does clean decision_* files under STATE_DIR.
void stateTtlSweep(time_t nowSec, int ttlHours);

// #35: record a placement's relative gain (%) and confidence (0-100) for
// accumulation into the diagnostic state files.
void placementAvgRecordGain(float gainPercent, int confidence);

// Write diagnostic counters to STATE_DIR:
//   placement_avg_confidence, placement_avg_gain
// (also used by #35 three-value output)
void writePlacementAvgStats();

// Returns true if a state file with the given last-modified time is expired.
// D-002: ttlHours <= 0 means "never expire" (always returns false) — this is
// the single source of truth for the unified TTL-disabled contract.
bool stateTtlIsExpired(time_t mtime, time_t nowSec, int ttlHours);

#endif // AATPE_STATE_TTL_H
