// AATPE - Helper directory content audit (#74)
#include "aatped.h"
#include "aatpe_helper_audit.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>

// Exact basenames that MUST NOT appear in the helper directory (world-readable).
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

// G1: dotfile-prefix sensitive basename fragments. A file starting with "."
// whose name contains a secret/token/key word is treated as sensitive. This
// deliberately does NOT use loose substrings on normal config/asset names like
// "thread_roles.conf.bak" or "ml_config.json" — those are legitimate user files.
static const char* kSensitiveDotfileHints[] = {
    "token",
    "hmac",
    "secret",
    "key",
    nullptr
};

// Extension whitelist: config/asset extensions that must NEVER be deleted even
// if a name coincidentally contains a sensitive substring.
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
    // Exact basename match FIRST (state.json/decisions.json/threads.json etc.
    // are explicitly sensitive even though .json is also a protected asset ext).
    for (int i = 0; kSensitiveHelperFiles[i]; i++) {
        if (strcmp(name, kSensitiveHelperFiles[i]) == 0) return true;
    }
    // Extension-protected config/asset files are never deleted.
    if (isProtectedAssetExtension(name)) return false;
    // Dotfile-prefix: ".daemon_token", ".hmac_key.bak" etc.
    if (name[0] == '.') {
        for (int i = 0; kSensitiveDotfileHints[i]; i++) {
            if (strstr(name, kSensitiveDotfileHints[i])) return true;
        }
    }
    return false;
}

// P2-15 : recursive delete WITHOUT invoking the shell. The old
// code did system("rm -rf \"" + path + "\"") where path included a filename read
// from readdir(); even though filenames are kernel-controlled, routing deletion
// through /bin/sh is an injection risk. This walks the tree with fdopendir() /
// unlinkat(), never exec()ing anything. Filenames are also whitelist-checked.
static bool safeFilename(const char* name) {
    if (!name || !*name) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char* p = name; *p; p++) {
        // only allow [A-Za-z0-9._-]
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
        // R8-024 / LED-131: record the inode we actually opened. There is a window
        // between opening this directory and the final unlinkat(parentDirFd, name)
        // below (the whole recursive walk). If a local attacker swaps the directory
        // entry in that window, a naive unlinkat would remove the replacement
        // instead of the tree we walked. fstatat(AT_SYMLINK_NOFOLLOW) the entry
        // again right before removing and only unlink when it is still the same
        // inode we opened and walked.
        struct stat openedSt;
        bool haveOpenedSt = (fstat(fd, &openedSt) == 0);

        // It's a directory: recurse into it.
        DIR* d = fdopendir(fd);
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                if (!safeFilename(de->d_name)) continue;
                unlinkatRecursive(fd, de->d_name);
            }
            closedir(d);  // closes fd too
        } else {
            close(fd);
        }
        // Verify the top-level entry still resolves to the inode we walked before
        // removing it. If it was swapped (or vanished), do NOT unlink — deleting a
        // replacement is exactly the TOCTOU this guard prevents.
        struct stat nowSt;
        if (haveOpenedSt &&
            fstatat(parentDirFd, name, &nowSt, AT_SYMLINK_NOFOLLOW) == 0 &&
            nowSt.st_ino == openedSt.st_ino &&
            nowSt.st_dev == openedSt.st_dev) {
            unlinkat(parentDirFd, name, AT_REMOVEDIR);
        }
    } else {
        // Not a directory (or unreadable): try to unlink as a file.
        unlinkat(parentDirFd, name, 0);
    }
}

int helperDirAudit() {
    // The helper directory is the module's helper/ subdir. We use the module dir
    // resolved at runtime. If it doesn't exist, nothing to do.
    std::string moduleDir = getModuleDir();
    if (moduleDir.empty()) return 0;
    std::string helperDir = moduleDir + "/helper";

    // G1-018: pin the directory by fd BEFORE enumerating. The old order
    // (opendir() then open() a second fd) left a window where the directory
    // entry could be swapped between the two. Open the dirfd first, wrap it in
    // fdopendir; closedir() then closes the fd.
    int helperFd = ::open(helperDir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR* dir = (helperFd >= 0) ? ::fdopendir(helperFd) : nullptr;
    if (!dir) {
        if (helperFd >= 0) ::close(helperFd);
        return 0;  // not an error — helper dir may not exist yet
    }

    int removed = 0;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        // P2: skip ONLY "." and "..". The old d_name[0]=='.' guard skipped all
        // dotfiles, letting sensitive dotfiles under a scanned dir evade the
        // extension/permission audit.
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (!isSensitiveHelperFile(de->d_name)) continue;
        if (!safeFilename(de->d_name)) continue;  // P2-15: never touch odd names

        LOG_WARN("HELPER-AUDIT: matched sensitive rule, deleting: %s", de->d_name);
        unlinkatRecursive(helperFd, de->d_name);
        removed++;
        LOG_WARN("HELPER-AUDIT: removed sensitive file from helper/: %s", de->d_name);
    }
    closedir(dir);  // closes helperFd too

    if (removed > 0) {
        LOG_WARN("HELPER-AUDIT: removed %d sensitive file(s) from helper dir", removed);
    }
    return removed;
}
