// fz_pm_packages.cpp - fuzz aatpe::frameaware::parsePmPackageList
// (surface #5: `pm list packages` output "package:com.foo").
#include <string>
#include <vector>
#include "aatped.h"
#include "fuzz_engine.h"

using namespace aatpe::frameaware;

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_pm_packages";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x17;
  opt.maxLen = 16384;
  opt.seeds = {
    "package:com.vmos.pro\npackage:com.tencent.mobileqq\npackage:org.cocos2dx.zmxy.mi\n",
    "package:com.foo\npackage:\npackage:\n",
    "pm list packages\nno prefix line\n",
  };
  fz::run(opt, [](const std::string& in) {
    std::vector<std::string> out = parsePmPackageList(in);
    (void)out;
  });
  return 0;
}
