#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "aatped.h"
#include "aatped_audio.h"
#include "aatped_state.h"

std::atomic<int> g_audioIsolatedCpu{4};
std::atomic<const char*> g_gpuType{"unknown"};

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) { printf("[PASS] %s\n", msg); } \
                              else { printf("[FAIL] %s\n", msg); ++failures; } } while (0)

int main() {
  g_audioIsolatedCpu.store(4, std::memory_order_relaxed);
  int cpu = g_audioIsolatedCpu.load(std::memory_order_relaxed);
  CHECK(cpu == 4, "g_audioIsolatedCpu atomic store/load round-trips to 4");

  g_audioIsolatedCpu.store(2, std::memory_order_relaxed);
  cpu = g_audioIsolatedCpu.load(std::memory_order_relaxed);
  CHECK(cpu == 2, "g_audioIsolatedCpu second store visible on load");

  const char* tag = "Mali-G715";
  g_gpuType.store(tag, std::memory_order_relaxed);
  const char* back = g_gpuType.load(std::memory_order_relaxed);
  CHECK(strcmp(back, "Mali-G715") == 0, "g_gpuType atomic<const char*> round-trips");

  g_gpuType.store("unknown", std::memory_order_relaxed);
  back = g_gpuType.load(std::memory_order_relaxed);
  CHECK(strcmp(back, "unknown") == 0, "g_gpuType re-store visible");

  if (failures == 0) { printf("a2_atomics: ALL PASS (4/4)\n"); return 0; }
  printf("a2_atomics: %d FAILED\n", failures);
  return 1;
}
