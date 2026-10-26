// fz_config.cpp - fuzz aatpe::configSchemaValidate (config schema validation,
// surface #5: config schema / key=value parsing). Input is split at the first '='
// into key and value so both fields are exercised under mutation.
#include <string>
#include <vector>
#include "aatpe_config_schema.h"
#include "fuzz_engine.h"

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_config_schema";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;   // light parser gate
  opt.minSeconds = 1500.0;     // wall-clock gate (~25 min)
  opt.seed = 0xC0FFEE ^ 0x1;
  opt.maxLen = 4096;
  // seeds: realistic config lines
  opt.seeds = {
    "prime_pool_cpu_list=0-3",
    "boost_duration_ms=1200",
    "jank_threshold_fps=45.5",
    "enable_gpu_boost=true",
    "unknown_key=xyz",
    "max_temp_degc=85",
    "cpu_freq_cap_khz=1800000",
  };
  fz::run(opt, [](const std::string& in) {
    std::string key = in, value;
    size_t eq = in.find('=');
    if (eq != std::string::npos) {
      key = in.substr(0, eq);
      value = in.substr(eq + 1);
    } else {
      // also try split at newline so a bare line is a key with empty value
      size_t nl = in.find('\n');
      if (nl != std::string::npos) key = in.substr(0, nl);
    }
    std::string clamped;
    bool ok = aatpe::configSchemaValidate(key, value, clamped);
    (void)ok;
    // invariant: a clamped value must be bounded; touch it so it is used
    if (!clamped.empty()) { volatile char c = clamped[0]; (void)c; }
  });
  return 0;
}
