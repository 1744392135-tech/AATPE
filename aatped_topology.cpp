#include "aatped.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <sched.h>
#include <dirent.h>
#include <sys/stat.h>

using namespace aatpe;

std::vector<CpuCluster> g_clusters;

std::set<int> parseAffinityMask(const std::string& maskStr) {
    std::set<int> cpus;
    std::string s = maskStr;
    trim(s);
    if (s.empty()) return cpus;
    int cpuCount;
    {
        std::lock_guard<std::mutex> topoLk(g_topologyRwMutex);
        cpuCount = g_cpuCount;
    }
    int maxCpu = (cpuCount > 0) ? cpuCount - 1 : 0;
    bool allHex = true;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            allHex = false; break;
        }
    }
    if (allHex && s.size() >= 2) {
        try {
            unsigned long long mask = std::stoull(s, nullptr, 16);
            int bits = std::min((int)topology::kHexBitWidth, cpuCount);
            for (int i = 0; i < bits; i++) {
                if (mask & (1ULL << i)) cpus.insert(i);
            }
        } catch (...) {
            LOG_WARN("parseAffinityMask: hex mask '%s' unparsable, falling back to all %d cpus",
                     s.c_str(), cpuCount);
            for (int i = 0; i < cpuCount && i < topology::kHexBitWidth; i++) cpus.insert(i);
        }
        return cpus;
    }
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        trim(token);
        if (token.empty()) continue;
        size_t dash = token.find('-');
        if (dash != std::string::npos) {
            try {
                int lo = std::stoi(token.substr(0, dash));
                int hi = std::stoi(token.substr(dash + 1));
                if (lo < 0) lo = 0;
                if (hi > maxCpu) hi = maxCpu;
                if (lo <= hi) {
                    for (int c = lo; c <= hi; c++) cpus.insert(c);
                }
            } catch (...) {
                static int n = 0; if ((++n & 0x3F) == 1)
                    LOG_DEBUG("parseAffinityMask: range token '%s' unparsable (throttled)", token.c_str());
            }
        } else {
            try {
                int one = std::stoi(token);
                if (one >= 0 && one <= maxCpu) cpus.insert(one);
            } catch (...) {
                static int n = 0; if ((++n & 0x3F) == 1)
                    LOG_DEBUG("parseAffinityMask: token '%s' unparsable (throttled)", token.c_str());
            }
        }
    }
    return cpus;
}

cpu_set_t getThreadAffinityCpus(int tid) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(tid, sizeof(mask), &mask) != 0) {
        CPU_ZERO(&mask);
    }
    return mask;
}

int readThreadCurrentCpu(int pid, int tid) {
    static std::map<std::pair<int,int>, std::pair<int, unsigned long long>> s_cpuCache;
    static std::mutex s_cacheMutex;
    static unsigned long long s_lastPurgeMs = 0;
    unsigned long long nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto key = std::make_pair(pid, tid);
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (nowMs - s_lastPurgeMs >= 30000ULL) {
            s_lastPurgeMs = nowMs;
            const unsigned long long kMaxAgeMs = 60000ULL;
            for (auto it = s_cpuCache.begin(); it != s_cpuCache.end(); ) {
                if (nowMs - it->second.second > kMaxAgeMs) {
                    it = s_cpuCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto it = s_cpuCache.find(key);
        if (it != s_cpuCache.end() && (nowMs - it->second.second) < (cpu_cache::kCacheTtlSec * 1000ULL)) {
            return it->second.first;
        }
    }
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return -1;
    std::string content;
    std::getline(f, content);
    size_t paren = content.rfind(')');
    if (paren == std::string::npos) return -1;
    std::string rest = content.substr(paren + 1);
    std::stringstream ss(rest);
    std::string field;
    int idx = 0;
    int processor = -1;
    while (ss >> field) {
        if (idx == 36) {
            try { processor = std::stoi(field); }
            catch (...) {
                static int n = 0; if ((++n & 0x3F) == 1)
                    LOG_DEBUG("readThreadCurrentCpu: stat field36='%s' unparsable (throttled)", field.c_str());
            }
            break;
        }
        idx++;
    }
    if (processor >= 0) {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_cpuCache[key] = {processor, nowMs};
    }
    return processor;
}

static std::string joinCpuList(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i > 0) s += ",";
        s += std::to_string(v[i]);
    }
    return s;
}

const char* tierName(CpuTier t) {
    switch (t) {
        case CpuTier::LITTLE: return "LITTLE";
        case CpuTier::MID:    return "MID";
        case CpuTier::BIG:    return "BIG";
        case CpuTier::PRIME:  return "PRIME";
        default:              return "UNKNOWN";
    }
}

