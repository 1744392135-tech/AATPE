// Federated Learning (file-based cross-device model exchange)
//
// CLARIFICATION (third-party audit): this is NOT real P2P/network federated
// learning. There is NO socket / network code anywhere in aatpe_federated_*.cpp.
// It only reads/writes exchangeDir_/local_model.bin (export) and scans the
// exchangeDir_/import/ directory for peer model files. Moving those files from
// one device to another is done by an EXTERNAL transport (adb push, shared
// storage, manual copy) — this module neither does nor sees that transfer.
// Authenticity + integrity are enforced locally by a PSK HMAC-SHA256 signature
// plus a CRC32 check on import. "P2P" below therefore means "no central
// server, device-to-device via external file transport", not a network peer.
//
// //
// 问题23 — PRIVACY MODEL:
//   This module implements HORIZONTAL FEDERATED MODEL AVERAGING, not gradient
//   sharing. The on-the-wire payload is the FULL local model weight vector
//   (written by exportLocalModel() / read by checkAndImportModels()). A full
//   weight snapshot can indirectly encode information about the local training
//   data (membership-inference / property-inference attacks are possible in
//   principle), so exchange must be
//   restricted to mutually trusted devices (the HMAC-PSK trust model below is
//   the access control). There is NO central server; exchange is P2P via plain
//   files under the exchange directory, and aggregation happens locally.
//   The struct is still named ModelGradient for ABI/source compatibility with
//   older model files and existing tests, but its payload is model weights, not
//   gradients. The dead `gradients` field was removed (问题24).
//
// 问题25 — DEVICE HETEROGENEITY:
//   Devices differ in CPU topology (big.LITTLE mix, core count, cluster
//   frequencies). Averaging weights across topologically-dissimilar devices can
//   distort the per-pool predictions. ImportedModelEntry now carries an optional
//   topologyFingerprint and runAggregation() records how many imported models
//   matched the local fingerprint in FederatedStats::topologyMatchCount. Because
//   the v1 model-file format does not yet embed a fingerprint, imported models
//   currently have an empty fingerprint and are treated as "unknown topology"
//   (still aggregated, but counted separately). This is a documented known
//   limitation, not a correctness bug.
#ifndef AATPE_FEDERATED_H
#define AATPE_FEDERATED_H
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
namespace aatpe_federated {

// P2-10: replay-protection metadata carried in the v2 wire format.
//   generation    : monotonic counter, bumped by the exporter on every export.
//                   Importer rejects any trusted model whose generation is
//                   <= lastAcceptedGeneration_ (prevents replay of an old,
//                   previously-valid HMAC-signed model).
//   timestamp     : unix epoch seconds at export. Importer rejects models whose
//                   clock drift vs. local time exceeds kMaxTimestampDriftSec.
//   modelVersion  : AATPE_VERSION_CODE of the exporter (informational; logged).
//   topologyHash  : FNV-1a hash of the exporter's topology fingerprint.
struct ModelMetadata {
    uint32_t generation = 0;
    int64_t  timestamp  = 0;
    uint32_t modelVersion = 0;
    uint32_t topologyHash = 0;
};

// carries the FULL local model weight vector used for
// model averaging. (The historical name "ModelGradient" is retained for source
// compatibility; the dead per-sample `gradients` buffer was removed.)
struct ModelGradient {
    std::vector<float> weights;
    float alpha = 0.0f;
    int sampleCount = 0;
};

struct FederatedStats {
    int rounds = 0;
    int exports = 0;
    int imports = 0;
    int trustedImports = 0;
    int aggregatedModels = 0;
    // how many trusted imported models had a topology
    // fingerprint matching the local device during the last aggregation.
    int topologyMatchCount = 0;
    // whether the local model was excluded from the last
    // aggregation because it was not yet mature (sampleCount below threshold).
    int localExcludedImmature = 0;
};

// computeGradients() was dead code (never consumed by
// FederatedLearner::runAggregation, ModelGradient::gradients never populated)
// and has been removed.
// P2-11: all on-wire numeric fields are read/written via memcpy() (no
// forced type-punned pointer casts) so the code is correct on unaligned
// buffers, under -fstrict-aliasing, and behaves identically on
// ARM64/ARM32/x86/x86_64.
//
// F-003 governance (v4.3.4): the previously-declared free functions
// exportModel()/exportModelEx()/importModel()/importModelEx() had NO production
// implementation (only a host test stub in tests/integration/stubs). They were
// dangling declarations that mislead readers into thinking the wire format was
// available production-side. The v2 export/import path is dormant (ROUND-014
// Plan B) and is exercised only by integration tests through their own stub;
// the declarations are removed from this production header so production code
// cannot reference them. aggregateModels() below IS implemented in
// aatpe_federated_aggregate.cpp and stays.
std::vector<float> aggregateModels(const std::vector<std::vector<float>>& models,
                                    const std::vector<int>& sampleCounts);

// no central server, no sockets. Full-weight model averaging
// over locally-exported model files (exchangeDir_/local_model.bin + import/),
// gated by a pre-shared HMAC key; files are moved between devices by an EXTERNAL
// file transport. See CLARIFICATION at the top of this header.
class FederatedLearner {
public:
    FederatedLearner();
    ~FederatedLearner();
    void init(const std::string& exchangeDir);
    void setLocalModel(const std::vector<float>& weights, float alpha, int sampleCount);
    // G7-001: opt-in gate. Default FALSE. When disabled, exportLocalModel(),
    // checkAndImportModels() and runAggregation() are all no-ops (zero weight
    // snapshot written to disk, zero import scan, zero aggregation) so the
    // feature is OFF unless the daemon explicitly opts in from FEDERATED_ENABLED.
    // This makes the module self-protecting even if the caller forgets to gate
    // the loop: a disabled learner never touches the filesystem or mutates state.
    void setEnabled(bool e);
    bool isEnabled() const;
    // M-FED-1 (G3 ROUND-005c): when federated is ENABLED but the shared PSK cannot
    // be read, import must FAIL CLOSED (reject the peer batch) rather than silently
    // degrading to CRC-only. This flag is an EXPLICIT opt-in for LAN lab/debug only;
    // it defaults OFF. With it on, unsigned-but-CRC-valid models are still imported
    // as UNTRUSTED (never aggregated). Never expose as a shipped config knob.
    void setAllowUnverifiedCrc(bool allow) {
        std::lock_guard<std::mutex> lk(mutex_);
        allowUnverifiedCrc_ = allow;
    }
    bool getAllowUnverifiedCrc() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return allowUnverifiedCrc_;
    }
    // stamp the local device's topology fingerprint so that
    // runAggregation() can count/weight imported models by topology similarity.
    // Empty (default) disables the match accounting.
    void setLocalTopologyFingerprint(const std::string& fp) {
        std::lock_guard<std::mutex> lk(mutex_);
        localTopoFp_ = fp;
    }
    void exportLocalModel(void);
    void checkAndImportModels(void);
    void runAggregation(void);
    // Public replay-state accessors (take the lock).
    void loadReplayState(void) {
        std::lock_guard<std::mutex> lk(mutex_);
        loadReplayStateLocked();
    }
    void saveReplayState(void) {
        std::lock_guard<std::mutex> lk(mutex_);
        saveReplayStateLocked();
    }
    // P1-3: return copies instead of const& to avoid races on internal state.
    FederatedStats getStats(void) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return stats_;
    }
    std::vector<float> getAggregatedModel(void) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return aggregatedModel_;
    }
