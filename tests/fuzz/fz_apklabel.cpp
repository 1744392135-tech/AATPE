#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include "fuzz_engine.h"

#include "aatped_apklabel.cpp"
#include "v94/fixture_arrays_apklabel.h"

static std::string g_tmpDir = "/tmp/aatpe_fz_apk";

struct Fixture {
  const char* name;
  const unsigned char* data;
  size_t len;
  const char* expected;
};

static Fixture g_fixtures[] = {
  {"direct_stored",    kApk_direct_stored,    sizeof(kApk_direct_stored),    "造梦西游4"},
  {"direct_deflated",  kApk_direct_deflated,  sizeof(kApk_direct_deflated),  "造梦西游4"},
  {"ref_zhcn",         kApk_ref_zhcn,         sizeof(kApk_ref_zhcn),         "造梦西游4"},
  {"ref_default",      kApk_ref_default,      sizeof(kApk_ref_default),      "快手"},
  {"ref_density",      kApk_ref_density,      sizeof(kApk_ref_density),      "造梦西游4"},
  {"real_deflated",    kApk_real_deflated,    sizeof(kApk_real_deflated),    "造梦西游4"},
  {"netease_direct",    kApk_netease_direct,    sizeof(kApk_netease_direct),    "梦幻西游"},
  {"bad_truncated",    kApk_bad_truncated_manifest, sizeof(kApk_bad_truncated_manifest), nullptr},
  {"bad_garbage",      kApk_bad_garbage,      sizeof(kApk_bad_garbage),      nullptr},
  {"bad_notazip",      kApk_bad_notazip,      sizeof(kApk_bad_notazip),      nullptr},
  {"bad_ref_oob",      kApk_bad_ref_outofrange, sizeof(kApk_bad_ref_outofrange), nullptr},
  {"bad_framework",    kApk_bad_framework_ref, sizeof(kApk_bad_framework_ref), nullptr},
  {"bad_trunc_arsc",   kApk_bad_truncated_arsc, sizeof(kApk_bad_truncated_arsc), nullptr},
  {"bad_central_oob",  kApk_bad_central_oob,  sizeof(kApk_bad_central_oob),  nullptr},
  {"bad_central_bound",kApk_bad_central_boundary, sizeof(kApk_bad_central_boundary), nullptr},
  {"real_defl_trunc",  kApk_real_deflated_truncated, sizeof(kApk_real_deflated_truncated), nullptr},
  {"real_defl_corr",   kApk_real_deflated_corrupt, sizeof(kApk_real_deflated_corrupt), nullptr},
};

static int writeFixture(const Fixture& fx) {
  std::string path = g_tmpDir + "/" + fx.name + ".apk";
  std::ofstream f(path, std::ios::trunc | std::ios::binary);
  if (!f) return -1;
  f.write((const char*)fx.data, fx.len);
  return 0;
}

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_apklabel";
  mkdir("/tmp", 0755);
  mkdir(g_tmpDir.c_str(), 0755);

  uint64_t posFails = 0, negFails = 0;

  fprintf(stderr, "[%s] PHASE1: positive semantic fixtures\n", logTag);
  for (auto& fx : g_fixtures) {
    if (!fx.expected) continue;
    writeFixture(fx);
    std::string path = g_tmpDir + "/" + fx.name + ".apk";
    std::string err;
    std::string label = aatpe::apklabel::readLabelFromApk(path, &err);
    if (label != fx.expected) {
      fprintf(stderr, "[POSFAIL] %s: expected=\"%s\" got=\"%s\" err=\"%s\"\n",
              fx.name, fx.expected, label.c_str(), err.c_str());
      posFails++;
    } else {
      fprintf(stderr, "[POSOK]   %s -> \"%s\"\n", fx.name, label.c_str());
    }
  }

  fprintf(stderr, "[%s] PHASE1b: negative fixtures must return \"\"\n", logTag);
  for (auto& fx : g_fixtures) {
    if (fx.expected) continue;
    writeFixture(fx);
    std::string path = g_tmpDir + "/" + fx.name + ".apk";
    std::string err;
    std::string label = aatpe::apklabel::readLabelFromApk(path, &err);
    if (!label.empty()) {
      fprintf(stderr, "[NEGFIXFAIL] %s: expected empty got=\"%s\"\n",
              fx.name, label.c_str());
      negFails++;
    }
  }

  fprintf(stderr, "[%s] PHASE2: fuzz mutations\n", logTag);
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 900.0;
  opt.seed = 0xA4B1 ^ 0xAB1E;
  opt.maxLen = 65536;

  for (auto& fx : g_fixtures) {
    if (!fx.expected) continue;
    opt.seeds.emplace_back((const char*)fx.data, fx.len);
  }
  opt.seeds.push_back("");
  opt.seeds.push_back(std::string(1000, '\x50'));

  fz::run(opt, [&](const std::string& in) {
    std::string path = g_tmpDir + "/fuzz.apk";
    {
      std::ofstream f(path, std::ios::trunc | std::ios::binary);
      f.write(in.data(), in.size());
    }
    std::string err;
    std::string label = aatpe::apklabel::readLabelFromApk(path, &err);
    (void)label; (void)err;
  });

  fprintf(stderr,
    "[%s] DONE: posFails=%llu negFails=%llu\n",
    logTag, (unsigned long long)posFails, (unsigned long long)negFails);
  return posFails > 0 ? 1 : 0;
}
