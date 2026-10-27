// c4_freezer.h — C4 in-background cgroup freezer (v4.4.4; default OFF until
// v6.0.0 round4c, now DEFAULT ON with foreground/audio/IM whitelist + auto-degrade).
//
// v6.1.0: cgroup v1 freezer fallback added. On kernels < 5.1 (e.g. chopin
// 4.14.186) cgroup v2 freezer does not exist; the glue now probes v2 first and
// automatically falls back to the v1 freezer hierarchy (/sys/fs/cgroup/freezer,
// freezer.state=FROZEN/THAWED with read-back confirmation). The pure decision
// layer only sees "freezerAvailable" (v2 OR v1); the actual backend used is
// reported in the state JSON as "freezerBackend":"v2"|"v1".
//
// Split mirrors scene_infer_wiring:
//   * decideC4Freeze() is PURE (no I/O, no globals) and is host-tested directly
//     from tests/v94/test_c4_freezer.cpp.
//   * c4FreezerTick() is the production glue (locates the app's cgroup v2 path,
//     reads foreground/audio/freezer availability, writes cgroup.freeze) and is
//     compiled out under AATPE_HOST_TEST (host links a stub instead).
//
// Default-off contract: when c4FreezerEnabled==false the pure decision returns
// NO_CHANGE for every input, so the daemon behaves exactly as v4.4.3 — zero
// freeze write, byte-identical off path.
//
// Safety contract (fail-safe):
//   * Foreground edge ALWAYS wins: if the app is foreground we UNFREEZE (or
//     NO_CHANGE if already unfrozen). We never leave an app frozen after it
//     returns to foreground.
//   * Audio whitelist: if audio is (or may be) active, do NOT freeze.
//   * No cgroup v2 freezer (v1 hierarchy / freezer controller absent) -> NO_CHANGE
//     + degraded flag (we never guess a freeze path).
//   * v6.1.0: v2 freezer absent but v1 freezer present -> FREEZE via v1 backend.
//   * Idempotent: already in the desired state -> NO_CHANGE (no redundant write).
#ifndef AATPE_C4_FREEZER_H
#define AATPE_C4_FREEZER_H

#include <cstdint>
#include <string>
#include <atomic>

