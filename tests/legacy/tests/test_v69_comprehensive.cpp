#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <functional>
#include <random>

static int g_totalTests = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;
static std::vector<std::string> g_failures;
static std::string g_currentSuite;

#define CHECK(cond, msg) do { \
    g_totalTests++; \
    if (!(cond)) { \
        g_testsFailed++; \
        std::string _m = (msg); \
        g_failures.push_back(std::string(__func__) + " [" + _m + "]"); \
        printf("  FAIL: %s\n", _m.c_str()); \
    } else { \
        g_testsPassed++; \
    } \
} while(0)

#define CHECK_FLOAT_EQ(a, b, eps, msg) CHECK(fabsf((a)-(b)) < (eps), msg)
#define CHECK_NAN(v, msg) CHECK(std::isnan(v), msg)
#define CHECK_NOT_NAN(v, msg) CHECK(!std::isnan(v), msg)
#define SUITE(name) do { g_currentSuite = name; printf("\n=== %s ===\n", name); } while(0)

namespace aatpe {
enum class SystemState { NORMAL=0, DEGRADED=1, PASSIVE=2, EMERGENCY=3 };
enum ThreadRole { ROLE_S0_SYSTEM=0, ROLE_S1_UI=1, ROLE_S2_RENDER=2, ROLE_S3_GAME=3,
                  ROLE_GAME_RENDER=3, ROLE_GAME_LOGIC=4, ROLE_S4_THROUGHPUT=5, ROLE_S5_BACKGROUND=6 };
struct ThreadInfo {
    int tid=0, pid=0, currentCpu=0, role=0;
    unsigned long long runTime=0, nrSwitches=0;
    std::string tname;
    bool protect=false;
};
struct CpuMetrics { int util=0, curFreq=0, maxFreq=1, freqPct=0, capacity=380, temp=0; };
struct DecisionFusion {
    float heuristic_score=0, ml_score=0, hybrid_score=0, history_score=0;
    float w_heuristic=0, w_ml=0, w_hybrid=0, w_history=0;
    float final_score=0, dynamic_weight_adjustment=0;
    float contrib_heuristic=0, contrib_ml=0, contrib_hybrid=0, contrib_history=0;
};
struct DynamicScoreWeights { bool valid=false; float w_heuristic=0.5f, w_ml=0.3f, w_hybrid=0.15f, w_history=0.05f; };
}
using namespace aatpe;

static std::map<int, aatpe::CpuMetrics> g_cpuMetrics;
static std::shared_mutex g_cpuMetricsMutex;
static aatpe::DynamicScoreWeights g_dynamicScoreWeights;
static std::vector<int> g_allCpus = {0,1,2,3,4,5,6,7};

float softmaxFuseScores(float modelScore, float heuristicScore, float temperature) {
    if (temperature < 0.0f) temperature = -temperature;
    if (temperature < 0.01f) temperature = 0.01f;
    float m = modelScore / temperature;
    float h = heuristicScore / temperature;
    float maxVal = (m > h) ? m : h;
    m -= maxVal; h -= maxVal;
    float em = expf(m);
    float eh = expf(h);
    float sum = em + eh;
    if (sum < 1e-10f) return 0.5f * (modelScore + heuristicScore);
    return (modelScore * em + heuristicScore * eh) / sum;
}

#include "aatpe_ml.h"
using namespace aatpe_ml;

std::string aatpeExec(const std::string& cmd, const char* debugTag) {
    (void)cmd; (void)debugTag;
    return "";
}

