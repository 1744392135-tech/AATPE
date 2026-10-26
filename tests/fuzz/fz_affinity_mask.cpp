// fz_affinity_mask.cpp - fuzz parseAffinityMask (surface #7: cpuset /
// cpu topology text: "0-3,7", hex bitmask "0f", singles, garbage).
#include <string>
#include <set>
#include "aatped.h"
#include "aatped_topology.h"
#include "fuzz_engine.h"

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_affinity_mask";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x7;
  opt.maxLen = 8192;
  opt.seeds = {
    "0-3", "0-7", "0,1,2,3", "0f", "ff", "0000000f", "0-3,7",
    "  ", "xyz!", "1-", "-5", "999999999999", "a-b-c", "7-0",
  };
  fz::run(opt, [](const std::string& in) {
    std::set<int> r = parseAffinityMask(in);
    (void)r;
  });
  return 0;
}
