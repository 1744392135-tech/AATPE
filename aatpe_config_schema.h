// AATPE - Config unified schema (#98/#99, partial)
// Defines ConfigField metadata (type, default, min, max, runtimeMutable, persisted,
// webuiVisible). New config items use this schema for validation; old items keep
// the existing parsing approach (gradual migration).
#ifndef AATPE_CONFIG_SCHEMA_H
#define AATPE_CONFIG_SCHEMA_H

#include <string>
#include <vector>

namespace aatpe {

enum class ConfigFieldType {
    INT,
    FLOAT,
    BOOL,
    STRING
};

struct ConfigField {
    const char* key;
    ConfigFieldType type;
    double defaultValue;
    double minValue;
    double maxValue;
    bool runtimeMutable;   // can be changed via hot-reload
    bool persisted;       // written to aatpe.conf on change
    bool webuiVisible;    // exposed in /state/all WebUI
};

// The schema for round-3 new config items.
// Old config items are NOT in this list (they use the legacy parser).
const std::vector<ConfigField>& configSchema();

// Validate a key/value pair against the schema. Returns true if the key is
// known and the value is within range. On out-of-range, clamps to min/max
// and returns true (with a warning). Unknown keys (not in schema) return false
// so the legacy parser can handle them.
bool configSchemaValidate(const std::string& key, const std::string& value,
                          std::string& outClampedValue);

// Look up a field by key. Returns nullptr if not found.
const ConfigField* configSchemaLookup(const std::string& key);

// ROUND-004: hot-reload effective-classification for a conf key, used by the
// WebUI to label a control "takes effect immediately" vs "restart required".
//   "live" : g_cfg is re-read every loop iteration -> effective immediately.
//   "hook" : applied at runtime via a registered reload hook (G4 eBPF / G1 HTTP).
//   "boot" : only read at subsystem init; a restart is required.
// Unknown keys default to "live" (the common case).
std::string configKeyHotReloadClass(const std::string& key);

} // namespace aatpe

#endif // AATPE_CONFIG_SCHEMA_H