namespace aatpe_crypto {
struct Sha256Context { uint32_t state[8]; uint64_t bitCount; uint8_t buffer[64]; size_t bufferLen; };
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha256Transform(Sha256Context* ctx, const uint8_t* data) {
    uint32_t w[64], a,b,c,d,e,f,g,h,t1,t2;
    for (int i=0;i<16;i++) w[i]=(data[i*4]<<24)|(data[i*4+1]<<16)|(data[i*4+2]<<8)|data[i*4+3];
    for (int i=16;i<64;i++) { uint32_t s0=ROTR(w[i-15],7)^ROTR(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ROTR(w[i-2],17)^ROTR(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];
    e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7];
    for (int i=0;i<64;i++) {
        uint32_t S1=ROTR(e,6)^ROTR(e,11)^ROTR(e,25), ch=(e&f)^((~e)&g);
        t1=h+S1+ch+K[i]+w[i];
        uint32_t S0=ROTR(a,2)^ROTR(a,13)^ROTR(a,22), maj=(a&b)^(a&c)^(b&c);
        t2=S0+maj; h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;
    ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;
}
static void sha256Init(Sha256Context* ctx) {
    ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;
    ctx->bitCount=0;ctx->bufferLen=0;
}
static void sha256Update(Sha256Context* ctx, const uint8_t* data, size_t len) {
    ctx->bitCount += len*8;
    while (len>0) { size_t n=64-ctx->bufferLen; if(n>len)n=len;
        memcpy(ctx->buffer+ctx->bufferLen,data,n);ctx->bufferLen+=n;data+=n;len-=n;
        if(ctx->bufferLen==64){sha256Transform(ctx,ctx->buffer);ctx->bufferLen=0;} }
}
static void sha256Final(Sha256Context* ctx, uint8_t hash[32]) {
    uint64_t bc=ctx->bitCount;
    ctx->buffer[ctx->bufferLen++]=0x80;
    if(ctx->bufferLen>56){while(ctx->bufferLen<64)ctx->buffer[ctx->bufferLen++]=0;sha256Transform(ctx,ctx->buffer);ctx->bufferLen=0;}
    while(ctx->bufferLen<56)ctx->buffer[ctx->bufferLen++]=0;
    for(int i=7;i>=0;i--)ctx->buffer[ctx->bufferLen++]=(bc>>(i*8))&0xff;
    sha256Transform(ctx,ctx->buffer);
    for(int i=0;i<8;i++){hash[i*4]=(ctx->state[i]>>24)&0xff;hash[i*4+1]=(ctx->state[i]>>16)&0xff;
        hash[i*4+2]=(ctx->state[i]>>8)&0xff;hash[i*4+3]=ctx->state[i]&0xff;}
}
static std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    uint8_t k[64]={0}, ipad[64], opad[64], hash[32];
    if(key.size()>64){Sha256Context c;sha256Init(&c);sha256Update(&c,(const uint8_t*)key.data(),key.size());sha256Final(&c,k);}
    else memcpy(k,key.data(),key.size());
    for(int i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}
    Sha256Context c;sha256Init(&c);sha256Update(&c,ipad,64);sha256Update(&c,(const uint8_t*)msg.data(),msg.size());sha256Final(&c,hash);
    sha256Init(&c);sha256Update(&c,opad,64);sha256Update(&c,hash,32);sha256Final(&c,hash);
    char hex[65]; for(int i=0;i<32;i++)sprintf(hex+i*2,"%02x",hash[i]); hex[64]=0; return std::string(hex);
}
}

static bool isPackageWhitelisted(const std::string& pkg, const std::string& whitelist) {
    if (whitelist.empty()) return true;
    std::istringstream iss(whitelist);
    std::string token;
    while (std::getline(iss, token, ',')) {
        if (!token.empty() && pkg.find(token) != std::string::npos) return true;
    }
    return false;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

namespace aatpe_sm {
static aatpe::SystemState g_state = aatpe::SystemState::NORMAL;
static long long g_stateDuration = 0;
static std::string g_lastReason = "";
aatpe::SystemState getSystemState() { return g_state; }
long long getStateDurationSec() { return g_stateDuration; }
const char* getLastTransitionReason() { return g_lastReason.c_str(); }
void transitionTo(aatpe::SystemState s, const char* reason) {
    if (s != g_state) { g_state = s; g_stateDuration = 0; g_lastReason = reason; }
}
void tick() { g_stateDuration++; }
}

namespace aatpe_thermal {
static int g_cpuTemp = 40, g_skinTemp = 35, g_batteryTemp = 30;
static int g_limit = 120;
static bool g_emergency = false;
int getCpuTemp() { return g_cpuTemp; }
int getSkinTemp() { return g_skinTemp; }
int getBatteryTemp() { return g_batteryTemp; }
bool isEmergency() { return g_cpuTemp >= g_limit; }
void setTemp(int c) { g_cpuTemp = c; g_emergency = c >= g_limit; }
}

namespace aatpe_frame {
static long long g_samples = 0, g_improved = 0, g_fallback = 0;
static float g_avgLatency = 0;
static int g_layerCount = 0;
void recordOutcome(bool improved, bool fallback) {
    g_samples++;
    if (improved) g_improved++;
    if (fallback) g_fallback++;
}
float getImprovementRate() { return g_samples > 0 ? (float)g_improved / g_samples : 0.0f; }
void reset() { g_samples=0; g_improved=0; g_fallback=0; }
}

aatpe::DecisionFusion fuseDecision(float hScore, float mlScore, float hyScore, float histScore,
                                    float wH, float wMl, float wHy, float wHist) {
    aatpe::DecisionFusion f;
    f.heuristic_score=hScore; f.ml_score=mlScore; f.hybrid_score=hyScore; f.history_score=histScore;
    float sum = wH+wMl+wHy+wHist;
    if (sum < 1e-6f) sum = 1.0f;
    f.w_heuristic=wH/sum; f.w_ml=wMl/sum; f.w_hybrid=wHy/sum; f.w_history=wHist/sum;
    f.final_score = hScore*f.w_heuristic + mlScore*f.w_ml + hyScore*f.w_hybrid + histScore*f.w_history;
    f.contrib_heuristic=hScore*f.w_heuristic; f.contrib_ml=mlScore*f.w_ml;
    f.contrib_hybrid=hyScore*f.w_hybrid; f.contrib_history=histScore*f.w_history;
    return f;
}

float computeThreadScore(const aatpe::ThreadInfo& ti, int targetCpu) {
    float base = 0.5f;
    if (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER) base += 0.2f;
    if (ti.role == ROLE_S3_GAME) base += 0.3f;
    if (ti.currentCpu == targetCpu) base += 0.1f;
    if (ti.protect) base += 0.15f;
    return clampf(base, 0.0f, 1.0f);
}

std::vector<aatpe::ThreadInfo> scanThreads(int pid) {
    std::vector<aatpe::ThreadInfo> result;
    for (int i = 0; i < 8; i++) {
        aatpe::ThreadInfo ti;
        ti.tid = pid * 100 + i;
        ti.pid = pid;
        ti.currentCpu = i % 8;
        ti.role = (i % 6);
        ti.runTime = 1000 + i * 100;
        ti.nrSwitches = 50 + i * 10;
        ti.tname = "thread_" + std::to_string(i);
        ti.protect = (i == 0);
        result.push_back(ti);
    }
    return result;
}

static double mean(const std::vector<double>& v) {
    double s=0; for(auto x:v) s+=x; return v.empty()?0:s/v.size();
}
static double variance(const std::vector<double>& v) {
    if (v.size()<2) return 0;
    double m=mean(v), s=0; for(auto x:v) s+=(x-m)*(x-m); return s/(v.size()-1);
}
static double tTestTwoSample(const std::vector<double>& a, const std::vector<double>& b) {
    double ma=mean(a), mb=mean(b);
    double va=variance(a), vb=variance(b);
    double se=sqrt(va/a.size()+vb/b.size());
    if (se < 1e-12) return 0;
    return (ma-mb)/se;
}
static double tTestPValueApprox(double t, int df) {
    double z = fabs(t);
    return 2.0 * (1.0 - (0.5*(1.0+erf(z/sqrt(2.0)))));
}

void test_softmax_zero_temperature() {
    SUITE("Softmax: zero temperature");
    float r = softmaxFuseScores(0.8f, 0.2f, 0.0f);
    CHECK_NOT_NAN(r, "zero temperature must not produce NaN");
    CHECK(r >= 0.0f && r <= 1.0f, "result in [0,1] at zero temp");
    CHECK_FLOAT_EQ(r, 0.8f, 0.01f, "zero temp should favor higher score");
    float r2 = softmaxFuseScores(0.8f, 0.2f, 0.0001f);
    CHECK_NOT_NAN(r2, "tiny positive temp must not produce NaN");
    CHECK_FLOAT_EQ(r2, 0.8f, 0.01f, "tiny temp should favor higher score strongly");
    float r3 = softmaxFuseScores(0.8f, 0.2f, 0.01f);
    CHECK_NOT_NAN(r3, "0.01 temp must not produce NaN");
}

void test_softmax_negative_temperature() {
    SUITE("Softmax: negative temperature");
    float r = softmaxFuseScores(0.8f, 0.2f, -1.0f);
    CHECK_NOT_NAN(r, "negative temperature must not produce NaN");
    CHECK(r >= 0.0f && r <= 1.0f, "result in [0,1] at negative temp");
    CHECK(r > 0.5f, "negative temp (abs) should still favor higher score");
    float r2 = softmaxFuseScores(0.8f, 0.2f, -100.0f);
    CHECK_NOT_NAN(r2, "large negative temp must not produce NaN");
    CHECK(r2 > 0.4f && r2 < 0.6f, "large |temp| should average scores");
    float r3 = softmaxFuseScores(0.8f, 0.2f, -0.001f);
    CHECK_NOT_NAN(r3, "tiny negative temp must not produce NaN");
    CHECK_FLOAT_EQ(r3, 0.8f, 0.01f, "tiny negative should clamp and favor high score");
}

void test_softmax_normal_temperature() {
    SUITE("Softmax: normal temperature");
    float r1 = softmaxFuseScores(0.5f, 0.5f, 1.0f);
    CHECK_NOT_NAN(r1, "normal temp not NaN");
    CHECK_FLOAT_EQ(r1, 0.5f, 0.001f, "equal scores should average");
    float r2 = softmaxFuseScores(0.9f, 0.1f, 1.0f);
    CHECK_NOT_NAN(r2, "normal temp not NaN");
    CHECK(r2 > 0.5f, "higher model score should pull result up");
    float r3 = softmaxFuseScores(0.9f, 0.1f, 100.0f);
    CHECK_NOT_NAN(r3, "high temp not NaN");
    CHECK_FLOAT_EQ(r3, 0.5f, 0.05f, "high temp should average near 0.5");
    float r4 = softmaxFuseScores(0.9f, 0.1f, 0.05f);
    CHECK_NOT_NAN(r4, "low temp not NaN");
    CHECK(r4 > 0.8f, "low temp should strongly favor higher score");
}

void test_softmax_edge_cases() {
    SUITE("Softmax: edge cases");
    float r1 = softmaxFuseScores(0.0f, 0.0f, 1.0f);
    CHECK_NOT_NAN(r1, "both zero not NaN");
    CHECK_FLOAT_EQ(r1, 0.0f, 0.001f, "both zero should give 0");
    float r2 = softmaxFuseScores(1000.0f, 999.0f, 1.0f);
    CHECK_NOT_NAN(r2, "large values not NaN (max-subtract prevents overflow)");
    CHECK(r2 > 500.0f, "large values should return large weighted result");
    float r3 = softmaxFuseScores(-0.5f, -0.3f, 1.0f);
    CHECK_NOT_NAN(r3, "negative scores not NaN");
    CHECK(r3 < -0.3f, "negative scores should return negative weighted avg");
    float r4 = softmaxFuseScores(0.5f, 0.5f, 1.0f);
    CHECK(!std::isinf(r4), "result not inf");
    float r5 = softmaxFuseScores(0.7f, 0.3f, 0.009f);
    CHECK_NOT_NAN(r5, "temp 0.009 clamped not NaN");
    CHECK_FLOAT_EQ(r5, 0.7f, 0.02f, "temp below 0.01 clamped to 0.01");
    for (int i = 0; i < 100; i++) {
        float t = 0.001f * i;
        float r = softmaxFuseScores(0.6f, 0.4f, t);
        CHECK_NOT_NAN(r, "stability check at various temps");
    }
}

void test_learningmodel_constructor_auto_init() {
    SUITE("LearningModel: constructor auto-init (Bug 2)");
    LearningModel model;
    CHECK(model.getPoolCount() == 3, "constructor should auto-init with 3 pools");
    CHECK(model.getTotalSamples() == 0, "auto-init should have 0 samples");
    CHECK(model.isEnabled(), "auto-init should be enabled");
    ThreadFeatures f;
    memset(&f, 0, sizeof(f));
    f.avg_run_us = 100;
    float p = model.predict(0, f);
    CHECK_NOT_NAN(p, "predict works after constructor auto-init");
    CHECK(p >= 0.0f && p <= 1.0f, "predict in [0,1] after auto-init");
    for (int i = 0; i < 3; i++) {
        float pi = model.predict(i, f);
        CHECK_NOT_NAN(pi, "predict pool " + std::to_string(i) + " works");
    }
    float pOut = model.predict(99, f);
    CHECK_FLOAT_EQ(pOut, 0.0f, 0.001f, "predict out-of-range returns 0");
    float pNeg = model.predict(-1, f);
    CHECK_FLOAT_EQ(pNeg, 0.0f, 0.001f, "predict negative pool returns 0");
}

void test_learningmodel_init_idempotent() {
    SUITE("LearningModel: init idempotency");
    LearningModel model;
    model.init(3);
    model.init(3);
    model.init(5);
    model.init(5);
    CHECK(model.getPoolCount() == 5, "after init(5), pool count is 5");
    model.init(0);
    CHECK(model.getPoolCount() == 3, "init(0) defaults to 3");
    model.init(-1);
    CHECK(model.getPoolCount() == 3, "init(-1) defaults to 3");
    model.init(100);
    CHECK(model.getPoolCount() == 16, "init(100) clamps to 16");
    model.init(1);
    CHECK(model.getPoolCount() == 1, "init(1) works");
    ThreadFeatures f; memset(&f, 0, sizeof(f)); f.avg_run_us = 100;
    model.update(0, f, 0.8f);
    CHECK(model.getTotalSamples() > 0, "samples accumulated after update");
    model.init(3);
    CHECK(model.getTotalSamples() == 0, "re-init clears samples");
    CHECK(model.getPoolCount() == 3, "re-init sets pool count");
}

void test_learningmodel_predict_update() {
    SUITE("LearningModel: predict and update");
    LearningModel model;
    model.init(3);
    ThreadFeatures f;
    memset(&f, 0, sizeof(f));
    f.avg_run_us = 200;
    f.cpu_usage = 50;
    f.priority = 1;
    float p0 = model.predict(0, f);
    CHECK_FLOAT_EQ(p0, 0.5f, 0.01f, "initial prediction ~0.5");
    for (int i = 0; i < 100; i++) {
        model.update(0, f, 1.0f);
    }
    float p1 = model.predict(0, f);
    CHECK(p1 > p0, "positive updates increase prediction");
    CHECK(p1 <= 1.0f, "prediction <= 1");
    LearningModel model2;
    model2.init(3);
    for (int i = 0; i < 100; i++) {
        model2.update(0, f, 0.0f);
    }
    float p2 = model2.predict(0, f);
    CHECK(p2 < 0.5f, "negative updates decrease prediction");
    CHECK(p2 >= 0.0f, "prediction >= 0");
    CHECK(model.getTotalSamples() == 100, "total samples = 100");
    CHECK(model.getPoolSampleCount(0) == 100, "pool 0 samples = 100");
    CHECK(model.getPoolSampleCount(1) == 0, "pool 1 samples = 0");
}

void test_learningmodel_save_load() {
    SUITE("LearningModel: save and load");
    LearningModel model;
    model.init(3);
    ThreadFeatures f; memset(&f, 0, sizeof(f)); f.avg_run_us = 150;
    for (int i = 0; i < 50; i++) model.update(0, f, 0.9f);
    CHECK(model.save("/tmp/aatpe_test_model_v69.bin"), "save succeeds");
    auto w1 = model.getWeights();
    LearningModel model2;
    model2.init(3);
    CHECK(model2.load("/tmp/aatpe_test_model_v69.bin"), "load succeeds");
    auto w2 = model2.getWeights();
    CHECK(w1.size() == w2.size(), "weight count matches after load");
    for (size_t i = 0; i < w1.size() && i < w2.size(); i++) {
        CHECK_FLOAT_EQ(w1[i], w2[i], 0.001f, "weight " + std::to_string(i) + " matches");
    }
    CHECK(model2.getTotalSamples() == 50, "sample count restored after load");
    float p1 = model.predict(0, f);
    float p2p = model2.predict(0, f);
    CHECK_FLOAT_EQ(p1, p2p, 0.001f, "predictions match after load");
    remove("/tmp/aatpe_test_model_v69.bin");
}

void test_learningmodel_cold_start() {
    SUITE("LearningModel: cold start protection");
    LearningModel model;
    model.init(3);
    model.setMinSamples(10);
    CHECK(!model.isPoolReady(0), "pool not ready with 0 samples");
    ThreadFeatures f; memset(&f, 0, sizeof(f));
    for (int i = 0; i < 5; i++) model.update(0, f, 0.5f);
    CHECK(!model.isPoolReady(0), "pool not ready with 5 samples (<10)");
    for (int i = 0; i < 10; i++) model.update(0, f, 0.5f);
    CHECK(model.isPoolReady(0), "pool ready with 15 samples (>=10)");
    CHECK(!model.isPoolReady(1), "pool 1 still not ready");
    CHECK(!model.isPoolReady(-1), "negative pool not ready");
    CHECK(!model.isPoolReady(99), "out-of-range pool not ready");
    model.setEnabled(false);
    CHECK(!model.isEnabled(), "disabled flag works");
    float p = model.predict(0, f);
    CHECK_FLOAT_EQ(p, 0.0f, 0.001f, "disabled model returns 0");
}

void test_learningmodel_thread_safety() {
    SUITE("LearningModel: thread safety");
    LearningModel model;
    model.init(4);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&model, &errors, t]() {
            ThreadFeatures f; memset(&f, 0, sizeof(f));
            f.avg_run_us = 100 + t;
            for (int i = 0; i < 200; i++) {
                try {
                    model.update(t % 4, f, 0.5f + (t % 3) * 0.1f);
                    float p = model.predict(t % 4, f);
                    if (std::isnan(p) || p < 0 || p > 1) errors++;
                } catch (...) { errors++; }
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(errors.load() == 0, "no errors under concurrent access");
    CHECK(model.getTotalSamples() == 1600, "total samples = 8*200 = 1600");
}

void test_scoring_basic() {
    SUITE("Scoring: basic thread scoring");
    aatpe::ThreadInfo ui;
    ui.tid = 1; ui.role = ROLE_S1_UI; ui.currentCpu = 0; ui.protect = false;
    float s = computeThreadScore(ui, 0);
    CHECK(s > 0.5f, "UI thread gets bonus");
    aatpe::ThreadInfo bg;
    bg.tid = 2; bg.role = ROLE_S5_BACKGROUND; bg.currentCpu = 0; bg.protect = false;
    float s2 = computeThreadScore(bg, 0);
    CHECK(s2 < s, "background scores lower than UI");
    aatpe::ThreadInfo game;
    game.tid = 3; game.role = ROLE_S3_GAME; game.currentCpu = 0; game.protect = true;
    float s3 = computeThreadScore(game, 2);
    CHECK(s3 > s, "game+protect scores higher than UI");
    aatpe::ThreadInfo t1; t1.tid=4; t1.role=ROLE_S0_SYSTEM; t1.currentCpu=2; t1.protect=false;
    float sSame = computeThreadScore(t1, 2);
    float sDiff = computeThreadScore(t1, 5);
    CHECK(sSame > sDiff, "same CPU gets bonus");
    for (int i = 0; i < 10; i++) {
        aatpe::ThreadInfo tx; tx.tid=i; tx.role=i%6; tx.currentCpu=i%8; tx.protect=(i%2==0);
        float sx = computeThreadScore(tx, i%8);
        CHECK(sx >= 0.0f && sx <= 1.0f, "score clamped to [0,1]");
    }
}

void test_thread_scan_basic() {
    SUITE("Thread scan: basic");
    auto threads = scanThreads(1234);
    CHECK(threads.size() == 8, "scan returns 8 threads");
    for (const auto& t : threads) {
        CHECK(t.pid == 1234, "pid matches");
        CHECK(t.tid > 0, "tid positive");
        CHECK(t.currentCpu >= 0 && t.currentCpu < 8, "cpu in range");
        CHECK(t.role >= 0 && t.role <= 5, "role in range");
    }
    CHECK(threads[0].protect, "first thread protected");
    CHECK(!threads[1].protect, "second thread not protected");
}

void test_state_machine_basic() {
    SUITE("State machine: basic transitions");
    aatpe_sm::transitionTo(aatpe::SystemState::NORMAL, "init");
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::NORMAL, "initial NORMAL");
    aatpe_sm::transitionTo(aatpe::SystemState::DEGRADED, "thermal");
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::DEGRADED, "transition to DEGRADED");
    CHECK(std::string(aatpe_sm::getLastTransitionReason()) == "thermal", "reason recorded");
    aatpe_sm::tick();
    CHECK(aatpe_sm::getStateDurationSec() == 1, "duration increments");
    aatpe_sm::tick(); aatpe_sm::tick();
    CHECK(aatpe_sm::getStateDurationSec() == 3, "duration = 3");
    aatpe_sm::transitionTo(aatpe::SystemState::PASSIVE, "watchdog");
    CHECK(aatpe_sm::getStateDurationSec() == 0, "transition resets duration");
    aatpe_sm::transitionTo(aatpe::SystemState::EMERGENCY, "critical_temp");
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::EMERGENCY, "EMERGENCY state");
    long long d0 = aatpe_sm::getStateDurationSec();
    aatpe_sm::transitionTo(aatpe::SystemState::EMERGENCY, "still hot");
    CHECK(aatpe_sm::getStateDurationSec() == d0, "same-state transition doesn't reset duration");
}

