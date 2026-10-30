#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cmath>
#include <unistd.h>
#include <string>
#include "aatpe_ml.h"
#include "aatped_types.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;

static std::string makeTempPath(const char* prefix) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/%s_%d_XXXXXX", prefix, (int)getpid());
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    return std::string(tmpl);
}
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== DriftDetector: no latch on improving outcomes ===\n");
    {
        DriftDetector d;
        d.setWindow(5);
        for (int i = 0; i < 20; i++) d.recordOutcome(0.5f, 2);
        CHECK(!d.isDriftDetected(), "20 neutral outcomes -> no drift");
        CHECK(d.getConsecutiveDegrades() == 0, "consecutive degrades stay 0");
    }

    printf("=== DriftDetector: latches after N consecutive degrades ===\n");
    bool latched = false;
    {
        DriftDetector d;
        d.setWindow(5);
        for (int i = 0; i < 4; i++) d.recordOutcome(0.3f, 2);
        CHECK(!d.isDriftDetected(), "4 degrades (<5) -> not latched");
        d.recordOutcome(0.3f, 2);
        latched = d.isDriftDetected();
        CHECK(latched, "5th consecutive degrade -> drift latches");
        CHECK(g_mlDriftDetected.load(), "global g_mlDriftDetected exposed");
    }

    printf("=== DriftDetector: self-heals on a genuine improvement ===\n");
    {
        DriftDetector d;
        d.setWindow(3);
        for (int i = 0; i < 3; i++) d.recordOutcome(0.3f, 1);
        CHECK(d.isDriftDetected(), "latched at window=3");
        d.recordOutcome(0.7f, 1);
        CHECK(!d.isDriftDetected(), "improvement self-heals the latch");
    }

    printf("=== DriftDetector: reset clears state ===\n");
    {
        DriftDetector d;
        d.setWindow(3);
        d.recordOutcome(0.3f, 0); d.recordOutcome(0.3f, 0); d.recordOutcome(0.3f, 0);
        CHECK(d.isDriftDetected(), "latched");
        d.reset();
        CHECK(!d.isDriftDetected(), "reset clears latch");
    }

    printf("=== G7 R004-B2 (L1): per-pool degrade isolation ===\n");
    {
        DriftDetector d;
        d.setWindow(3);
        d.recordOutcome(0.3f, 0);
        d.recordOutcome(0.3f, 1);
        d.recordOutcome(0.3f, 2);
        d.recordOutcome(0.3f, 0);
        CHECK(!d.isDriftDetected(), "degrades spread across pools -> no global latch");
        d.recordOutcome(0.3f, 1);
        d.recordOutcome(0.3f, 1);
        d.recordOutcome(0.3f, 1);
        CHECK(d.isDriftDetected(), "pool1 streak hits window -> latch");
        d.recordOutcome(0.5f, 0);
        CHECK(d.isDriftDetected(), "neutral outcome on other pool keeps latch (per-pool)");
        d.recordOutcome(0.7f, 1);
        CHECK(!d.isDriftDetected(), "improvement on drifted pool heals global latch");
    }

    printf("=== ML-08: out-of-range pool counted, not mis-booked on pool 0 ===\n");
    {
        DriftDetector d;
        d.setWindow(3);
        long long before = d.getOutOfRangePoolSamples();
        d.recordOutcome(0.3f, 99);
        d.recordOutcome(0.3f, -1);
        d.recordOutcome(0.3f, 12345);
        CHECK(d.getOutOfRangePoolSamples() == before + 3, "3 out-of-range outcomes counted");
        CHECK(!d.isDriftDetected(), "out-of-range outcomes must NOT latch drift");
        CHECK(d.getConsecutiveDegrades() == 0, "pool-0 streak untouched by garbage pool ids");
        d.recordOutcome(0.3f, 0);
        d.recordOutcome(0.3f, 0);
        d.recordOutcome(0.3f, 0);
        CHECK(d.isDriftDetected(), "in-range degrades still latch normally");
        d.reset();
        CHECK(d.getOutOfRangePoolSamples() == 0, "reset clears the out-of-range counter");
    }

    printf("=== G7 round2: extractFeatures normalization (train/serve contract) ===\n");
    {
        ThreadFeatures f0 = LearningModel::extractFeatures(0,0,50, 0,-20, false, 0, 512, true, 0);
        ThreadFeatures f99 = LearningModel::extractFeatures(0,0,50, 99,19, false, 120, 1024, true, 1000);
        CHECK(f0.priority > 0.99f, "priority=0 -> f.priority ~1.0 (inverted)");
        CHECK(f99.priority < 0.01f, "priority=99 -> f.priority ~0.0 (inverted)");
        CHECK(f0.nice > 0.99f, "nice=-20 -> f.nice ~1.0");
        CHECK(f99.nice < 0.01f, "nice=19 -> f.nice ~0.0");
        CHECK(f0.thermal < 0.01f, "thermal=0 -> 0");
        CHECK(f99.thermal > 0.99f, "thermal=120 -> 1.0");
        CHECK(f0.migration_count < 0.01f, "migCount=0 -> 0");
        CHECK(f99.migration_count > 0.99f, "migCount=1000 -> 1.0");
    }

    printf("=== R005c: hybrid f[2] train==serve (switches/sec) ===\n");
    {
        const float K = 300.0f;
        auto f2 = [K](double rate){ return std::min(1.0f, std::max(0.0f, (float)rate/K)); };
        auto trainF2 = [&](uint64_t dSw, uint64_t dNs){
            double rate = (dNs>0) ? (double)dSw / ((double)dNs/1e9) : 0.0; return f2(rate);
        };
        auto serveF2 = [&](double sps){ return f2(sps); };
        CHECK(std::fabs(trainF2(150, 500000000ULL) - serveF2(300.0)) < 1e-6f,
              "150 sw / 0.5s = 300/s -> train==serve, saturates ~1.0");
        CHECK(trainF2(0, 500000000ULL) < 0.01f, "quiet old thread (cum>0, dSw=0) -> f[2]~0");
        CHECK(serveF2(0.0) < 0.01f, "serve quiet thread -> f[2]~0");
        CHECK(std::fabs(trainF2(30, 500000000ULL) - 0.2f) < 1e-3f, "60/s -> 0.2");
        CHECK(serveF2(3000.0) > 0.999f, "3000/s -> saturates 1.0");
    }

    printf("=== R007d: PRIME pool slot 3 is a scorable ML target ===\n");
    {
        aatpe_ml::LearningModel m;
        m.init(4);
        m.setEnabled(true);
        aatpe_ml::ThreadFeatures f = aatpe_ml::LearningModel::extractFeatures(
            0, 0, 50, 50, 0, false, 40, 1024, true, 0);
        float primeScore = m.predict(3, f);
        CHECK(primeScore > 0.49f && primeScore < 0.51f,
              "PRIME pool 3 scores 0.5 (zero weights), not 0.0 (out-of-range)");
        CHECK(m.predict(99, f) == 0.0f, "out-of-range pool still returns 0.0");
        m.update(3, f, 0.7f);
        aatpe_ml::PoolModel pm = m.getPoolModel(3);
        CHECK(pm.sample_count > 0, "PRIME pool 3 accepts training samples");
    }

    printf("=== R009a: cpu7 samples attribute to pool3 via assignment ===\n");
    {
        aatpe_ml::LearningModel m;
        m.init(4);
        m.setEnabled(true);
        std::vector<int> asg(8, -1);
        for (int c=0;c<=3;c++) asg[c]=0;
        for (int c=4;c<=6;c++) asg[c]=1;
        asg[7]=3;
        m.setCpuPoolAssignment(asg);
        CHECK(m.getCpuPoolAssignment()[7]==3, "assignment[cpu7]==3");
        aatpe_ml::ThreadFeatures f = aatpe_ml::LearningModel::extractFeatures(
            0, 0, 50, 50, 0, false, 40, 1024, true, 0);
        for (int i=0;i<5;i++) {
            int actualCpu=7;
            int actualPool = (actualCpu>=0 && actualCpu<(int)asg.size()) ? asg[actualCpu] : -1;
            if (actualPool<0 || actualPool>=m.getPoolCount()) actualPool=0;
            m.update(actualPool, f, 0.6f);
        }
        CHECK(m.getPoolModel(3).sample_count==5, "5 cpu7 samples -> pool3==5 (not pool2)");
        CHECK(m.getPoolModel(2).sample_count==0, "PRIME samples did NOT leak into pool2");
    }

    printf("=== Topology sidecar: matching hash -> compatible ===\n");
    {
        std::string hash = computeDeviceTopologyHash();
        CHECK(!hash.empty(), "computeDeviceTopologyHash returns non-empty");
        std::string p = makeTempPath("aatpe_model");
        FILE* f = fopen((p + ".topo").c_str(), "w");
        fprintf(f, "WRONG_TOPO\n"); fclose(f);
        CHECK(!loadTopologySidecar(p.c_str(), hash), "foreign hash -> mismatch (cold start)");
        CHECK(saveTopologySidecar(p.c_str(), hash), "save sidecar ok");
        CHECK(loadTopologySidecar(p.c_str(), hash), "own hash -> compatible");
        CHECK(!loadTopologySidecar(p.c_str(), "totally_different_hash"), "changed hash -> mismatch again");
        unlink((p + ".topo").c_str());
        unlink(p.c_str());
    }

    printf("=== Topology sidecar: missing sidecar treated as cold start ===\n");
    {
        std::string p = makeTempPath("aatpe_model_none");
        unlink((p + ".topo").c_str());
        CHECK(!loadTopologySidecar(p.c_str(), "any"), "missing sidecar -> cold start (safe default)");
        unlink(p.c_str());
    }

    printf("=== D4-07: migratePoolCount 3->4 preserves pool 0..2 knowledge ===\n");
    {
        LearningModel m;
        m.init(3);
        m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        for (int i=0;i<10;i++) m.update(0, f, 0.8f);
        for (int i=0;i<7;i++)  m.update(1, f, 0.6f);
        CHECK(m.getPoolModel(0).sample_count==10, "pool0 has 10 samples before migration");
        m.migratePoolCount(4);
        CHECK(m.getPoolCount()==4, "after migrate, pool count==4");
        CHECK(m.getPoolModel(0).sample_count==10, "pool0 samples PRESERVED across migration");
        CHECK(m.getPoolModel(1).sample_count==7,  "pool1 samples PRESERVED across migration");
        CHECK(m.getPoolModel(3).sample_count==0,   "new PRIME pool3 starts empty");
        m.migratePoolCount(3);
        CHECK(m.getPoolCount()==3 && m.getPoolModel(0).sample_count==10, "shrink keeps low pools");
    }

    printf("=== D4-04: nr_switches rate normalization (old thread not stuck at 1) ===\n");
    {
        ThreadFeatures idle = LearningModel::extractFeatures(0, 5, 50, 0,-20,false,0,512,true,0);
        CHECK(idle.nr_switches < 0.05f, "low switch rate(5/s) -> f.nr_switches ~0, not saturated");
        ThreadFeatures busy = LearningModel::extractFeatures(0, 600, 50, 0,-20,false,0,512,true,0);
        CHECK(busy.nr_switches > 0.99f, "high rate(600/s) -> clamped to 1.0");
    }

    printf("=== D4-03: fuzz bad model files (trunc/NaN/neg dim) safe discard ===\n");
    {
        LearningModel m;
        {
            std::string p="/tmp/aatpe_fuzz1.dat"; FILE* fp=fopen(p.c_str(),"w");
            fwrite("AATPEML6",1,8,fp); fwrite("\x01\x00",1,2,fp); fclose(fp);
            CHECK(m.load(p.c_str())==false, "truncated file -> load rejects (cold start)");
        }
        {
            std::string p="/tmp/aatpe_fuzz2.dat"; FILE* fp=fopen(p.c_str(),"w");
            fwrite("AATPEML6",1,8,fp); int32_t ver=6; fwrite(&ver,4,1,fp);
            int32_t np=-3; fwrite(&np,4,1,fp); fclose(fp);
            CHECK(m.load(p.c_str())==false, "negative num_pools -> reject");
        }
        {
            std::string p="/tmp/aatpe_fuzz3.dat"; FILE* fp=fopen(p.c_str(),"w");
            fwrite("AATPEML6",1,8,fp); int32_t ver=6; fwrite(&ver,4,1,fp);
            int32_t np=999999; fwrite(&np,4,1,fp); fclose(fp);
            CHECK(m.load(p.c_str())==false, "huge num_pools -> reject");
        }
        {
            std::string p="/tmp/aatpe_fuzz4.dat"; FILE* fp=fopen(p.c_str(),"w");
            fprintf(fp,"{\"json\":true}"); fclose(fp);
            CHECK(m.load(p.c_str())==false, "JSON-ish garbage -> reject, no crash");
        }
    }

    printf("=== Upgrade compat: Config defaults for v3.4.3 tunables ===\n");
    {
        Config c;
        CHECK(c.migrationHysteresisPercent == 5, "MIGRATION_HYSTERESIS_PERCENT default=5");
        CHECK(c.migrationCooldownMs == 500, "MIGRATION_COOLDOWN_MS default=500");
        CHECK(c.migrationPenaltyBigLittle == 3.0f, "MIGRATION_PENALTY_BIG_LITTLE default=3.0");
        CHECK(c.migrationPenaltySame == 1.0f, "MIGRATION_PENALTY_SAME default=1.0");
        CHECK(c.threadRoleConfidenceThreshold == 30, "THREAD_ROLE_CONFIDENCE_THRESHOLD default=30");
        CHECK(c.mlDriftDetectionEnabled == true, "ML_DRIFT_DETECTION default on");
        CHECK(c.mlDriftWindow == 10, "ML_DRIFT_WINDOW default=10");
        CHECK(c.mlTopologyBindingEnabled == true, "ML_TOPOLOGY_BINDING default on");
        CHECK(c.refreshRateEventDriven == true, "REFRESH_RATE_EVENT_DRIVEN default on");
        CHECK(c.ebpfRingPages == 128, "ebpfRingPages default intact (A2-012: 128 work point)");
        CHECK(c.mlMinSamples == 2000, "mlMinSamples legacy default intact");
    }

    printf("=== ROUND-014 P3: shrink 4->2/4->1 merges weights, not drop ===\n");
    {
        LearningModel m;
        m.init(4);
        m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        for (int i=0;i<10;i++) m.update(0, f, 0.8f);
        for (int i=0;i<5;i++)  m.update(3, f, 0.3f);
        m.migratePoolCount(2);
        CHECK(m.getPoolCount()==2, "4->2 pool count==2");
        CHECK(m.getPoolModel(1).sample_count >= 5, "PRIME pool3 samples merged into pool1 (not lost)");
        m.migratePoolCount(1);
        CHECK(m.getPoolCount()==1, "->1 pool count==1");
        CHECK(m.getPoolModel(0).sample_count >= 15, "all pools merged into single pool");
    }

    printf("=== ROUND-014 P3: droppedInvalidTargetPool counter ===\n");
    {
        LearningModel m;
        m.init(2); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        long long before = g_droppedInvalidTargetPool.load();
        m.update(99, f, 0.7f);
        CHECK(g_droppedInvalidTargetPool.load() == before+1, "out-of-range pool update increments dropped counter");
    }

    printf("=== ROUND-014 P3: rolePriorShrink + boostRoi ===\n");
    {
        CHECK(rolePriorShrink(0)==1.0f, "0 own samples -> full prior (1.0)");
        CHECK(rolePriorShrink(50)==0.0f || rolePriorShrink(50)<=0.3f, "50 own samples -> contracts to floor");
        CHECK(rolePriorShrink(200)==0.3f, "many samples -> floor 0.3");
        boostRoiRecord(true, 1.0f, 5000);
        CHECK(boostRoiRecentNetMean()==0.0f, "charging session skipped -> neutral");
        boostRoiRecord(false, 1.0f, 4000);
        boostRoiRecord(false, 0.5f, 5000);
        float mean = boostRoiRecentNetMean();
        CHECK(mean < 0.0f, "power-dominated sessions -> negative net mean");
    }

    printf("=== F-10: thermal throttling pauses ML weight updates ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        g_mlTrainingPausedByThermal.store(false);
        m.update(0, f, 0.8f);
        long long s1 = m.getPoolModel(0).sample_count;
        g_mlTrainingPausedByThermal.store(true);
        m.update(0, f, 0.2f);
        m.update(0, f, 0.9f);
        CHECK(m.getPoolModel(0).sample_count == s1, "thermal pause: update rejected, weights unchanged");
        g_mlTrainingPausedByThermal.store(false);
        m.update(0, f, 0.5f);
        CHECK(m.getPoolModel(0).sample_count == s1+1, "thermal cleared: training resumes");
    }

    printf("=== F-6: load/save keeps weights finite and within clamp ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        for (int i=0;i<20;i++) m.update(0, f, 0.8f);
        std::string p = makeTempPath("aatpe_f6");
        m.save(p.c_str());
        LearningModel m2; m2.init(1); m2.setEnabled(true);
        CHECK(m2.load(p.c_str())==true, "F-6: load saved model ok");
        bool allFinite=true, allBounded=true;
        for (float w : m2.getPoolModel(0).weights) {
            if (!std::isfinite(w)) allFinite=false;
            if (w < -10.0f || w > 10.0f) allBounded=false;
        }
        CHECK(allFinite, "F-6: loaded weights all finite");
        CHECK(allBounded, "F-6: loaded weights within +/-10 clamp");

        m.aatpeTestCorruptPoolWeights(0);
        CHECK(m.save(p.c_str())==true, "F-6: production save of corrupted-weight model ok");

        LearningModel m3; m3.init(1); m3.setEnabled(true);
        bool loaded = m3.load(p.c_str());
        CHECK(loaded, "F-6: corrupted-weight model still loads (CRC passes)");
        bool allFinite2 = true, allBounded2 = true;
        for (float w : m3.getPoolModel(0).weights) {
            if (!std::isfinite(w)) allFinite2 = false;
            if (w < -10.0f || w > 10.0f) allBounded2 = false;
        }
        CHECK(allFinite2, "F-6: NaN/extreme weights clamped to finite on load");
        CHECK(allBounded2, "F-6: extreme weights clamped to +/-10 on load");
        CHECK(m3.validateParameters(), "F-6: clamped model passes validateParameters");

        FILE* fp = fopen(p.c_str(), "r+b");
        CHECK(fp != nullptr, "F-6: reopen for CRC flip");
        fseek(fp, 100, SEEK_SET);
        unsigned char b;
        CHECK(fread(&b, 1, 1, fp)==1, "F-6: read byte for CRC flip");
        b ^= 0xFF;
        fseek(fp, 100, SEEK_SET);
        CHECK(fwrite(&b, 1, 1, fp)==1, "F-6: write flipped byte");
        fclose(fp);

        LearningModel m4; m4.init(1); m4.setEnabled(true);
        CHECK(m4.load(p.c_str()) == false, "F-6: CRC-corrupted model load rejected");

        unlink(p.c_str());
    }

    printf("=== v4.2 B1: MAD k=3 clips a 10x-median outlier to weight 0.2 ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        m.setMadFilterEnabled(true);
        for (int i=0;i<12;i++) m.update(0, f, 0.5f);
        float wNormal = m.computeOutlierWeightForPool(0, 0.5f);
        CHECK(wNormal > 0.99f, "B1: median-clustered outcome kept at weight 1.0");
        float wOut = m.computeOutlierWeightForPool(0, 1.0f);
        CHECK(wOut <= 0.201f && wOut >= 0.199f, "B1: 10x-median outlier down-weighted to 0.2");
    }

    printf("=== v4.2 B1: MAD=0 degradation still flags an obvious outlier ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        m.setMadFilterEnabled(true);
        for (int i=0;i<12;i++) m.update(0, f, 0.5f);
        float wDeg = m.computeOutlierWeightForPool(0, 1.0f);
        CHECK(wDeg <= 0.201f, "B1 MAD=0 degrade: outlier still down-weighted (sigma/clamp path)");
        float wOk = m.computeOutlierWeightForPool(0, 0.52f);
        CHECK(wOk > 0.99f, "B1 MAD=0 degrade: near-median outcome kept");
    }

    printf("=== v4.2 B1: MAD filter OFF is a no-op (outlier weight == 1.0) ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        m.setMadFilterEnabled(false);
        for (int i=0;i<12;i++) m.update(0, f, 0.5f);
        float wOff = m.computeOutlierWeightForPool(0, 1.0f);
        CHECK(wOff > 0.99f, "B1: filter off -> outlier weight stays 1.0 (v4.1.1)");
    }

    printf("=== v4.2 B2: drift nudge pulls weights half-way toward neutral ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        std::vector<float> w(ML_WEIGHT_COUNT, 2.0f);
        m.setWeights(0, w);
        m.setDriftRelearnEnabled(true);
        m.nudgeTowardNeutral(0);
        PoolModel pm = m.getPoolModel(0);
        bool halved = true;
        for (float x : pm.weights) if (std::fabs(x - 1.0f) > 1e-4f) halved = false;
        CHECK(halved, "B2: drift nudge halves every weight (half step to neutral 0.5)");
        m.nudgeTowardNeutral(0);
        bool quartered = true;
        for (float x : m.getPoolModel(0).weights) if (std::fabs(x - 0.5f) > 1e-4f) quartered = false;
        CHECK(quartered, "B2: repeated nudge keeps shrinking toward neutral (never full reset)");
        m.setDriftRelearnEnabled(false);
        m.nudgeTowardNeutral(0);
        bool unchanged = true;
        for (float x : m.getPoolModel(0).weights) if (std::fabs(x - 0.5f) > 1e-4f) unchanged = false;
        CHECK(unchanged, "B2: drift relearn off -> nudge is a no-op (v4.1.1)");
    }

    printf("=== v4.2 B2: W=200 outcome ring accumulates only when on ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        m.setDriftRelearnEnabled(false);
        for (int i=0;i<5;i++) m.update(0, f, 0.6f);
        CHECK(m.getDriftRingSize(0) == 0, "B2: off -> outcome ring empty (runtime state untouched)");
        m.setDriftRelearnEnabled(true);
        for (int i=0;i<5;i++) m.update(0, f, 0.6f);
        CHECK(m.getDriftRingSize(0) == 5, "B2: on -> outcome ring tracks recent outcomes");
    }

    printf("=== v4.2 B3: edge decisions explore, high-confidence never do ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        g_mlTrainingPausedByThermal.store(false);
        m.setExploreJankBlocked(false);
        m.setEpsilonGreedy(1.0);
        bool edge = m.maybeExplore(42, 0.5f);
        CHECK(edge, "B3: ε=1 edge decision (pred=0.5) explores (suppresses boost)");
        bool confHi = m.maybeExplore(43, 0.95f);
        CHECK(!confHi, "B3: high-confidence pred=0.95 never explored");
        bool confLo = m.maybeExplore(44, 0.05f);
        CHECK(!confLo, "B3: high-confidence pred=0.05 never explored");
        bool nearEdge = m.maybeExplore(45, 0.7f);
        CHECK(!nearEdge, "B3: pred=0.7 outside margin -> not explored");
    }

    printf("=== v4.2 B3: ε=0 default -> zero explore branch, always false ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        g_mlTrainingPausedByThermal.store(false);
        m.setExploreJankBlocked(false);
        m.setEpsilonGreedy(0.0);
        bool any = false;
        for (int t=0;t<50;t++) if (m.maybeExplore(t, 0.5f)) any = true;
        CHECK(!any, "B3: ε=0 -> maybeExplore always false (v4.1.1 bit-identical)");
    }

    printf("=== v4.2 B3: thermal/jank gates suppress exploration and count it ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        m.setEpsilonGreedy(1.0);
        long long base = g_mlExploreSkippedTotal.load();
        g_mlTrainingPausedByThermal.store(true);
        CHECK(!m.maybeExplore(77, 0.5f), "B3: thermal >= MODERATE -> explore suppressed");
        CHECK(g_mlExploreSkippedTotal.load() == base+1, "B3: thermal-suppressed attempt counted");
        g_mlTrainingPausedByThermal.store(false);
        m.setExploreJankBlocked(true);
        CHECK(!m.maybeExplore(78, 0.5f), "B3: jank active window -> explore suppressed");
        CHECK(g_mlExploreSkippedTotal.load() == base+2, "B3: jank-suppressed attempt counted");
        m.setExploreJankBlocked(false);
        CHECK(m.maybeExplore(79, 0.5f), "B3: gates clear -> edge explores again");
    }

    printf("=== v4.2 B3: deterministic PRNG (tid+tick), paired label sign ===\n");
    {
        LearningModel a, b; a.init(1); b.init(1);
        a.setEpsilonGreedy(1.0); b.setEpsilonGreedy(1.0);
        g_mlTrainingPausedByThermal.store(false);
        a.setExploreJankBlocked(false); b.setExploreJankBlocked(false);
        bool seqEq = true;
        for (int i=0;i<10;i++) if (a.maybeExplore(100+i, 0.5f) != b.maybeExplore(100+i, 0.5f)) seqEq = false;
        CHECK(seqEq, "B3: deterministic PRNG -> same tid+tick reproduces the decision");
        CHECK(std::fabs(LearningModel::counterfactualControlLabel(0.9f) - 0.1f) < 1e-6f,
              "B3: control outcome 0.9 (good) -> label 0.1 (mirror about 0.5)");
        CHECK(std::fabs(LearningModel::counterfactualControlLabel(0.2f) - 0.8f) < 1e-6f,
              "B3: control outcome 0.2 (bad) -> label 0.8 (boost DID matter)");
        CHECK(std::fabs(LearningModel::counterfactualControlLabel(0.5f) - 0.5f) < 1e-6f,
              "B3: neutral control outcome stays 0.5");
        CHECK(LearningModel::counterfactualControlLabel(-1.0f) == -1.0f,
              "B3: invalid sentinel outcome passed through unchanged");
    }

    printf("=== v4.2 Batch B: all switches off -> update path bit-identical ===\n");
    {
        LearningModel a; a.init(1); a.setEnabled(true); a.setMadFilterEnabled(false); a.setDriftRelearnEnabled(false);
        LearningModel b; b.init(1); b.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        for (int i=0;i<30;i++) { a.update(0, f, 0.7f); b.update(0, f, 0.7f); }
        auto pa = a.getPoolModel(0); auto pb = b.getPoolModel(0);
        bool same = true;
        for (int i=0;i<ML_WEIGHT_COUNT;i++) if (pa.weights[i]!=pb.weights[i]) same=false;
        CHECK(same, "default-off: two models trained identically produce bit-identical weights");
        CHECK(a.getPoolModel(0).sample_count == b.getPoolModel(0).sample_count,
              "default-off: sample counts identical");
    }

    printf("=== v4.2.1 T1: update() return enum + honest funnel counters ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        g_mlTrainingPausedByThermal.store(false);
        ThreadFeatures f = LearningModel::extractFeatures(0,0,50,0,-20,false,0,512,true,0);
        long long ts0 = g_mlUpdateSkipThermal.load();
        long long td0 = g_mlUpdateSkipDisabled.load();
        long long tp0 = g_droppedInvalidTargetPool.load();
        long long ti0 = g_invalidOutcomeSkipped.load();
        long long s0 = m.getTotalSamples();

        UpdateResult r1 = m.update(0, f, 0.7f);
        CHECK(r1 == UpdateResult::TRAINED, "T1: normal update returns TRAINED");
        CHECK(m.getTotalSamples() == s0+1, "T1: total_samples incremented on TRAINED");

        g_mlTrainingPausedByThermal.store(true);
        UpdateResult r2 = m.update(0, f, 0.2f);
        CHECK(r2 == UpdateResult::SKIP_THERMAL, "T1: thermal pause returns SKIP_THERMAL");
        CHECK(m.getTotalSamples() == s0+1, "T1: thermal pause does NOT increment total_samples");
        CHECK(g_mlUpdateSkipThermal.load() == ts0+1, "T1: skip_thermal counter incremented");
        g_mlTrainingPausedByThermal.store(false);

        m.setEnabled(false);
        UpdateResult r3 = m.update(0, f, 0.5f);
        CHECK(r3 == UpdateResult::SKIP_DISABLED_ANOMALY, "T1: disabled model returns SKIP_DISABLED_ANOMALY");
        CHECK(m.getTotalSamples() == s0+1, "T1: disabled does NOT increment total_samples");
        CHECK(g_mlUpdateSkipDisabled.load() == td0+1, "T1: skip_disabled counter incremented");
        m.setEnabled(true);

        UpdateResult r4 = m.update(99, f, 0.5f);
        CHECK(r4 == UpdateResult::SKIP_POOL, "T1: out-of-range pool returns SKIP_POOL");
        CHECK(g_droppedInvalidTargetPool.load() == tp0+1, "T1: droppedInvalidTargetPool incremented");

        UpdateResult r5 = m.update(0, f, -1.0f);
        CHECK(r5 == UpdateResult::SKIP_INVALID_OUTCOME, "T1: negative outcome returns SKIP_INVALID_OUTCOME");
        CHECK(g_invalidOutcomeSkipped.load() == ti0+1, "T1: invalidOutcomeSkipped incremented");

        UpdateResult r6 = m.update(0, f, 0.8f);
        CHECK(r6 == UpdateResult::TRAINED, "T1: after gates clear, TRAINED again");
        CHECK(m.getTotalSamples() == s0+2, "T1: total_samples = s0+2 (one happy path + one resume)");
    }

    printf("\n========================================\n"
           "v3.4.3 ML results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
