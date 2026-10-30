#include "aatped.h"
#include "aatpe_config_schema.h"
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cmath>
#include <set>

namespace aatpe {

static const std::vector<ConfigField> g_schema = {
    { "STATE_CACHE_TTL_HOURS", ConfigFieldType::INT,   24,    1,   720, true,  true,  true  },
    { "PLACEMENT_MIN_GAIN_PCT", ConfigFieldType::INT,   5,    0,   100, true,  true,  true  },
    { "PLACEMENT_MIN_CONFIDENCE", ConfigFieldType::INT, 50,    0,   100, true,  true,  true  },
    { "MIN_USEFUL_CAPACITY_RATIO", ConfigFieldType::FLOAT, 0.3,  0.0,  1.0, true,  true,  true  },
    { "MIGRATION_HYSTERESIS_PCT", ConfigFieldType::INT,  5,    0,   50,  true,  true,  true  },
    { "MIGRATION_HYSTERESIS_PERCENT", ConfigFieldType::INT, 5, 0, 50, true, true, true },
    { "MIGRATION_COOLDOWN_MS",    ConfigFieldType::INT, 500,  0,  5000, true,  true,  true  },
    { "MIGRATION_PENALTY_BIG_LITTLE", ConfigFieldType::FLOAT, 3.0, 1.0, 10.0, true, true, false },
    { "THREAD_ROLE_CONFIDENCE_THRESHOLD", ConfigFieldType::INT, 30, 0, 100, true, true, true },
    { "ML_DRIFT_DETECTION_ENABLED", ConfigFieldType::BOOL, 1.0, 0.0, 1.0, true, true, true },
    { "ML_DRIFT_DETECTION", ConfigFieldType::BOOL, 1.0, 0.0, 1.0, true, true, true },
    { "PRIME_POOL", ConfigFieldType::STRING, 0.0, 0.0, 0.0, false, true, true },
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

std::string configKeyHotReloadClass(const std::string& key) {
    static const std::set<std::string> hookKeys = {
        "EBPF_ENABLED", "EBPF_RING_PAGES", "EBPF_POLL_FALLBACK",
        "ENABLE_HTTP_SERVER",
    };
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
    if (!f) return false;

    if (f->type == ConfigFieldType::STRING) {
        std::string t = value;
        size_t a = t.find_first_not_of(" \t");
        size_t b = t.find_last_not_of(" \t");
        outClampedValue = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
        for (char c : outClampedValue) {
            if (!((c >= '0' && c <= '9') || c == ',' || c == '-' ||
                  c == ' ' || c == '\t')) {
                return false;
            }
        }
        return true;
    }

    errno = 0;
    char* end = nullptr;
    double v = strtod(value.c_str(), &end);
    if (end == value.c_str()) {
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
    if (!std::isfinite(v)) return false;

    {
        const char* p = end;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '\0') {
            LOG_WARN("[SCHEMA] %s='%s': non-numeric suffix '%s' treated as a unit and IGNORED, using value %g",
                     key.c_str(), value.c_str(), p, v);
        }
    }

    if (f->type == ConfigFieldType::BOOL && v != 0.0 && v != 1.0) {
        return false;
    }

    if (v < f->minValue) v = f->minValue;
    if (v > f->maxValue) v = f->maxValue;

    if (f->type == ConfigFieldType::INT) {
        outClampedValue = std::to_string((long long)v);
    } else if (f->type == ConfigFieldType::BOOL) {
        outClampedValue = (v >= 0.5) ? "1" : "0";
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.3f", v);
        outClampedValue = buf;
    }
    return true;
}

}