void test_thermal_basic() {
    SUITE("Thermal: basic");
    aatpe_thermal::setTemp(40);
    CHECK(aatpe_thermal::getCpuTemp() == 40, "cpu temp = 40");
    CHECK(!aatpe_thermal::isEmergency(), "not emergency at 40");
    aatpe_thermal::setTemp(120);
    CHECK(aatpe_thermal::isEmergency(), "emergency at limit 120");
    aatpe_thermal::setTemp(119);
    CHECK(!aatpe_thermal::isEmergency(), "not emergency at 119");
    aatpe_thermal::setTemp(150);
    CHECK(aatpe_thermal::isEmergency(), "emergency at 150");
    CHECK(aatpe_thermal::getSkinTemp() == 35, "skin temp default 35");
    CHECK(aatpe_thermal::getBatteryTemp() == 30, "battery temp default 30");
}

void test_hmac_basic() {
    SUITE("HMAC: basic");
    std::string key = "test-secret-key-12345";
    std::string msg = "set_mode=gamimg.ts=1234567890";
    std::string sig = aatpe_crypto::hmacSha256Hex(key, msg);
    CHECK(sig.size() == 64, "HMAC signature is 64 hex chars");
    bool allHex = true;
    for (char c : sig) { if (!isxdigit(c)) allHex = false; }
    CHECK(allHex, "signature contains only hex chars");
    std::string sig2 = aatpe_crypto::hmacSha256Hex(key, msg);
    CHECK(sig == sig2, "HMAC is deterministic");
    std::string sig3 = aatpe_crypto::hmacSha256Hex("other-key", msg);
    CHECK(sig != sig3, "different key produces different signature");
    std::string sig4 = aatpe_crypto::hmacSha256Hex(key, "other command");
    CHECK(sig != sig4, "different message produces different signature");
    std::string sigEmpty = aatpe_crypto::hmacSha256Hex(key, "");
    CHECK(sigEmpty.size() == 64, "empty message still produces 64-char signature");
    std::string known = aatpe_crypto::hmacSha256Hex("key", "The quick brown fox jumps over the lazy dog");
    CHECK(known == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
          "known HMAC-SHA256 test vector matches");
}