namespace aatpe {
namespace c4freezer {

// Desired action for one tick.
enum class C4Action : int {
    NO_CHANGE = 0,  // do nothing (gate off / already in state / blocked)
    FREEZE,         // write cgroup.freeze=1
    UNFREEZE,       // write cgroup.freeze=0 (fail-safe foreground release)
};

// Already-sampled inputs (the production glue fills these from sysfs/globals).
struct C4DecisionInputs {
    bool featureEnabled   = false; // C4_FREEZER_ENABLED master switch
    bool isForeground     = true;   // true = game currently in foreground
    bool audioActive      = false;  // whitelist: app may be playing audio
    bool freezerAvailable = false;  // cgroup v2 unified + freezer controller present
    bool alreadyFrozen    = false;  // last applied freeze state (idempotency)
};

struct C4Decision {
    C4Action  action       = C4Action::NO_CHANGE;
    bool      degraded     = false;  // platform can't act (v1 / no freezer / audio-unknown)
    const char* reason     = "";     // human-readable reason (static string)
};

// PURE: decide freeze/unfreeze/no-change for one tick. No I/O, no globals.
//   - gate off            -> NO_CHANGE (byte-identical to v4.4.3)
//   - foreground          -> UNFREEZE if already frozen, else NO_CHANGE (safe-first)
//   - background+audio    -> NO_CHANGE (audio whitelist)
//   - background+!freezer  -> NO_CHANGE + degraded (v1 / no freezer controller)
//   - background+frozen    -> NO_CHANGE (idempotent)
//   - background otherwise -> FREEZE
C4Decision decideC4Freeze(const C4DecisionInputs& in);

// ---- v4.4.4 rewire: pure helpers (always-compiled, host-tested) --------------

// PURE: parse the body of /proc/<tid>/cgroup. The v2 unified line looks like
// "0::/a/b/top-app". Returns the path WITHOUT the leading slash ("a/b/top-app"),
// or "" when no "0::" line is present (v1-only / unknown / corrupt).
std::string parseV2CgroupPath(const std::string& procCgroupContent);

// PURE: join a v2 cgroup path onto the unified mount root (e.g.
// "/sys/fs/cgroup"). path="" -> "". Collapses double slashes.
std::string buildFreezeDir(const std::string& v2Path, const std::string& mountRoot);

// PURE: parse `dumpsys media_session` (or `cmd media_session list-sessions`) output.
// Returns true ONLY when `pkg` appears inside a session block whose playback state
// is 3 (STATE_PLAYING). Absent pkg / no playing session / empty or corrupt output
// all return false. The caller distinguishes "exec failed" (treat as unknown ->
// do not freeze) from "ran fine, no playing session" (safe to freeze).
bool mediaSessionMentionsPlaying(const std::string& output, const std::string& pkg);

// PURE (MJ-1): does a v2 cgroup path belong to app uid `appUid`?
// Android app cgroups are like "uid_10123/cgroup_4567" or "/uid_10123". We match
// the segment EXACTLY ("uid_<appUid>") so "uid_1012" does NOT match "uid_10123".
// System/root cgroups ("init", "top-app", "system"...) contain no such segment and
// return false. appUid<=0 -> false.
bool cgroupPathBelongsToUid(const std::string& v2Path, int appUid);

// ---- v6.1.0: cgroup v1 freezer helpers (pure, host-tested) ------------------

// PURE: parse the body of /proc/<tid>/cgroup for the v1 freezer hierarchy line.
// A v1 line looks like "3:freezer:/uid_10123/cg_42" (or with other controllers
// mounted together: "2:cpu,freezer:/uid_10123"). Returns the path WITHOUT the
// leading slash ("uid_10123/cg_42"), or "" when no freezer controller line is
// present (v2-only / unknown / corrupt).
std::string parseV1FreezerPath(const std::string& procCgroupContent);

// PURE (#96): parse the body of /proc/mounts for the cgroup v1 freezer mount.
// A v1 cgroup mount line is whitespace-separated:
//   none  /sys/fs/cgroup/freezer  cgroup  rw,relatime,freezer  0  0
// We require vfstype=="cgroup" (v2 unified is "cgroup2") and an exact "freezer"
// entry in the comma-separated controller list. Returns the mount point WITHOUT a
// trailing slash ("/sys/fs/cgroup/freezer"), or "" when no such mount exists.
// Runtime-detects wherever the v1 freezer hierarchy is actually mounted on a
// given device -- never hardcodes the conventional path.
std::string parseV1FreezerMountPoint(const std::string& procMountsContent);

// PURE: normalize the contents of a v1 freezer.state file. Trims trailing
// whitespace/newline. Returns "FROZEN", "THAWED", "FREEZING", or "THAWING"
// verbatim; "" on empty/unknown content.
std::string parseV1FreezerState(const std::string& content);

// PURE: does a resolved cgroup dir belong to the v1 freezer hierarchy?
// v1 dirs look like "/sys/fs/cgroup/freezer/uid_10123/..."; v2 dirs look like
// "/sys/fs/cgroup/uid_10123/..." (no "freezer" segment). Used to dispatch
// freeze/unfreeze writes to the correct backend.
bool isV1FreezerDir(const std::string& dir);

// PURE (MJ-2): should we (re)run the media_session probe on this background tick?
// bgTicks = number of slow ticks accumulated since entering background (>=0).
// period = kAudioReevalPeriod. Due at bgTicks==0 (edge) and every `period` ticks
// afterwards. bgTicks<0 -> false.
bool audioReevalDue(int bgTicks, int period);

// Audio re-evaluation period (slow-thread ticks). Edge + every N background ticks.
constexpr int kAudioReevalPeriod = 5;

// ---- v4.4.5 Part A: NN persistence + MN retarget (pure, host-tested) ----------

// PURE (NN): serialize a frozen-state record to a small key=value text block:
//   cg=<dir>\npkg=<pkg>\nuid=<uid>\n[lg=<lastGame>]\n
// lastGame may be empty; the lg= line is OPTIONAL for backwards compat.
std::string serializeC4Frozen(const std::string& cgDir, const std::string& pkg,
                             int uid, const std::string& lastGame);

// PURE (NN): parse the block above. valid=false on missing/invalid cg/pkg/uid
// (corrupt / half-written / truncated). lastGame is OPTIONAL: absent on old
// v4.4.5 files -> empty (backwards compatible, still valid). On valid=true out* set.
struct C4FrozenState {
    std::string cgDir;
    std::string pkg;
    std::string lastGame;  // v4.4.6: optionally persisted (NN-NIT)
    int         uid = 0;
    bool        valid = false;
};
C4FrozenState parseC4Frozen(const std::string& content);

// PURE (MN): choose the freeze target pkg given the last foreground game and the
// current foreground situation.
//   fgIsGame=true            -> the foreground game IS the target (it will freeze
//                               when it later backgrounds); also refreshes lastGame.
//   fgIsGame=false,lastGame  -> target = lastGame (launcher/non-game foreground:
//                               keep freezing the previously backgrounded game).
//   fgIsGame=false,lastGame=""-> no target ("").
// Returns the pkg string; "" means "do not freeze anything this episode".
std::string c4ChooseTarget(const std::string& lastGame,
                           const std::string& fgPkg,
                           bool fgIsGame);

// PURE (v6.0.1): OR the three independent "foreground is a game" signals.
//   manualGame   = pkg in game_overrides.list (WebUI game_add=, frame.cpp)
//   configGame   = pkg in GAME_PACKAGES= (aatpe.conf, aatped_config.cpp)
//   engineThread = a live thread of this pkg matches Unity/Unreal/Vulkan/etc
//                  signatures (aatped_threadutil.cpp isGameEngineThread)
// The historical v6.0.0 code only consulted manualGame, so a fresh device with
// no manual entry sat at degraded="no-game-target" forever even while the user
// was actively gaming. Any one positive signal is enough; all three are
// conservative (empty/unknown -> false) so a non-game app is never promoted.
bool c4FgIsGameSignals(bool manualGame, bool configGame, bool engineThreadGame);

// PURE (v4.4.6 WebUI; v6.1.0 backend): build the read-only C4 state JSON object.
// Produces: {"enabled":..,"target":..,"frozen":..,"cgDir":..,"lastAction":..,
//            "audioActive":..,"freezerAvailable":..,"freezerBackend":..,
//            "degraded":..}
// freezerBackend is "v2", "v1", or "" (unknown / none).
std::string c4StateJson(bool enabled, const std::string& target, bool frozen,
                        const std::string& cgDir, const std::string& lastAction,
                        bool audioActive, bool freezerAvailable,
                        const std::string& freezerBackend,
                        const std::string& degraded);

// Production glue (DEFINITION compiled out under AATPE_HOST_TEST): thread-safe
// snapshot of the slow-thread bookkeeping as a JSON string for WebUI. The export
// copies all shared scalars/strings under s_stateMutex (R2-M1) before releasing
// it for sysfs I/O, so it is safe to call from the WebUI/control thread while
// c4FreezerTick() runs on the slow thread. Read-only.
std::string c4FreezerStateJson();

// Production glue: one slow-thread tick. Samples foreground/audio/cgroup path,
// runs decideC4Freeze, writes cgroup.freeze=1/0. Any write failure / missing node /
// permission denial is downgraded (never crashes, never affects the main scheduler).
// DECLARATION always visible (aatped_main.cpp may call it); the DEFINITION is
// compiled out under AATPE_HOST_TEST.
void c4FreezerTick();

// Switch getter/setter (header-inline, shared function-local static atomic) so
// config load and main can reference them WITHOUT linking c4_freezer.o. Default
// false. OFF: c4FreezerEnabled() stays false, c4FreezerTick() never writes anything.
inline std::atomic<bool>& c4FreezerSwitch() {
    // v6.0.0 round4c: default ON (cgroup freezer tick self-skips foreground/top-app,
    // audio, and IM/push whitelist; auto-degrades on thaw failure).
    static std::atomic<bool> g_enabled{true};
    return g_enabled;
}
inline void setC4FreezerEnabled(bool on) {
    c4FreezerSwitch().store(on, std::memory_order_relaxed);
}
inline bool c4FreezerEnabled() {
    return c4FreezerSwitch().load(std::memory_order_relaxed);
}

} // namespace c4freezer
} // namespace aatpe

#endif // AATPE_C4_FREEZER_H
