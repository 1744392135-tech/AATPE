#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include "aatpe_diag.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } } while (0)

static void mkdirp(const std::string& p) {
    std::string cur;
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') { cur = p.substr(0, i); mkdir(cur.c_str(), 0755); }
    }
}
static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream o(path, std::ios::binary); o << content; o.close();
}
bool atomicWriteFile(const char*, const std::string&) { return true; }

int main() {
    printf("=== EXT-PSI-1: pressure dir missing -> unavailable ===\n");
    {
        char tmp[] = "/tmp/extpsi_no_XXXXXX";
        std::string root = mkdtemp(tmp);
        mkdirp(root + "/proc");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiAvailable == false, "psiAvailable=false when /proc/pressure missing");
        CHECK(s.psiCpuSomePct == 0.0f && s.psiCpuSome60Pct == 0.0f && s.psiCpuSome300Pct == 0.0f,
              "cpu tiers all 0 when unavailable");
        CHECK(s.psiCpuTotalUsec == 0, "cpu total=0 when unavailable");
    }

    printf("\n=== EXT-PSI-2: garbage / malformed numbers ===\n");
    {
        char tmp[] = "/tmp/extpsi_garbage_XXXXXX";
        std::string root = mkdtemp(tmp);
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu",
                  "some avg10=abc avg60=-5x avg300=NaN total=notanumber\n");
        writeFile(root + "/proc/pressure/io", "\x01\x02 binary garbage\n");
        writeFile(root + "/proc/pressure/mem", "");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiAvailable == true, "cpu file present -> available=true");
        CHECK(s.psiCpuSomePct == 0.0f, "garbage avg10='abc' degrades to 0");
        printf("  [INFO] atof prefix parse: avg60=%.3f avg300=%g (P3: sanitize v4.2.3)\n",
               s.psiCpuSome60Pct, s.psiCpuSome300Pct);
        CHECK(s.psiCpuTotalUsec == 0, "garbage total -> 0");
    }

    printf("\n=== EXT-PSI-3: cpu missing avg60/avg300 tier ===\n");
    {
        char tmp[] = "/tmp/extpsi_partial_XXXXXX";
        std::string root = mkdtemp(tmp);
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu", "some avg10=4.25 total=777\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiCpuSomePct > 4.0f && s.psiCpuSomePct < 4.5f, "avg10=4.25 parsed");
        CHECK(s.psiCpuSome60Pct == 0.0f && s.psiCpuSome300Pct == 0.0f,
              "absent 60/300 tiers -> 0 (frontend shows --)");
        CHECK(s.psiCpuTotalUsec == 777, "total=777");
    }

    printf("\n=== EXT-PSI-4: io/mem full line does not leak into some ===\n");
    {
        char tmp[] = "/tmp/extpsi_full_XXXXXX";
        std::string root = mkdtemp(tmp);
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu", "some avg10=1.00 avg60=1.00 avg300=1.00 total=1\n");
        writeFile(root + "/proc/pressure/io",
                  "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
                  "full avg10=88.00 avg60=88.00 avg300=88.00 total=99999\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiIoSomePct == 0.0f, "io some=0 (full=88 must NOT leak into some)");
    }

    printf("\next_neg_psi: %s\n", g_fail == 0 ? "ALL PASS" : "HAS FAILURES");
    return g_fail == 0 ? 0 : 1;
}
