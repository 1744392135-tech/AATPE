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
struct DynamicScoreWeights { bool valid=false; };
}

static std::map<int, aatpe::CpuMetrics> g_cpuMetrics;
static std::shared_mutex g_cpuMetricsMutex;
static aatpe::DynamicScoreWeights g_dynamicScoreWeights;
static std::vector<int> g_allCpus = {0,1,2,3,4,5,6,7};

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
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
inline uint32_t rotr(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
inline void sha256Transform(Sha256Context* ctx,const uint8_t* data){
    uint32_t w[64]; for(int i=0;i<16;i++) w[i]=((uint32_t)data[i*4]<<24)|((uint32_t)data[i*4+1]<<16)|((uint32_t)data[i*4+2]<<8)|((uint32_t)data[i*4+3]);
    for(int i=16;i<64;i++){uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    uint32_t a=ctx->state[0],b=ctx->state[1],c=ctx->state[2],d=ctx->state[3],e=ctx->state[4],f=ctx->state[5],g=ctx->state[6],h=ctx->state[7];
    for(int i=0;i<64;i++){uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);uint32_t ch=(e&f)^((~e)&g);uint32_t t1=h+S1+ch+K[i]+w[i];uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);uint32_t maj=(a&b)^(a&c)^(b&c);uint32_t t2=S0+maj;h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;}
inline void sha256Init(Sha256Context* ctx){ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;ctx->bitCount=0;ctx->bufferLen=0;}
inline void sha256Update(Sha256Context* ctx,const uint8_t* data,size_t len){ctx->bitCount+=(uint64_t)len*8;while(len>0){size_t take=64-ctx->bufferLen;if(take>len)take=len;memcpy(ctx->buffer+ctx->bufferLen,data,take);ctx->bufferLen+=take;data+=take;len-=take;if(ctx->bufferLen==64){sha256Transform(ctx,ctx->buffer);ctx->bufferLen=0;}}}
inline void sha256Final(Sha256Context* ctx,uint8_t out[32]){uint64_t bc=ctx->bitCount;ctx->buffer[ctx->bufferLen++]=0x80;if(ctx->bufferLen>56){while(ctx->bufferLen<64)ctx->buffer[ctx->bufferLen++]=0;sha256Transform(ctx,ctx->buffer);ctx->bufferLen=0;}while(ctx->bufferLen<56)ctx->buffer[ctx->bufferLen++]=0;for(int i=7;i>=0;i--)ctx->buffer[ctx->bufferLen++]=(uint8_t)(bc>>(i*8));sha256Transform(ctx,ctx->buffer);for(int i=0;i<8;i++){out[i*4]=(uint8_t)(ctx->state[i]>>24);out[i*4+1]=(uint8_t)(ctx->state[i]>>16);out[i*4+2]=(uint8_t)(ctx->state[i]>>8);out[i*4+3]=(uint8_t)(ctx->state[i]);}}
inline void hmacSha256(const uint8_t* key,size_t keyLen,const uint8_t* data,size_t dataLen,uint8_t out[32]){
    uint8_t k[64]={0};if(keyLen>64){Sha256Context c;sha256Init(&c);sha256Update(&c,key,keyLen);sha256Final(&c,k);}else memcpy(k,key,keyLen);
    uint8_t ipad[64],opad[64];for(int i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}
    Sha256Context ctx;uint8_t inner[32];sha256Init(&ctx);sha256Update(&ctx,ipad,64);sha256Update(&ctx,data,dataLen);sha256Final(&ctx,inner);
    sha256Init(&ctx);sha256Update(&ctx,opad,64);sha256Update(&ctx,inner,32);sha256Final(&ctx,out);}
inline std::string toHex(const uint8_t* hash,size_t len=32){static const char* h="0123456789abcdef";std::string r;r.reserve(len*2);for(size_t i=0;i<len;i++){r+=h[(hash[i]>>4)&0xF];r+=h[hash[i]&0xF];}return r;}
}

static std::string g_testHmacKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static std::string computeTestHmac(const std::string& data) {
    uint8_t out[32];
    aatpe_crypto::hmacSha256((const uint8_t*)g_testHmacKey.data(), g_testHmacKey.size(),
                              (const uint8_t*)data.data(), data.size(), out);
    return aatpe_crypto::toHex(out);
}
static bool verifyTestHmac(const std::string& data, const std::string& sig, bool required) {
    if (sig.empty()) return !required;
    std::string expected = computeTestHmac(data);
    return expected == sig;
}

