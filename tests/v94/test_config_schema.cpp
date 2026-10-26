// test_config_schema.cpp - v94 hermetic unit tests for aatpe_config_schema.cpp.
// Links aatpe_config_schema.cpp + log_stub (see build.sh recipe).
// Exercises: field lookup, valid/invalid keys, INT clamping, BOOL parsing,
// FLOAT formatting, and unknown-key rejection.
#include <cstdio>
#include <string>
#include "aatpe_config_schema.h"

using namespace aatpe;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== ConfigSchema: schema is non-empty and lookup ===\n");
    CHECK(!configSchema().empty(), "schema table is populated");
    CHECK(configSchemaLookup("STATE_CACHE_TTL_HOURS") != nullptr,
          "known key STATE_CACHE_TTL_HOURS found");
    CHECK(configSchemaLookup("NO_SUCH_KEY_XYZ") == nullptr,
          "unknown key lookup returns nullptr");

    std::string out;

    printf("=== ConfigSchema: INT valid value ===\n");
    out = "unset";
    bool ok = configSchemaValidate("STATE_CACHE_TTL_HOURS", "48", out);
    CHECK(ok && out == "48", "INT within range accepted, value echoed");

    printf("=== ConfigSchema: INT out-of-range clamping ===\n");
    out = "";
    ok = configSchemaValidate("STATE_CACHE_TTL_HOURS", "99999", out);
    CHECK(ok && out == "720", "INT above max clamped to 720 (returns true)");
    out = "";
    ok = configSchemaValidate("PLACEMENT_MIN_GAIN_PCT", "-5", out);
    CHECK(ok && out == "0", "INT below min clamped to 0");

    printf("=== ConfigSchema: BOOL parsing ===\n");
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "true", out);
    CHECK(ok && out == "1", "BOOL 'true' -> '1'");
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "false", out);
    CHECK(ok && out == "0", "BOOL 'false' -> '0'");
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "0", out);
    CHECK(ok && out == "0", "BOOL numeric 0 -> '0'");
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "1", out);
    CHECK(ok && out == "1", "BOOL numeric 1 -> '1'");
    // RC6 (C5): case-insensitive bool parsing.
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "True", out);
    CHECK(ok && out == "1", "C5: BOOL 'True' -> '1'");
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "OFF", out);
    CHECK(ok && out == "0", "C5: BOOL 'OFF' -> '0'");
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "on", out);
    CHECK(ok && out == "1", "C5: BOOL 'on' -> '1'");
    // RC7 (C5 profile ML segment): bool keys ML_ENABLED/ML_HYBRID_ENABLED/
    // ML_COLD_START_PROTECTION now route through parseFlexBool (case-insensitive,
    // illegal value fail-safes to default). Cover the illegal-value rejection path
    // on the shared BOOL schema type; profile parse uses the same parser.
    out = "";
    ok = configSchemaValidate("ML_DRIFT_DETECTION", "maybe", out);
    CHECK(ok == false, "C5: BOOL illegal 'maybe' rejected (fail-safe to default)");

    printf("=== ConfigSchema: FLOAT formatting ===\n");
    out = "";
    ok = configSchemaValidate("MIN_USEFUL_CAPACITY_RATIO", "0.5", out);
    CHECK(ok && out == "0.500", "FLOAT formatted to 3 decimals");
    out = "";
    ok = configSchemaValidate("MIN_USEFUL_CAPACITY_RATIO", "9.9", out);
    CHECK(ok && out == "1.000", "FLOAT above max clamped to 1.0");

    printf("=== ConfigSchema: alias key recognized ===\n");
    CHECK(configSchemaLookup("MIGRATION_HYSTERESIS_PERCENT") != nullptr,
          "alias key _PERCENT is in the schema");

    printf("=== ConfigSchema: error paths ===\n");
    out = "";
    ok = configSchemaValidate("NO_SUCH_KEY_XYZ", "123", out);
    CHECK(ok == false, "unknown key rejected (legacy parser handles it)");
    out = "";
    ok = configSchemaValidate("STATE_CACHE_TTL_HOURS", "abc", out);
    CHECK(ok == false, "non-numeric INT value rejected");

    // ROUND-003 Defect-1: a trailing unit suffix must be tolerated (legacy
    // std::stoi accepted "500ms" as 500; the schema path used to reject it).
    printf("=== ConfigSchema ROUND-003: unit suffix tolerated ===\n");
    out = "";
    ok = configSchemaValidate("MIGRATION_COOLDOWN_MS", "500ms", out);
    CHECK(ok && out == "500", "'500ms' accepted as 500 (unit ignored)");
    out = "";
    ok = configSchemaValidate("MIGRATION_COOLDOWN_MS", "500 ms", out);
    CHECK(ok && out == "500", "'500 ms' accepted as 500 (space + unit)");
    out = "";
    ok = configSchemaValidate("STATE_CACHE_TTL_HOURS", " 500 ", out);
    CHECK(ok && out == "500", "' 500 ' (surrounding whitespace) accepted as 500");
    out = "";
    ok = configSchemaValidate("MIN_USEFUL_CAPACITY_RATIO", "0.5f", out);
    CHECK(ok && out == "0.500", "FLOAT '0.5f' unit suffix tolerated");

    // Still reject: empty / pure-non-numeric / no numeric prefix.
    printf("=== ConfigSchema ROUND-003: still rejected ===\n");
    out = "";
    ok = configSchemaValidate("MIGRATION_COOLDOWN_MS", "", out);
    CHECK(ok == false, "empty value rejected");
    out = "";
    ok = configSchemaValidate("MIGRATION_COOLDOWN_MS", "ms", out);
    CHECK(ok == false, "pure unit with no number rejected");

    // ROUND-004: dead key EBPF_CAPABILITY_DETECTION must no longer be registered.
    printf("=== ConfigSchema ROUND-004: dead key removed ===\n");
    CHECK(configSchemaLookup("EBPF_CAPABILITY_DETECTION") == nullptr,
          "EBPF_CAPABILITY_DETECTION removed from schema (dead key)");

    // ROUND-004: hot-reload classification metadata.
    printf("=== ConfigSchema ROUND-004: hot-reload class metadata ===\n");
    CHECK(configKeyHotReloadClass("DECISION_INTERVAL") == "live",
          "DECISION_INTERVAL classified live");
    CHECK(configKeyHotReloadClass("EBPF_ENABLED") == "hook",
          "EBPF_ENABLED classified hook (G4 wired)");
    CHECK(configKeyHotReloadClass("EBPF_RING_PAGES") == "hook",
          "EBPF_RING_PAGES classified hook (G4 wired)");
    CHECK(configKeyHotReloadClass("EBPF_POLL_FALLBACK") == "hook",
          "EBPF_POLL_FALLBACK classified hook (G4 wired)");
    CHECK(configKeyHotReloadClass("ENABLE_HTTP_SERVER") == "hook",
          "ENABLE_HTTP_SERVER classified hook (G1 wired)");
    CHECK(configKeyHotReloadClass("LITTLE_POOL") == "boot",
          "LITTLE_POOL classified boot (topology, restart)");
    CHECK(configKeyHotReloadClass("PRIME_POOL") == "boot",
          "PRIME_POOL classified boot (topology override, restart)");
    CHECK(configKeyHotReloadClass("REFRESH_RATE_EVENT_DRIVEN") == "boot",
          "REFRESH_RATE_EVENT_DRIVEN classified boot");
    CHECK(configKeyHotReloadClass("WATCHDOG_ENABLED") == "boot",
          "WATCHDOG_ENABLED classified boot (cross-process watchdog)");
    CHECK(configKeyHotReloadClass("WATCHDOG_MAX_RESTARTS") == "boot",
          "WATCHDOG_MAX_RESTARTS classified boot (cross-process watchdog)");
    CHECK(configKeyHotReloadClass("WATCHDOG_INDEP_THERMAL_WALL") == "boot",
          "WATCHDOG_INDEP_THERMAL_WALL classified boot (cross-process)");
    CHECK(configKeyHotReloadClass("STATE_CACHE_TTL_HOURS") == "live",
          "schema key defaults live");

    // ROUND-004: PRIME_POOL cpulist schema validation.
    printf("=== ConfigSchema ROUND-004: PRIME_POOL cpulist validation ===\n");
    out = "";
    ok = configSchemaValidate("PRIME_POOL", "", out);
    CHECK(ok && out.empty(), "PRIME_POOL empty = auto-detect (accepted)");
    out = "";
    ok = configSchemaValidate("PRIME_POOL", "6,7", out);
    CHECK(ok && out == "6,7", "PRIME_POOL '6,7' accepted");
    out = "";
    ok = configSchemaValidate("PRIME_POOL", "4-7", out);
    CHECK(ok && out == "4-7", "PRIME_POOL range '4-7' accepted");
    out = "";
    ok = configSchemaValidate("PRIME_POOL", "0,2-3,6", out);
    CHECK(ok && out == "0,2-3,6", "PRIME_POOL mixed list accepted");
    out = "";
    ok = configSchemaValidate("PRIME_POOL", "abc", out);
    CHECK(ok == false, "PRIME_POOL illegal chars 'abc' rejected");
    out = "";
    ok = configSchemaValidate("PRIME_POOL", "6;7", out);
    CHECK(ok == false, "PRIME_POOL separator ';' rejected");

    printf("\n========================================\n"
           "ConfigSchema results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