CpuTier tierOfCpu(int cpu) {
    if (cpu < 0) return CpuTier::UNKNOWN;
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    for (const auto& cl : g_clusters) {
        if (std::find(cl.cpus.begin(), cl.cpus.end(), cpu) != cl.cpus.end())
            return cl.tier;
    }
    return CpuTier::UNKNOWN;
}

const std::vector<CpuCluster>& clusters() {
    return g_clusters;
}

std::vector<CpuCluster> clustersSnapshot() {
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    return g_clusters;
}

int clusterCount() {
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    return (int)g_clusters.size();
}

std::vector<int> primePoolLocked() {
    if (g_clusters.empty()) return {};
    if (g_clusters.back().tier == CpuTier::PRIME) return g_clusters.back().cpus;
    return {};
}

std::vector<int> primePool() {
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    return primePoolLocked();
}

std::string clustersStateText() {
    std::string out;
    for (size_t i = 0; i < g_clusters.size(); i++) {
        const auto& cl = g_clusters[i];
        if (i) out += "\n";
        out += std::string(tierName(cl.tier)) + "," +
               std::to_string(cl.capacity) + "," +
               std::to_string(cl.minKhz) + "-" + std::to_string(cl.maxKhz) + ":" +
               joinCpuList(cl.cpus);
    }
    return out;
}

bool clustersTextPublishable(const std::string& txt) {
    if (txt.empty()) return false;
    size_t lines = 0;
    std::stringstream ss(txt);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        lines++;
        size_t c1 = line.find(',');
        size_t c2 = line.find(',', c1+1);
        size_t dash = line.find('-', c2+1);
        size_t colon = line.find(':', c2+1);
        if (c1==std::string::npos || c2==std::string::npos ||
            dash==std::string::npos || colon==std::string::npos ||
            dash < c2 || colon < dash) return false;
        std::string cpuPart = line.substr(colon+1);
        if (cpuPart.empty()) return false;
    }
    return lines >= 1;
}

static bool g_topologyReady = false;
static int  g_topologyRetryCount = 0;
static uint64_t g_topologyLastRetryMs = 0;

void markTopologyReady() { g_topologyReady = true; g_topologyRetryCount = 0; }

bool topologyRetryDue(uint64_t nowMs) {
    if (g_topologyReady) return false;
    static const uint64_t schedule[] = {1000, 3000, 10000, 30000};
    uint64_t wait;
    if (g_topologyRetryCount < 4) wait = schedule[g_topologyRetryCount];
    else wait = 60000;
    if (nowMs - g_topologyLastRetryMs >= wait) {
        g_topologyLastRetryMs = nowMs;
        g_topologyRetryCount++;
        return true;
    }
    return false;
}

static int inferCapacityFromPart(int part) {
    switch (part) {
        case 0xd03: return 200;
        case 0xd05: return 240;
        case 0xd07: return 600;
        case 0xd08: return 700;
        case 0xd09: return 750;
        case 0xd0a: return 800;
        case 0xd0b: return 900;
        case 0xd0d: return 950;
        case 0xd40: return 1000;
        case 0xd4b: return 1024;
        case 0xd46: return 1024;
        case 0xd48: return 1024;
        case 0xd90: return 1024;
        case 0xd44: return 1024;
        case 0xd47: return 1024;
        case 0xd4d: return 1024;
        case 0xd82: return 1024;
        case 0xd95: return 1024;
        case 0xd83: return 1024;
        case 0xd81: return 1024;
        default:    return 0;
    }
}
#ifdef AATPE_HOST_TEST
static std::map<int,int> g_testPartMap;
static std::map<int,bool> g_testOnlineMap;
#endif

static void readCpuPolicyMap(std::map<int,int>& cpuPolicy) {
    cpuPolicy.clear();
    DIR* d = opendir("/sys/devices/system/cpu/cpufreq");
    if (!d) return;
    struct dirent* pe;
    while ((pe = readdir(d)) != nullptr) {
        if (strncmp(pe->d_name, "policy", 6) != 0) continue;
        int pid = parseIntSafe(pe->d_name + 6);
        std::string path = std::string("/sys/devices/system/cpu/cpufreq/") + pe->d_name + "/affected_cpus";
        std::string aff = readFile(path.c_str());
        std::stringstream ss(aff);
        int cpuId;
        while (ss >> cpuId) cpuPolicy[cpuId] = pid;
    }
    closedir(d);
}

static std::map<int,int> g_testPolicyMap;
#ifdef AATPE_HOST_TEST
void topologyInjectPolicyForTest(const std::map<int,int>& m) { g_testPolicyMap = m; }
void topologyInjectPartForTest(const std::map<int,int>& m) { g_testPartMap = m; }
void topologyInjectOnlineForTest(const std::map<int,bool>& m) { g_testOnlineMap = m; }
#endif

