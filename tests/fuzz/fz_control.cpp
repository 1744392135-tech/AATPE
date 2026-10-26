// fz_control.cpp - fuzz executeControlLine (surface #4: control command text -
// whitelist / length / '=' count / dispatcher). Must be fail-closed: unknown or
// malformed lines return success=false; must never crash or hang.
#include <string>
#include "aatped.h"
#include "aatped_control.h"
#include "fuzz_engine.h"

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_control";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 1000000ULL;    // heavy parser gate
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x31;
  opt.maxLen = 4096;
  opt.seeds = {
    "set_mode=performance",
    "set_thermal_limit_cpu=9999999999",
    "set_ml_hybrid=2",
    "boost_game=com.tencent.tmgp.sgame",
    "unknown_command_xyz",
    "set_mode=",
    "a=b=c=d",
  };
  fz::run(opt, [](const std::string& in) {
    ControlResult r = executeControlLine(in);
    (void)r;
  });
  return 0;
}
