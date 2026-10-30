#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <cstring>
#include "aatpe_ml_hybrid.h"

using namespace aatpe_ml_hybrid;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    const char* path = "/tmp/aatpe_hybrid_model.bin";
    printf("=== HybridPersist: save before init fails ===\n");
    HybridLearner L;
    CHECK(L.save(path) == false, "save on uninitialized learner -> false");

    printf("=== HybridPersist: save/load round-trip ===\n");
    L.init(3); L.setEnabled(true);
    std::vector<uint64_t> cpuRt(8, 0);
    uint64_t runNs = 1000, ts = 1000;
    for (int i = 0; i < 55; i++) {
        runNs += 1000; ts += 15000000;
        ThreadCpuSnapshot s; s.tid = 5; s.cpu = 0; s.pool = 0;
        s.runTimeNs = runNs; s.timestamp = ts; s.priority = 50; s.nice = 0; s.role = 3;
        cpuRt[0] = runNs;
        L.tick({s}, cpuRt, 8);
    }
    HybridPoolDelta before = L.getPoolDelta(0);
    CHECK(L.save(path) == true, "save() writes CRC32+atomic file");

    HybridLearner L2;
    L2.init(3);
    CHECK(L2.load(path) == true, "load() reads the saved file");
    HybridPoolDelta after = L2.getPoolDelta(0);
    CHECK(after.updateCount == before.updateCount, "loaded updateCount matches");
    CHECK(std::fabs(after.weights[1] - before.weights[1]) < 1e-5f,
          "loaded weight[1] round-trips");

    printf("=== HybridPersist: load missing file fails ===\n");
    CHECK(L2.load("/tmp/aatpe_hybrid_does_not_exist.bin") == false, "missing file -> false");

    printf("=== HybridPersist: load rejects bad magic ===\n");
    FILE* fp = fopen("/tmp/aatpe_hybrid_bad.bin", "wb");
    uint32_t badMagic = 0xDEADBEEF; fwrite(&badMagic, 4, 1, fp); fclose(fp);
    CHECK(L2.load("/tmp/aatpe_hybrid_bad.bin") == false, "bad magic -> rejected");

    printf("=== HybridPersist: old v2 weights rejected (feature layout changed) ===\n");
    FILE* v2 = fopen("/tmp/aatpe_hybrid_oldv2.bin", "wb");
    uint32_t magic = 0x4859424D, ver = 2;
    fwrite(&magic, 4, 1, v2); fwrite(&ver, 4, 1, v2); fclose(v2);
    CHECK(L2.load("/tmp/aatpe_hybrid_oldv2.bin") == false,
          "v2 (old distribution) -> rejected, cold start");

    printf("=== HybridPersist: load rejects CRC corruption ===\n");
    std::string goodPath = path;
    FILE* g = fopen(goodPath.c_str(), "rb");
    fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
    std::vector<unsigned char> buf(sz); fread(buf.data(), 1, sz, g); fclose(g);
    buf[8] ^= 0xFF;
    FILE* w = fopen("/tmp/aatpe_hybrid_corrupt.bin", "wb");
    fwrite(buf.data(), 1, sz, w); fclose(w);
    HybridLearner L3; L3.init(3);
    CHECK(L3.load("/tmp/aatpe_hybrid_corrupt.bin") == false, "CRC mismatch -> rejected");

    printf("=== HybridPersist G7-002: truncated CRC trailer rejected (fail-closed) ===\n");
    for (int cut = 1; cut <= 4; cut++) {
        std::string tpath = "/tmp/aatpe_hybrid_trunc" + std::to_string(cut) + ".bin";
        FILE* tw = fopen(tpath.c_str(), "wb");
        fwrite(buf.data(), 1, (size_t)(sz - cut), tw);
        fclose(tw);
        HybridLearner Lt; Lt.init(3);
        CHECK(Lt.load(tpath.c_str()) == false,
              ("truncated CRC by " + std::to_string(cut) + " byte(s) -> rejected").c_str());
    }
    HybridLearner Lg; Lg.init(3);
    CHECK(Lg.load(path) == true, "full untouched file still loads");

    printf("=== HybridPersist ML-07: topology-hash mismatch rejected ===\n");
    {
        FILE* g = fopen(path, "rb");
        fseek(g, 0, SEEK_END); long sz = ftell(g); fseek(g, 0, SEEK_SET);
        std::vector<unsigned char> buf(sz);
        fread(buf.data(), 1, sz, g); fclose(g);
        for (int i = 0; i < 8; i++) buf[16 + i] ^= 0xFF;
        long dataEnd = sz - 4;
        uint32_t crc = 0xFFFFFFFF;
        for (long i = 0; i < dataEnd; i++) {
            crc ^= buf[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
        crc ^= 0xFFFFFFFF;
        memcpy(buf.data() + dataEnd, &crc, sizeof(crc));
        const char* badTopoPath = "/tmp/aatpe_hybrid_badtopo.bin";
        FILE* w = fopen(badTopoPath, "wb");
        fwrite(buf.data(), 1, sz, w); fclose(w);
        HybridLearner Lt; Lt.init(3);
        CHECK(Lt.load(badTopoPath) == false,
              "topoHash flipped + CRC recomputed -> rejected (topology mismatch)");
    }

    printf("=== HybridPersist ML-07: v4 file rejected (missing topology hash) ===\n");
    {
        FILE* v4 = fopen("/tmp/aatpe_hybrid_v4.bin", "wb");
        uint32_t magic = 0x4859424D, ver = 4;
        fwrite(&magic, 4, 1, v4); fwrite(&ver, 4, 1, v4); fclose(v4);
        HybridLearner Lv4; Lv4.init(3);
        CHECK(Lv4.load("/tmp/aatpe_hybrid_v4.bin") == false,
              "v4 (pre-topology-hash) -> rejected, cold start");
    }

    printf("\n========================================\n"
           "HybridPersist results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