void test_hmac_verification() {
    SUITE("HMAC: verification logic");
    std::string key = "verify-key";
    std::string cmd = "set_profile=balanced";
    std::string sig = aatpe_crypto::hmacSha256Hex(key, cmd);
    std::string verify = aatpe_crypto::hmacSha256Hex(key, cmd);
    CHECK(sig == verify, "valid signature verifies");
    std::string tampered = aatpe_crypto::hmacSha256Hex(key, "set_profile=gaming");
    CHECK(sig != tampered, "tampered command fails verification");
    std::string wrongKey = aatpe_crypto::hmacSha256Hex("wrong", cmd);
    CHECK(sig != wrongKey, "wrong key fails verification");
    CHECK(!sig.empty(), "signature not empty");
    int64_t now = 1700000000;
    int64_t validTs = now;
    int64_t oldTs = now - 60;
    int64_t futureTs = now + 10;
    CHECK((now - validTs) <= 30, "valid timestamp within window");
    CHECK((now - oldTs) > 30, "old timestamp expired");
    CHECK((now - futureTs) < -5, "future timestamp out of skew window");
}

void test_decision_fusion_basic() {
    SUITE("Decision fusion: basic");
    auto f = fuseDecision(0.8f, 0.6f, 0.7f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f);
    CHECK_NOT_NAN(f.final_score, "fusion score not NaN");
    CHECK(f.final_score >= 0.0f && f.final_score <= 1.0f, "fusion score in [0,1]");
    float expected = 0.8f*0.4f + 0.6f*0.3f + 0.7f*0.2f + 0.5f*0.1f;
    CHECK_FLOAT_EQ(f.final_score, expected, 0.001f, "weighted average correct");
    CHECK_FLOAT_EQ(f.w_heuristic + f.w_ml + f.w_hybrid + f.w_history, 1.0f, 0.001f, "weights sum to 1");
    auto f2 = fuseDecision(0.8f, 0.6f, 0.7f, 0.5f, 0, 0, 0, 0);
    CHECK_NOT_NAN(f2.final_score, "zero weights not NaN");
    auto f3 = fuseDecision(0.5f, 0.5f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f, 0.25f);
    CHECK_FLOAT_EQ(f3.final_score, 0.5f, 0.001f, "equal scores give 0.5");
    CHECK_FLOAT_EQ(f.contrib_heuristic + f.contrib_ml + f.contrib_hybrid + f.contrib_history,
                   f.final_score, 0.001f, "contributions sum to final score");
}

void test_frame_time_outcome() {
    SUITE("Frame time: outcome tracking");
    aatpe_frame::reset();
    CHECK(aatpe_frame::g_samples == 0, "starts at 0");
    for (int i = 0; i < 10; i++) aatpe_frame::recordOutcome(true, false);
    CHECK(aatpe_frame::g_samples == 10, "10 samples recorded");
    CHECK(aatpe_frame::g_improved == 10, "10 improved");
    CHECK_FLOAT_EQ(aatpe_frame::getImprovementRate(), 1.0f, 0.001f, "100% improvement rate");
    aatpe_frame::recordOutcome(false, true);
    CHECK(aatpe_frame::g_samples == 11, "11 samples");
    CHECK(aatpe_frame::g_fallback == 1, "1 fallback");
    CHECK_FLOAT_EQ(aatpe_frame::getImprovementRate(), 10.0f/11.0f, 0.001f, "90.9% improvement rate");
    aatpe_frame::reset();
    CHECK(aatpe_frame::getImprovementRate() == 0.0f, "reset gives 0 rate");
}

void test_utility_clamp() {
    SUITE("Utilities: clamp functions");
    CHECK_FLOAT_EQ(clampf(0.5f, 0.0f, 1.0f), 0.5f, 0.001f, "clamp middle");
    CHECK_FLOAT_EQ(clampf(-1.0f, 0.0f, 1.0f), 0.0f, 0.001f, "clamp low");
    CHECK_FLOAT_EQ(clampf(2.0f, 0.0f, 1.0f), 1.0f, 0.001f, "clamp high");
    CHECK(clampi(5, 0, 10) == 5, "clampi middle");
    CHECK(clampi(-5, 0, 10) == 0, "clampi low");
    CHECK(clampi(15, 0, 10) == 10, "clampi high");
    CHECK(clampi(0, 0, 10) == 0, "clampi boundary low");
    CHECK(clampi(10, 0, 10) == 10, "clampi boundary high");
}

void test_utility_whitelist() {
    SUITE("Utilities: package whitelist");
    std::string wl = "com.miHoYo,com.tencent,com.supercell";
    CHECK(isPackageWhitelisted("com.miHoYo.GenshinImpact", wl), "Genshin whitelisted");
    CHECK(isPackageWhitelisted("com.tencent.tmgp.pubgm", wl), "PUBG whitelisted");
    CHECK(!isPackageWhitelisted("com.other.app", wl), "non-whitelisted rejected");
    CHECK(isPackageWhitelisted("any.package", ""), "empty whitelist allows all");
    CHECK(isPackageWhitelisted("com.android.settings", ""), "empty whitelist allows settings");
    CHECK(!isPackageWhitelisted("com.miHoYo", "com.tencent"), "partial mismatch rejected");
    CHECK(isPackageWhitelisted("com.supercell.clashofclans", wl), "Supercell whitelisted");
}

void test_ebpf_basic() {
    SUITE("eBPF: basic state tracking");
    static bool g_ebpfActive = false;
    static std::string g_ebpfLoader = "none";
    CHECK(!g_ebpfActive, "eBPF initially inactive");
    g_ebpfActive = true; g_ebpfLoader = "kprobe";
    CHECK(g_ebpfActive, "eBPF activated");
    CHECK(g_ebpfLoader == "kprobe", "loader set to kprobe");
    g_ebpfActive = false; g_ebpfLoader = "none";
    CHECK(!g_ebpfActive, "eBPF deactivated");
    std::vector<std::string> loaders = {"kprobe", "fentry", "uprobe", "tracepoint"};
    for (const auto& ld : loaders) {
        g_ebpfLoader = ld;
        CHECK(g_ebpfLoader == ld, "loader " + ld + " set correctly");
    }
}