void buildGenericClusters(const std::vector<int>& caps,
                          const std::vector<int>& cpuinfoFreqs) {
    g_clusters.clear();
    int n = (int)g_allCpus.size();
    if (n == 0) return;

    int capMax = 0, freqMax = 0;
    for (int i = 0; i < n; i++) {
        if (caps[i] > 0 && caps[i] != topology::kDefaultCapacity && caps[i] > capMax) capMax = caps[i];
        if (cpuinfoFreqs[i] > freqMax) freqMax = cpuinfoFreqs[i];
    }
    bool hasCap = (capMax > 0);

    struct Core { int cpu; int cap; int freq; double cn; double fn; double strength; };
    std::vector<Core> cores;
    for (int i = 0; i < n; i++) {
        double cn = hasCap ? (double)caps[i] / capMax : 0.0;
        double fn = freqMax > 0 ? (double)cpuinfoFreqs[i] / freqMax : 0.0;
        cores.push_back({g_allCpus[i], caps[i], cpuinfoFreqs[i], cn, fn, std::max(cn, fn)});
    }

    std::map<int,int> cpuPolicy;
    if (!g_testPolicyMap.empty()) cpuPolicy = g_testPolicyMap;
    else readCpuPolicyMap(cpuPolicy);

    bool havePolicy = !cpuPolicy.empty();
    std::vector<std::vector<Core>> groups;
    if (havePolicy) {
        std::map<int, std::vector<Core>> byPolicy;
        std::vector<Core> noPolicy;
        for (auto& c : cores) {
            if (cpuPolicy.count(c.cpu)) byPolicy[cpuPolicy[c.cpu]].push_back(c);
            else noPolicy.push_back(c);
        }
        if (!noPolicy.empty()) {
            std::sort(noPolicy.begin(), noPolicy.end(), [](const Core& a, const Core& b){
                if (a.strength != b.strength) return a.strength < b.strength;
                return a.cpu < b.cpu;
            });
            std::vector<Core> cur;
            for (size_t i = 0; i < noPolicy.size(); i++) {
                const Core& hi = noPolicy[i];
                if (i > 0) {
                    const Core& lo = noPolicy[i-1];
                    bool capJump = (lo.cap>0 && hi.cap>0 && (double)hi.cap/lo.cap>=topology::kGapCapRatio);
                    bool freqJump = (lo.freq>0 && hi.freq>0 && (double)hi.freq/lo.freq>=topology::kGapFreqRatio);
                    if (capJump || freqJump) { groups.push_back(cur); cur.clear(); }
                }
                cur.push_back(hi);
            }
            if (!cur.empty()) groups.push_back(cur);
        }
        for (auto& [pid, members] : byPolicy) {
            std::sort(members.begin(), members.end(), [](const Core& a, const Core& b){
                if (a.strength != b.strength) return a.strength < b.strength;
                return a.cpu < b.cpu;
            });
            std::vector<std::vector<Core>> sub;
            sub.push_back({members[0]});
            for (size_t i = 1; i < members.size(); i++) {
                const Core& lo = sub.back().back();
                const Core& hi = members[i];
                bool capJump = (lo.cap > 0 && hi.cap > 0 && (double)hi.cap / lo.cap >= topology::kGapCapRatio);
                bool freqJump = (lo.freq > 0 && hi.freq > 0 && (double)hi.freq / lo.freq >= topology::kGapFreqRatio);
                if (capJump || freqJump) sub.push_back({});
                sub.back().push_back(hi);
            }
            for (auto& s : sub) groups.push_back(s);
        }
        std::sort(groups.begin(), groups.end(), [](const std::vector<Core>& a, const std::vector<Core>& b){
            double sa = 0, sb = 0;
            for (auto& c : a) sa = std::max(sa, c.strength);
            for (auto& c : b) sb = std::max(sb, c.strength);
            if (sa != sb) return sa < sb;
            return a[0].cpu < b[0].cpu;
        });
    } else {
        std::sort(cores.begin(), cores.end(), [](const Core& a, const Core& b) {
            if (a.strength != b.strength) return a.strength < b.strength;
            return a.cpu < b.cpu;
        });
        groups.push_back({cores[0]});
        for (int i = 1; i < n; i++) {
            const Core& lo = groups.back().back();
            const Core& hi = cores[i];
            bool capJump = (lo.cap > 0 && hi.cap > 0 && (double)hi.cap / lo.cap >= topology::kGapCapRatio);
            bool freqJump = (lo.freq > 0 && hi.freq > 0 && (double)hi.freq / lo.freq >= topology::kGapFreqRatio);
            if (capJump || freqJump) groups.push_back({});
            groups.back().push_back(hi);
        }
    }

    for (auto& g : groups) {
        CpuCluster cl;
        int capRep = 0, fMin = 0, fMax = 0;
        bool firstF = true;
        for (auto& s : g) {
            cl.cpus.push_back(s.cpu);
            if (s.cap > capRep) capRep = s.cap;
            if (s.freq > 0) {
                if (firstF) { fMin = fMax = s.freq; firstF = false; }
                else { if (s.freq < fMin) fMin = s.freq; if (s.freq > fMax) fMax = s.freq; }
            }
        }
        std::sort(cl.cpus.begin(), cl.cpus.end());
        cl.capacity = capRep; cl.minKhz = fMin; cl.maxKhz = fMax;
        int pid = -1; bool pidUniform = true;
        for (size_t k = 0; k < cl.cpus.size(); k++) {
            auto it = cpuPolicy.find(cl.cpus[k]);
            int p = (it != cpuPolicy.end()) ? it->second : -1;
            if (k == 0) pid = p;
            else if (p != pid) pidUniform = false;
        }
        if (pidUniform && pid >= 0) cl.policyId = pid;
        cl.cpus.shrink_to_fit();
        g_clusters.push_back(cl);
    }

    size_t k = g_clusters.size();
    bool topIsPrime = false;
    if (k >= 2) {
        CpuCluster& top = g_clusters[k-1];
        CpuCluster& nxt = g_clusters[k-2];
        bool freqLead = (top.maxKhz > 0 && nxt.maxKhz > 0 &&
                         (double)top.maxKhz / nxt.maxKhz >= topology::kPrimeMinFreqRatio);
        bool capLead  = (top.capacity > 0 && nxt.capacity > 0 &&
                         (double)top.capacity / nxt.capacity >= topology::kPrimeMinCapRatio);
        bool independentPolicyAnyLead = (top.policyId >= 0 && top.policyId != nxt.policyId &&
                                        (top.capacity > nxt.capacity || top.maxKhz > nxt.maxKhz));
        bool sizeOk = (top.cpus.size() <= 3);
        bool ratioLead = (freqLead || capLead);
        topIsPrime = (k >= 3 && ratioLead) ||
                     (independentPolicyAnyLead && (k >= 3 || sizeOk)) ||
                     (ratioLead && sizeOk);
    }
    for (size_t i = 0; i < k; i++) {
        if (i == 0) {
            g_clusters[i].tier = (k == 2 && topIsPrime) ? CpuTier::BIG
                                                       : CpuTier::LITTLE;
        } else if (i == k - 1) {
            g_clusters[i].tier = topIsPrime ? CpuTier::PRIME : CpuTier::BIG;
        } else if (i == k - 2 && topIsPrime) {
            g_clusters[i].tier = CpuTier::BIG;
        } else {
            g_clusters[i].tier = CpuTier::MID;
        }
    }

    if (g_clusters.empty()) return;
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    g_littlePool = g_clusters[0].cpus;
    if (k == 1) {
        g_bigPool = g_clusters[0].cpus;
    } else if (k == 2) {
        if (g_clusters[k-1].tier == CpuTier::PRIME) {
            g_bigPool = g_clusters[0].cpus;
            g_littlePool.clear();
        } else {
            g_bigPool = g_clusters[1].cpus;
        }
    } else {
        bool hasPrime = (g_clusters[k-1].tier == CpuTier::PRIME);
        size_t highIdx = hasPrime ? k - 2 : k - 1;
        g_bigPool = g_clusters[highIdx].cpus;
        for (size_t i = 1; i < highIdx; i++) {
            for (int cpu : g_clusters[i].cpus) g_midPool.push_back(cpu);
        }
    }
}

