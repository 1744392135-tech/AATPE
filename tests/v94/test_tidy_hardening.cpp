#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <thread>
#include "aatped_logging.h"

const char* RUN_DIR = "/tmp";
const char* LOG_FILE = "/tmp/aatpe_test.log";
int mkdirIfNotExist(const char*, unsigned int) { return 0; }

static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

int main() {
    printf("=== tidied aatpeErrStr (reentrant) ===\n");
    {
        const char* e = aatpeErrStr(ENOENT);
        CHECK(e != nullptr && strstr(e, "No such") != nullptr, "ENOENT message");
        const char* e2 = aatpeErrStr(EPERM);
        CHECK(strstr(e2, "Operation not permitted") != nullptr || strstr(e2, "Not permitted") != nullptr, "EPERM message");
        const char* e3 = aatpeErrStr(999999);
        CHECK(e3 != nullptr && e3[0] != '\0', "unknown errno fallback non-empty");
    }
    printf("=== tidied aatpeErrStr thread safety ===\n");
    {
        std::string acc[8];
        std::thread ts[8];
        for (int t = 0; t < 8; t++) {
            int ec = (t % 2) ? ENOENT : EPERM;
            ts[t] = std::thread([ec, t, &acc]{
                for (int i = 0; i < 2000; i++) acc[t] = aatpeErrStr(ec);
            });
        }
        for (auto& x : ts) x.join();
        bool ok = true;
        for (int t = 0; t < 8; t++)
            if (acc[t].empty()) ok = false;
        CHECK(ok, "8 threads x 2000 calls: every result usable");
    }
    printf("=== tidied rounding lround ===\n");
    {
        double vals[] = {59.4, 59.6, 120.0, 60.5, 33.3, 99.9};
        bool agree = true;
        for (double v : vals)
            if ((int)(v + 0.5) != (int)lround(v)) agree = false;
        CHECK(agree, "lround == (int)(x+0.5) for positive fps");
        CHECK((int)lround(60.5) == 61, "lround(60.5)=61 (away-from-zero half-up)");
    }
    printf("v94 tidy-hardening: %d ok, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