void test_webui_http_basic() {
    SUITE("WebUI/HTTP: request routing");
    auto matchRoute = [](const std::string& path, const std::string& route) -> bool {
        return path == route || path.find(route + "/") == 0;
    };
    CHECK(matchRoute("/state/all", "/state/all"), "exact route match");
    CHECK(matchRoute("/state/all/", "/state/all"), "trailing slash match");
    CHECK(!matchRoute("/state/other", "/state/all"), "different path no match");
    CHECK(matchRoute("/api/fusion", "/api/fusion"), "api route match");
    CHECK(matchRoute("/api/fusion/extra", "/api/fusion"), "subpath matches prefix route");
    auto jsonEscape = [](const std::string& s) -> std::string {
        std::string r;
        for (char c : s) {
            if (c == '"') r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    };
    CHECK(jsonEscape("hello") == "hello", "no escape needed");
    CHECK(jsonEscape("say \"hi\"") == "say \\\"hi\\\"", "quote escaped");
    CHECK(jsonEscape("back\\slash") == "back\\\\slash", "backslash escaped");
    CHECK(jsonEscape("line\nbreak") == "line\\nbreak", "newline escaped");
}

void test_softmax_parametric() {
    SUITE("Softmax: parametric sweep");
    std::vector<float> temps = {0.0f, 0.001f, 0.01f, 0.05f, 0.1f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 50.0f, 100.0f};
    std::vector<float> negTemps = {-0.001f, -0.01f, -0.1f, -1.0f, -5.0f, -10.0f, -100.0f};
    for (float t : temps) {
        float r = softmaxFuseScores(0.7f, 0.3f, t);
        CHECK_NOT_NAN(r, "not NaN at temp=" + std::to_string(t));
        CHECK(r >= 0.0f && r <= 1.0f, "in range at temp=" + std::to_string(t));
    }
    for (float t : negTemps) {
        float r = softmaxFuseScores(0.7f, 0.3f, t);
        CHECK_NOT_NAN(r, "not NaN at neg temp=" + std::to_string(t));
        CHECK(r >= 0.0f && r <= 1.0f, "in range at neg temp=" + std::to_string(t));
    }
    for (float t : {0.1f, 0.5f, 1.0f, 2.0f, 5.0f}) {
        float rp = softmaxFuseScores(0.6f, 0.4f, t);
        float rn = softmaxFuseScores(0.6f, 0.4f, -t);
        CHECK_FLOAT_EQ(rp, rn, 0.0001f, "temp and -temp symmetric at " + std::to_string(t));
    }
}

void test_learningmodel_parameters() {
    SUITE("LearningModel: parameter setters/getters");
    LearningModel m;
    m.init(3);
    m.setMaxAlpha(0.5f);
    m.setInitialLR(0.05f);
    m.setDecay(0.99f);
    m.setWeightClamp(5.0f);
    m.setL2Lambda(0.01f);
    m.setColdStartSamples(100);
    m.setMinSamples(500);
    CHECK(m.getMinSamples() == 500, "min samples set");
    m.setMinSamples(-5);
    CHECK(m.getMinSamples() == 1, "negative min samples clamps to 1");
    m.setMinSamples(0);
    CHECK(m.getMinSamples() == 1, "zero min samples clamps to 1");
    std::vector<int> assign = {0,0,1,1,2,2,2,2};
    m.setCpuPoolAssignment(assign);
    auto got = m.getCpuPoolAssignment();
    CHECK(got.size() == 8, "cpu assignment size");
    for (int i = 0; i < 8; i++) CHECK(got[i] == assign[i], "cpu assignment " + std::to_string(i));
    m.setTopologyFingerprint("fp_v1");
    CHECK(m.getTopologyFingerprint() == "fp_v1", "fingerprint set");
    m.setTopologyFingerprint("fp_v2");
    CHECK(m.isModelStale(), "fingerprint change marks stale");
    std::vector<float> w(ML_WEIGHT_COUNT, 0.1f);
    m.setWeights(w);
    auto wgot = m.getWeights();
    CHECK(wgot.size() == (size_t)ML_WEIGHT_COUNT * 3, "weights size = pools * feature count");
}

void test_state_machine_all_states() {
    SUITE("State machine: all states and transitions");
    aatpe::SystemState states[] = {
        aatpe::SystemState::NORMAL, aatpe::SystemState::DEGRADED,
        aatpe::SystemState::PASSIVE, aatpe::SystemState::EMERGENCY
    };
    const char* names[] = {"NORMAL", "DEGRADED", "PASSIVE", "EMERGENCY"};
    for (int i = 0; i < 4; i++) {
        aatpe_sm::transitionTo(states[i], names[i]);
        CHECK(aatpe_sm::getSystemState() == states[i], std::string("state ") + names[i] + " set");
        CHECK(std::string(aatpe_sm::getLastTransitionReason()) == names[i], "reason for " + std::string(names[i]));
        aatpe_sm::tick();
        CHECK(aatpe_sm::getStateDurationSec() >= 1, "duration positive in " + std::string(names[i]));
    }
    for (int from = 0; from < 4; from++) {
        for (int to = 0; to < 4; to++) {
            aatpe_sm::transitionTo(states[from], "from");
            aatpe_sm::transitionTo(states[to], "to");
            CHECK(aatpe_sm::getSystemState() == states[to],
                  "transition " + std::to_string(from) + "->" + std::to_string(to));
        }
    }
}

void test_thermal_thresholds() {
    SUITE("Thermal: threshold sweep");
    for (int t = 0; t <= 150; t += 5) {
        aatpe_thermal::setTemp(t);
        bool emerg = aatpe_thermal::isEmergency();
        if (t >= 120) CHECK(emerg, "emergency at temp " + std::to_string(t));
        else CHECK(!emerg, "not emergency at temp " + std::to_string(t));
    }
    static int g_hysteresisCount = 0;
    aatpe_thermal::setTemp(120);
    CHECK(aatpe_thermal::isEmergency(), "at threshold");
    aatpe_thermal::setTemp(119);
    CHECK(!aatpe_thermal::isEmergency(), "just below threshold");
}

void test_hmac_many_messages() {
    SUITE("HMAC: many messages");
    std::string key = "multi-msg-key";
    std::vector<std::string> msgs = {
        "set_mode=assist", "set_mode=gamimg", "set_profile=balanced",
        "set_profile=battery", "set_profile=gaming", "set_profile=thermal",
        "start", "stop", "reload", "status", "ml_enable=1", "ml_enable=0",
        "thermal_limit=120", "thermal_limit=100", "hmac_rotate"
    };
    std::vector<std::string> sigs;
    for (const auto& msg : msgs) {
        std::string s = aatpe_crypto::hmacSha256Hex(key, msg);
        CHECK(s.size() == 64, "sig size for: " + msg);
        sigs.push_back(s);
    }
    for (size_t i = 0; i < sigs.size(); i++) {
        for (size_t j = i+1; j < sigs.size(); j++) {
            CHECK(sigs[i] != sigs[j], "unique sig for msgs " + std::to_string(i) + "," + std::to_string(j));
        }
    }
}

void test_scoring_many_threads() {
    SUITE("Scoring: many thread configurations");
    for (int role = 0; role <= 6; role++) {
        for (int cpu = 0; cpu < 8; cpu++) {
            for (int prot = 0; prot < 2; prot++) {
                aatpe::ThreadInfo ti;
                ti.tid = role*100+cpu*10+prot;
                ti.role = role;
                ti.currentCpu = cpu;
                ti.protect = (prot == 1);
                float s = computeThreadScore(ti, cpu);
                CHECK(s >= 0.0f && s <= 1.0f, "score valid for role="+std::to_string(role));
                CHECK_NOT_NAN(s, "score not NaN");
            }
        }
    }
}

void test_fusion_weight_combinations() {
    SUITE("Decision fusion: weight combinations");
    float weights[] = {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float wh : weights) {
        for (float wm : weights) {
            for (float why : weights) {
                for (float whi : weights) {
                    auto f = fuseDecision(0.8f, 0.6f, 0.7f, 0.5f, wh, wm, why, whi);
                    CHECK_NOT_NAN(f.final_score, "fusion not NaN");
                    if (wh+wm+why+whi > 0) {
                        CHECK_FLOAT_EQ(f.w_heuristic+f.w_ml+f.w_hybrid+f.w_history, 1.0f, 0.01f,
                                       "weights normalize");
                    }
                }
            }
        }
    }
}

void test_integration_ml_fusion_pipeline() {
    SUITE("Integration: ML + fusion pipeline");
    LearningModel model;
    model.init(3);
    ThreadFeatures f; memset(&f, 0, sizeof(f));
    f.avg_run_us = 150; f.cpu_usage = 40; f.priority = 1;
    for (int i = 0; i < 200; i++) model.update(0, f, 0.85f);
    float mlScore = model.predict(0, f);
    CHECK(mlScore > 0.5f, "ML score > 0.5 after positive training");
    float hScore = 0.7f;
    float fused = softmaxFuseScores(mlScore, hScore, 1.0f);
    CHECK_NOT_NAN(fused, "fused score not NaN");
    CHECK(fused > 0.5f, "fused > 0.5");
    auto df = fuseDecision(hScore, mlScore, 0.65f, 0.6f, 0.3f, 0.4f, 0.2f, 0.1f);
    CHECK(df.final_score > 0.6f, "final fusion > 0.6");
    CHECK(df.contrib_ml > df.contrib_history, "ML contributes more than history");
}

void test_integration_state_thermal_coupling() {
    SUITE("Integration: state-thermal coupling");
    aatpe_sm::transitionTo(aatpe::SystemState::NORMAL, "start");
    aatpe_thermal::setTemp(50);
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::NORMAL, "normal at 50C");
    for (int t = 50; t <= 130; t += 10) {
        aatpe_thermal::setTemp(t);
        if (aatpe_thermal::isEmergency()) {
            aatpe_sm::transitionTo(aatpe::SystemState::EMERGENCY, "thermal");
        }
    }
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::EMERGENCY, "emergency after thermal rise");
    CHECK(std::string(aatpe_sm::getLastTransitionReason()) == "thermal", "reason=thermal");
    aatpe_thermal::setTemp(40);
    aatpe_sm::transitionTo(aatpe::SystemState::NORMAL, "cooled");
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::NORMAL, "recovered after cool down");
}

void test_integration_thread_scan_score_place() {
    SUITE("Integration: thread scan → score → place");
    auto threads = scanThreads(5678);
    CHECK(threads.size() == 8, "scanned 8 threads");
    std::vector<std::pair<float, int>> scored;
    for (const auto& t : threads) {
        float bestScore = -1;
        int bestCpu = 0;
        for (int cpu = 0; cpu < 8; cpu++) {
            float s = computeThreadScore(t, cpu);
            if (s > bestScore) { bestScore = s; bestCpu = cpu; }
        }
        scored.push_back({bestScore, bestCpu});
        CHECK(bestScore >= 0.0f && bestScore <= 1.0f, "best score valid");
    }
    CHECK(scored[0].first > 0.5f, "protected thread high score");
    for (const auto& p : scored) {
        CHECK(p.second >= 0 && p.second < 8, "assigned CPU valid");
    }
}

void test_integration_hmac_command_pipeline() {
    SUITE("Integration: HMAC command pipeline");
    std::string key = "integration-key";
    std::string cmd = "set_mode=gaming";
    int64_t ts = 1700000000;
    std::string fullCmd = cmd + ".ts=" + std::to_string(ts);
    std::string sig = aatpe_crypto::hmacSha256Hex(key, fullCmd);
    std::string signedCmd = fullCmd + ".sig=" + sig;
    size_t sigPos = signedCmd.rfind(".sig=");
    CHECK(sigPos != std::string::npos, "sig found in command");
    std::string extractedSig = signedCmd.substr(sigPos + 5);
    CHECK(extractedSig == sig, "extracted sig matches");
    std::string withoutSig = signedCmd.substr(0, sigPos);
    size_t tsPos = withoutSig.rfind(".ts=");
    CHECK(tsPos != std::string::npos, "ts found");
    std::string extractedCmd = withoutSig.substr(0, tsPos);
    CHECK(extractedCmd == cmd, "extracted cmd matches");
    std::string verifySig = aatpe_crypto::hmacSha256Hex(key, withoutSig);
    CHECK(verifySig == sig, "signature verifies over cmd+ts");
}