void applyManualPrimeOverride(const std::string& primeCfg) {
    if (primeCfg.empty()) return;
    std::vector<int> pc;
    {
        std::stringstream ss(primeCfg);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            trim(tok);
            if (tok.empty()) continue;
            size_t d = tok.find('-');
            try {
                if (d != std::string::npos) {
                    int lo = std::stoi(tok.substr(0, d));
                    int hi = std::stoi(tok.substr(d + 1));
                    if (lo < 0) lo = 0;
                    if (hi >= g_cpuCount) hi = g_cpuCount - 1;
                    for (int c = lo; c <= hi; c++) pc.push_back(c);
                } else {
                    int c = std::stoi(tok);
                    if (c >= 0 && c < g_cpuCount) pc.push_back(c);
                }
            } catch (...) {
                LOG_WARN("applyManualPrimeOverride: token '%s' unparsable, discarding whole override", tok.c_str());
                pc.clear(); break;
            }
        }
    }
    std::sort(pc.begin(), pc.end());
    pc.erase(std::unique(pc.begin(), pc.end()), pc.end());
    if (pc.empty()) {
        LOG_WARN("PRIME_POOL override: empty cpulist after parse '%s', keeping auto topology", primeCfg.c_str());
        return;
    }
    for (int c : pc) {
        if (std::find(g_bigPool.begin(), g_bigPool.end(), c) != g_bigPool.end()) {
            LOG_WARN("PRIME_POOL override: cpu%d overlaps BIG pool, rejecting; keeping auto", c);
            return;
        }
    }
    auto strip = [](std::vector<int>& pool, const std::vector<int>& pc) {
        std::vector<int> kept;
        for (int c : pool) if (std::find(pc.begin(), pc.end(), c) == pc.end()) kept.push_back(c);
        pool = kept;
    };
    strip(g_littlePool, pc); strip(g_midPool, pc); strip(g_bigPool, pc);
    for (auto& cl : g_clusters) {
        std::vector<int> kept;
        for (int c : cl.cpus) if (std::find(pc.begin(), pc.end(), c) == pc.end()) kept.push_back(c);
        cl.cpus = kept;
    }
    std::vector<CpuCluster> kept;
    for (auto& cl : g_clusters) if (!cl.cpus.empty()) kept.push_back(cl);
    g_clusters = kept;
    CpuCluster prime;
    prime.cpus = pc;
    prime.tier = CpuTier::PRIME;
    g_clusters.push_back(prime);
    LOG_INFO("PRIME_POOL override: cpus=[%s] pinned as PRIME", joinCpuList(pc).c_str());
}

