#ifndef AATPED_HMAC_H
#define AATPED_HMAC_H

#include <string>
#include <mutex>

extern char g_hmacKey[128];
extern std::string g_hmacKeyStatus;
extern bool g_hmacActive;
extern bool g_hmacRequired;

std::string getHmacKeyStatus(void);

extern std::mutex g_hmacMutex;

extern bool g_hmacReadonly;
bool isHmacReadonly(void);

void hmacForceReadonly(const char* reason);

void hmacClearReadonly(const char* reason);

bool verifyCommandHmac(const char* command, const char* signature);
std::string computeCommandHmac(const char* command);

void initHmacKey(void);
void checkHmacKeyRotation(void);

void shutdownHmacKey(void);

#endif
