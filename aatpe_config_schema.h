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
    bool runtimeMutable;
    bool persisted;
    bool webuiVisible;
};

const std::vector<ConfigField>& configSchema();

bool configSchemaValidate(const std::string& key, const std::string& value,
                          std::string& outClampedValue);

const ConfigField* configSchemaLookup(const std::string& key);

std::string configKeyHotReloadClass(const std::string& key);

}

#endif