static void classifyCpuPools(const std::vector<int>& caps,
                              const std::vector<int>& cpuinfoFreqs) {
    int n = (int)g_allCpus.size();

    buildGenericClusters(caps, cpuinfoFreqs);

    if (g_clusters.empty()) {
        if (n <= 4) {
            g_littlePool = g_allCpus;
        } else if (n <= 6) {
            int half = n / 2;
            for (int i = 0; i < half; i++) g_littlePool.push_back(g_allCpus[i]);
            for (int i = half; i < n; i++) g_bigPool.push_back(g_allCpus[i]);
        } else {
            for (int i = 0; i < n - 1; i++) g_littlePool.push_back(g_allCpus[i]);
            g_bigPool.push_back(g_allCpus[n - 1]);
        }
        return;
    }

    size_t k = g_clusters.size();

    std::string cs;
    for (size_t i = 0; i < g_clusters.size(); i++) {
        const auto& cl = g_clusters[i];
        cs += " [tier=";
        switch (cl.tier) {
            case CpuTier::LITTLE: cs += "LITTLE"; break;
            case CpuTier::MID:    cs += "MID"; break;
            case CpuTier::BIG:    cs += "BIG"; break;
            case CpuTier::PRIME:  cs += "PRIME"; break;
            default: cs += "?";
        }
        cs += " cap=" + std::to_string(cl.capacity) +
              " f=" + std::to_string(cl.minKhz) + "-" + std::to_string(cl.maxKhz) +
              " cpus=" + joinCpuList(cl.cpus) + "]";
    }
    LOG_INFO("Topology: generic clusters (k=%zu):%s", k, cs.c_str());
}

