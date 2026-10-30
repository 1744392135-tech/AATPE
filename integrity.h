#ifndef AATPE_INTEGRITY_H
#define AATPE_INTEGRITY_H

#include <string>
#include <vector>
#include <cstdint>
#include "aatpe_crypto.h"

namespace aatpe {
namespace integrity {

enum class ReadState { Clean, DefiniteTamper, Transient };

struct FileEntry {
    std::string relPath;
    std::string absPath;
    std::string hexDigest;
    bool        exists = false;
    ReadState   readState = ReadState::Clean;
};

struct Sha256Ctx {
    aatpe_crypto::Sha256Context inner;
};

void sha256Init(Sha256Ctx* ctx);
void sha256Update(Sha256Ctx* ctx, const uint8_t* data, size_t len);
void sha256Final(Sha256Ctx* ctx, uint8_t out[32]);
void sha256OneShot(const uint8_t* data, size_t len, uint8_t out[32]);

std::string toHexStr(const uint8_t* digest, size_t len = 32);

bool hexEqual(const std::string& a, const std::string& b);

std::string resolveModuleRoot();

void setModuleRootOverride(const std::string& root);

std::vector<FileEntry> enumerateCriticalFiles(const std::string& moduleRoot);

std::vector<std::string> enumerateCriticalRelPaths();

bool criticalSetComplete(const std::string& moduleRoot, std::string& firstMissing);

bool generateAndStoreManifestAt(const std::string& moduleRoot);

std::string sha256FileHex(const std::string& absPath);

std::string hashFileHex(const std::string& absPath, int* errOut);

std::string computeChainedManifest(const std::vector<FileEntry>& entries);

std::string readStoredManifest();

bool generateAndStoreManifest();

bool verifyModuleIntegrity();

std::string getIntegrityReport();

int lastVerdict();

enum class IntegrityVerdict : int { Unknown = -1, Ok = 1, Failed = 0 };

IntegrityVerdict verdict();

bool privilegedOpsAllowed();

using IntegrityFailHook = void (*)(const char* reason);
void setIntegrityFailHook(IntegrityFailHook hook);

using IntegrityRestoreHook = void (*)(const char* reason);
void setIntegrityRestoreHook(IntegrityRestoreHook hook);

#ifdef AATPE_HOST_TEST
void integrityTestInjectTransient(const std::string& absPath);
void integrityTestClearInject();
#endif

#ifdef AATPE_HOST_TEST
long integrityTestHashFileCount();
void integrityTestResetHashFileCount();
#endif

#ifdef AATPE_HOST_TEST
void setIntegrityManifestPathForTest(const std::string& path);
#endif
bool verifyAgainstRootAndManifest(const std::string& root, const std::string& storedManifest);

bool runBootGate();

#ifdef AATPE_HOST_TEST
void setIntegrityVerdictForTest(IntegrityVerdict v);
#endif

std::string manifestPath();

std::string snapshotValueSha256(const std::string& kvFilePath);

bool verifySnapshotFile(const std::string& kvFilePath);

}
}

#endif
