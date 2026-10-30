#include <string>
#include <vector>
#include "aatped.h"
#include "fuzz_engine.h"

using namespace aatpe::frameaware;

std::vector<double> parseGfxinfoFramestats(const std::string& out);

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_gfxinfo_framestats";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 1000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x19;
  opt.maxLen = 65536;
  opt.seeds = {
    "Stats since: 1234567890ms\nTotal frames rendered: 1234\nJanky frames: 42 (3.4%)\n50th percentile: 8ms\n90th percentile: 16ms\n95th percentile: 20ms\n99th percentile: 32ms\nNumber Missed Vsync: 10\nNumber High input latency: 2\nNumber Slow UI thread: 5\nNumber Slow bitmap uploads: 1\nNumber Slow draw: 4\n",
    "FrameStats:\n0,0,5000000,5166667,5166667,5166667,5166667,5166667,5166667,5166667,5166667,5166667,5166667\n",
    "Histograms:\nDelay: 5ms=100\n",
  };
  fz::run(opt, [](const std::string& in) {
    std::vector<double> d = parseGfxinfoFramestats(in);
    (void)d;
  });
  return 0;
}
