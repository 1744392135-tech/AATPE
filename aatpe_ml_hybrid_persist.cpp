#include "aatpe_ml_hybrid.h"
#include "aatped.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
namespace aatpe_ml_hybrid {

static uint64_t hybridComputeTopoHash(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu <= 0) ncpu = 1;
    if (ncpu > 16) ncpu = 16;
    std::ostringstream os;
    os << "n" << ncpu << ":cap<";
    for (long c = 0; c < ncpu; c++) {
        if (c) os << ",";
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpu_capacity";
        std::ifstream f(p);
        int cap = 0;
        if (f) f >> cap;
        if (cap <= 0) cap = 380;
        os << cap;
    }
    os << ">";
    const std::string s = os.str();
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : s) {
        h ^= (uint64_t)ch;
        h *= 1099511628211ULL;
    }
    return h;
}

bool HybridLearner::save(const char* path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_||!path||path[0]=='\0') return false;
    char tmp[1024];
    int sn = snprintf(tmp,sizeof(tmp),"%s.tmp.%d",path,(int)getpid());
    if (sn < 0 || (size_t)sn >= sizeof(tmp)) return false;
    int tmpFd = ::open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (tmpFd < 0) return false;
    FILE* fp = fdopen(tmpFd, "w+b");
    if (!fp) { close(tmpFd); unlink(tmp); return false; }
    bool ok=true;
    #define HY_FW(p,s,c) do { if(fwrite((p),(s),(c),fp)!=(c)) ok=false; } while(0)
    uint32_t magic=0x4859424D, version=5, poolCount=(uint32_t)poolDeltas_.size();
    int32_t enabledInt=enabled_?1:0;
    HY_FW(&magic,sizeof(magic),1); HY_FW(&version,sizeof(version),1);
    HY_FW(&poolCount,sizeof(poolCount),1); HY_FW(&enabledInt,sizeof(enabledInt),1);
    uint64_t topoHash = hybridComputeTopoHash();
    HY_FW(&topoHash,sizeof(topoHash),1);
    for (const auto& d : poolDeltas_) {
        HY_FW(d.weights,sizeof(float),15);
        int32_t uc=d.updateCount; HY_FW(&uc,sizeof(uc),1);
    }
    int32_t tu=totalUpdates_; HY_FW(&tu,sizeof(tu),1);
    if (ok) {
        long dl=ftell(fp);
        if (dl<=0||dl>65536) ok=false;
        if (ok&&fseek(fp,0,SEEK_SET)==0) {
            std::vector<unsigned char> buf((size_t)dl);
            if (fread(buf.data(),1,(size_t)dl,fp)==(size_t)dl) {
                uint32_t crc=0xFFFFFFFF;
                for (unsigned char c:buf) { crc^=c; for(int i=0;i<8;i++) crc=(crc>>1)^(0xEDB88320&-(crc&1)); }
                crc^=0xFFFFFFFF;
                if (fseek(fp,dl,SEEK_SET)!=0||fwrite(&crc,sizeof(uint32_t),1,fp)!=1) ok=false;
            } else ok=false;
        }
    }
    #undef HY_FW
    if (ok&&fflush(fp)!=0) ok=false;
    if (ok && fsync(fileno(fp)) != 0) ok = false;
    if (fclose(fp)!=0) ok=false;
    if (!ok) {
        if (unlink(tmp) != 0) LOG_WARN("[ML-HYBRID] unlink tmp failed");
        return false;
    }
    if (rename(tmp,path)!=0) {
        if (unlink(tmp) != 0) LOG_WARN("[ML-HYBRID] unlink tmp after rename fail");
        return false;
    }
    LOG_INFO("[ML-HYBRID] saved to %s (%u pools, %d updates, CRC32+atomic)", path, poolCount, totalUpdates_);
    return true;
}

