// fz_threadrules.cpp - R-W3 strengthened: fuzz loadThreadRules() right-anchor
// parsing with semantic assertions. We write mutated rules content, call the
// production loadThreadRules(), then compare g_rules against a reference parser
// that mirrors the production split/right-anchor logic.
//
// INVARIANTS:
//   - g_rules.size() == reference rule count
//   - For each rule: procPattern/threadPattern/subtype/role/priority match
//   - <5-token lines skipped (not counted)
//   - Comments (#) and empty lines skipped
//   - 6-token lines: middle tokens concatenated into threadPattern
//   - Bad priority -> default priority
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <shared_mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include "aatped.h"
#include "aatped_utils.h"
#include "fuzz_engine.h"

extern const char* CONF_DIR;
// g_rulesMutex is static in thread_scan.cpp; in the single-threaded fuzzer
// we don't need to lock (no concurrent writer after loadThreadRules() returns).

// Mirror the trim() used by loadThreadRules.
static void refTrim(std::string& s) {
  while (!s.empty() && (s.back()==' '||s.back()=='\t'||s.back()=='\r'||s.back()=='\n'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n')) i++;
  s.erase(0, i);
}

struct RefRule {
  std::string procPattern, threadPattern, subtype;
  ThreadRole role;
  int priority;
};

static ThreadRole refRole(const std::string& s) {
  if (s == "S0") return ROLE_S0_SYSTEM;
  if (s == "S1") return ROLE_S1_UI;
  if (s == "S2") return ROLE_S2_RENDER;
  if (s == "S3") return ROLE_S3_GAME;
  if (s == "S4") return ROLE_S4_THROUGHPUT;
  return ROLE_S5_BACKGROUND;
}

static std::vector<RefRule> refParse(const std::string& content) {
  std::vector<RefRule> out;
  std::istringstream iss(content);
  std::string line;
  while (std::getline(iss, line)) {
    refTrim(line);
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> toks;
    size_t pos = 0;
    while (true) {
      size_t c = line.find(':', pos);
      if (c == std::string::npos) { toks.push_back(line.substr(pos)); break; }
      toks.push_back(line.substr(pos, c - pos));
      pos = c + 1;
    }
    if (toks.size() < 5) continue;
    RefRule r;
    r.procPattern = toks[0];
    r.subtype = toks[toks.size()-2];
    std::string prioStr = toks[toks.size()-1];
    std::string roleStr = toks[toks.size()-3];
    r.threadPattern.clear();
    for (size_t i = 1; i + 3 < toks.size(); ++i) r.threadPattern += toks[i];
    try { r.priority = std::stoi(prioStr); }
    catch (...) { r.priority = aatpe::thread_classify::kDefaultPriority; }
    r.role = refRole(roleStr);
    refTrim(r.procPattern);
    refTrim(r.threadPattern);
    out.push_back(r);
  }
  return out;
}

static std::atomic<uint64_t> g_semanticFails{0};

int main(int argc, char** argv) {
  const char* crashDir = argc > 1 ? argv[1] : "crash";
  const char* logTag   = argc > 2 ? argv[2] : "fz_threadrules";
  fz::Options opt;
  opt.name = logTag;
  opt.crashDir = crashDir;
  opt.minIter = 20000000ULL;
  opt.minSeconds = 900.0;
  opt.seed = 0xC0FFEE ^ 0x43;
  opt.maxLen = 4096;
  opt.seeds = {
    "*:binder:*:S0:BINDER:95\n",
    "com.game:RenderThread:S2:RENDER:95\n",
    "*:*:S1:UI:-10\n",
    "proc:thread:role:subtype:99999999999999999999\n",
    "# comment line\n",
    "a:b:c:d:e:f:g:h\n",
    "only:two\n",
    ":::S3:GAME:0\n",
    "*:binder*:S0:BINDER:95\n",
  };
  std::string dir = std::string(CONF_DIR) + "/rules";
  mkdir(CONF_DIR, 0755);
  mkdir(dir.c_str(), 0755);
  std::string path = dir + "/thread_roles.conf";

  fz::run(opt, [&](const std::string& in) {
    {
      std::ofstream f(path, std::ios::trunc|std::ios::binary);
      f << in;
    }
    loadThreadRules();
    auto expected = refParse(in);
    // Read g_rules directly (single-threaded fuzzer, no concurrent writer).
    bool mismatch = false;
    std::string reason;
    if (g_rules.size() != expected.size()) {
      mismatch = true;
      reason = "size mismatch";
    } else {
      for (size_t i = 0; i < g_rules.size(); ++i) {
        const auto& a = g_rules[i];
        const auto& b = expected[i];
        if (a.procPattern != b.procPattern ||
            a.threadPattern != b.threadPattern ||
            a.subtype != b.subtype ||
            a.role != b.role ||
            a.priority != b.priority) {
          mismatch = true;
          reason = "field mismatch at rule " + std::to_string(i);
          break;
        }
      }
    }
    if (mismatch) {
      uint64_t n = ++g_semanticFails;
      char fp[320];
      snprintf(fp, sizeof(fp), "%s/SEMFAIL_threadrules_%u.bin",
               crashDir, (unsigned)n);
      std::ofstream f(fp, std::ios::trunc|std::ios::binary);
      f << in;
      fprintf(stderr,
        "[SEMFAIL #%llu] threadrules parse mismatch: %s\n"
        "  got_rules=%zu expected=%zu\n  input=%s\n  dumped to %s\n",
        (unsigned long long)n, reason.c_str(),
        g_rules.size(), expected.size(), in.c_str(), fp);
      fflush(stderr);
      abort();
    }
  });
  fprintf(stderr, "[%s] DONE: semantic_fails=%llu\n",
          logTag, (unsigned long long)g_semanticFails.load());
  return 0;
}
