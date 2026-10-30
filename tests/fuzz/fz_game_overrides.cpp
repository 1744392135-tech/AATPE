#include <string>
#include <set>
#include "aatped.h"
#include "fuzz_engine.h"

using namespace aatpe::frameaware;

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_game_overrides";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 1500.0;
  opt.seed = 0xC0FFEE ^ 0x13;
  opt.maxLen = 8192;
  opt.seeds = {
    "com.tencent.tmgp.sgame\n# comment\n\norg.cocos2dx.zmxy.mi\n",
    "bad-pkg-name!\n123startswithdigit\ncom.foo.bar\n",
    "  spaced  pkg  \n",
  };
  fz::run(opt, [](const std::string& in) {
    std::set<std::string> out = parseGameOverridesContent(in);
    (void)out;
  });
  return 0;
}