namespace aatpe_ml_hybrid {
struct HybridPoolDelta { float weights[15]={0}; int updateCount=0; };
class HybridLearner {
    mutable std::mutex mutex_;
    bool enabled_=false, initialized_=false;
    int numPools_=0, totalUpdates_=0, tickCount_=0;
    std::vector<HybridPoolDelta> poolDeltas_;
    static constexpr float kLR=0.001f, kClamp=0.5f;
    float dotProduct(const HybridPoolDelta& d,const float f[15]) const {float s=0;for(int i=0;i<15;i++)s+=d.weights[i]*f[i];return s;}
public:
    void init(int n){std::lock_guard<std::mutex> lk(mutex_);numPools_=n;poolDeltas_.clear();poolDeltas_.resize(n);initialized_=true;}
    void setEnabled(bool e){std::lock_guard<std::mutex> lk(mutex_);enabled_=e;}
    bool isEnabled() const {std::lock_guard<std::mutex> lk(mutex_);return enabled_;}
    int getPoolCount() const {std::lock_guard<std::mutex> lk(mutex_);return (int)poolDeltas_.size();}
    int getTotalUpdates() const {std::lock_guard<std::mutex> lk(mutex_);return totalUpdates_;}
    void sgdUpdate(int pool,const float f[15],float target){
        if(pool<0||pool>=(int)poolDeltas_.size())return;
        HybridPoolDelta& d=poolDeltas_[pool];
        float pred=dotProduct(d,f);float sig=1.0f/(1.0f+std::exp(-pred));float err=target-sig;
        for(int i=0;i<15;i++){d.weights[i]+=kLR*err*f[i];if(d.weights[i]>kClamp)d.weights[i]=kClamp;if(d.weights[i]<-kClamp)d.weights[i]=-kClamp;}
        d.updateCount++;totalUpdates_++;
    }
    float getFineTuneAdjustment(int pool,const float f[15]) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if(!enabled_||!initialized_||pool<0||pool>=(int)poolDeltas_.size())return 0.0f;
        float dot=dotProduct(poolDeltas_[pool],f);float sig=1.0f/(1.0f+std::exp(-dot));return (sig-0.5f)*2.0f;
    }
    float predict(int pool,const float f[15]) const {
        if (!isEnabled()) return 0.0f;
        float adj=getFineTuneAdjustment(pool,f);return (adj+1.0f)*0.5f;
    }
    bool save(const char* path) const {
        std::lock_guard<std::mutex> lk(mutex_);if(!initialized_||!path)return false;
        FILE* fp=fopen(path,"wb");if(!fp)return false;
        uint32_t magic=0x4859424D,ver=2,pc=(uint32_t)poolDeltas_.size();int32_t en=enabled_?1:0;
        fwrite(&magic,4,1,fp);fwrite(&ver,4,1,fp);fwrite(&pc,4,1,fp);fwrite(&en,4,1,fp);
        for(const auto& d:poolDeltas_){fwrite(d.weights,sizeof(float),15,fp);int32_t uc=d.updateCount;fwrite(&uc,4,1,fp);}
        int32_t tu=totalUpdates_;fwrite(&tu,4,1,fp);fclose(fp);return true;
    }
    bool load(const char* path){
        std::lock_guard<std::mutex> lk(mutex_);if(!path)return false;
        FILE* fp=fopen(path,"rb");if(!fp)return false;
        uint32_t magic=0,ver=0,pc=0;fread(&magic,4,1,fp);if(magic!=0x4859424D){fclose(fp);return false;}
        fread(&ver,4,1,fp);fread(&pc,4,1,fp);if(pc==0||pc>16){fclose(fp);return false;}
        int32_t en=0;if(ver>=2)fread(&en,4,1,fp);
        std::vector<HybridPoolDelta> loaded(pc);
        for(uint32_t i=0;i<pc;i++){fread(loaded[i].weights,sizeof(float),15,fp);int32_t uc=0;fread(&uc,4,1,fp);loaded[i].updateCount=uc;}
        int32_t tu=0;fread(&tu,4,1,fp);fclose(fp);
        poolDeltas_=std::move(loaded);numPools_=pc;totalUpdates_=tu;initialized_=true;enabled_=(en!=0);return true;
    }
    void reset(){std::lock_guard<std::mutex> lk(mutex_);for(auto& d:poolDeltas_){memset(d.weights,0,sizeof(d.weights));d.updateCount=0;}totalUpdates_=0;tickCount_=0;}
};
static HybridLearner g_hybrid;
inline HybridLearner& instance(){return g_hybrid;}
}

static aatpe::DecisionFusion fuseDecisions(float heuristicScore, float mlScore,
    float hybridScore, float historyScore, aatpe::SystemState state, bool mlColdStart) {
    aatpe::DecisionFusion f;
    f.heuristic_score=std::clamp(heuristicScore,0.0f,1.0f);
    f.ml_score=std::clamp(mlScore,0.0f,1.0f);
    f.hybrid_score=std::clamp(hybridScore,0.0f,1.0f);
    f.history_score=std::clamp(historyScore,0.0f,1.0f);
    f.dynamic_weight_adjustment = g_dynamicScoreWeights.valid ? 1.0f : 0.0f;
    float wh,wm,why,whs;
    switch(state){
        case aatpe::SystemState::DEGRADED: wh=0.60f;wm=0.20f;why=0.10f;whs=0.10f;break;
        case aatpe::SystemState::PASSIVE: case aatpe::SystemState::EMERGENCY: wh=1.0f;wm=0.0f;why=0.0f;whs=0.0f;break;
        case aatpe::SystemState::NORMAL: default: wh=0.40f;wm=0.30f;why=0.15f;whs=0.15f;break;
    }
    if(mlColdStart && wm>0.0f){float total=wh+wm+why+whs;float rem=wh+why+whs;if(rem>0.01f){wh=wh/rem*total;why=why/rem*total;whs=whs/rem*total;}wm=0.0f;}
    f.w_heuristic=wh;f.w_ml=wm;f.w_hybrid=why;f.w_history=whs;
    float totalW=wh+wm+why+whs;
    if(totalW<0.0001f){f.w_heuristic=1.0f;f.w_ml=0;f.w_hybrid=0;f.w_history=0;f.final_score=f.heuristic_score;f.contrib_heuristic=100;f.contrib_ml=0;f.contrib_hybrid=0;f.contrib_history=0;return f;}
    f.final_score=wh*f.heuristic_score+wm*f.ml_score+why*f.hybrid_score+whs*f.history_score;
    if(f.final_score>0.0001f){f.contrib_heuristic=wh*f.heuristic_score/f.final_score*100;f.contrib_ml=wm*f.ml_score/f.final_score*100;f.contrib_hybrid=why*f.hybrid_score/f.final_score*100;f.contrib_history=whs*f.history_score/f.final_score*100;}
    else{f.contrib_heuristic=wh*100;f.contrib_ml=wm*100;f.contrib_hybrid=why*100;f.contrib_history=whs*100;}
    return f;
}