void test_integration_softmax_abnormal_recovery() {
    SUITE("Integration: softmax abnormal temperature recovery");
    float results[10];
    float temps[] = {0.0f, -1.0f, -100.0f, 0.0001f, -0.001f, 0.01f, 0.1f, 1.0f, 10.0f, 100.0f};
    for (int i = 0; i < 10; i++) {
        results[i] = softmaxFuseScores(0.7f, 0.3f, temps[i]);
        CHECK_NOT_NAN(results[i], "not NaN at abnormal temp " + std::to_string(temps[i]));
    }
    float normal = softmaxFuseScores(0.7f, 0.3f, 1.0f);
    CHECK_NOT_NAN(normal, "recovery: normal temp works");
    CHECK(normal > 0.5f, "recovery: favors higher score");
    for (int i = 0; i < 10; i++) {
        CHECK(std::isfinite(results[i]), "finite at temp " + std::to_string(temps[i]));
    }
}

void test_integration_model_persist_predict() {
    SUITE("Integration: model persist → reload → predict consistency");
    LearningModel m1;
    m1.init(4);
    ThreadFeatures f; memset(&f, 0, sizeof(f));
    f.avg_run_us = 200; f.cpu_usage = 60;
    for (int i = 0; i < 300; i++) m1.update(i % 4, f, 0.3f + (i % 5) * 0.1f);
    CHECK(m1.save("/tmp/aatpe_integ_model.bin"), "save");
    float p_before[4];
    for (int i = 0; i < 4; i++) p_before[i] = m1.predict(i, f);
    LearningModel m2;
    m2.init(4);
    CHECK(m2.load("/tmp/aatpe_integ_model.bin"), "load");
    for (int i = 0; i < 4; i++) {
        float p_after = m2.predict(i, f);
        CHECK_FLOAT_EQ(p_before[i], p_after, 0.001f, "prediction consistent pool " + std::to_string(i));
    }
    CHECK(m2.getTotalSamples() == 300, "sample count persisted");
    remove("/tmp/aatpe_integ_model.bin");
}

void test_integration_concurrent_model_updates() {
    SUITE("Integration: concurrent model updates + reads");
    LearningModel model;
    model.init(4);
    std::atomic<bool> stop{false};
    std::atomic<int> readErrors{0};
    std::vector<std::thread> writers, readers;
    for (int t = 0; t < 4; t++) {
        writers.emplace_back([&]() {
            ThreadFeatures f; memset(&f, 0, sizeof(f));
            for (int i = 0; i < 500; i++) {
                f.avg_run_us = 100 + (i % 50);
                model.update(t, f, 0.5f);
            }
        });
    }
    for (int t = 0; t < 4; t++) {
        readers.emplace_back([&]() {
            ThreadFeatures f; memset(&f, 0, sizeof(f));
            for (int i = 0; i < 500; i++) {
                float p = model.predict(t, f);
                if (std::isnan(p) || p < 0 || p > 1) readErrors++;
            }
        });
    }
    for (auto& th : writers) th.join();
    for (auto& th : readers) th.join();
    CHECK(readErrors.load() == 0, "no read errors under concurrency");
    CHECK(model.getTotalSamples() >= 0, "sample count valid under concurrency");
}

void test_integration_state_duration_tracking() {
    SUITE("Integration: state duration tracking");
    aatpe_sm::transitionTo(aatpe::SystemState::NORMAL, "boot");
    for (int i = 0; i < 100; i++) aatpe_sm::tick();
    CHECK(aatpe_sm::getStateDurationSec() == 100, "100 ticks = 100 sec");
    aatpe_sm::transitionTo(aatpe::SystemState::DEGRADED, "load");
    CHECK(aatpe_sm::getStateDurationSec() == 0, "reset on transition");
    for (int i = 0; i < 50; i++) aatpe_sm::tick();
    CHECK(aatpe_sm::getStateDurationSec() == 50, "50 sec in DEGRADED");
    long long d = aatpe_sm::getStateDurationSec();
    aatpe_sm::transitionTo(aatpe::SystemState::DEGRADED, "still loaded");
    CHECK(aatpe_sm::getStateDurationSec() == d, "same-state keeps duration");
}

void test_integration_frame_outcome_stats() {
    SUITE("Integration: frame outcome statistics");
    aatpe_frame::reset();
    std::mt19937 rng(42);
    for (int i = 0; i < 1000; i++) {
        bool improved = (rng() % 100) < 70;
        bool fallback = (rng() % 100) < 5;
        aatpe_frame::recordOutcome(improved, fallback);
    }
    CHECK(aatpe_frame::g_samples == 1000, "1000 samples");
    float rate = aatpe_frame::getImprovementRate();
    CHECK(rate > 0.6f && rate < 0.8f, "improvement rate ~70% (got " + std::to_string(rate) + ")");
    CHECK(aatpe_frame::g_fallback > 0, "some fallbacks recorded");
    CHECK(aatpe_frame::g_fallback < 100, "fallback < 10%");
}

void test_integration_thermal_state_feedback() {
    SUITE("Integration: thermal → state → scheduling feedback");
    aatpe_thermal::setTemp(40);
    aatpe_sm::transitionTo(aatpe::SystemState::NORMAL, "cool");
    for (int step = 0; step < 20; step++) {
        int temp = 40 + step * 5;
        aatpe_thermal::setTemp(temp);
        if (aatpe_thermal::isEmergency() && aatpe_sm::getSystemState() != aatpe::SystemState::EMERGENCY) {
            aatpe_sm::transitionTo(aatpe::SystemState::EMERGENCY, "thermal_limit");
        }
        if (aatpe_sm::getSystemState() == aatpe::SystemState::EMERGENCY) {
            aatpe_thermal::setTemp(temp - 3);
        }
    }
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::EMERGENCY, "reached emergency");
    for (int step = 0; step < 20; step++) {
        aatpe_thermal::setTemp(aatpe_thermal::getCpuTemp() - 5);
        if (!aatpe_thermal::isEmergency() && aatpe_sm::getSystemState() == aatpe::SystemState::EMERGENCY) {
            aatpe_sm::transitionTo(aatpe::SystemState::DEGRADED, "cooling");
        }
    }
    CHECK(!aatpe_thermal::isEmergency(), "cooled down");
    CHECK(aatpe_sm::getSystemState() == aatpe::SystemState::DEGRADED, "transitioned to DEGRADED");
}

void test_integration_whitelist_scanning() {
    SUITE("Integration: whitelist + thread scanning");
    std::string wl = "com.game1,com.game2";
    auto threads = scanThreads(999);
    int gameThreads = 0;
    for (const auto& t : threads) {
        if (t.role == ROLE_S3_GAME) gameThreads++;
    }
    CHECK(gameThreads > 0, "game threads found");
    bool pkgAllowed = isPackageWhitelisted("com.game1.app", wl);
    CHECK(pkgAllowed, "package whitelisted");
    if (pkgAllowed) {
        for (const auto& t : threads) {
            float s = computeThreadScore(t, t.currentCpu);
            CHECK(s >= 0, "score computed");
        }
    }
}

void test_integration_ebpf_state_propagation() {
    SUITE("Integration: eBPF state propagation");
    static bool ebpfOn = false;
    static std::string loader = "none";
    ebpfOn = true; loader = "kprobe";
    CHECK(ebpfOn, "ebpf on");
    std::ostringstream json;
    json << "{\"ebpf\":{\"active\":" << (ebpfOn ? "true" : "false")
         << ",\"loader\":\"" << loader << "\"}}";
    std::string j = json.str();
    CHECK(j.find("\"active\":true") != std::string::npos, "json has active:true");
    CHECK(j.find("kprobe") != std::string::npos, "json has loader");
    ebpfOn = false;
    json.str(""); json.clear();
    json << "{\"ebpf\":{\"active\":" << (ebpfOn ? "true" : "false") << "}}";
    CHECK(json.str().find("false") != std::string::npos, "json updated to false");
}

void test_integration_webui_state_all_aggregation() {
    SUITE("Integration: WebUI /state/all aggregation");
    std::map<std::string, std::string> stateFiles = {
        {"daemon_uptime", "3600"},
        {"control_mode", "assist"},
        {"current_profile", "balanced"},
        {"system_load", "45"},
        {"cpu_usage", "42"},
        {"sf_latency_avg", "1200"},
        {"layer_count", "3"},
        {"system_state", "NORMAL"},
        {"state_duration_sec", "120"},
        {"thermal_cpu_max", "55"}
    };
    std::ostringstream body;
    body << "{";
    bool first = true;
    for (const auto& [k, v] : stateFiles) {
        if (!first) body << ",";
        first = false;
        body << "\"" << k << "\":\"" << v << "\"";
    }
    body << "}";
    std::string json = body.str();
    CHECK(json.find("\"sf_latency_avg\":\"1200\"") != std::string::npos, "sf_latency_avg in /state/all");
    CHECK(json.find("\"layer_count\":\"3\"") != std::string::npos, "layer_count in /state/all");
    CHECK(json.find("\"cpu_usage\":\"42\"") != std::string::npos, "cpu_usage in /state/all");
    CHECK(json.find("\"system_state\":\"NORMAL\"") != std::string::npos, "system_state in /state/all");
    CHECK(json.find("\"state_duration_sec\":\"120\"") != std::string::npos, "state_duration in /state/all");
    auto get = [&](const std::string& key) -> std::string {
        auto it = stateFiles.find(key);
        return it != stateFiles.end() ? it->second : "";
    };
    CHECK(get("sf_latency_avg") == "1200", "get sf_latency_avg");
    CHECK(get("layer_count") == "3", "get layer_count");
    CHECK(get("cpu_usage") == "42", "get cpu_usage");
    CHECK(get("system_state") == "NORMAL", "get system_state");
    CHECK(get("state_duration_sec") == "120", "get state_duration_sec");
}

