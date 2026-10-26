#ifndef AATPE_HOST_STUB_SYS_SYSTEM_PROPERTIES_H
#define AATPE_HOST_STUB_SYS_SYSTEM_PROPERTIES_H
#include <cstddef>
#define PROP_VALUE_MAX 92
extern "C" int __system_property_get(const char* name, char* value);
#endif
