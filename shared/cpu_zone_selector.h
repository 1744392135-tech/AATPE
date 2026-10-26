// cpu_zone_selector.h — generic CPU thermal zone selector (shared by thermal.cpp +
// aatpe-watchdog.cpp). PURE, state carried in caller-owned history map.
//
// W13 P1 design: a zone is judged rogue ONLY on (time-series defect) AND (statistical
// disagreement with majority). A legitimately hotter BIG core (85C vs LITTLE 65C,
// smooth ramp, clean samples) is NEVER rogue. WARNING/profile uses the ROBUST HIGH side
// (max of trusted zones), so a real 85C die still triggers WARNING. Emergency wall still
// uses anyCpu max (>=110C) and is unchanged.
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
    int tempC;       // normalized C, or INT_MIN if unreadable/-1/out-of-range this scan
    bool valid;
};

struct ZoneHist {
    int lastGoodTemp = INT_MIN;   // last accepted sample
    int badStreak    = 0;          // consecutive invalid/-1 scans
    int spikeStreak  = 0;          // consecutive unfollowed single-point spikes
    int goodStreak   = 0;          // consecutive valid reads (badStreak decay counter)
    bool rogue       = false;
};

struct SelectResult {
    int trustedCpuTempC;   // max over NON-rogue valid CPU zones (robust high side)
    int anyCpuTempC;       // max over ALL valid CPU zones (emergency wall)
    std::string selectedName;
    std::vector<std::string> rogueNames;
    std::vector<std::string> cpuNames;
};

inline bool isCpuDieType(const std::string& t) {
    static const char* kCpuPrefix[] = {
        "mtktscpu","mtktsap","mtktspa","mtktsmcu",
        "cpu","soc","cluster","tscpu","bigcore","littlecore",
        "cdsp","adsp","cpuss","cpu_","cpu-",
        "ap_therm"   // P3: AP = application-processor/SoC die, NOT skin
    };
    static const char* kDenylist[] = {
        "battery","charger","pmic","lcd","wmt","conn","bms",
        "quiet","tzimg","skin","gpuss","gpu_",
        // P3: Qualcomm pm8xxx/pm6xxx nodes are PMIC sensors (pm8937_therm,
        // pm6359...), not the CPU/SoC die. The bare "pm8" prefix historically
        // misclassified the PMIC temperature as a CPU die reading and folded it
        // into the emergency wall / trusted high-side. The real SoC die on
        // Qualcomm is exposed as cpu-*-thermal / soc / cpuss, never pm8xxx.
        "pm8","pm6","pm8937","pmic_therm"
    };
    for (auto d : kDenylist) if (t.find(d) != std::string::npos) return false;
    for (auto p : kCpuPrefix) if (t.find(p) != std::string::npos) return true;
    return false;
}

// Tunable generic constants.
static constexpr int kMaxSlewC      = 25;   // scan-to-scan C that is physically implausible solo
static constexpr int kFollowBandC   = 6;    // other zone moved within this => "follows" the spike
static constexpr int kRogueBadStreak  = 2;    // N invalid/-1 scans => rogue
static constexpr int kRogueSpikeStreak= 2;    // N unfollowed spikes => rogue
static constexpr int kConsensusDivergeC = 12;  // diverge from CPU median by >12C
// THM-11: documented reference value (the real emergency wall comes from Config
// at runtime). Kept because test_thermzone.cpp asserts against this literal.
static constexpr int kEmergencyWallC  = 110;

