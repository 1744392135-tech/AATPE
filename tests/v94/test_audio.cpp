#include <cstdio>
#include "aatped.h"
#include "aatped_audio.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Audio: isAudioThread name matching ===\n");
    CHECK(isAudioThread("AudioFlinger::0") == true, "AudioFlinger matched");
    CHECK(isAudioThread("AudioTrack") == true, "AudioTrack matched");
    CHECK(isAudioThread("audioserver") == true, "audioserver matched");
    CHECK(isAudioThread("FastMixer") == true, "FastMixer matched");
    CHECK(isAudioThread("AudioOut") == true, "AudioOut matched");
    CHECK(isAudioThread("RenderThread") == false, "RenderThread NOT audio");
    CHECK(isAudioThread("") == false, "empty string NOT audio");
    CHECK(isAudioThread(nullptr) == false, "nullptr NOT audio");

    printf("=== Audio: migration penalty gated on audio mode ===\n");
    g_audioModeActive = false;
    CHECK(getAudioThreadMigrationPenalty("AudioTrack") == 0, "mode off -> 0 penalty");
    g_audioModeActive = true;
    CHECK(getAudioThreadMigrationPenalty("AudioTrack") == 200, "mode on + audio thread -> 200");
    CHECK(getAudioThreadMigrationPenalty("RenderThread") == 0, "mode on + non-audio -> 0");

    printf("=== Audio: detectAudioPlayback on host (no audioserver) ===\n");
    bool ap = detectAudioPlayback();
    CHECK(ap == false, "no audioserver process on host -> false");
    CHECK(g_audioModeActive == false, "mode stays inactive");

    printf("=== Audio: C-006 active-track detection (conservative) ===\n");
    CHECK(aatpeAudioDumpShowsActiveTrack("") == false, "C-006: empty dump -> no active track");
    CHECK(aatpeAudioDumpShowsActiveTrack("AudioFlinger pid: 1234\nMixer threads: 1\n") == false,
          "C-006: idle server dump (no started track) -> no active track");
    CHECK(aatpeAudioDumpShowsActiveTrack(
              "AudioTrack: 0xabc\n  state: started\n  format: pcm\n") == true,
          "C-006: dump with started AudioTrack -> active track");
    CHECK(aatpeAudioDumpShowsActiveTrack(
              "Playback Threads:\n  tid: 456\n  State: started\n") == true,
          "C-006: 'State: started' marker -> active track");

    printf("\n========================================\n"
           "Audio results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
