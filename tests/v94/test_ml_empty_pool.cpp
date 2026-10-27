// test_ml_empty_pool.cpp — rel15 #97 regression tests.
//
// On the audit device the topology is 1 PRIME + 3 BIG + 4 LITTLE with NO MID tier:
//   LITTLE=cpu0-3, MID=(empty), BIG=cpu4-6, PRIME=cpu7.
// The ML model reserves a fixed slot per tier, so pool slot 1 (MID) exists but has
// ZERO CPUs and therefore ZERO samples. The old isMature() required EVERY pool slot
// to hold >20 samples, so the empty MID pool (sample_count==0) dragged global
// maturity to false FOREVER and the online-learning page read "就绪度 0%" even
// though LITTLE/BIG/PRIME were fully trained.
//
// These tests pin the fix:
//   * presence is derived from the live cpu->pool assignment (never hardcoded);
//   * an absent tier (no mapped CPU) is NOT in the maturity denominator;
//   * isPoolPresent() reports the absent tier separately ("no such tier").
//
// Links the real aatpe_ml.o + ml_deps stub (same as v343_ml / f_v434_ml).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "aatpe_ml.h"
#include "aatped_types.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s  (line %d)\n", m, __LINE__); } \
} while(0)

static std::string makeTempPath(const char* prefix) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/%s_%d_XXXXXX", prefix, (int)getpid());
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    return std::string(tmpl);
}

int main() {
    // Mirror the audit device: 8 cpus, 4 tier slots, MID (slot 1) EMPTY.
    //   slot0 LITTLE <- cpus 0,1,2,3
    //   slot1 MID    <- (no cpu maps here)
    //   slot2 BIG    <- cpus 4,5,6
    //   slot3 PRIME  <- cpu 7
    printf("=== #97: empty MID pool is NOT in the maturity denominator ===\n");
    {
        LearningModel m;
        m.init(4);
        m.setEnabled(true);

        ThreadFeatures f = LearningModel::extractFeatures(0, 0, 50, 0, -20, false, 0, 512, true, 0);
        // Train the three REAL clusters well past the >20-sample gate.
        for (int i = 0; i < 120; i++) {
            m.update(0, f, 0.7f);   // LITTLE
            m.update(2, f, 0.6f);   // BIG
            m.update(3, f, 0.8f);   // PRIME
        }
        // slot1 (MID) intentionally gets zero samples.

        // Build the runtime cpu->pool assignment (MID slot 1 has no cpu).
        std::vector<int> asg(8, -1);
        for (int c = 0; c <= 3; c++) asg[c] = 0;   // LITTLE
        // slot1 left with no cpu -> MID absent
        for (int c = 4; c <= 6; c++) asg[c] = 2;   // BIG
        asg[7] = 3;                                // PRIME
        m.setCpuPoolAssignment(asg);

        // Presence semantics.
        CHECK(m.isPoolPresent(0) == true, "#97: LITTLE slot0 present");
        CHECK(m.isPoolPresent(1) == false, "#97: MID slot1 ABSENT (no mapped cpu)");
        CHECK(m.isPoolPresent(2) == true, "#97: BIG slot2 present");
        CHECK(m.isPoolPresent(3) == true, "#97: PRIME slot3 present");

        // Persist + reload so m_modelLoaded becomes true (the only path that sets it).
        std::string p = makeTempPath("aatpe_ml97");
        CHECK(m.save(p.c_str()) == true, "#97: save trained model ok");
        LearningModel m2;
        m2.init(4);
        m2.setEnabled(true);
        CHECK(m2.load(p.c_str()) == true, "#97: reload trained model ok");
        m2.setCpuPoolAssignment(asg);   // assignment is in-memory only; re-push it

        // THE core assertion: with a fully-trained LITTLE/BIG/PRIME and an empty MID,
        // the model is MATURE. Before the fix this returned false forever.
        CHECK(m2.isMature() == true,
              "#97: mature despite empty MID pool (empty pool not in denominator)");

        // Control: if we LIE and map a cpu onto the empty MID slot, it suddenly has 0
        // samples and correctly blocks maturity (proves the gate still works when a
        // real-but-untrained cluster exists).
        std::vector<int> asgFull = asg;
        asgFull[5] = 1;   // now cpu5 is "MID" -> slot1 is present but has 0 samples
        m2.setCpuPoolAssignment(asgFull);
        CHECK(m2.isMature() == false,
              "#97: a real but untrained cluster STILL blocks maturity (gate intact)");

        unlink(p.c_str());
    }

    // Default (no assignment synced yet): all slots treated as present -> a zero
    // slot still blocks maturity. This pins the boot-time safe default so we don't
    // accidentally call every pool "absent" before topology sync.
    printf("=== #97: no assignment synced -> all slots present (safe default) ===\n");
    {
        LearningModel m;
        m.init(4);
        m.setEnabled(true);
        CHECK(m.isPoolPresent(0) == true, "#97: unsynced slot0 present by default");
        CHECK(m.isPoolPresent(1) == true, "#97: unsynced slot1 present by default");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