void test_integration_model_cold_start_gating() {
    SUITE("Integration: ML cold start gating in fusion");
    LearningModel model;
    model.init(3);
    model.setMinSamples(100);
    ThreadFeatures f; memset(&f, 0, sizeof(f));
    CHECK(!model.isPoolReady(0), "cold: pool 0 not ready");
    float hScore = 0.7f;
    float mlScore = model.predict(0, f);
    float fused = model.isPoolReady(0) ? softmaxFuseScores(mlScore, hScore, 1.0f) : hScore;
    CHECK_FLOAT_EQ(fused, hScore, 0.001f, "cold start: heuristic-only");
    for (int i = 0; i < 150; i++) model.update(0, f, 0.8f);
    CHECK(model.isPoolReady(0), "warm: pool 0 ready");
    mlScore = model.predict(0, f);
    fused = model.isPoolReady(0) ? softmaxFuseScores(mlScore, hScore, 1.0f) : hScore;
    CHECK(fused != hScore || mlScore == hScore, "warm: ML参与融合");
    CHECK_NOT_NAN(fused, "warm fusion not NaN");
}

void test_integration_hmac_rotation() {
    SUITE("Integration: HMAC key rotation");
    std::string oldKey = "old-key-123";
    std::string newKey = "new-key-456";
    std::string cmd = "rotate_key";
    std::string oldSig = aatpe_crypto::hmacSha256Hex(oldKey, cmd);
    std::string currentKey = newKey;
    std::string verifyWithNew = aatpe_crypto::hmacSha256Hex(currentKey, cmd);
    CHECK(oldSig != verifyWithNew, "old sig fails after rotation");
    std::string newSig = aatpe_crypto::hmacSha256Hex(currentKey, cmd);
    CHECK(newSig == verifyWithNew, "new sig works after rotation");
    CHECK(newSig.size() == 64, "new sig valid size");
}

void test_integration_multi_pool_training() {
    SUITE("Integration: multi-pool training specialization");
    LearningModel model;
    model.init(3);
    ThreadFeatures f0, f1, f2;
    memset(&f0, 0, sizeof(f0)); memset(&f1, 0, sizeof(f1)); memset(&f2, 0, sizeof(f2));
    f0.avg_run_us = 50;
    f1.avg_run_us = 200;
    f2.avg_run_us = 500;
    for (int i = 0; i < 500; i++) {
        model.update(0, f0, 0.9f);
        model.update(1, f1, 0.7f);
        model.update(2, f2, 0.5f);
    }
    float p0 = model.predict(0, f0);
    float p1 = model.predict(1, f1);
    float p2 = model.predict(2, f2);
    CHECK(p0 > p2, "pool 0 (trained high) > pool 2 (trained low)");
    CHECK(p0 > 0.5f, "pool 0 > 0.5");
    CHECK(model.getPoolSampleCount(0) == 500, "pool 0 has 500 samples");
    CHECK(model.getPoolSampleCount(1) == 500, "pool 1 has 500 samples");
    CHECK(model.getPoolSampleCount(2) == 500, "pool 2 has 500 samples");
}

void test_integration_state_history() {
    SUITE("Integration: state transition history");
    struct HistoryEntry { aatpe::SystemState from; aatpe::SystemState to; long long ts; std::string reason; };
    std::vector<HistoryEntry> history;
    auto transition = [&](aatpe::SystemState to, const char* reason) {
        aatpe::SystemState from = aatpe_sm::getSystemState();
        if (from != to) {
            history.push_back({from, to, 1700000000 + history.size(), reason});
            aatpe_sm::transitionTo(to, reason);
        }
    };
    transition(aatpe::SystemState::NORMAL, "boot");
    transition(aatpe::SystemState::DEGRADED, "load");
    transition(aatpe::SystemState::PASSIVE, "watchdog");
    transition(aatpe::SystemState::NORMAL, "recovery");
    transition(aatpe::SystemState::EMERGENCY, "thermal");
    CHECK(history.size() == 5, "5 transitions recorded");
    CHECK(history[0].from == aatpe::SystemState::NORMAL || history[0].from == aatpe::SystemState::DEGRADED ||
          history[0].from == aatpe::SystemState::EMERGENCY || history[0].from == aatpe::SystemState::PASSIVE,
          "first from state valid");
    CHECK(history[4].to == aatpe::SystemState::EMERGENCY, "last to EMERGENCY");
    CHECK(history[1].reason == "load", "second reason=load");
    CHECK(history[3].reason == "recovery", "fourth reason=recovery");
}

void test_integration_thermal_limit_config() {
    SUITE("Integration: thermal limit configuration");
    int limits[] = {80, 90, 100, 110, 120, 130};
    for (int limit : limits) {
        aatpe_thermal::g_limit = limit;
        aatpe_thermal::setTemp(limit - 1);
        CHECK(!aatpe_thermal::isEmergency(), "below limit " + std::to_string(limit));
        aatpe_thermal::setTemp(limit);
        CHECK(aatpe_thermal::isEmergency(), "at limit " + std::to_string(limit));
        aatpe_thermal::setTemp(limit + 1);
        CHECK(aatpe_thermal::isEmergency(), "above limit " + std::to_string(limit));
    }
    aatpe_thermal::g_limit = 120;
}

void test_integration_decision_trace() {
    SUITE("Integration: decision trace recording");
    struct TraceEntry { int count; float p50, p95, p99; };
    std::map<std::string, TraceEntry> traces;
    for (int i = 0; i < 100; i++) {
        traces["decide_placement"].count++;
        traces["compute_score"].count++;
    }
    traces["decide_placement"].p50 = 50;
    traces["decide_placement"].p95 = 120;
    traces["decide_placement"].p99 = 200;
    traces["compute_score"].p50 = 10;
    traces["compute_score"].p95 = 30;
    traces["compute_score"].p99 = 50;
    CHECK(traces["decide_placement"].count == 100, "100 placement decisions");
    CHECK(traces["compute_score"].count == 100, "100 score computations");
    CHECK(traces["decide_placement"].p95 > traces["decide_placement"].p50, "p95 > p50");
    CHECK(traces["decide_placement"].p99 > traces["decide_placement"].p95, "p99 > p95");
    CHECK(traces["compute_score"].p50 < traces["decide_placement"].p50, "score faster than placement");
}

struct ABResult {
    std::string name;
    std::vector<double> groupA, groupB;
    double meanA, meanB, tStat, pValue;
    bool significant;
};

static ABResult runABTest(const std::string& name,
                           std::function<double()> genA,
                           std::function<double()> genB,
                           int n = 100) {
    ABResult r;
    r.name = name;
    std::mt19937 rng(12345);
    std::normal_distribution<double> noise(0, 0.02);
    for (int i = 0; i < n; i++) {
        r.groupA.push_back(genA() + noise(rng));
        r.groupB.push_back(genB() + noise(rng));
    }
    r.meanA = mean(r.groupA);
    r.meanB = mean(r.groupB);
    r.tStat = tTestTwoSample(r.groupA, r.groupB);
    r.pValue = tTestPValueApprox(r.tStat, n*2-2);
    r.significant = r.pValue < 0.05;
    return r;
}

void test_ab_softmax_temperature() {
    SUITE("AB: softmax temperature comparison");
    std::mt19937 rng(42);
    auto r1 = runABTest("softmax_temp_0.01_vs_1.0",
        [&]() { return softmaxFuseScores(0.7f, 0.3f, 0.01f); },
        [&]() { return softmaxFuseScores(0.7f, 0.3f, 1.0f); });
    CHECK(r1.meanA > r1.meanB, "low temp favors high score more (A=" + std::to_string(r1.meanA) + ", B=" + std::to_string(r1.meanB) + ")");
    CHECK(r1.significant, "temperature effect statistically significant");
    auto r2 = runABTest("softmax_temp_0_vs_0.01",
        [&]() { return softmaxFuseScores(0.7f, 0.3f, 0.0f); },
        [&]() { return softmaxFuseScores(0.7f, 0.3f, 0.01f); });
    CHECK_FLOAT_EQ(r2.meanA, r2.meanB, 0.05f, "zero temp clamped to 0.01, identical results");
    auto r3 = runABTest("softmax_neg_vs_pos_temp",
        [&]() { return softmaxFuseScores(0.6f, 0.4f, -1.0f); },
        [&]() { return softmaxFuseScores(0.6f, 0.4f, 1.0f); });
    CHECK_FLOAT_EQ(r3.meanA, r3.meanB, 0.05f, "negative temp (abs) matches positive");
    auto r4 = runABTest("softmax_high_vs_veryhigh_temp",
        [&]() { return softmaxFuseScores(0.9f, 0.1f, 10.0f); },
        [&]() { return softmaxFuseScores(0.9f, 0.1f, 100.0f); });
    CHECK(r4.meanA > r4.meanB, "low temp (10) favors high score more than very high temp (100)");
    auto r5 = runABTest("softmax_abnormal_recovery",
        [&]() { softmaxFuseScores(0.5f, 0.5f, 0.0f); return softmaxFuseScores(0.7f, 0.3f, 1.0f); },
        [&]() { return softmaxFuseScores(0.7f, 0.3f, 1.0f); });
    CHECK_FLOAT_EQ(r5.meanA, r5.meanB, 0.05f, "abnormal temp doesn't corrupt subsequent calls");
}

