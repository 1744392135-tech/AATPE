// AATPE - Audio low-latency sub-header
#ifndef AATPED_AUDIO_H
#define AATPED_AUDIO_H

// Audio low-latency mode
#include <atomic>
#include <string>
extern std::atomic<bool> g_audioModeActive;
extern std::atomic<int> g_audioIsolatedCpu;
bool detectAudioPlayback(void);
bool isAudioThread(const char* threadName);
int getAudioThreadMigrationPenalty(const char* threadName);
// C-006: pure parser — does `dumpsys media.audio_flinger` show an ACTIVE playback
// track? Conservative: empty/unrecognized -> false (no audio mode without positive
// evidence). Declared here for the host hermetic test.
bool aatpeAudioDumpShowsActiveTrack(const std::string& dump);

#endif // AATPED_AUDIO_H
