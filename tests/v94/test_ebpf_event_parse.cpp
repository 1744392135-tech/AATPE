#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "aatpe_ebpf.h"
using namespace aatpe_ebpf;

namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

static constexpr uint16_t kPERF_RECORD_SAMPLE = 9;

struct RecBuilder {
    std::vector<uint8_t> b;
    void add(const void* payload, size_t rawSize) {
        size_t recSize = 8u + 4u + rawSize;
        size_t off = b.size();
        b.resize(off + recSize);
        uint8_t* p = b.data() + off;
        uint32_t type = kPERF_RECORD_SAMPLE; std::memcpy(p + 0, &type, 4);
        uint16_t misc = 0; std::memcpy(p + 4, &misc, 2);
        uint16_t sz = (uint16_t)recSize; std::memcpy(p + 6, &sz, 2);
        uint32_t rs = (uint32_t)rawSize; std::memcpy(p + 8, &rs, 4);
        std::memcpy(p + 12, payload, rawSize);
    }
};

struct Cap : public IEventConsumer {
    int forks=0, exits=0, switches=0, execs=0, runqs=0;
    ForkEvent fk; ExitEvent ex; SwitchEvent sw; ExecEvent exx; RunqEvent rq;
    void onFork(const ForkEvent& e) override { forks++; fk = e; }
    void onExit(const ExitEvent& e) override { exits++; ex = e; }
    void onSwitch(const SwitchEvent& e) override { switches++; sw = e; }
    void onExec(const ExecEvent& e) override { execs++; exx = e; }
    void onRunqLatency(const RunqEvent& e) override { runqs++; rq = e; }
};

static ForkEvent mkFork(uint32_t parent, uint32_t childPid, uint32_t childTid) {
    ForkEvent e;
    e.hdr.type = (uint32_t)EventType::FORK; e.hdr.cpu = 2; e.hdr.timestampNs = 12345;
    e.parentPid = parent; e.childPid = childPid; e.childTid = childTid;
    return e;
}
static ExitEvent mkExit(uint32_t pid, uint32_t tid) {
    ExitEvent e;
    e.hdr.type = (uint32_t)EventType::EXIT; e.hdr.cpu = 1; e.hdr.timestampNs = 12346;
    e.pid = pid; e.tid = tid; return e;
}
static SwitchEvent mkSwitch(uint32_t prevPid, uint32_t nextPid, const char* nextComm) {
    SwitchEvent e;
    e.hdr.type = (uint32_t)EventType::SWITCH; e.hdr.cpu = 3; e.hdr.timestampNs = 12347;
    e.prevPid = prevPid; e.nextPid = nextPid;
    std::strncpy(e.nextComm, nextComm, sizeof(e.nextComm) - 1);
    return e;
}
static ExecEvent mkExec(uint32_t pid, uint32_t tid, const char* comm) {
    ExecEvent e;
    e.hdr.type = (uint32_t)EventType::EXEC; e.hdr.cpu = 0; e.hdr.timestampNs = 12348;
    e.pid = pid; e.tid = tid; std::strncpy(e.comm, comm, sizeof(e.comm) - 1);
    return e;
}
static RunqEvent mkRunq(uint32_t pid, uint32_t targetCpu, uint32_t wakerTid, uint64_t delayNs) {
    RunqEvent e;
    e.hdr.type = (uint32_t)EventType::RUNQ; e.hdr.cpu = 0; e.hdr.timestampNs = 12349;
    e.pid = pid; e.targetCpu = targetCpu; e.wakerTid = wakerTid; e.delayNs = delayNs;
    return e;
}