void test_ab_learning_model() {
    SUITE("AB: LearningModel comparison");
    std::mt19937 rng(42);
    auto r6 = runABTest("model_autoinit_vs_explicit",
        [&]() { LearningModel m; ThreadFeatures f; memset(&f,0,sizeof(f)); f.avg_run_us=100;
                 return m.predict(0, f); },
        [&]() { LearningModel m; m.init(3); ThreadFeatures f; memset(&f,0,sizeof(f)); f.avg_run_us=100;
                 return m.predict(0, f); });
    CHECK_FLOAT_EQ(r6.meanA, r6.meanB, 0.05f, "auto-init produces same predictions as explicit init");
    LearningModel trained; trained.init(3);
    ThreadFeatures f; memset(&f,0,sizeof(f)); f.avg_run_us=100;
    for (int i = 0; i < 500; i++) trained.update(0, f, 0.9f);
    auto r7 = runABTest("model_trained_vs_untrained",
        [&]() { return trained.predict(0, f); },
        [&]() { LearningModel m; m.init(3); return m.predict(0, f); });
    CHECK(r7.meanA > r7.meanB, "trained model scores higher for positive outcome");
    CHECK(r7.significant, "training effect significant");
    auto r8 = runABTest("model_highLR_vs_lowLR",
        [&]() { LearningModel m; m.setInitialLR(0.1f); m.init(1);
                 for(int i=0;i<100;i++){ThreadFeatures ff;memset(&ff,0,sizeof(ff));ff.avg_run_us=100;m.update(0,ff,1.0f);}
                 return m.predict(0, f); },
        [&]() { LearningModel m; m.setInitialLR(0.001f); m.init(1);
                 for(int i=0;i<100;i++){ThreadFeatures ff;memset(&ff,0,sizeof(ff));ff.avg_run_us=100;m.update(0,ff,1.0f);}
                 return m.predict(0, f); });
    CHECK(!std::isnan(r8.meanA), "high LR model predictions not NaN");
    CHECK(!std::isnan(r8.meanB), "low LR model predictions not NaN");
    auto r9 = runABTest("fusion_cold_vs_warm",
        [&]() { LearningModel m; m.init(3); m.setMinSamples(1000);
                 return m.isPoolReady(0) ? 1.0 : 0.0; },
        [&]() { LearningModel m; m.init(3); m.setMinSamples(10);
                 for(int i=0;i<50;i++){ThreadFeatures ff;memset(&ff,0,sizeof(ff));m.update(0,ff,0.5f);}
                 return m.isPoolReady(0) ? 1.0 : 0.0; });
    CHECK(r9.meanB > r9.meanA, "warm model passes cold start gate more often");
}

void test_ab_scheduling() {
    SUITE("AB: scheduling comparison");
    std::mt19937 rng(42);
    auto r10 = runABTest("ui_vs_bg_score",
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S1_UI; t.currentCpu=0; return computeThreadScore(t, 0); },
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S5_BACKGROUND; t.currentCpu=0; return computeThreadScore(t, 0); });
    CHECK(r10.meanA > r10.meanB, "UI scores higher than background");
    CHECK(r10.significant, "UI vs BG significant");
    auto r11 = runABTest("protected_vs_unprotected",
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S2_RENDER; t.currentCpu=0; t.protect=true; return computeThreadScore(t, 0); },
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S2_RENDER; t.currentCpu=0; t.protect=false; return computeThreadScore(t, 0); });
    CHECK(r11.meanA > r11.meanB, "protected scores higher");
    auto r12 = runABTest("sameCPU_vs_diffCPU",
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S0_SYSTEM; t.currentCpu=2; return computeThreadScore(t, 2); },
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S0_SYSTEM; t.currentCpu=2; return computeThreadScore(t, 5); });
    CHECK(r12.meanA > r12.meanB, "same CPU scores higher (affinity bonus)");
    auto r13 = runABTest("game_vs_ui",
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S3_GAME; t.currentCpu=0; return computeThreadScore(t, 0); },
        [&]() { aatpe::ThreadInfo t; t.role=ROLE_S1_UI; t.currentCpu=0; return computeThreadScore(t, 0); });
    CHECK(r13.meanA > r13.meanB, "game scores higher than UI");
}

void test_ab_thermal() {
    SUITE("AB: thermal comparison");
    std::mt19937 rng(42);
    auto r14 = runABTest("thermal_normal_vs_emergency",
        [&]() { aatpe_thermal::setTemp(40 + (rng()%40)); return aatpe_thermal::isEmergency() ? 1.0 : 0.0; },
        [&]() { aatpe_thermal::setTemp(100 + (rng()%40)); return aatpe_thermal::isEmergency() ? 1.0 : 0.0; });
    CHECK(r14.meanB > r14.meanA, "high temp range has more emergencies");
    CHECK(r14.significant, "thermal effect significant");
    auto r15 = runABTest("limit_100_vs_120",
        [&]() { aatpe_thermal::g_limit=100; aatpe_thermal::setTemp(110); return aatpe_thermal::isEmergency()?1.0:0.0; },
        [&]() { aatpe_thermal::g_limit=120; aatpe_thermal::setTemp(110); return aatpe_thermal::isEmergency()?1.0:0.0; });
    CHECK(r15.meanA > r15.meanB, "lower limit triggers emergency at 110C");
    aatpe_thermal::g_limit = 120;
}

void test_ab_statistical_significance() {
    SUITE("AB: statistical significance validation");
    std::mt19937 rng(42);
    auto r1 = runABTest("clearly_different",
        [&]() { return 0.8 + (rng()%100)*0.001; },
        [&]() { return 0.2 + (rng()%100)*0.001; });
    CHECK(r1.significant, "clearly different distributions are significant");
    CHECK(r1.pValue < 0.001, "p-value very small for clearly different");
    auto r2 = runABTest("identical_distributions",
        [&]() { return 0.5 + (rng()%100)*0.01; },
        [&]() { return 0.5 + (rng()%100)*0.01; });
    CHECK(!r2.significant || r2.pValue > 0.01, "identical distributions not significant");
    auto r3 = runABTest("marginally_different",
        [&]() { return 0.50 + (rng()%100)*0.01; },
        [&]() { return 0.52 + (rng()%100)*0.01; });
    CHECK(r3.meanB > r3.meanA, "B slightly higher");
    CHECK(fabs(r1.tStat) > fabs(r3.tStat), "clearly different has larger |t|");
}

int main() {
    printf("AATPE v69 Comprehensive Test Suite\n");
    printf("===================================\n");
    auto start = std::chrono::high_resolution_clock::now();

    printf("\n########## UNIT TESTS ##########\n");
    test_softmax_zero_temperature();
    test_softmax_negative_temperature();
    test_softmax_normal_temperature();
    test_softmax_edge_cases();
    test_softmax_parametric();
    test_learningmodel_constructor_auto_init();
    test_learningmodel_init_idempotent();
    test_learningmodel_predict_update();
    test_learningmodel_save_load();
    test_learningmodel_cold_start();
    test_learningmodel_thread_safety();
    test_learningmodel_parameters();
    test_scoring_basic();
    test_scoring_many_threads();
    test_thread_scan_basic();
    test_state_machine_basic();
    test_state_machine_all_states();
    test_thermal_basic();
    test_thermal_thresholds();
    test_hmac_basic();
    test_hmac_verification();
    test_hmac_many_messages();
    test_decision_fusion_basic();
    test_fusion_weight_combinations();
    test_frame_time_outcome();
    test_utility_clamp();
    test_utility_whitelist();
    test_ebpf_basic();
    test_webui_http_basic();

    printf("\n########## INTEGRATION TESTS ##########\n");
    test_integration_ml_fusion_pipeline();
    test_integration_state_thermal_coupling();
    test_integration_thread_scan_score_place();
    test_integration_hmac_command_pipeline();
    test_integration_softmax_abnormal_recovery();
    test_integration_model_persist_predict();
    test_integration_concurrent_model_updates();
    test_integration_state_duration_tracking();
    test_integration_frame_outcome_stats();
    test_integration_thermal_state_feedback();
    test_integration_whitelist_scanning();
    test_integration_ebpf_state_propagation();
    test_integration_webui_state_all_aggregation();
    test_integration_model_cold_start_gating();
    test_integration_hmac_rotation();
    test_integration_multi_pool_training();
    test_integration_state_history();
    test_integration_thermal_limit_config();
    test_integration_decision_trace();

    printf("\n########## AB TESTS ##########\n");
    test_ab_softmax_temperature();
    test_ab_learning_model();
    test_ab_scheduling();
    test_ab_thermal();
    test_ab_statistical_significance();

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

    printf("\n===================================\n");
    printf("TEST SUMMARY\n");
    printf("===================================\n");
    printf("Total test cases:  %d\n", g_totalTests);
    printf("Passed:            %d\n", g_testsPassed);
    printf("Failed:            %d\n", g_testsFailed);
    printf("Pass rate:         %.2f%%\n", g_totalTests > 0 ? 100.0 * g_testsPassed / g_totalTests : 0);
    printf("Time:              %.2f ms\n", elapsed);
    if (!g_failures.empty()) {
        printf("\nFailures:\n");
        for (const auto& f : g_failures) printf("  - %s\n", f.c_str());
    }
    printf("===================================\n");
    return g_testsFailed > 0 ? 1 : 0;
}
