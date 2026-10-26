// AATPE - Config unified schema (#98/#99, partial)
#include "aatped.h"
#include "aatpe_config_schema.h"
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cmath>
#include <set>

namespace aatpe {

static const std::vector<ConfigField> g_schema = {
    // #77: state/cache TTL (hours)
    { "STATE_CACHE_TTL_HOURS", ConfigFieldType::INT,   24,    1,   720, true,  true,  true  },
    // #35: placement three-value thresholds
    { "PLACEMENT_MIN_GAIN_PCT", ConfigFieldType::INT,   5,    0,   100, true,  true,  true  },
    { "PLACEMENT_MIN_CONFIDENCE", ConfigFieldType::INT, 50,    0,   100, true,  true,  true  },
    // #33: minimum useful capacity ratio
    { "MIN_USEFUL_CAPACITY_RATIO", ConfigFieldType::FLOAT, 0.3,  0.0,  1.0, true,  true,  true  },
    // round-2 existing items (for completeness / future migration)
    { "MIGRATION_HYSTERESIS_PCT", ConfigFieldType::INT,  5,    0,   50,  true,  true,  true  },
    // Alias: on-disk aatpe.conf historically uses the _PERCENT suffix.
    { "MIGRATION_HYSTERESIS_PERCENT", ConfigFieldType::INT, 5, 0, 50, true, true, true },
    { "MIGRATION_COOLDOWN_MS",    ConfigFieldType::INT, 500,  0,  5000, true,  true,  true  },
    { "MIGRATION_PENALTY_BIG_LITTLE", ConfigFieldType::FLOAT, 3.0, 1.0, 10.0, true, true, false },
    { "THREAD_ROLE_CONFIDENCE_THRESHOLD", ConfigFieldType::INT, 30, 0, 100, true, true, true },
    { "ML_DRIFT_DETECTION_ENABLED", ConfigFieldType::BOOL, 1.0, 0.0, 1.0, true, true, true },
    // Alias: on-disk aatpe.conf historically uses the shorter key.
    { "ML_DRIFT_DETECTION", ConfigFieldType::BOOL, 1.0, 0.0, 1.0, true, true, true },
    // ROUND-004: PRIME_POOL — manual cpulist override for the prime/big core pool
    // (empty = auto-detect). BOOT-only (topology re-detect required). String type.
    { "PRIME_POOL", ConfigFieldType::STRING, 0.0, 0.0, 0.0, false, true, true },
    // ROUND-004: EBPF_CAPABILITY_DETECTION removed — capability probing is a
    // mandatory security step with no runtime switch, and the key had zero
    // consumers (dead config). Do not re-add.
};

const std::vector<ConfigField>& configSchema() {
    return g_schema;
}

const ConfigField* configSchemaLookup(const std::string& key) {
    for (const auto& f : g_schema) {
        if (key == f.key) return &f;
    }
    return nullptr;
}

// ROUND-004: BOOT-only / hook-application classification. Most g_cfg fields are
// snapshotted into loopCfg every iteration, so they are "live". The exceptions
// are subsystems configured once at init and not re-driven by reloadConfig()
// unless a reload hook is registered for them.
std::string configKeyHotReloadClass(const std::string& key) {
    // Applied at runtime via aatpe::config::registerReloadHook.
    // G4 wired eBPF_ENABLED/EBPF_RING_PAGES/EBPF_POLL_FALLBACK; G1 wired
    // ENABLE_HTTP_SERVER — all now take effect on hot-reload via their hooks.
    static const std::set<std::string> hookKeys = {
        "EBPF_ENABLED", "EBPF_RING_PAGES", "EBPF_POLL_FALLBACK",
        "ENABLE_HTTP_SERVER",
    };
    // Only read at subsystem init; no reload hook exists yet -> restart required.
    // (HTTP server bind address / port range are still BOOT even though start/stop
    //  is hot, but those are not exposed as conf keys here.)
    // WATCHDOG_* are read by the standalone aatpe-watchdog process (cross-process),
    // not by aatped at runtime, so they are effectively boot for the daemon.
    static const std::set<std::string> bootKeys = {
        "LITTLE_POOL", "MID_POOL", "BIG_POOL", "PRIME_POOL",
        "REFRESH_RATE_EVENT_DRIVEN",
        "WATCHDOG_ENABLED", "WATCHDOG_INTERVAL",
        "WATCHDOG_MAX_RESTARTS", "WATCHDOG_RESTART_WINDOW",
        "WATCHDOG_INDEP_THERMAL_WALL",
    };
    if (hookKeys.count(key)) return "hook";
    if (bootKeys.count(key)) return "boot";
    return "live";
}

bool configSchemaValidate(const std::string& key, const std::string& value,
                          std::string& outClampedValue) {
    const ConfigField* f = configSchemaLookup(key);
    if (!f) return false;  // unknown key — let legacy parser handle it

    // ROUND-004: STRING fields (e.g. PRIME_POOL cpulist). Empty = "auto detect".
    // Otherwise accept only cpulist characters (digits, comma, dash, spaces);
    // reject anything else so a typo'd value is not silently adopted.
    if (f->type == ConfigFieldType::STRING) {
        std::string t = value;
        size_t a = t.find_first_not_of(" \t");
        size_t b = t.find_last_not_of(" \t");
        outClampedValue = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
        for (char c : outClampedValue) {
            if (!((c >= '0' && c <= '9') || c == ',' || c == '-' ||
                  c == ' ' || c == '\t')) {
                return false;  // illegal cpulist characters -> reject
            }
        }
        return true;
    }

    errno = 0;
    char* end = nullptr;
    double v = strtod(value.c_str(), &end);
    if (end == value.c_str()) {
        // Not a number — for BOOL, accept "1"/"0"/"true"/"false"/"yes"/"no"/"on"/"off".
        // RC6 (C5): case-insensitive (was == "true", so "True"/"TRUE" fell through to
        // the default). Normalize to lower case; unrecognized strings are rejected.
        if (f->type == ConfigFieldType::BOOL) {
            std::string lv;
            lv.reserve(value.size());
            for (char c : value) lv.push_back((char)tolower((unsigned char)c));
            if (lv == "1" || lv == "true" || lv == "yes" || lv == "on") {
                outClampedValue = "1";
                return true;
            }
            if (lv == "0" || lv == "false" || lv == "no" || lv == "off") {
                outClampedValue = "0";
                return true;
            }
        }
        return false;
    }
    // P1: reject NaN/Inf — strtod("nan")/("inf") parse successfully but
    // propagate non-finite values into ML weights and scoring.
    if (!std::isfinite(v)) return false;

    // P3-21 / ROUND-003 Defect-1: tolerate a trailing unit suffix. The legacy
    // parser used std::stoi() which silently stops at the first non-digit, so
    // values like MIGRATION_COOLDOWN_MS=500ms were accepted as 500. The schema
    // path previously required the whole string to be a number and rejected
    // "500ms" outright (falling back to the compiled default) — a benign
    // unit suffix that legacy configs use silently changed tuning.
    // Rule: once a non-empty numeric prefix parsed (end != value.c_str()), skip
    // trailing whitespace; if a non-empty remainder follows, treat it as a unit
    // suffix, adopt the numeric value, and LOG_WARN that the unit is ignored.
    // Empty / pure-non-numeric / NaN / Inf are still rejected above.
    {
        const char* p = end;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '\0') {
            // There is a non-digit remainder. Adopt the numeric prefix but warn so
            // the user learns the unit is not authoritative.
            LOG_WARN("[SCHEMA] %s='%s': non-numeric suffix '%s' treated as a unit and IGNORED, using value %g",
                     key.c_str(), value.c_str(), p, v);
        }
    }

    // P3-22: BOOL accepts ONLY 0 or 1 (numeric path). Previously 0.3 -> "0"
    // and 0.7 -> "1" were silently clamped into a boolean, which is not a valid
    // bool representation. Reject any other numeric value; the string forms
    // "true"/"false"/"yes"/"no" are handled by the end==value.c_str() branch.
    if (f->type == ConfigFieldType::BOOL && v != 0.0 && v != 1.0) {
        return false;
    }

    // Clamp to [min, max]
    if (v < f->minValue) v = f->minValue;
    if (v > f->maxValue) v = f->maxValue;

    if (f->type == ConfigFieldType::INT) {
        outClampedValue = std::to_string((long long)v);
    } else if (f->type == ConfigFieldType::BOOL) {
        // v is guaranteed to be exactly 0.0 or 1.0 by the P3-22 check above.
        outClampedValue = (v >= 0.5) ? "1" : "0";
    } else {
        // FLOAT — format with reasonable precision
        char buf[32];
        snprintf(buf, sizeof(buf), "%.3f", v);
        outClampedValue = buf;
    }
    return true;
}

} // namespace aatpe
