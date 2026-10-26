#ifndef AATPE_STUB_SYSPROPS_H
#define AATPE_STUB_SYSPROPS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PROP_VALUE_MAX 92
int __system_property_get(const char* name, char* value);
#ifdef __cplusplus
}
#endif
#endif