void detectCpuTopologyUnlocked() {
    Config cfg; snapshotConfig(cfg);
    LOG_INFO("Detecting CPU topology...");
    g_allCpus.clear();
    g_littlePool.clear();
    g_midPool.clear();
    g_bigPool.clear();
    g_clusters.clear();
    std::vector<int> caps;
    DIR* dir = opendir("/sys/devices/system/cpu");
    if (!dir) {
        int e0 = errno; char ebuf[128];
        LOG_ERROR("detectCpuTopology: opendir(/sys/devices/system/cpu) failed (errno=%d %s), using sysconf fallback",
                  e0, aatpeStrError(e0, ebuf, sizeof(ebuf)));
        long nCpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (nCpus <= 0) nCpus = 4;
        for (int c = 0; c < (int)nCpus; c++) {
            g_allCpus.push_back(c);
            caps.push_back(topology::kDefaultCapacity);
            {
                std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                g_cpuMetrics[c].capacity = topology::kDefaultCapacity;
                g_cpuMetrics[c].maxFreq = 1;
            }
        }
        g_cpuCount = (int)g_allCpus.size();
        int half = g_cpuCount / 2;
        for (int c = 0; c < half; c++) g_littlePool.push_back(c);
        for (int c = half; c < g_cpuCount; c++) g_bigPool.push_back(c);
        atomicWriteFile((std::string(STATE_DIR) + "/cpu_count").c_str(), g_cpuCount);
        atomicWriteFile((std::string(STATE_DIR) + "/little_pool").c_str(), joinCpuList(g_littlePool));
        atomicWriteFile((std::string(STATE_DIR) + "/mid_pool").c_str(), joinCpuList(g_midPool));
        atomicWriteFile((std::string(STATE_DIR) + "/big_pool").c_str(), joinCpuList(g_bigPool));
        atomicWriteFile((std::string(STATE_DIR) + "/prime_pool").c_str(), joinCpuList(primePoolLocked()));
        LOG_WARN("detectCpuTopology: sysconf fallback active: %d CPUs (L=%zu M=%zu B=%zu), will retry /sys next cycle",
                 g_cpuCount, g_littlePool.size(), g_midPool.size(), g_bigPool.size());
        initScoringArrays(g_cpuCount);
        return;
    }
    struct dirent* entry;
    std::vector<int> cpuinfoFreqs;
    std::vector<bool> capValid;
    std::vector<bool> freqValid;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "cpu", 3) != 0) continue;
        char* end;
        int cpuId = strtol(entry->d_name + 3, &end, 10);
        if (*end != '\0') continue;
        bool online = true;
#ifdef AATPE_HOST_TEST
        if (g_testOnlineMap.count(cpuId)) online = g_testOnlineMap[cpuId];
        else
