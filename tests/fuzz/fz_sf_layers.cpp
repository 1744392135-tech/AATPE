// fz_sf_layers.cpp - fuzz aatpe::frameaware::sfListLayers (surface #1:
// `dumpsys SurfaceFlinger --list` layer-name list parsing).
#include <string>
#include <vector>
#include "aatped.h"
#include "fuzz_engine.h"

using namespace aatpe::frameaware;

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_sf_layers";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x11;
  opt.maxLen = 16384;
  opt.seeds = {
    "SurfaceView - com.tencent.mobileqq/com.tencent.mobileqq.activity.SplashActivity#0\nStatusBar\nNavigationBar\n",
    "com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport#0\n",
    "0xdeadbeef\nHexOnlyLayer\n",
    "##comment\n\n",
  };
  fz::run(opt, [](const std::string& in) {
    std::vector<std::string> layers = sfListLayers(in);
    (void)layers;
  });
  return 0;
}
