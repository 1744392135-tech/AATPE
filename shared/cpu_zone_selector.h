#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <climits>

namespace thermzone {

struct ZoneReading {
    std::string name;
    std::string typeLower;
    int tempC;
    bool valid;
};

struct ZoneHist {
    int lastGoodTemp = INT_MIN;
    int badStreak    = 0;
    int spikeStreak  = 0;
    int goodStreak   = 0;
    bool rogue       = false;
};

struct SelectResult {
    int trustedCpuTempC;
    int anyCpuTempC;
    std::string selectedName;
    std::vector<std::string> rogueNames;
    std::vector<std::string> cpuNames;
};

inline bool isCpuDieType(const std::string& t) {
    static const char* kCpuPrefix[] = {
        "mtktscpu","mtktsap","mtktspa","mtktsmcu",
        "cpu","soc","cluster","tscpu","bigcore","littlecore",
        "cdsp","adsp","cpuss","cpu_","cpu-",
        "ap_therm"
    };
    static const char* kDenylist[] = {
        "battery","charger","pmic","lcd","wmt","conn","bms",
        "quiet","tzimg","skin","gpuss","gpu_",
        "pm8","pm6","pm8937","pmic_therm"
    };
    for (auto d : kDenylist) if (t.find(d) != std::string::npos) return false;
    for (auto p : kCpuPrefix) if (t.find(p) != std::string::npos) return true;
    return false;
}

static constexpr int kMaxSlewC      = 25;
static constexpr int kFollowBandC   = 6;
static constexpr int kRogueBadStreak  = 2;
static constexpr int kRogueSpikeStreak= 2;
static constexpr int kConsensusDivergeC = 12;
static constexpr int kEmergencyWallC  = 110;

inline SelectResult select(const std::vector<ZoneReading>& zones,
                           std::map<std::string, ZoneHist>& hist) {
    SelectResult r; r.trustedCpuTempC = 0; r.anyCpuTempC = 0;

    std::vector<const ZoneReading*> cpus;
    for (auto& z : zones) {
        if (!isCpuDieType(z.typeLower)) continue;
        r.cpuNames.push_back(z.name);
        ZoneHist& h = hist[z.name];
        if (!z.valid || z.tempC == INT_MIN) { h.badStreak++; h.goodStreak = 0; continue; }
        cpus.push_back(&z);
    }
    if (cpus.empty()) return r;

    const bool multiCpuZone = (cpus.size() >= 2);
    for (auto z : cpus) {
        ZoneHist& h = hist[z->name];
        if (h.lastGoodTemp == INT_MIN) {
            h.lastGoodTemp = z->tempC;
            continue;
        }
        int slew = z->tempC - h.lastGoodTemp;
        if (multiCpuZone && abs(slew) > kMaxSlewC) {
            int followers = 0, others = 0; (void)others;
            for (auto o : cpus) {
                if (o == z) continue;
                others++;
                ZoneHist& ho = hist[o->name];
                if (ho.lastGoodTemp != INT_MIN &&
                    abs((o->tempC - ho.lastGoodTemp) - slew) <= kFollowBandC) followers++;
            }
            bool unfollowed = (followers == 0);
            if (unfollowed) {
                h.spikeStreak++;
                continue;
            }
        }
        h.lastGoodTemp = z->tempC;
        if (h.spikeStreak > 0) h.spikeStreak--;
        if (h.badStreak > 0) {
            h.goodStreak++;
            if (h.goodStreak >= 2) { h.badStreak--; h.goodStreak = 0; }
        }
    }

    {
        std::vector<int> ts;
        for (auto z : cpus) ts.push_back(z->tempC);
        std::sort(ts.begin(), ts.end());
        int medianC = ts[(ts.size()-1)/2];
        for (auto& kv : hist) {
            ZoneHist& h = kv.second;
            bool timeBad = (h.badStreak >= kRogueBadStreak) || (h.spikeStreak >= kRogueSpikeStreak);
            bool inCpus = false;
            for (auto z : cpus) if (z->name == kv.first) { inCpus = true; break; }
            if (inCpus) {
                int t = 0; for (auto z : cpus) if (z->name==kv.first) t=z->tempC;
                bool diverges = abs(t - medianC) > kConsensusDivergeC;
                h.rogue = timeBad && diverges;
            } else {
                h.rogue = timeBad;
            }
        }
    }

    int trustedMax = 0, anyMax = 0;
    bool sawTrusted = false;
    for (auto z : cpus) {
        anyMax = std::max(anyMax, z->tempC);
        ZoneHist& h = hist[z->name];
        if (h.rogue) { r.rogueNames.push_back(z->name); continue; }
        trustedMax = std::max(trustedMax, z->tempC);
        sawTrusted = true;
        if (r.selectedName.empty()) r.selectedName = z->name;
    }
    for (auto& kv : hist) {
        bool inCpus = false;
        for (auto z : cpus) if (z->name == kv.first) { inCpus = true; break; }
        if (!inCpus && kv.second.rogue) r.rogueNames.push_back(kv.first);
    }
    r.trustedCpuTempC = trustedMax;
    r.anyCpuTempC = anyMax;
    if (!sawTrusted) { r.trustedCpuTempC = r.anyCpuTempC; r.rogueNames.clear(); }
    return r;
}

}