private:
    // P1-3: mutex guarding all FederatedLearner internal state. All public
    // methods must take this lock before reading/writing members.
    mutable std::mutex mutex_;
    std::string exchangeDir_;
    // G7-001: opt-in master switch. Default false (off). The daemon calls
    // setEnabled(g_cfg.federatedEnabled) after init(); until then the learner
    // performs zero disk writes and zero aggregation.
    bool enabled_ = false;
    // M-FED-1: default OFF. When federated is on but PSK is unreadable, imports are
    // rejected unless this explicit lab/debug switch is set.
    bool allowUnverifiedCrc_ = false;
    std::vector<float> localWeights_;
    float localAlpha_ = 0.0f;
    int localSampleCount_ = 0;
    std::string localTopoFp_;
    std::vector<float> aggregatedModel_;
    FederatedStats stats_;
    long long lastExportTime_ = 0;
    // P2-10: monotonic generation counter for replay protection.
    // localGeneration_ bumps on every exportLocalModel(); lastAcceptedGeneration_
    // is the highest generation number ever accepted from a peer (persisted to
    // disk so it survives daemon restarts). A trusted peer model whose generation
    // <= lastAcceptedGeneration_ is rejected as a replay.
    uint32_t localGeneration_ = 1;
    uint32_t lastAcceptedGeneration_ = 0;
    // LED-060 (R5-003): per-source replay watermark. A single global watermark
    // rejected a second peer's first export (each device starts localGeneration_ at
    // 1) once the first peer's generation had advanced. Key the highest accepted
    // generation by the import source filename (the exporter's stable file name), so
    // replaying the SAME source's older generation is still rejected while a new
    // source starting at generation=1 is accepted. lastAcceptedGeneration_ remains
    // the global max (persisted on line 1 of replay_state for backward compatibility).
    std::map<std::string, uint32_t> lastAcceptedBySource_;
    // maximum allowed clock skew between exporter timestamp and local now.
    static constexpr int64_t kMaxTimestampDriftSec = 86400;  // 24h
    // minimum local samples before the local model may
    // participate in averaging (问题26 gate).
    static constexpr int kFederatedLocalMinSamples = 1000;
    struct ImportedModelEntry {
        std::vector<float> weights;
        int sampleCount = 0;
        bool trusted = false;
        std::string topologyFingerprint;  // v94 fix (问题25); empty if unknown
        uint32_t generation = 0;           // P2-10
    };
    static constexpr int MAX_IMPORTED_MODELS = 64;
    std::vector<ImportedModelEntry> importedModels_;
    // P2-10: persist/restore lastAcceptedGeneration_ across restarts.
    // Locked variants assume the caller already holds mutex_.
    void loadReplayStateLocked(void);
    void saveReplayStateLocked(void);
    // ML-06: create exchangeDir_/import/ lazily on first opt-in. No-op when the
    // dirs already exist. Called from setEnabled(true).
    void ensureDirsLocked(void);
};
} // namespace aatpe_federated
#endif
