#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %d: %s\n", __LINE__, #cond);} } while(0)

static bool watchdogStale(const std::string& tf) {
    return tf.rfind("stale", 0) == 0;
}

static bool efficiencySoftDeprefer(const std::string& raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    float v = 0.0f;
    if (b != std::string::npos) v = strtof(raw.c_str() + b, nullptr);
    return std::isfinite(v) && v >= 0.5f;
}

int main() {
    CHECK(watchdogStale("stale=15") == true);
    CHECK(watchdogStale("stale=200") == true);
    CHECK(watchdogStale("stale") == true);
    CHECK(watchdogStale("stale_read") == true);
    CHECK(watchdogStale("fresh") == false);
    CHECK(watchdogStale("") == false);
    CHECK(watchdogStale("fresh=3") == false);
    auto oldExact = [](const std::string& tf){ return tf=="stale_read"||tf=="stale"; };
    CHECK(oldExact("stale=15") == false);

    CHECK(efficiencySoftDeprefer("0.700000") == true);
    CHECK(efficiencySoftDeprefer("0.5") == true);
    CHECK(efficiencySoftDeprefer("0.49") == false);
    CHECK(efficiencySoftDeprefer("0.0") == false);
    CHECK(efficiencySoftDeprefer("") == false);
    CHECK(efficiencySoftDeprefer("   ") == false);
    CHECK(efficiencySoftDeprefer("garbage") == false);

    unsigned oldEffective = 0;
    auto oldTouchSet    = [&](unsigned v){ oldEffective = v; };
    auto oldPrioritySet = [&](unsigned v){ oldEffective = v; };
    auto oldTouchRelease= [&](){ oldEffective = 0; };
    oldTouchSet(512);
    oldPrioritySet(1024);
    oldTouchRelease();
    CHECK(oldEffective == 0);

    unsigned src[3] = {0,0,0};
    auto eff = [&](){ unsigned m=0; for(unsigned v:src) m=std::max(m,v); return m; };
    auto setSrc = [&](int s, unsigned v){ src[s]=v; };
    auto clearSrc = [&](int s){ src[s]=0; };
    setSrc(0, 512);
    setSrc(2, 1024);
    CHECK(eff() == 1024);
    setSrc(0, 0);
    CHECK(eff() == 1024);
    setSrc(1, 800);
    CHECK(eff() == 1024);
    clearSrc(2);
    CHECK(eff() == 800);
    clearSrc(1);
    CHECK(eff() == 0);

    std::printf("audit4 unit: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
