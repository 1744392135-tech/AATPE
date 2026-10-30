#ifndef AATPE_C4_FREEZER_H
#define AATPE_C4_FREEZER_H

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace aatpe {
namespace c4freezer {

enum class C4Action : int {
    NO_CHANGE = 0,
    FREEZE,
    UNFREEZE,
};

struct C4DecisionInputs {
    bool featureEnabled   = false;
    bool isForeground     = true;
    bool audioActive      = false;
    bool freezerAvailable = false;
    bool alreadyFrozen    = false;
};

struct C4Decision {
    C4Action  action       = C4Action::NO_CHANGE;
    bool      degraded     = false;
    const char* reason     = "";
};

C4Decision decideC4Freeze(const C4DecisionInputs& in);

std::string parseV2CgroupPath(const std::string& procCgroupContent);

std::string buildFreezeDir(const std::string& v2Path, const std::string& mountRoot);

bool mediaSessionMentionsPlaying(const std::string& output, const std::string& pkg);

bool cgroupPathBelongsToUid(const std::string& v2Path, int appUid);

std::string parseV1FreezerPath(const std::string& procCgroupContent);

std::string parseV1FreezerMountPoint(const std::string& procMountsContent);

std::string parseV1FreezerState(const std::string& content);

bool isV1FreezerDir(const std::string& dir);

struct C4BucketPaths {
    std::string frozenDir;
    std::string thawedDir;
    bool        valid = false;
};

C4BucketPaths buildV1BucketPaths(const std::string& v1Root);

bool isV1FrozenBucketDir(const std::string& dir);

std::string thawedBucketFromFrozen(const std::string& frozenDir);

std::vector<int> parsePidsList(const std::string& s);

std::string formatPidsList(const std::vector<int>& pids);

bool procCgroupOnFrozenBucket(const std::string& procCgroupContent);

bool audioReevalDue(int bgTicks, int period);

constexpr int kAudioReevalPeriod = 5;

std::string serializeC4Frozen(const std::string& cgDir, const std::string& pkg,
                             int uid, const std::string& lastGame,
                             const std::vector<int>& pids = {});

struct C4FrozenState {
    std::string cgDir;
    std::string pkg;
    std::string lastGame;
    std::vector<int> pids;
    int         uid = 0;
    bool        valid = false;
};
C4FrozenState parseC4Frozen(const std::string& content);

std::string c4ChooseTarget(const std::string& lastGame,
                           const std::string& fgPkg,
                           bool fgIsGame);

bool c4FgIsGameSignals(bool manualGame, bool configGame, bool engineThreadGame);

std::string c4StateJson(bool enabled, const std::string& target, bool frozen,
                        const std::string& cgDir, const std::string& lastAction,
                        bool audioActive, bool freezerAvailable,
                        const std::string& freezerBackend,
                        const std::string& degraded);

std::string c4FreezerStateJson();

void c4FreezerTick();

inline std::atomic<bool>& c4FreezerSwitch() {
    static std::atomic<bool> g_enabled{true};
    return g_enabled;
}
inline void setC4FreezerEnabled(bool on) {
    c4FreezerSwitch().store(on, std::memory_order_relaxed);
}
inline bool c4FreezerEnabled() {
    return c4FreezerSwitch().load(std::memory_order_relaxed);
}

}
}

#endif