#endif
        {
            std::string onPath = std::string("/sys/devices/system/cpu/cpu") + std::to_string(cpuId) + "/online";
            int on = readInt(onPath.c_str(), 1);
            online = (on != 0);
        }
        if (!online) {
            LOG_INFO("topology: cpu%d offline -- excluded from pools", cpuId);
            continue;
        }
        g_allCpus.push_back(cpuId);
        std::string capPath = std::string("/sys/devices/system/cpu/cpu") + std::to_string(cpuId) + "/cpu_capacity";
        std::string vendorCapPath = std::string("/sys/devices/system/cpu/cpu_capacity/cpu") + std::to_string(cpuId) + "_capacity";
        std::string cpuinfoFreqPath = std::string("/sys/devices/system/cpu/cpu") + std::to_string(cpuId) + "/cpufreq/cpuinfo_max_freq";
        std::string capRaw = readFile(capPath.c_str());
        if (capRaw.empty()) capRaw = readFile(vendorCapPath.c_str());
        int cap = 0; bool cv = false;
        try {
            long v = std::stol(capRaw);
            if (v > 0) { cap = (int)v; cv = true; }
        } catch (...) {
            LOG_WARN("detectCpuTopology: cpu_capacity '%s' unparsable for cpu%d, using 0 (freq-only fallback)",
                     capRaw.c_str(), cpuId);
            cap = 0;
        }
        int cpuinfoFreq = readInt(cpuinfoFreqPath.c_str(), 0);
        caps.push_back(cv ? cap : topology::kDefaultCapacity);
        cpuinfoFreqs.push_back(cpuinfoFreq);
        capValid.push_back(cv);
        freqValid.push_back(cpuinfoFreq > 0);
        {
            std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
            g_cpuMetrics[cpuId].capacity = cap;
            g_cpuMetrics[cpuId].maxFreq = cpuinfoFreq;
        }
        std::string cpuStateDir = std::string(STATE_DIR) + "/cpu" + std::to_string(cpuId);
        if (mkdir(cpuStateDir.c_str(), 0755) != 0 && errno != EEXIST) {
            { int e1 = errno; char ebuf[128];
              LOG_WARN("detectCpuTopology: mkdir %s failed: %s", cpuStateDir.c_str(),
                       aatpeStrError(e1, ebuf, sizeof(ebuf))); }
        }
        writeFile((cpuStateDir + "/capacity").c_str(), cap);
        writeFile((cpuStateDir + "/max_freq").c_str(), cpuinfoFreq);
    }
    closedir(dir);
    {
        std::vector<std::tuple<int,int,int,bool,bool>> sorted;
        for (size_t i = 0; i < g_allCpus.size(); i++) {
            sorted.push_back({g_allCpus[i], caps[i], cpuinfoFreqs[i], capValid[i], freqValid[i]});
        }
        std::sort(sorted.begin(), sorted.end());
        g_allCpus.clear(); caps.clear(); cpuinfoFreqs.clear(); capValid.clear(); freqValid.clear();
        for (auto& e : sorted) {
            g_allCpus.push_back(std::get<0>(e));
            caps.push_back(std::get<1>(e));
            cpuinfoFreqs.push_back(std::get<2>(e));
            capValid.push_back(std::get<3>(e));
            freqValid.push_back(std::get<4>(e));
        }
    }
    g_cpuCount = (int)g_allCpus.size();
    long sysconfCpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (sysconfCpus > 0 && (int)sysconfCpus != g_cpuCount) {
        LOG_WARN("CPU count mismatch: /sys reports %d, sysconf reports %ld (using /sys count)",
                 g_cpuCount, sysconfCpus);
    }
    atomicWriteFile((std::string(STATE_DIR) + "/cpu_count").c_str(), g_cpuCount);
    int capFail = 0, freqFail = 0;
    for (size_t i = 0; i < capValid.size(); i++) {
        if (!capValid[i]) capFail++;
        if (!freqValid[i]) freqFail++;
    }
    if (capFail > 0) {
        for (size_t i = 0; i < caps.size(); i++) caps[i] = 0;
        LOG_WARN("topology: %d/%d cores capacity read failed -> capacity layer invalid, freq-only", capFail, g_cpuCount);
    }
    bool allCapsZero = true;
    for (int c : caps) if (c > 0) allCapsZero = false;
    if (allCapsZero) {
        std::map<int,int> cpuPart;
#ifndef AATPE_HOST_TEST
        {
            std::string ci;
            int cfd = open("/proc/cpuinfo", O_RDONLY | O_CLOEXEC);
            if (cfd >= 0) {
                char cbuf[8192]; ssize_t cn;
                while ((cn = read(cfd, cbuf, sizeof(cbuf))) > 0) ci.append(cbuf, cn);
                close(cfd);
            }
            size_t pos = 0;
            int curCpu = -1;
            while (pos < ci.size()) {
                size_t nl = ci.find('\n', pos);
                if (nl == std::string::npos) nl = ci.size();
                std::string line = ci.substr(pos, nl - pos);
                size_t c = line.find(':');
                if (c != std::string::npos) {
                    std::string key = line.substr(0, c);
                    std::string val = line.substr(c+1);
                    size_t a = val.find_first_not_of(" \t");
                    size_t b = val.find_last_not_of(" \t\r");
                    if (a != std::string::npos) val = val.substr(a, b-a+1);
                    if (key.find("processor") != std::string::npos) {
                        curCpu = atoi(val.c_str());
                    } else if (key.find("CPU part") != std::string::npos && curCpu >= 0) {
                        if (val.size() > 2 && val[0]=='0' && (val[1]=='x'||val[1]=='X'))
                            cpuPart[curCpu] = (int)strtol(val.c_str(), nullptr, 16);
                    }
                }
                pos = (nl == ci.size()) ? nl : nl + 1;
            }
        }
#endif
        for (size_t i = 0; i < g_allCpus.size(); i++) {
            int cpu = g_allCpus[i];
            int part = 0;
#ifdef AATPE_HOST_TEST
            if (g_testPartMap.count(cpu)) part = g_testPartMap[cpu];
#endif
            if (part == 0) {
                auto it = cpuPart.find(cpu);
                if (it != cpuPart.end()) part = it->second;
            }
            if (part > 0) caps[i] = inferCapacityFromPart(part);
        }
    }
    if (freqFail > 0) {
        for (size_t i = 0; i < cpuinfoFreqs.size(); i++) cpuinfoFreqs[i] = 0;
        LOG_WARN("topology: %d/%d cores cpuinfo_max_freq read failed -> freq layer degraded", freqFail, g_cpuCount);
    }
    if (capFail == 0 && freqFail == 0) markTopologyReady();
    if (cfg.littlePool.empty() || cfg.midPool.empty() || cfg.bigPool.empty()) {
        classifyCpuPools(caps, cpuinfoFreqs);
        if (g_bigPool.empty() && g_allCpus.size() >= 4) {
            if (!g_midPool.empty()) {
                g_bigPool.push_back(g_midPool.back());
                g_midPool.pop_back();
            } else if (g_littlePool.size() > 1) {
                g_bigPool.push_back(g_littlePool.back());
                g_littlePool.pop_back();
            }
        }
    } else {
        auto parsePool = [](const std::string& s) {
            std::vector<int> v;
            std::stringstream ss(s);
            std::string token;
            while (std::getline(ss, token, ',')) {
                trim(token);
                if (token.empty()) continue;
                size_t dash = token.find('-');
                if (dash != std::string::npos) {
                    try {
                        int lo = std::stoi(token.substr(0, dash));
                        int hi = std::stoi(token.substr(dash + 1));
                        if (lo < 0) lo = 0;
                        if (hi >= g_cpuCount) hi = g_cpuCount - 1;
                        for (int c = lo; c <= hi; c++) v.push_back(c);
                    } catch (...) {
                        LOG_DEBUG("parsePool: range token '%s' unparsable", token.c_str());
                    }
                } else {
                    try {
                        int c = std::stoi(token);
                        if (c >= 0 && c < g_cpuCount) v.push_back(c);
                    } catch (...) {
                        LOG_DEBUG("parsePool: token '%s' unparsable", token.c_str());
                    }
                }
            }
            return v;
        };
        g_littlePool = parsePool(cfg.littlePool);
        g_midPool = parsePool(cfg.midPool);
        g_bigPool = parsePool(cfg.bigPool);
    }
    applyManualPrimeOverride(cfg.primePoolCfg);
    atomicWriteFile((std::string(STATE_DIR) + "/little_pool").c_str(), joinCpuList(g_littlePool));
    atomicWriteFile((std::string(STATE_DIR) + "/mid_pool").c_str(), joinCpuList(g_midPool));
    atomicWriteFile((std::string(STATE_DIR) + "/big_pool").c_str(), joinCpuList(g_bigPool));
    atomicWriteFile((std::string(STATE_DIR) + "/prime_pool").c_str(), joinCpuList(primePoolLocked()));
    {
        std::string ctxt = clustersStateText();
        if (clustersTextPublishable(ctxt)) {
            atomicWriteFile((std::string(STATE_DIR) + "/clusters").c_str(), ctxt);
        } else {
            LOG_WARN("Topology: clusters text not publishable (k=%zu, len=%zu) -- keeping previous on-disk file",
                     g_clusters.size(), ctxt.size());
        }
        std::string raw;
        for (const auto& cl : g_clusters) {
            raw += tierName(cl.tier);
            raw += " cap=" + std::to_string(cl.capacity);
            raw += " " + std::to_string(cl.minKhz) + "-" + std::to_string(cl.maxKhz) + "kHz";
            raw += " cpus=[" + joinCpuList(cl.cpus) + "]\n";
        }
        atomicWriteFile((std::string(STATE_DIR) + "/clusters_raw_debug").c_str(), raw);
    }
    {
        std::string diag = "layer: capFail=" + std::to_string(capFail) +
                           " freqFail=" + std::to_string(freqFail) +
                           (capFail ? " (capacity invalid, freq-only)" : " (capacity OK)") + "\n";
        for (size_t i = 0; i < g_allCpus.size() && i < caps.size(); i++) {
            int cpu = g_allCpus[i];
            const char* tn = "UNKNOWN";
            for (const auto& cl : g_clusters) {
                if (std::find(cl.cpus.begin(), cl.cpus.end(), cpu) != cl.cpus.end()) { tn = tierName(cl.tier); break; }
            }
            diag += "cpu" + std::to_string(cpu) +
                    " cap=" + std::to_string(caps[i]) +
                    (i < capValid.size() ? (capValid[i] ? "(ok)" : "(fail)") : "(?)") +
                    " freq=" + std::to_string(i < cpuinfoFreqs.size() ? cpuinfoFreqs[i] : -1) +
                    " tier=" + tn + "\n";
        }
        diag += "result:\n" + clustersStateText() + "\n";
        atomicWriteFile((std::string(STATE_DIR) + "/topology_diagnostics").c_str(), diag);
    }
    LOG_INFO("CPUs=%d LITTLE=%zu MID=%zu BIG=%zu clusters=%zu",
             g_cpuCount, g_littlePool.size(), g_midPool.size(), g_bigPool.size(), g_clusters.size());
    gpuDetectPlatform();
    {
        int gp = gpuGetPlatform();
        std::string tag = gpuGetPlatformTag();
        std::string model = gpuGetModel();
        bool ok = (gp == (int)GPU_PLATFORM_KGSL || gp == (int)GPU_PLATFORM_MALI);
        if (ok) {
            LOG_INFO("GPU topology: platform=%s model=%s (sysfs nodes resolved)",
                     tag.c_str(), model.c_str());
        } else {
            LOG_WARN("GPU topology: platform=%s model=%s (no usable sysfs nodes)",
                     tag.c_str(), model.c_str());
        }
    }
    initScoringArrays(g_cpuCount);
}

void detectCpuTopology() {
    try {
        std::unique_lock<std::mutex> topoLock(g_topologyRwMutex);
        detectCpuTopologyUnlocked();
        seedOnlineCacheFromSysfs();
    } catch (const std::exception& e) {
        LOG_ERROR("detectCpuTopology() threw '%s' — keeping previous topology", e.what());
    } catch (...) {
        LOG_ERROR("detectCpuTopology() threw unknown exception — keeping previous topology");
    }
}