inline SelectResult select(const std::vector<ZoneReading>& zones,
                           std::map<std::string, ZoneHist>& hist) {
    SelectResult r; r.trustedCpuTempC = 0; r.anyCpuTempC = 0;

    // Collect CPU-typed zones (valid this scan). Track invalid ones for badStreak.
    std::vector<const ZoneReading*> cpus;
    for (auto& z : zones) {
        if (!isCpuDieType(z.typeLower)) continue;
        r.cpuNames.push_back(z.name);
        ZoneHist& h = hist[z.name];
        if (!z.valid || z.tempC == INT_MIN) { h.badStreak++; h.goodStreak = 0; continue; }
        cpus.push_back(&z);
    }
    if (cpus.empty()) return r;

    // For each zone, decide if THIS sample is an unfollowed spike:
    //   |t - lastGood| > kMaxSlewC AND no other valid CPU zone moved a similar amount.
    // A clean smooth ramp (others move in the same direction) is NOT a spike.
    // D-005: with only ONE CPU zone there are no "other" zones to follow, so the
    // spike test is meaningless: every >25C slew would be flagged unfollowed and
    // spikeStreak would accumulate forever (it can never promote the lone zone
    // to rogue because median==itself -> diverges==false, but the stale streak
    // could misfire if a second zone later hot-plugs in). On a single-zone device
    // accept every valid sample outright (update lastGood, decay defect counters)
    // and skip spike detection entirely.
    const bool multiCpuZone = (cpus.size() >= 2);
    for (auto z : cpus) {
        ZoneHist& h = hist[z->name];
        if (h.lastGoodTemp == INT_MIN) {
            h.lastGoodTemp = z->tempC;   // first sample seeds baseline
            continue;
        }
        int slew = z->tempC - h.lastGoodTemp;
        if (multiCpuZone && abs(slew) > kMaxSlewC) {
            // Did the MAJORITY of other CPU zones also move this scan? Count follow.
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
                h.spikeStreak++;          // reject this sample; keep lastGood
                continue;                 // do NOT update lastGoodTemp
            }
        }
        // Accepted sample: smooth ramp or flat. Decode defect counters toward good.
        // W14 P2: badStreak now decays symmetrically with spikeStreak, but SLOWER
        // than marking (mark needs 2 consecutive bad reads; recovery needs ~2 valid
        // reads per decay unit, so a persistently-flaky zone is not prematurely cleared).
        h.lastGoodTemp = z->tempC;
        if (h.spikeStreak > 0) h.spikeStreak--;
        if (h.badStreak > 0) {
            h.goodStreak++;
            if (h.goodStreak >= 2) { h.badStreak--; h.goodStreak = 0; }
        }
    }

    // Rogue = time-series defect (badStreak/spikeStreak) AND sustained statistical
    // divergence from the CPU consensus median. A clean big-core zone (smooth ramp, no
    // invalid reads, no unfollowed spikes) is NEVER rogue even if it is the hottest.
    // Invalid zones (not in cpus this scan): badStreak>=threshold => rogue outright
    // (they have no temp to contribute, and their time-series is broken).
    {
        std::vector<int> ts;
        for (auto z : cpus) ts.push_back(z->tempC);
        std::sort(ts.begin(), ts.end());
        int medianC = ts[(ts.size()-1)/2];   // lower median: with 2 zones => the cooler one
        for (auto& kv : hist) {
            ZoneHist& h = kv.second;
            bool timeBad = (h.badStreak >= kRogueBadStreak) || (h.spikeStreak >= kRogueSpikeStreak);
            // Only evaluate divergence for zones that produced a valid temp this scan.
            bool inCpus = false;
            for (auto z : cpus) if (z->name == kv.first) { inCpus = true; break; }
            if (inCpus) {
                int t = 0; for (auto z : cpus) if (z->name==kv.first) t=z->tempC;
                bool diverges = abs(t - medianC) > kConsensusDivergeC;
                h.rogue = timeBad && diverges;
            } else {
                h.rogue = timeBad;   // invalid this scan: time-bad => rogue
            }
        }
    }

    // Assemble: trusted = HIGH side (max of non-rogue valid zones). anyCpu = global max.
    // RC6 (C2): use an explicit "saw a trusted zone" flag as the fail-safe sentinel,
    // NOT trustedCpuTempC==0. A genuinely-trusted zone reading 0C must NOT be mistaken
    // for "nothing trusted" and trigger the anyMax fallback + rogueNames.clear().
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
    // Also surface invalid-but-rogue zones (not in cpus this scan) in rogueNames.
    for (auto& kv : hist) {
        bool inCpus = false;
        for (auto z : cpus) if (z->name == kv.first) { inCpus = true; break; }
        if (!inCpus && kv.second.rogue) r.rogueNames.push_back(kv.first);
    }
    r.trustedCpuTempC = trustedMax;
    r.anyCpuTempC = anyMax;
    // Fail-safe: ONLY when no trusted zone survived filtering.
    if (!sawTrusted) { r.trustedCpuTempC = r.anyCpuTempC; r.rogueNames.clear(); }
    return r;
}

} // namespace thermzone
