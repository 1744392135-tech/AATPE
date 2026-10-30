#ifndef AATPED_AUDIO_H
#define AATPED_AUDIO_H

#include <atomic>
#include <string>
extern std::atomic<bool> g_audioModeActive;
extern std::atomic<int> g_audioIsolatedCpu;
bool detectAudioPlayback(void);
bool isAudioThread(const char* threadName);
int getAudioThreadMigrationPenalty(const char* threadName);
bool aatpeAudioDumpShowsActiveTrack(const std::string& dump);

#endif
