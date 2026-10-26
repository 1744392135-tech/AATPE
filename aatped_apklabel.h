// aatped_apklabel.h - resolve an installed package's real human-readable label
//                     straight out of its APK binary resources (zero dependencies).
//
// Problem being fixed:
//   The WebUI "游戏管理" picker (/api/apps) currently emits "label":"" for every
//   package (aatpe-cli.cpp cmdAppsJson, webui_http.cpp handleApps). Production
//   devices ship NO aapt, and neither `pm list packages` nor `dumpsys package`
//   returns the *resolved* application label — only the package name. Users then
//   see raw package names (com.netease.my ...) and, worse, ad-hoc guesses that
//   cross-wire look-alike packages (e.g. rendering a Cocos2d-x channel build of
//   造梦西游4 as if it were 梦幻西游).
//
// This module is the ONLY trustworthy source: it opens the installed base.apk,
// parses the binary AndroidManifest.xml (AXML) for <application android:label>,
// and when the attribute is a @resource reference it parses resources.arsc to
// resolve the string — choosing the best-matching device configuration
// (zh-CN preferred, then default). It falls through to split_*.apk resources
// when the label lives outside base.apk. There is deliberately NO package-name
// -> Chinese-name static mapping table anywhere in this file: the APK's own
// resources are the only source of truth.
//
// Degradation contract (untrusted input surface):
//   Every parser is written against hostile bytes. Any truncation, oversize
//   length, bad offset, or malformed chunk returns an empty string — never a
//   crash, never an out-of-bounds read, never a guessed name. Callers MUST
//   treat "" as "unknown, display the raw package name instead".
//
// Self-contained: standard C++17 only, no NDK extras, no third-party libs.
#ifndef AATPED_APKLABEL_H
#define AATPED_APKLABEL_H

#include <cstdint>
#include <string>
#include <vector>

namespace aatpe {
namespace apklabel {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Extract <application android:label> from a single APK file on disk.
//   apkPath      : path to a base.apk / split*.apk (a ZIP).
//   err (optional): receives a short ASCII diagnostic on failure.
// Returns the resolved label (UTF-8), or "" when the file cannot be read, is
// not a parseable ZIP, has no resolvable <application android:label>, or the
// referenced resource cannot be decoded. Never throws, never aborts on bad
// bytes.
std::string readLabelFromApk(const std::string& apkPath, std::string* err = nullptr);

// Resolve the installed label for package `pkg`.
//   packagesListPath : defaults to the on-device /data/system/packages.list.
//   appsRoot         : defaults to /data/app (real layout is
//                      /data/app/~~<rand>==/<pkg>-<rand>==/base.apk).
// Pipeline: (1) confirm pkg exists in packages.list; (2) locate base.apk under
// appsRoot (random -XXXX suffixes, multi-user/work dirs tolerated); (3) read
// the manifest label; if it is a resource reference, resolve it in base.apk's
// resources.arsc, and if that misses, in the sibling split_*.apk files.
// Returns "" when anything fails — caller MUST then display the package name.
std::string labelForPackage(const std::string& pkg,
                            const std::string& packagesListPath = "/data/system/packages.list",
                            const std::string& appsRoot = "/data/app");

// Non-blocking read-only cache probe: returns the cached label for `pkg` if a
// fresh (unexpired) entry exists — positive labels live 300s, negative (empty)
// entries live 15s. NEVER triggers any APK parse or filesystem IO; returns ""
// on miss or expiry so callers (the HTTP thread) can simply fall back to the
// package name while a background warmer runs labelForPackage(). Thread-safe.
std::string peekCachedLabel(const std::string& pkg);

// ---------------------------------------------------------------------------
// Testable pure helpers (also exercised by the host test harness)
// ---------------------------------------------------------------------------

// True iff packages.list text contains a line whose FIRST token is exactly
// `pkg` (line-anchored exact match — no substring, so org.cocos2dx.zmxy.* is
// never confused with com.netease.my).
bool packageListHasPackage(const std::string& packagesListText, const std::string& pkg);

// Pure directory-name matcher: given the packages.list text blob, the apps
// root to scan, and a bounded directory walk, return ordered candidate
// base.apk paths for `pkg`. Does no I/O beyond the root walk; every candidate
// is validated as an existing file by callers. Shared by labelForPackage and
// the host tests (which synthesize a fake /data/app tree).
std::vector<std::string> baseApkCandidates(const std::string& pkg,
                                           const std::string& packagesListText,
                                           const std::string& appsRoot);

// R7: List all third-party installed packages across all users.
// Reads the global /data/system/packages.list (which contains every user)
// and supplements with a bounded /data/app scan. System apps (uid < 10000)
// and packages installed to /system, /product, /vendor, /system_ext are
// excluded — only user-installed apps (uid >= 10000, base APK under /data/app)
// are returned. Fail-closed: any read error returns what was readable, never
// throws. This includes work-profile (user 10/99) apps that `pm list packages`
// omits by default.
std::vector<std::string> listInstalledPackagesAllUsers(
    const std::string& packagesListPath = "/data/system/packages.list",
    const std::string& appsRoot = "/data/app",
    const std::string& dataUserRoot = "/data/user");

// R7: Extract Android user ID from a packages.list uid field.
// Android: uid = 100000 * user_id + app_id. System apps (uid < 10000)
// are reported as user 0. Isolated (99xxx) and shared-uid (>99000) are
// decoded to their numeric user; callers can filter if needed.
int userIdFromUid(long uid);

} // namespace apklabel
} // namespace aatpe

#endif // AATPED_APKLABEL_H