int main() {
    printf("=== eBPF event parse: normal 5 event types field extraction ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        ForkEvent f = mkFork(1000, 2000, 2000);
        ExitEvent x = mkExit(3000, 3001);
        SwitchEvent sw = mkSwitch(111, 222, "game.pkg");
        ExecEvent ex = mkExec(4000, 4000, "com.x.app");
        RunqEvent rq = mkRunq(5000, 3, 6000, 9999);
        rb.add(&f, sizeof(f)); rb.add(&x, sizeof(x)); rb.add(&sw, sizeof(sw));
        rb.add(&ex, sizeof(ex)); rb.add(&rq, sizeof(rq));
        sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(cap.forks == 1, "Fork delivered once");
        CHECK(cap.fk.parentPid == 1000 && cap.fk.childPid == 2000 && cap.fk.childTid == 2000,
              "Fork fields parent/child/tid parsed");
        CHECK(cap.exits == 1 && cap.ex.pid == 3000 && cap.ex.tid == 3001, "Exit pid/tid parsed");
        CHECK(cap.switches >= 1 && cap.sw.nextPid == 222 &&
              std::string(cap.sw.nextComm) == "game.pkg", "Switch nextPid+comm parsed");
        CHECK(cap.execs == 1 && cap.exx.pid == 4000 && std::string(cap.exx.comm) == "com.x.app",
              "Exec pid+comm parsed");
        CHECK(cap.runqs == 1 && cap.rq.delayNs == 9999 && cap.rq.targetCpu == 3 && cap.rq.wakerTid == 6000,
              "Runq delay/targetCpu/wakerTid parsed");
        CHECK(sched.getForkEvents() == 1 && sched.getExitEvents() == 1 &&
              sched.getExecEvents() == 1,
              "per-type counters incremented (fork/exit/exec)");
    }

    printf("=== eBPF event parse: truncated (undersized) record not counted/delivered ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        ForkEvent f = mkFork(1000, 2000, 2000);
        rb.add(&f, sizeof(f));
        ForkEvent bad = mkFork(9, 9, 9);
        rb.add(&bad, sizeof(ForkEvent) - 4);
        sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(cap.forks == 1, "truncated fork NOT delivered (only valid fork)");
        CHECK(sched.getForkEvents() == 1, "truncated fork NOT counted");
    }

    printf("=== eBPF event parse: unknown evtType (not 1..5) skipped, no crash ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        ForkEvent f = mkFork(1000, 2000, 2000);
        rb.add(&f, sizeof(f));
        EventHeader uh; uh.type = 7;
        rb.add(&uh, sizeof(uh));
        sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(cap.forks == 1, "valid fork still delivered after unknown-type record");
        CHECK(sched.getForkEvents() == 1, "unknown evtType not counted");
    }

    printf("=== eBPF event parse: zero-size framing record -> break, no crash ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        ForkEvent f = mkFork(1000, 2000, 2000);
        rb.add(&f, sizeof(f));
        size_t off = rb.b.size();
        rb.b.resize(off + 12);
        uint8_t* p = rb.b.data() + off;
        uint32_t t = kPERF_RECORD_SAMPLE; std::memcpy(p, &t, 4);
        uint16_t misc = 0; std::memcpy(p + 4, &misc, 2);
        uint16_t z = 0; std::memcpy(p + 6, &z, 2);
        uint32_t zr = 0; std::memcpy(p + 8, &zr, 4);
        int rc = sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(rc >= 0, "size==0 record breaks loop without crash");
        CHECK(cap.forks == 1, "valid fork before malformed record still delivered");
    }

    printf("=== eBPF event parse: over-declared rawSize beyond buffer -> skipped, no crash ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        ForkEvent f = mkFork(1000, 2000, 2000);
        rb.add(&f, sizeof(f));
        size_t rec2Off = rb.b.size();
        size_t payloadLen = sizeof(ForkEvent);
        size_t rec2Len = 8u + 4u + payloadLen;
        rb.b.resize(rec2Off + rec2Len);
        uint8_t* p = rb.b.data() + rec2Off;
        uint32_t t = kPERF_RECORD_SAMPLE; std::memcpy(p + 0, &t, 4);
        uint16_t misc = 0; std::memcpy(p + 4, &misc, 2);
        uint16_t sz = (uint16_t)rec2Len; std::memcpy(p + 6, &sz, 2);
        uint32_t bigRaw = (uint32_t)(payloadLen + 64u); std::memcpy(p + 8, &bigRaw, 4);
        ForkEvent fake = mkFork(9, 9, 9); std::memcpy(p + 12, &fake, payloadLen);
        int rc = sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(rc >= 0, "over-declared rawSize does not crash");
        CHECK(cap.forks == 1, "over-declared rawSize record NOT delivered");
        CHECK(sched.getForkEvents() == 1, "over-declared rawSize NOT counted");
    }

    printf("=== eBPF event parse: oversize framing size (>buffer) -> break, no crash ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        ForkEvent f = mkFork(1000, 2000, 2000);
        rb.add(&f, sizeof(f));
        size_t off = rb.b.size();
        rb.b.resize(off + 12);
        uint8_t* p = rb.b.data() + off;
        uint32_t t = kPERF_RECORD_SAMPLE; std::memcpy(p, &t, 4);
        uint16_t misc = 0; std::memcpy(p + 4, &misc, 2);
        uint16_t huge = 0xFFF0; std::memcpy(p + 6, &huge, 2);
        uint32_t rs = (uint32_t)sizeof(ForkEvent); std::memcpy(p + 8, &rs, 4);
        int rc = sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(rc >= 0, "oversize size record breaks without crash");
        CHECK(cap.forks == 1, "valid fork before oversize record still delivered");
    }

    printf("=== eBPF event parse: multiple mixed records, offsets advance correctly ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb;
        for (int i = 0; i < 5; ++i) {
            ForkEvent f = mkFork(1000 + i, 2000 + i, 2000 + i);
            rb.add(&f, sizeof(f));
        }
        sched.hostTestParseRingBlob(rb.b.data(), rb.b.size());
        CHECK(cap.forks == 5, "5 sequential fork records all parsed (offsets advance)");
        CHECK(cap.fk.childPid == 2004, "last fork fields correct (no overlap/desync)");
    }

    printf("=== eBPF event parse: switch batching by nextPid (threshold=10) ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        RecBuilder rb1; SwitchEvent sw1 = mkSwitch(1, 777, "game");
        rb1.add(&sw1, sizeof(sw1));
        sched.hostTestParseRingBlob(rb1.b.data(), rb1.b.size());
        int afterDrain1 = cap.switches;
        CHECK(afterDrain1 >= 1, "first switch drain delivers (first occurrence)");
        RecBuilder rb2;
        for (int i = 0; i < 10; ++i) { SwitchEvent sw = mkSwitch(1, 777, "game"); rb2.add(&sw, sizeof(sw)); }
        sched.hostTestParseRingBlob(rb2.b.data(), rb2.b.size());
        CHECK(cap.switches > afterDrain1, "switch aggregation threshold crossing delivers");
    }

    printf("=== eBPF event parse: empty blob is a no-op ===\n");
    {
        EbpfScheduler sched;
        Cap cap; sched.setEventConsumer(&cap);
        int rc = sched.hostTestParseRingBlob(nullptr, 0);
        CHECK(rc == 0, "empty blob returns 0 (no events)");
        CHECK(cap.forks == 0 && cap.exits == 0, "empty blob delivers nothing");
    }

    printf("\n========================================\n");
    printf("ebpf_event_parse: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
