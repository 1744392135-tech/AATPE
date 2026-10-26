// fz_timestats.cpp - fuzz aatpe::frameaware::parseTimestatsBlock
// (surface #1: `dumpsys SurfaceFlinger --timestats -dump` block parsing).
// Heavier: line-by-line istringstream over a block; fgPkg selects the layer.
#include <string>
#include "aatped.h"
#include "fuzz_engine.h"

using namespace aatpe::frameaware;

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_timestats";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 1000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x23;
  opt.maxLen = 65536;
  opt.seeds = {
    "SurfaceFlinger timestats:\n  Layer 0 com.mygame/.MainActivity:\n    total frames: 600\n    janky frames: 12 (2.0%)\n    50th percentile: 8ms\n    90th percentile: 16ms\n    95th percentile: 20ms\n    99th percentile: 32ms\n    refresh period: 16.6ms\n",
    "  Layer 0 StatusBar:\n    total frames: 100\n",
    "no blocks here at all\n",
  };
  fz::run(opt, [](const std::string& in) {
    TimestatsResult r = parseTimestatsBlock(in, "com.mygame");
    (void)r;
  });
  return 0;
}