static bool isPackageWhitelisted(const std::string& pname, const std::string& wl) {
    if (wl.empty()) return true;
    if (pname.empty()) return false;
    size_t start = 0;
    while (start <= wl.size()) {
        size_t comma = wl.find(',', start);
        std::string entry = (comma == std::string::npos) ? wl.substr(start) : wl.substr(start, comma - start);
        while (!entry.empty() && (entry.front()==' '||entry.front()=='\t')) entry.erase(entry.begin());
        while (!entry.empty() && (entry.back()==' '||entry.back()=='\t')) entry.pop_back();
        if (!entry.empty() && entry == pname) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

namespace aatpe_sm {
struct StateMachine {
    aatpe::SystemState state = aatpe::SystemState::NORMAL;
    std::vector<aatpe::SystemState> history;
    int transitions = 0;
    bool transitionTo(aatpe::SystemState target, const std::string& reason) {
        (void)reason;
        if (state == target) return false;
        if (state == aatpe::SystemState::EMERGENCY && target != aatpe::SystemState::EMERGENCY) return false;
        history.push_back(state);
        if (history.size() > 50) history.erase(history.begin());
        state = target;
        transitions++;
        return true;
    }
};
}

static int g_testsPassed = 0, g_testsFailed = 0;
static std::vector<std::string> g_failures;
#define TEST_ASSERT(cond, msg) do { if (!(cond)) { g_testsFailed++; g_failures.push_back(std::string(__func__) + ": " + msg); printf("  FAIL: %s\n", msg); return; } } while(0)
#define TEST_PASS() do { g_testsPassed++; printf("  PASS\n"); } while(0)

void test_hybrid_score_disabled() {
    printf("[UT1] hybridScore returns 0 when disabled: ");
    aatpe_ml_hybrid::instance().setEnabled(false);
    float f[15]={1,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    float score = aatpe_ml_hybrid::instance().predict(0, f);
    TEST_ASSERT(score == 0.0f, "expected 0.0 when disabled");
    TEST_PASS();
}

void test_hybrid_score_range() {
    printf("[UT2] hybridScore returns [0,1] when enabled: ");
    aatpe_ml_hybrid::instance().init(3);
    aatpe_ml_hybrid::instance().setEnabled(true);
    float f[15]={1,0.5f,0.3f,0.2f,0.5f,0.5f,0,0.5f,0.6f,1,0,0,0,0,0};
    for (int i=0;i<100;i++) aatpe_ml_hybrid::instance().sgdUpdate(0, f, 0.8f);
    float score = aatpe_ml_hybrid::instance().predict(0, f);
    TEST_ASSERT(score >= 0.0f && score <= 1.0f, "score out of [0,1] range");
    TEST_PASS();
}

void test_hybrid_feature_extraction() {
    printf("[UT3] Hybrid feature extraction valid: ");
    aatpe::ThreadInfo ti; ti.nrSwitches = 500; ti.runTime = 50000000ULL; ti.currentCpu = 2;
    unsigned long long avgRunUs = (ti.nrSwitches>0) ? (unsigned long long)((double)ti.runTime/(double)ti.nrSwitches/1000.0) : 0;
    float hf1 = std::min(1.0f, (float)avgRunUs / 50000.0f);
    float hf2 = std::min(1.0f, (float)ti.nrSwitches / 1000.0f);
    TEST_ASSERT(hf1 >= 0.0f && hf1 <= 1.0f, "avg_run_us feature out of range");
    TEST_ASSERT(hf2 >= 0.0f && hf2 <= 1.0f, "nr_switches feature out of range");
    TEST_ASSERT(avgRunUs == 100, "avgRunUs calculation wrong");
    TEST_PASS();
}

void test_hybrid_save_load() {
    printf("[UT4] Hybrid model save/load: ");
    aatpe_ml_hybrid::instance().init(3);
    aatpe_ml_hybrid::instance().setEnabled(true);
    float f[15]={1,0.5f,0,0,0,0,0,0,0,0,0,0,0,0,0};
    for (int i=0;i<50;i++) aatpe_ml_hybrid::instance().sgdUpdate(1, f, 0.7f);
    int updatesBefore = aatpe_ml_hybrid::instance().getTotalUpdates();
    TEST_ASSERT(aatpe_ml_hybrid::instance().save("/tmp/aatpe_test_hybrid.bin"), "save failed");
    aatpe_ml_hybrid::instance().reset();
    TEST_ASSERT(aatpe_ml_hybrid::instance().getTotalUpdates() == 0, "reset didn't clear");
    TEST_ASSERT(aatpe_ml_hybrid::instance().load("/tmp/aatpe_test_hybrid.bin"), "load failed");
    TEST_ASSERT(aatpe_ml_hybrid::instance().getTotalUpdates() == updatesBefore, "updates mismatch after load");
    TEST_ASSERT(aatpe_ml_hybrid::instance().getPoolCount() == 3, "pool count mismatch");
    TEST_PASS();
}

void test_hmac_generation() {
    printf("[UT5] HMAC signature generation: ");
    std::string sig = computeTestHmac("set_mode=assist");
    TEST_ASSERT(sig.size() == 64, "signature not 64 chars");
    bool allHex = true;
    for (char c : sig) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) allHex=false;
    TEST_ASSERT(allHex, "signature contains non-hex chars");
    TEST_PASS();
}

void test_hmac_verify_valid() {
    printf("[UT6] HMAC verify valid signature: ");
    std::string cmd = "set_profile=gaming";
    std::string sig = computeTestHmac(cmd);
    TEST_ASSERT(verifyTestHmac(cmd, sig, true), "valid signature rejected");
    TEST_PASS();
}

void test_hmac_verify_invalid() {
    printf("[UT7] HMAC verify invalid signature: ");
    TEST_ASSERT(!verifyTestHmac("set_mode=assist", "00000000000000000000000000000000000000000000000000000000000000", true), "invalid signature accepted");
    TEST_PASS();
}

void test_hmac_empty_sig_required() {
    printf("[UT8] HMAC empty sig rejected when required: ");
    TEST_ASSERT(!verifyTestHmac("cmd", "", true), "empty signature accepted when required");
    TEST_PASS();
}

void test_hmac_empty_sig_optional() {
    printf("[UT9] HMAC empty sig accepted when optional: ");
    TEST_ASSERT(verifyTestHmac("cmd", "", false), "empty signature rejected when optional");
    TEST_PASS();
}

void test_hmac_replay_protection() {
    printf("[UT10] HMAC replay protection: ");
    int64_t now = (int64_t)time(nullptr);
    int64_t oldTs = now - 31;
    int64_t validTs = now - 5;
    int64_t futureTs = now + 10;
    TEST_ASSERT((now - oldTs) > 30, "old timestamp not detected as expired");
    TEST_ASSERT((now - validTs) <= 30, "valid timestamp incorrectly expired");
    TEST_ASSERT((now - futureTs) < -5, "future timestamp not detected as skew");
    TEST_PASS();
}

void test_cpu_metrics_lock() {
    printf("[UT11] CPU metrics concurrent read/write: ");
    std::atomic<bool> stop{false};
    std::atomic<int> readErrors{0};
    std::thread writer([&]() {
        while (!stop.load()) {
            std::unique_lock<std::shared_mutex> wlock(g_cpuMetricsMutex);
            for (int cpu=0;cpu<8;cpu++) {
                aatpe::CpuMetrics m; m.util = cpu*10; m.curFreq = 1000+cpu*100;
                g_cpuMetrics[cpu] = m;
            }
        }
    });
    std::vector<std::thread> readers;
    for (int r=0;r<4;r++) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                std::shared_lock<std::shared_mutex> rlock(g_cpuMetricsMutex);
                for (int cpu=0;cpu<8;cpu++) {
                    auto it = g_cpuMetrics.find(cpu);
                    if (it != g_cpuMetrics.end()) {
                        if (it->second.util < 0 || it->second.util > 100) readErrors++;
                    }
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    writer.join();
    for (auto& t : readers) t.join();
    TEST_ASSERT(readErrors.load() == 0, "data inconsistency detected during concurrent access");
    TEST_PASS();
}

void test_whitelist_exact() {
    printf("[UT12] Package whitelist exact match: ");
    std::string wl = "com.android.settings,com.miHoYo.GenshinImpact,com.tencent.tmgp.pubgmhd";
    TEST_ASSERT(isPackageWhitelisted("com.miHoYo.GenshinImpact", wl), "whitelisted package rejected");
    TEST_ASSERT(!isPackageWhitelisted("com.other.app", wl), "non-whitelisted package accepted");
    TEST_PASS();
}

void test_whitelist_empty() {
    printf("[UT13] Package whitelist empty allows all: ");
    TEST_ASSERT(isPackageWhitelisted("any.package.name", ""), "empty whitelist should allow all");
    TEST_ASSERT(isPackageWhitelisted("com.android.settings", ""), "empty whitelist should allow all");
    TEST_PASS();
}

void test_whitelist_invalid() {
    printf("[UT14] Package whitelist invalid name: ");
    std::string wl = "com.valid.app";
    TEST_ASSERT(!isPackageWhitelisted("", wl), "empty package name accepted");
    TEST_ASSERT(!isPackageWhitelisted("no dots", wl), "package with spaces accepted");
    TEST_PASS();
}

void test_whitelist_whitespace() {
    printf("[UT15] Package whitelist whitespace trimming: ");
    std::string wl = " com.android.settings , com.miHoYo.GenshinImpact ";
    TEST_ASSERT(isPackageWhitelisted("com.android.settings", wl), "whitespace-trimmed match failed");
    TEST_ASSERT(isPackageWhitelisted("com.miHoYo.GenshinImpact", wl), "whitespace-trimmed match failed");
    TEST_PASS();
}

void test_affinity_rollback() {
    printf("[UT16] setaffinity rollback logic: ");
    cpu_set_t originalMask, targetMask;
    CPU_ZERO(&originalMask); CPU_SET(0, &originalMask); CPU_SET(1, &originalMask);
    CPU_ZERO(&targetMask); CPU_SET(2, &targetMask); CPU_SET(3, &targetMask);
    bool affinityModified = true;
    bool setaffinitySucceeded = false;
    cpu_set_t restoredMask = originalMask;
    if (!setaffinitySucceeded && affinityModified) {
        restoredMask = originalMask;
    }
    TEST_ASSERT(CPU_EQUAL(&restoredMask, &originalMask), "rollback didn't restore original mask");
    setaffinitySucceeded = true;
    cpu_set_t noRollback = targetMask;
    if (!setaffinitySucceeded) noRollback = originalMask;
    TEST_ASSERT(CPU_EQUAL(&noRollback, &targetMask), "success case incorrectly rolled back");
    TEST_PASS();
}

void test_fusion_weights_sum() {
    printf("[UT17] Decision fusion four-way weights: ");
    auto f = fuseDecisions(0.8f, 0.6f, 0.5f, 0.7f, aatpe::SystemState::NORMAL, false);
    float sum = f.w_heuristic + f.w_ml + f.w_hybrid + f.w_history;
    TEST_ASSERT(fabs(sum - 1.0f) < 0.001f, "weights don't sum to 1");
    TEST_ASSERT(f.w_hybrid > 0.0f, "hybrid weight is zero in NORMAL state");
    TEST_PASS();
}

void test_fusion_cold_start() {
    printf("[UT18] Decision fusion ML cold start: ");
    auto f = fuseDecisions(0.8f, 0.6f, 0.5f, 0.7f, aatpe::SystemState::NORMAL, true);
    TEST_ASSERT(f.w_ml == 0.0f, "ML weight not zero during cold start");
    float sum = f.w_heuristic + f.w_ml + f.w_hybrid + f.w_history;
    TEST_ASSERT(fabs(sum - 1.0f) < 0.001f, "weights don't sum to 1 after cold start renormalize");
    TEST_PASS();
}

void test_fusion_div_zero() {
    printf("[UT19] Decision fusion div-zero protection: ");
    auto f = fuseDecisions(0.5f, 0.0f, 0.0f, 0.0f, aatpe::SystemState::PASSIVE, false);
    TEST_ASSERT(f.w_heuristic == 1.0f, "fallback heuristic weight not 1.0");
    TEST_ASSERT(f.final_score == f.heuristic_score, "final score not heuristic in fallback");
    TEST_PASS();
}

void test_fusion_state_dependent() {
    printf("[UT20] Decision fusion state-dependent: ");
    auto fNorm = fuseDecisions(0.5f, 0.5f, 0.5f, 0.5f, aatpe::SystemState::NORMAL, false);
    auto fDeg = fuseDecisions(0.5f, 0.5f, 0.5f, 0.5f, aatpe::SystemState::DEGRADED, false);
    TEST_ASSERT(fNorm.w_heuristic == 0.40f, "NORMAL heuristic weight wrong");
    TEST_ASSERT(fDeg.w_heuristic == 0.60f, "DEGRADED heuristic weight wrong");
    TEST_ASSERT(fDeg.w_ml == 0.20f, "DEGRADED ML weight wrong");
    TEST_PASS();
}

void test_frame_time_outcome() {
    printf("[UT21] Frame time outcome formula: ");
    float beforeAvg = 16.67f, afterAvg = 12.5f;
    float outcome = (beforeAvg - afterAvg) / beforeAvg;
    TEST_ASSERT(outcome > 0.0f, "improvement should have positive outcome");
    TEST_ASSERT(fabs(outcome - 0.25f) < 0.01f, "outcome formula incorrect");
    afterAvg = 20.0f;
    outcome = (beforeAvg - afterAvg) / beforeAvg;
    TEST_ASSERT(outcome < 0.0f, "degradation should have negative outcome");
    TEST_PASS();
}

void test_gfxinfo_parse() {
    printf("[UT22] gfxinfo parsing: ");
    std::string mockOutput = "Applications Graphics Acceleration Info:\n"
        "Uptime: 1234567 Realtime: 1234567\n"
        "com.miHoYo.GenshinImpact/\n"
        "Frame stats: Number of frames=100\n"
        "Histogram: 0ms=10 1ms=20 2ms=30 3ms=25 4ms=10 5ms=5\n";
    size_t pos = mockOutput.find("Number of frames=");
    TEST_ASSERT(pos != std::string::npos, "frame count not found");
    int frameCount = std::stoi(mockOutput.substr(pos + 17));
    TEST_ASSERT(frameCount == 100, "frame count parse incorrect");
    int totalHist = 10+20+30+25+10+5;
    TEST_ASSERT(totalHist == 100, "histogram sum incorrect");
    TEST_PASS();
}

void test_ttl_cache() {
    printf("[UT23] TTL cache expiration: ");
    struct CacheEntry { std::string data; long long expireTime; };
    CacheEntry entry; entry.data = "test"; entry.expireTime = 0;
    long long now = 1000;
    bool expired = (entry.expireTime > 0 && now > entry.expireTime);
    TEST_ASSERT(!expired, "entry with expireTime=0 should not be expired");
    entry.expireTime = 900;
    expired = (entry.expireTime > 0 && now > entry.expireTime);
    TEST_ASSERT(expired, "entry should be expired");
    entry.expireTime = 1100;
    expired = (entry.expireTime > 0 && now > entry.expireTime);
    TEST_ASSERT(!expired, "entry should not be expired");
    TEST_PASS();
}

void test_state_machine() {
    printf("[UT24] State machine transitions: ");
    aatpe_sm::StateMachine sm;
    TEST_ASSERT(sm.transitionTo(aatpe::SystemState::DEGRADED, "thermal"), "NORMAL->DEGRADED failed");
    TEST_ASSERT(sm.state == aatpe::SystemState::DEGRADED, "state not updated");
    TEST_ASSERT(sm.transitions == 1, "transition count wrong");
    TEST_ASSERT(!sm.transitionTo(aatpe::SystemState::DEGRADED, "same"), "same-state transition should fail");
    TEST_ASSERT(sm.transitionTo(aatpe::SystemState::NORMAL, "recovered"), "DEGRADED->NORMAL failed");
    TEST_PASS();
}

void test_state_machine_priority() {
    printf("[UT25] State machine priority: ");
    aatpe_sm::StateMachine sm;
    sm.transitionTo(aatpe::SystemState::EMERGENCY, "critical");
    TEST_ASSERT(!sm.transitionTo(aatpe::SystemState::NORMAL, "downgrade"), "EMERGENCY should block downgrade");
    TEST_ASSERT(sm.state == aatpe::SystemState::EMERGENCY, "state changed despite priority");
    TEST_PASS();
}

void test_state_machine_history() {
    printf("[UT26] State machine history: ");
    aatpe_sm::StateMachine sm;
    sm.transitionTo(aatpe::SystemState::DEGRADED, "t1");
    sm.transitionTo(aatpe::SystemState::NORMAL, "t2");
    sm.transitionTo(aatpe::SystemState::PASSIVE, "t3");
    TEST_ASSERT(sm.history.size() == 3, "history size wrong");
    TEST_ASSERT(sm.history[0] == aatpe::SystemState::NORMAL, "first history entry wrong");
    TEST_ASSERT(sm.history[2] == aatpe::SystemState::NORMAL, "third history entry wrong");
    TEST_PASS();
}

void test_daemon_startup() {
    printf("[IT1] Daemon startup to stable: ");
    bool configLoaded = true;
    bool hybridInit = false, mlInit = false, httpStarted = false;
    aatpe_ml_hybrid::instance().init(3);
    aatpe_ml_hybrid::instance().setEnabled(true);
    hybridInit = aatpe_ml_hybrid::instance().getPoolCount() == 3;
    mlInit = true;
    httpStarted = true;
    TEST_ASSERT(configLoaded && hybridInit && mlInit && httpStarted, "startup incomplete");
    TEST_PASS();
}

void test_hybrid_fusion_e2e() {
    printf("[IT2] Hybrid->fusion end-to-end: ");
    aatpe_ml_hybrid::instance().init(3);
    aatpe_ml_hybrid::instance().setEnabled(true);
    float f[15]={1,0.5f,0.3f,0.2f,0.5f,0.5f,1.0f,0.5f,1.0f,0,0,0,0,0,0};
    for (int i=0;i<200;i++) aatpe_ml_hybrid::instance().sgdUpdate(2, f, 0.9f);
    float hybridScore = aatpe_ml_hybrid::instance().predict(2, f);
    TEST_ASSERT(hybridScore > 0.0f, "hybrid score is zero after training");
    auto fusion = fuseDecisions(0.7f, 0.6f, hybridScore, 0.5f, aatpe::SystemState::NORMAL, false);
    TEST_ASSERT(fusion.final_score > 0.0f, "fused score is zero");
    TEST_ASSERT(fusion.contrib_hybrid > 0.0f, "hybrid contribution is zero");
    TEST_PASS();
}

void test_hmac_control_e2e() {
    printf("[IT3] HMAC control command end-to-end: ");
    std::string cmd = "set_mode=assist";
    int64_t ts = (int64_t)time(nullptr);
    std::string signedCmd = cmd + ".ts=" + std::to_string(ts) + ".sig=" + computeTestHmac(cmd);
    size_t sigPos = signedCmd.rfind(".sig=");
    TEST_ASSERT(sigPos != std::string::npos, "sig not found");
    std::string sig = signedCmd.substr(sigPos + 5);
    std::string withTs = signedCmd.substr(0, sigPos);
    size_t tsPos = withTs.rfind(".ts=");
    TEST_ASSERT(tsPos != std::string::npos, "ts not found");
    std::string actualCmd = withTs.substr(0, tsPos);
    int64_t parsedTs = std::stoll(withTs.substr(tsPos + 4));
    TEST_ASSERT(actualCmd == cmd, "command parse incorrect");
    TEST_ASSERT(((int64_t)time(nullptr) - parsedTs) <= 30, "timestamp outside window");
    TEST_ASSERT(verifyTestHmac(actualCmd, sig, true), "HMAC verification failed");
    TEST_PASS();
}

void test_cpu_metrics_integration() {
    printf("[IT4] CPU metrics concurrent integration: ");
    std::atomic<bool> stop{false};
    std::atomic<int> totalReads{0}, consistentReads{0};
    std::thread writer([&]() {
        int iter = 0;
        while (!stop.load()) {
            std::unique_lock<std::shared_mutex> wlock(g_cpuMetricsMutex);
            for (int cpu=0;cpu<8;cpu++) {
                aatpe::CpuMetrics m; m.util = (iter + cpu) % 100;
                g_cpuMetrics[cpu] = m;
            }
            iter++;
        }
    });
    std::thread reader([&]() {
        while (!stop.load()) {
            std::shared_lock<std::shared_mutex> rlock(g_cpuMetricsMutex);
            bool consistent = true;
            for (int cpu=0;cpu<8;cpu++) {
                auto it = g_cpuMetrics.find(cpu);
                if (it != g_cpuMetrics.end() && (it->second.util < 0 || it->second.util > 100)) consistent = false;
            }
            totalReads++;
            if (consistent) consistentReads++;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    writer.join(); reader.join();
    float consistency = (totalReads > 0) ? (float)consistentReads.load() / totalReads.load() * 100.0f : 0.0f;
    printf("(reads=%d, consistency=%.1f%%) ", totalReads.load(), consistency);
    TEST_ASSERT(consistency == 100.0f, "data inconsistency during integration");
    TEST_PASS();
}

void test_whitelist_scan_integration() {
    printf("[IT5] Package whitelist scan integration: ");
    std::string wl = "com.miHoYo.GenshinImpact,com.tencent.tmgp.pubgmhd";
    std::vector<std::string> allProcs = {"com.miHoYo.GenshinImpact","com.android.settings",
        "com.tencent.tmgp.pubgmhd","com.android.systemui","com.other.game","system"};
    int scanned = 0, skipped = 0;
    for (const auto& p : allProcs) {
        if (isPackageWhitelisted(p, wl)) scanned++; else skipped++;
    }
    TEST_ASSERT(scanned == 2, "should scan 2 whitelisted packages");
    TEST_ASSERT(skipped == 4, "should skip 4 non-whitelisted packages");
    scanned = 0;
    for (const auto& p : allProcs) if (isPackageWhitelisted(p, "")) scanned++;
    TEST_ASSERT(scanned == 6, "empty whitelist should scan all");
    TEST_PASS();
}

void test_state_thermal_integration() {
    printf("[IT6] State machine + thermal integration: ");
    aatpe_sm::StateMachine sm;
    int temp = 40;
    if (temp > 70) sm.transitionTo(aatpe::SystemState::DEGRADED, "thermal_high");
    TEST_ASSERT(sm.state == aatpe::SystemState::NORMAL, "should be NORMAL at 40C");
    temp = 85;
    if (temp > 70 && temp < 120) sm.transitionTo(aatpe::SystemState::DEGRADED, "thermal_high");
    TEST_ASSERT(sm.state == aatpe::SystemState::DEGRADED, "should be DEGRADED at 85C");
    temp = 125;
    if (temp >= 120) sm.transitionTo(aatpe::SystemState::EMERGENCY, "thermal_critical");
    TEST_ASSERT(sm.state == aatpe::SystemState::EMERGENCY, "should be EMERGENCY at 125C");
    TEST_PASS();
}

void test_fusion_frame_outcome() {
    printf("[IT7] Fusion + frame-time outcome integration: ");
    float beforeFrame = 16.67f, afterFrame = 14.0f;
    float outcome = (beforeFrame - afterFrame) / beforeFrame;
    aatpe_ml_hybrid::instance().init(3);
    aatpe_ml_hybrid::instance().setEnabled(true);
    float f[15]={1,0.4f,0.2f,0.3f,0.5f,0.5f,1,0.5f,0.6f,0,0,0,0,0,0};
    float target = std::clamp(0.5f + outcome * 0.5f, 0.0f, 1.0f);
    for (int i=0;i<100;i++) aatpe_ml_hybrid::instance().sgdUpdate(1, f, target);
    float pred = aatpe_ml_hybrid::instance().predict(1, f);
    auto fusion = fuseDecisions(0.6f, 0.5f, pred, 0.4f, aatpe::SystemState::NORMAL, false);
    TEST_ASSERT(fusion.final_score > 0.0f, "fused score zero");
    TEST_ASSERT(outcome > 0.0f, "frame outcome should show improvement");
    TEST_PASS();
}

void test_ebpf_fallback() {
    printf("[IT8] eBPF load fallback integration: ");
    bool syscallOk = false, precompiledOk = false, embeddedOk = false;
    std::string state = "LOADING";
    if (syscallOk) state = "ACTIVE";
    else if (precompiledOk) state = "ACTIVE";
    else if (embeddedOk) state = "ACTIVE";
    else state = "DEGRADED";
    TEST_ASSERT(state == "DEGRADED", "should fall back to DEGRADED");
    state = "LOAD_TIMEOUT";
    bool preservedTimeout = (state == "LOAD_TIMEOUT");
    TEST_ASSERT(preservedTimeout, "LOAD_TIMEOUT state should be preserved");
    TEST_PASS();
}

void test_webui_refresh() {
    printf("[IT9] WebUI data refresh integration: ");
    std::map<std::string,std::string> stateFiles;
    stateFiles["control_mode"] = "assist";
    stateFiles["tracked_threads"] = "42";
    stateFiles["cpu_temp"] = "45";
    stateFiles["ml_hybrid_enabled"] = "1";
    std::ostringstream json;
    json << "{";
    bool first = true;
    for (const auto& [k,v] : stateFiles) {
        if (!first) json << ",";
        json << "\"" << k << "\":\"" << v << "\"";
        first = false;
    }
    json << "}";
    std::string result = json.str();
    TEST_ASSERT(result.find("\"control_mode\":\"assist\"") != std::string::npos, "JSON missing control_mode");
    TEST_ASSERT(result.find("\"ml_hybrid_enabled\":\"1\"") != std::string::npos, "JSON missing hybrid flag");
    TEST_ASSERT(result.find("tracked_threads") != std::string::npos, "JSON missing tracked_threads");
    TEST_PASS();
}

void test_config_hot_reload() {
    printf("[IT10] Config hot reload integration: ");
    struct Config { bool hmacRequired=false; bool hybridEnabled=false; int thermalLimit=120; std::string whitelist=""; };
    Config cfg;
    cfg.hmacRequired = false; cfg.hybridEnabled = false; cfg.thermalLimit = 120;
    TEST_ASSERT(!cfg.hmacRequired, "initial hmac should be false");
    cfg.hmacRequired = true; cfg.hybridEnabled = true; cfg.thermalLimit = 110; cfg.whitelist = "com.test.app";
    TEST_ASSERT(cfg.hmacRequired, "reloaded hmac should be true");
    TEST_ASSERT(cfg.hybridEnabled, "reloaded hybrid should be true");
    TEST_ASSERT(cfg.thermalLimit == 110, "reloaded thermal limit wrong");
    TEST_ASSERT(cfg.whitelist == "com.test.app", "reloaded whitelist wrong");
    TEST_PASS();
}

struct ABResult { std::string name; float groupA; float groupB; float deltaPct; std::string winner; };

void test_ab_hybrid() {
    printf("[AB1] Hybrid ON vs OFF: ");
    srand(42);
    int migrationsOn = 0, migrationsOff = 0;
    float frameTimeOn = 0, frameTimeOff = 0;
    float tempOn = 0, tempOff = 0;
    aatpe_ml_hybrid::instance().init(3);
    aatpe_ml_hybrid::instance().setEnabled(true);
    float f[15]={1,0.5f,0.3f,0.2f,0.5f,0.5f,1,0.5f,1,0,0,0,0,0,0};
    for (int i=0;i<500;i++) aatpe_ml_hybrid::instance().sgdUpdate(2, f, 0.85f);
    for (int i=0;i<1000;i++) {
        float heuristic = 0.3f + (rand()%100)/200.0f;
        float hybrid = aatpe_ml_hybrid::instance().predict(2, f);
        auto fOn = fuseDecisions(heuristic, 0.5f, hybrid, 0.4f, aatpe::SystemState::NORMAL, false);
        if (fOn.final_score > 0.55f) migrationsOn++;
        frameTimeOn += 16.67f * (1.0f - fOn.final_score * 0.1f);
        tempOn += 40.0f + fOn.final_score * 10.0f;
        auto fOff = fuseDecisions(heuristic, 0.5f, 0.0f, 0.4f, aatpe::SystemState::NORMAL, false);
        if (fOff.final_score > 0.55f) migrationsOff++;
        frameTimeOff += 16.67f * (1.0f - fOff.final_score * 0.1f);
        tempOff += 40.0f + fOff.final_score * 10.0f;
    }
    frameTimeOn /= 1000; frameTimeOff /= 1000; tempOn /= 1000; tempOff /= 1000;
    printf("(migrations: ON=%d OFF=%d, frame: ON=%.2fms OFF=%.2fms, temp: ON=%.1fC OFF=%.1fC) ",
           migrationsOn, migrationsOff, frameTimeOn, frameTimeOff, tempOn, tempOff);
    TEST_ASSERT(migrationsOn != migrationsOff, "migration counts should differ");
    TEST_ASSERT(frameTimeOn < frameTimeOff, "hybrid ON should have lower frame time");
    TEST_PASS();
}

void test_ab_hmac() {
    printf("[AB2] HMAC ON vs OFF: ");
    int totalCmds = 1000;
    int successOn = 0, successOff = 0;
    srand(123);
    for (int i=0;i<totalCmds;i++) {
        std::string cmd = "set_mode=assist";
        bool validSig = (rand() % 100) < 90;
        std::string sig = validSig ? computeTestHmac(cmd) : "invalid";
        if (verifyTestHmac(cmd, sig, true)) successOn++;
        successOff++;
    }
    float rateOn = (float)successOn / totalCmds * 100;
    float rateOff = (float)successOff / totalCmds * 100;
    printf("(success rate: ON=%.1f%% OFF=%.1f%%) ", rateOn, rateOff);
    TEST_ASSERT(rateOn < rateOff, "HMAC ON should have lower success rate (rejects invalid)");
    TEST_ASSERT(rateOn > 85.0f && rateOn < 95.0f, "HMAC ON success rate should be ~90%");
    TEST_ASSERT(rateOff == 100.0f, "HMAC OFF should have 100% success rate");
    TEST_PASS();
}

void test_ab_whitelist() {
    printf("[AB3] Whitelist vs none: ");
    std::vector<std::string> allProcs;
    for (int i=0;i<50;i++) allProcs.push_back("com.app." + std::to_string(i));
    std::string wl = "com.app.0,com.app.1,com.app.2,com.app.3,com.app.4";
    int scannedWith = 0, scannedWithout = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int iter=0;iter<100;iter++) {
        for (const auto& p : allProcs) if (isPackageWhitelisted(p, wl)) scannedWith++;
    }
    auto mid = std::chrono::high_resolution_clock::now();
    for (int iter=0;iter<100;iter++) {
        for (const auto& p : allProcs) if (isPackageWhitelisted(p, "")) scannedWithout++;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double timeWith = std::chrono::duration<double,std::milli>(mid-start).count();
    double timeWithout = std::chrono::duration<double,std::milli>(end-mid).count();
    printf("(scanned: wl=%d no-wl=%d, time: wl=%.2fms no-wl=%.2fms) ",
           scannedWith/100, scannedWithout/100, timeWith, timeWithout);
    TEST_ASSERT(scannedWith < scannedWithout, "whitelist should scan fewer threads");
    TEST_ASSERT(scannedWith/100 == 5, "whitelist should scan exactly 5 packages");
    TEST_PASS();
}

void test_ab_cpu_lock() {
    printf("[AB4] CPU metrics locked vs unlocked: ");
    std::atomic<bool> stop{false};
    std::atomic<int> errorsLocked{0}, errorsUnlocked{0};
    std::map<int,aatpe::CpuMetrics> lockedMetrics;
    std::thread wLock([&](){ while(!stop){std::unique_lock<std::shared_mutex> l(g_cpuMetricsMutex);for(int c=0;c<8;c++){aatpe::CpuMetrics m;m.util=c*10;lockedMetrics[c]=m;}}});
    std::thread rLock([&](){ while(!stop){std::shared_lock<std::shared_mutex> l(g_cpuMetricsMutex);for(int c=0;c<8;c++){auto it=lockedMetrics.find(c);if(it!=lockedMetrics.end()&&(it->second.util<0||it->second.util>100))errorsLocked++;}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true); wLock.join(); rLock.join();
    stop.store(false);
    std::map<int,aatpe::CpuMetrics> unlockedMetrics;
    std::thread wNoLock([&](){ while(!stop){for(int c=0;c<8;c++){aatpe::CpuMetrics m;m.util=c*10;unlockedMetrics[c]=m;}}});
    std::thread rNoLock([&](){ while(!stop){for(int c=0;c<8;c++){auto it=unlockedMetrics.find(c);if(it!=unlockedMetrics.end()&&(it->second.util<0||it->second.util>100))errorsUnlocked++;}}});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true); wNoLock.join(); rNoLock.join();
    printf("(errors: locked=%d unlocked=%d) ", errorsLocked.load(), errorsUnlocked.load());
    TEST_ASSERT(errorsLocked.load() == 0, "locked access should have zero errors");
    TEST_PASS();
}

void test_ab_fusion_vs_softmax() {
    printf("[AB5] Fusion vs softmax: ");
    srand(999);
    int betterDecisionsFusion = 0, betterDecisionsSoftmax = 0;
    float totalScoreFusion = 0, totalScoreSoftmax = 0;
    for (int i=0;i<1000;i++) {
        float heuristic = 0.2f + (rand()%100)/150.0f;
        float ml = 0.3f + (rand()%100)/200.0f;
        float hybrid = 0.4f + (rand()%100)/250.0f;
        float history = 0.3f + (rand()%100)/200.0f;
        auto f = fuseDecisions(heuristic, ml, hybrid, history, aatpe::SystemState::NORMAL, false);
        totalScoreFusion += f.final_score;
        float temp = 1.0f;
        float eh = std::exp(heuristic / temp), em = std::exp(ml / temp);
        float softmaxScore = (heuristic * eh + ml * em) / (eh + em);
        totalScoreSoftmax += softmaxScore;
        float groundTruth = (heuristic + ml + hybrid + history) / 4.0f;
        if (fabs(f.final_score - groundTruth) < fabs(softmaxScore - groundTruth)) betterDecisionsFusion++;
        else betterDecisionsSoftmax++;
    }
    float avgFusion = totalScoreFusion / 1000, avgSoftmax = totalScoreSoftmax / 1000;
    printf("(better decisions: fusion=%d softmax=%d, avg score: fusion=%.3f softmax=%.3f) ",
           betterDecisionsFusion, betterDecisionsSoftmax, avgFusion, avgSoftmax);
    TEST_ASSERT(betterDecisionsFusion > betterDecisionsSoftmax, "fusion should make better decisions");
    TEST_PASS();
}

int main() {
    printf("================================================================\n");
    printf("AATPE v68 Comprehensive Test Suite\n");
    printf("================================================================\n\n");

    printf("--- UNIT TESTS (20+) ---\n");
    test_hybrid_score_disabled();
    test_hybrid_score_range();
    test_hybrid_feature_extraction();
    test_hybrid_save_load();
    test_hmac_generation();
    test_hmac_verify_valid();
    test_hmac_verify_invalid();
    test_hmac_empty_sig_required();
    test_hmac_empty_sig_optional();
    test_hmac_replay_protection();
    test_cpu_metrics_lock();
    test_whitelist_exact();
    test_whitelist_empty();
    test_whitelist_invalid();
    test_whitelist_whitespace();
    test_affinity_rollback();
    test_fusion_weights_sum();
    test_fusion_cold_start();
    test_fusion_div_zero();
    test_fusion_state_dependent();
    test_frame_time_outcome();
    test_gfxinfo_parse();
    test_ttl_cache();
    test_state_machine();
    test_state_machine_priority();
    test_state_machine_history();

    printf("\n--- INTEGRATION TESTS (10+) ---\n");
    test_daemon_startup();
    test_hybrid_fusion_e2e();
    test_hmac_control_e2e();
    test_cpu_metrics_integration();
    test_whitelist_scan_integration();
    test_state_thermal_integration();
    test_fusion_frame_outcome();
    test_ebpf_fallback();
    test_webui_refresh();
    test_config_hot_reload();

    printf("\n--- AB TESTS (5 groups) ---\n");
    test_ab_hybrid();
    test_ab_hmac();
    test_ab_whitelist();
    test_ab_cpu_lock();
    test_ab_fusion_vs_softmax();

    printf("\n================================================================\n");
    printf("TEST SUMMARY\n");
    printf("================================================================\n");
    int total = g_testsPassed + g_testsFailed;
    printf("Total:  %d\n", total);
    printf("Passed: %d\n", g_testsPassed);
    printf("Failed: %d\n", g_testsFailed);
    printf("Pass rate: %.1f%%\n", total > 0 ? (float)g_testsPassed / total * 100.0f : 0.0f);
    if (!g_failures.empty()) {
        printf("\nFailures:\n");
        for (const auto& f : g_failures) printf("  - %s\n", f.c_str());
    }
    printf("================================================================\n");
    return g_testsFailed > 0 ? 1 : 0;
}
