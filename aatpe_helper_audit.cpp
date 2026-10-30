#include "aatped.h"
#include "aatpe_helper_audit.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>

static const char* kSensitiveHelperFiles[] = {
    "daemon_token",
    "hmac_key",
    "model.dat",
    "model.bin",
    "ml_model",
    "thread_db",
    "thread_db.json",
    "decisions",
    "decisions.json",
    "state.json",
    "snapshot.json",
    "threads.json",
    "explain.json",
    "config.json",
    nullptr
};

static const char* kSensitiveDotfileHints[] = {
    "token",
    "hmac",
    "secret",
    "key",
    nullptr
};

static bool isProtectedAssetExtension(const char* name) {
    size_t n = strlen(name);
    const char* exts[] = {".conf", ".rules", ".json", ".profiles", ".xml",
                          ".prop", ".txt", ".md", nullptr};
    for (int i = 0; exts[i]; ++i) {
        size_t el = strlen(exts[i]);
        if (n >= el && strcmp(name + n - el, exts[i]) == 0) return true;
    }
    return false;
}

bool isSensitiveHelperFile(const char* name) {
    if (!name || !*name) return false;
    for (int i = 0; kSensitiveHelperFiles[i]; i++) {
        if (strcmp(name, kSensitiveHelperFiles[i]) == 0) return true;
    }
    if (isProtectedAssetExtension(name)) return false;
    if (name[0] == '.') {
        for (int i = 0; kSensitiveDotfileHints[i]; i++) {
            if (strstr(name, kSensitiveDotfileHints[i])) return true;
        }
    }
    return false;
}

static bool safeFilename(const char* name) {
    if (!name || !*name) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char* p = name; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static void unlinkatRecursive(int parentDirFd, const char* name) {
    int fd = openat(parentDirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat openedSt;
        bool haveOpenedSt = (fstat(fd, &openedSt) == 0);

        DIR* d = fdopendir(fd);
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                if (!safeFilename(de->d_name)) continue;
                unlinkatRecursive(fd, de->d_name);
            }
            closedir(d);
        } else {
            close(fd);
        }
        struct stat nowSt;
        if (haveOpenedSt &&
            fstatat(parentDirFd, name, &nowSt, AT_SYMLINK_NOFOLLOW) == 0 &&
            nowSt.st_ino == openedSt.st_ino &&
            nowSt.st_dev == openedSt.st_dev) {
            unlinkat(parentDirFd, name, AT_REMOVEDIR);
        }
    } else {
        unlinkat(parentDirFd, name, 0);
    }
}

int helperDirAudit() {
    std::string moduleDir = getModuleDir();
    if (moduleDir.empty()) return 0;
    std::string helperDir = moduleDir + "/helper";

    int helperFd = ::open(helperDir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR* dir = (helperFd >= 0) ? ::fdopendir(helperFd) : nullptr;
    if (!dir) {
        if (helperFd >= 0) ::close(helperFd);
        return 0;
    }

    int removed = 0;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (!isSensitiveHelperFile(de->d_name)) continue;
        if (!safeFilename(de->d_name)) continue;

        LOG_WARN("HELPER-AUDIT: matched sensitive rule, deleting: %s", de->d_name);
        unlinkatRecursive(helperFd, de->d_name);
        removed++;
        LOG_WARN("HELPER-AUDIT: removed sensitive file from helper/: %s", de->d_name);
    }
    closedir(dir);

    if (removed > 0) {
        LOG_WARN("HELPER-AUDIT: removed %d sensitive file(s) from helper dir", removed);
    }
    return removed;
}