bool HybridLearner::load(const char* path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!path||path[0]=='\0') return false;
    FILE* fp=fopen(path,"rb"); if (!fp) return false;
    uint32_t magic=0,version=0,poolCount=0;
    if (fread(&magic,sizeof(magic),1,fp)!=1||magic!=0x4859424D) { fclose(fp); return false; }
    if (fread(&version,sizeof(version),1,fp)!=1||version!=5) { fclose(fp); return false; }
    if (fread(&poolCount,sizeof(poolCount),1,fp)!=1||poolCount==0||poolCount>16) { fclose(fp); return false; }
    if (initialized_ && (uint32_t)numPools_!=poolCount) {
        LOG_WARN("[ML-HYBRID] reject load: pool-count mismatch (file=%u, current=%d)", poolCount, numPools_);
        fclose(fp); return false;
    }
    int32_t loadedEnabled=0;
    if (fread(&loadedEnabled,sizeof(loadedEnabled),1,fp)!=1) { fclose(fp); return false; }
    uint64_t loadedTopoHash = 0;
    if (fread(&loadedTopoHash,sizeof(loadedTopoHash),1,fp)!=1) { fclose(fp); return false; }
    uint64_t curTopoHash = hybridComputeTopoHash();
    if (loadedTopoHash != curTopoHash) {
        LOG_WARN("[ML-HYBRID] reject load: topology mismatch (file=%016llx cur=%016llx); cold-start",
                 (unsigned long long)loadedTopoHash, (unsigned long long)curTopoHash);
        fclose(fp); return false;
    }
    std::vector<HybridPoolDelta> loadedDeltas(poolCount);
    for (uint32_t i=0;i<poolCount;i++) {
        if (fread(loadedDeltas[i].weights,sizeof(float),15,fp)!=15) { fclose(fp); return false; }
        int32_t uc=0;
        if (fread(&uc,sizeof(uc),1,fp)!=1) { fclose(fp); return false; }
        loadedDeltas[i].updateCount=uc;
        for (int w=0;w<15;w++) {
            float v = loadedDeltas[i].weights[w];
            if (!std::isfinite(v)) {
                loadedDeltas[i].weights[w] = 0.0f;
            } else {
                if (v > kWeightClamp) v = kWeightClamp;
                else if (v < -kWeightClamp) v = -kWeightClamp;
                loadedDeltas[i].weights[w] = v;
            }
        }
    }
    int32_t totalUpdates=0;
    if (fread(&totalUpdates,sizeof(totalUpdates),1,fp)!=1) { fclose(fp); return false; }
    long dataEnd=ftell(fp);
    if (dataEnd <= 0) { LOG_WARN("[ML-HYBRID] load: ftell failed on %s", path); fclose(fp); return false; }
    long fileSize=0;
    if (fseek(fp,0,SEEK_END)==0) { fileSize=ftell(fp); fseek(fp,dataEnd,SEEK_SET); }
    if (fileSize <= 0 || fileSize < dataEnd) { LOG_WARN("[ML-HYBRID] load: bad size on %s", path); fclose(fp); return false; }
    if (version >= 2) {
        if (fileSize != dataEnd + (long)sizeof(uint32_t)) {
            LOG_WARN("[ML-HYBRID] load: v>=2 size mismatch (dataEnd=%ld fileSize=%ld) on %s",
                     dataEnd, fileSize, path);
            fclose(fp); return false;
        }
        uint32_t storedCrc=0;
        if (fread(&storedCrc,sizeof(uint32_t),1,fp)!=1||fseek(fp,0,SEEK_SET)!=0) {
            LOG_WARN("[ML-HYBRID] CRC trailer unreadable in %s", path); fclose(fp); return false;
        }
        std::vector<unsigned char> buf((size_t)dataEnd);
        if (fread(buf.data(),1,(size_t)dataEnd,fp)!=(size_t)dataEnd) {
            LOG_WARN("[ML-HYBRID] CRC body read failed in %s", path); fclose(fp); return false;
        }
        uint32_t calc=0xFFFFFFFF;
        for (unsigned char c:buf) { calc^=c; for(int i=0;i<8;i++) calc=(calc>>1)^(0xEDB88320&-(calc&1)); }
        calc^=0xFFFFFFFF;
        if (calc!=storedCrc) { LOG_WARN("[ML-HYBRID] CRC mismatch in %s",path); fclose(fp); return false; }
    }
    fclose(fp);
    poolDeltas_=std::move(loadedDeltas);
    numPools_=(int)poolCount; totalUpdates_=totalUpdates; initialized_=true;
    enabled_=(loadedEnabled!=0); prevTickTimestampNs_=0;
    recomputeAlphaLocked();
    LOG_INFO("[ML-HYBRID] loaded from %s (%u pools, %d updates, enabled=%d)", path, poolCount, totalUpdates_, enabled_?1:0);
    return true;
}

}
