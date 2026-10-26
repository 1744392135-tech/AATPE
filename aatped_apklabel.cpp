// aatped_apklabel.cpp - see aatped_apklabel.h for the problem statement.
//
// Layout of this file:
//   1. Bounded buffered-reader + tiny inflate (DEFLATE stored/fixed/dynamic).
//   2. ZIP central-directory entry extractor (zero third-party deps).
//   3. ResStringPool (UTF-8 / UTF-16) parser.
//   4. Binary AndroidManifest (AXML) -> <application android:label>.
//   5. resources.arsc -> resource ID resolution with config scoring.
//   6. packages.list / /data/app location logic + public entry points.
//
// EVERY parser below is hostile-input safe: all reads go through a checked
// cursor, all lengths are bounded against the remaining buffer, no fixed
// stack buffers receive untrusted copies, and any anomaly aborts that
// resolution with an empty string. This file must never SIGSEGV even on
// hand-crafted APKs pulled off a hostile disk.

#include "aatped_apklabel.h"
#include <chrono>
#include <mutex>
#include <unordered_map>

#include "aatped_apklabel.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <string>
#include <vector>
#include <set>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <zlib.h>

namespace aatpe {
namespace apklabel {

namespace {

// ===========================================================================
// 0. Small utilities
// ===========================================================================

constexpr size_t kMaxFileProbe = size_t(64u) * 1024u;      // EOCD search window
constexpr size_t kMaxEntryBytes = size_t(96u) * 1024u * 1024u; // sanity bound on one zip entry
constexpr size_t kMaxPoolStrings = 1u << 20;       // reject absurd string pools
constexpr size_t kMaxChildrenPerDir = 4096;         // directory-walk bound
constexpr int    kMaxResolveDepth = 8;            // reference chains

inline void setErr(std::string* err, const char* msg) {
    if (err) *err = msg;
}

// Bounds-checked little-endian reader over an immutable byte buffer.
struct Cursor {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    const char* fail = nullptr;

    bool need(size_t bytes) {
        if (pos > n) { if (!fail) fail = "pos-oob"; return false; }
        if (bytes > n - pos) { if (!fail) fail = "truncated"; return false; }
        return true;
    }
    uint8_t u8()  { if (!need(1)) return 0; return p[pos++]; }
    uint16_t u16(){ if (!need(2)) { if(!fail)fail="truncated"; return 0; }
                    uint16_t v = (uint16_t)p[pos] | ((uint16_t)p[pos+1]<<8); pos+=2; return v; }
    uint32_t u32(){ if (!need(4)) { if(!fail)fail="truncated"; return 0; }
                    uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos+1]<<8)
                               | ((uint32_t)p[pos+2]<<16) | ((uint32_t)p[pos+3]<<24);
                    pos+=4; return v; }
    const uint8_t* ptr(size_t bytes) { if (!need(bytes)) return nullptr;
                                       const uint8_t* r = p+pos; pos+=bytes; return r; }
};

// Read an entire file into buf. Returns false on open/read error or if the
// file exceeds a sane ceiling (we only seek+read ranges on huge APKs).
bool readFileWhole(const std::string& path, std::vector<uint8_t>& buf, size_t ceiling) {
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        if (::fseek(f, 0, SEEK_END) != 0) break;
        long sz = ::ftell(f);
        if (sz < 0) break;
        if ((size_t)sz > ceiling) break; // callers read APKs by range, never whole
        ::fseek(f, 0, SEEK_SET);
        buf.resize((size_t)sz);
        size_t rd = ::fread(buf.data(), 1, (size_t)sz, f);
        ok = (rd == (size_t)sz);
    } while (false);
    ::fclose(f);
    return ok;
}

// ===========================================================================
// 1. DEFLATE decompression via system zlib (raw stream, wbits=-15).
// ===========================================================================

// Inflate raw DEFLATE (no zlib header) into out. out is pre-sized to the
// expected uncompressed length from the ZIP central directory. Returns false
// on malformed stream, truncation, or overflow. Strictly bounded to 96MB.
bool inflate(const uint8_t* src, size_t len, std::vector<uint8_t>& out) {
    uLongf cap = out.size();
    if (cap > kMaxEntryBytes) return false;
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    // wbits=-15 => raw deflate, no header, no adler
    if (inflateInit2(&strm, -15) != Z_OK) return false;
    strm.next_in  = const_cast<Bytef*>(src);
    strm.avail_in = (uInt)len;
    strm.next_out = out.data();
    strm.avail_out = (uInt)cap;
    int rc = inflate(&strm, Z_NO_FLUSH);
    // Accept Z_STREAM_END (complete) or Z_OK (needs more input, but since
    // out is pre-sized to expected uncomp size, Z_OK with avail_out>0 means
    // truncated/corrupt stream).
    bool ok = (rc == Z_STREAM_END);
    if (!ok) inflateEnd(&strm);
    else     inflateEnd(&strm);
    return ok;
}

// ===========================================================================
// 2. ZIP reader: find one entry by exact name.
// ===========================================================================

struct ZipEntry {
    uint32_t compMethod = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint64_t dataOffset = 0; // absolute in file
};

// Locate EOCD by scanning the tail backwards for signature 0x06054b50.
bool findEocd(const std::vector<uint8_t>& fbuf, size_t& cdOffset, size_t& cdSize) {
    size_t n = fbuf.size();
    size_t window = std::min(n, kMaxFileProbe);
    for (size_t i = n - window; i + 22 <= n; i++) {
        uint32_t sig = (uint32_t)fbuf[i] | ((uint32_t)fbuf[i+1]<<8)
                     | ((uint32_t)fbuf[i+2]<<16) | ((uint32_t)fbuf[i+3]<<24);
        if (sig != 0x06054b50u) continue;
        uint16_t commentLen = (uint16_t)(fbuf[i+20] | (fbuf[i+21]<<8));
        if (i + 22 + commentLen != n) continue; // comment length must reach EOF
        cdSize   = (size_t)fbuf[i+12] | ((size_t)fbuf[i+13]<<8)
                 | ((size_t)fbuf[i+14]<<16) | ((size_t)fbuf[i+15]<<24);
        cdOffset = (size_t)fbuf[i+16] | ((size_t)fbuf[i+17]<<8)
                 | ((size_t)fbuf[i+18]<<16) | ((size_t)fbuf[i+19]<<24);
        return true;
    }
    return false;
}

// Extract zip entry `wantName` into outBytes. Returns false if absent/error.
bool extractZipEntry(FILE* f, const std::vector<uint8_t>& cd, const std::string& wantName,
                     std::vector<uint8_t>& outBytes) {
    Cursor c{cd.data(), cd.size()};
    size_t count = 0; (void)count;
    // central dir entries: sig 0x02014b50
    while (c.need(4)) {
        uint32_t sig = c.u32();
        if (sig == 0x02024b01u) break; // defensive
        if (sig != 0x02014b50u) return false;
        c.u16(); c.u16(); c.u16();
        uint16_t method = c.u16();
        c.u16(); c.u16();
        c.u32(); // crc
        uint32_t compSize = c.u32();
        uint32_t uncompSize = c.u32();
        uint16_t nameLen = c.u16();
        uint16_t extraLen = c.u16();
        uint16_t cmtLen = c.u16();
        c.u16(); c.u16(); c.u32();
        uint32_t localOff = c.u32();
        const uint8_t* nameP = c.ptr(nameLen);
        if (!nameP) return false;
        // Defensive: explicitly bound the extra/comment skip against the
        // remaining central-directory buffer BEFORE advancing. A crafted
        // extraLen+cmtLen (both up to 65535) must never be able to push the
        // cursor past the buffer end or wrap around.
        if ((size_t)extraLen + (size_t)cmtLen > c.n - c.pos) return false;
        c.pos += extraLen + cmtLen;
        std::string name((const char*)nameP, nameLen);
        if (name == wantName) {
            // read local header to skip name/extra
            if (::fseek(f, (long)localOff, SEEK_SET) != 0) return false;
            uint8_t lh[30];
            if (::fread(lh, 1, 30, f) != 30) return false;
            uint32_t lsig = (uint32_t)lh[0] | ((uint32_t)lh[1]<<8)
                          | ((uint32_t)lh[2]<<16) | ((uint32_t)lh[3]<<24);
            if (lsig != 0x04034b50u) return false;
            uint16_t lName = (uint16_t)(lh[26] | (lh[27]<<8));
            uint16_t lExtra = (uint16_t)(lh[28] | (lh[29]<<8));
            if (::fseek(f, lName + lExtra, SEEK_CUR) != 0) return false;
            if (compSize > kMaxEntryBytes || uncompSize > kMaxEntryBytes) return false;
            std::vector<uint8_t> data(compSize ? compSize : 0);
            if (compSize && ::fread(data.data(), 1, compSize, f) != compSize) return false;
            if (method == 0) {
                outBytes = std::move(data);
                return true;
            }
            if (method == 8) {
                outBytes.resize(uncompSize);
                return inflate(data.data(), data.size(), outBytes);
            }
            return false; // unsupported compression
        }
    }
    return false; // entry not found
}

// Full pipeline: open apk, pull central directory, extract one entry.
bool readApkEntry(const std::string& apkPath, const std::string& entryName,
                  std::vector<uint8_t>& out) {
    FILE* f = ::fopen(apkPath.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        if (::fseek(f, 0, SEEK_END) != 0) break;
        long sz = ::ftell(f);
        if (sz < 22) break;
        // EOCD window
        size_t window = std::min((size_t)sz, kMaxFileProbe);
        if (::fseek(f, (long)(sz - (long)window), SEEK_SET) != 0) break;
        std::vector<uint8_t> tail(window);
        if (::fread(tail.data(), 1, window, f) != window) break;
        size_t cdOff, cdSize;
        if (!findEocd(tail, cdOff, cdSize)) break;
        if (cdSize > (size_t)sz || cdOff + cdSize > (size_t)sz) break;
        std::vector<uint8_t> cd(cdSize);
        if (::fseek(f, (long)cdOff, SEEK_SET) != 0) break;
        if (cdSize && ::fread(cd.data(), 1, cdSize, f) != cdSize) break;
        ok = extractZipEntry(f, cd, entryName, out);
    } while (false);
    ::fclose(f);
    return ok;
}

// ===========================================================================
// 3. ResStringPool
// ===========================================================================

struct StringPool {
    uint32_t count = 0;
    bool utf8 = false;
    std::vector<std::string> strings; // decoded UTF-8
    bool ok = false;
};

std::string utf16ToUtf8(const uint8_t* p, size_t u16count) {
    std::string out;
    out.reserve(u16count);
    for (size_t i = 0; i < u16count; i++) {
        uint16_t w = (uint16_t)p[2*i] | ((uint16_t)p[2*i+1] << 8);
        if (w >= 0xD800 && w <= 0xDBFF && i + 1 < u16count) {
            uint16_t w2 = (uint16_t)p[2*(i+1)] | ((uint16_t)p[2*(i+1)+1] << 8);
            if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                uint32_t cp = 0x10000 + (((uint32_t)(w - 0xD800) << 10) | (w2 - 0xDC00));
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
                i++;
                continue;
            }
        }
        if (w < 0x80) out += (char)w;
        else if (w < 0x800) {
            out += (char)(0xC0 | (w >> 6));
            out += (char)(0x80 | (w & 0x3F));
        } else {
            out += (char)(0xE0 | (w >> 12));
            out += (char)(0x80 | ((w >> 6) & 0x3F));
            out += (char)(0x80 | (w & 0x3F));
        }
    }
    return out;
}

bool parseStringPool(Cursor& c, StringPool& sp) {
    size_t start = c.pos;
    uint16_t type = c.u16();
    uint16_t headerSize = c.u16(); (void)headerSize;
    uint32_t size = c.u32();
    if (type != 0x0001) return false;
    if (size < 28 || start + size > c.n) return false;
    sp.count = c.u32();
    uint32_t styleCount = c.u32();
    uint32_t flags = c.u32();
    uint32_t stringsStart = c.u32();
    uint32_t stylesStart = c.u32();
    (void)styleCount; (void)stylesStart;
    sp.utf8 = (flags & 0x100) != 0;
    if (sp.count > kMaxPoolStrings) return false;
    // offsets array
    std::vector<uint32_t> offsets(sp.count);
    for (uint32_t i = 0; i < sp.count; i++) offsets[i] = c.u32();
    sp.strings.assign(sp.count, std::string());
    size_t dataBase = start + stringsStart;
    for (uint32_t i = 0; i < sp.count; i++) {
        size_t sp2 = dataBase + offsets[i];
        if (sp2 + 2 > start + size) return false;
        if (sp.utf8) {
            // two length bytes (packed), then bytes, then NUL
            Cursor d{c.p, c.n}; d.pos = sp2;
            uint32_t a = d.u8();
            if (a & 0x80) { uint32_t b = d.u8(); a = ((a & 0x7F) << 8) | b; }
            uint32_t byteLen = d.u8();
            if (byteLen > c.n - d.pos) return false;
            const uint8_t* bp = d.ptr(byteLen);
            if (!bp) return false;
            sp.strings[i].assign((const char*)bp, byteLen);
        } else {
            Cursor d{c.p, c.n}; d.pos = sp2;
            uint32_t n = d.u16();
            if (n & 0x8000) {
                uint32_t n2 = d.u16();
                n = ((n & 0x7FFF) << 16) | n2;
            }
            if (n > (1u << 20)) return false;
            if (size_t(2u) * n > c.n - d.pos) return false;
            sp.strings[i] = utf16ToUtf8(d.p + d.pos, n);
        }
    }
    sp.ok = true;
    c.pos = start + size; // honor chunk size (4-byte aligned)
    return true;
}

// Safe pool index fetch: -1 / out-of-range -> "".
const std::string& poolAt(const StringPool& sp, uint32_t idx) {
    static const std::string kEmpty;
    if (!sp.ok) return kEmpty;
    if (idx >= sp.count) return kEmpty;
    return sp.strings[idx];
}

// ===========================================================================
// 4. AXML: find <application android:label>
// ===========================================================================

struct LabelResult {
    bool hasAttr = false;
    std::string direct;      // non-empty when manifest has a literal string
    uint32_t refId = 0;      // nonzero when manifest points at @resource
};

void parseAxmlLabel(const uint8_t* axml, size_t len, LabelResult& lr) {
    Cursor c{axml, len};
    uint16_t rootType = c.u16();
    uint16_t rootHdr = c.u16();
    uint32_t rootSize = c.u32();
    if (rootType != 0x0003 || rootSize > len || rootSize < 8) return;
    (void)rootHdr;
    StringPool sp;
    bool havePool = false;
    // iterate child chunks from offset 8
    c.pos = 8;
    while (c.pos + 8 <= rootSize) {
        Cursor cc{c.p, c.n}; cc.pos = c.pos;
        uint16_t ctype = cc.u16();
        uint16_t chdr = cc.u16();
        uint32_t csize = cc.u32();
        if (csize < 8 || c.pos + csize > rootSize) return;
        if (ctype == 0x0001) {
            cc.pos = c.pos;
            if (!parseStringPool(cc, sp)) return;
            havePool = true;
        } else if (ctype == 0x0102) { // START_TAG
            if (!havePool) { c.pos += csize; continue; }
            // node: lineNumber(4) comment(4)
            cc.pos = c.pos + 8 + 4 + 4;
            uint32_t ns = cc.u32();
            uint32_t nameIdx = cc.u32();
            (void)ns;
            uint16_t attrStart = cc.u16();
            uint16_t attrSize = cc.u16();
            uint16_t attrCount = cc.u16();
            cc.u16(); cc.u16(); cc.u16(); // id/class/style indexes
            if (!havePool) { c.pos += csize; continue; }
            const std::string& tag = poolAt(sp, nameIdx);
            if (tag != "application") { c.pos += csize; continue; }
            size_t base = c.pos + chdr + attrStart;
            // Explicit overflow-safe bounds check on the attribute array.
            if (base > c.n) { c.pos += csize; continue; }
            if (attrSize == 0) { c.pos += csize; continue; }
            {
                size_t total = (size_t)attrCount * (size_t)attrSize;
                if (total / attrSize != attrCount || base + total > c.n) {
                    c.pos += csize; continue;
                }
            }
            for (uint16_t i = 0; i < attrCount; i++) {
                Cursor ac{c.p, c.n}; ac.pos = base + (size_t)i * attrSize;
                if (!ac.need(20)) break; // attr entry is 20 bytes min
                ac.u32(); // attr ns
                uint32_t aName = ac.u32();
                uint32_t rawVal = ac.u32();
                uint16_t vSize = ac.u16();
                ac.u8(); // res0
                uint8_t vType = ac.u8();
                uint32_t vData = ac.u32();
                (void)vSize;
                const std::string& an = poolAt(sp, aName);
                if (an != "label") continue;
                lr.hasAttr = true;
                if (rawVal != 0xFFFFFFFFu) {
                    lr.direct = poolAt(sp, rawVal);
                    if (!lr.direct.empty()) return;
                }
                if (vType == 0x03) { // TYPE_STRING
                    lr.direct = poolAt(sp, vData);
                    if (!lr.direct.empty()) return;
                } else if (vType == 0x01) { // TYPE_REFERENCE
                    if (vData != 0xFFFFFFFFu) lr.refId = vData;
                }
                return; // found <application label> attr, done
            }
        }
        c.pos += csize;
    }
}

// ===========================================================================
// 5. resources.arsc resolution
// ===========================================================================

struct TypeChunk {
    uint8_t pkgId = 0;          // package this TYPE belongs to (0x7f = app)
    uint8_t typeId = 0;
    uint16_t headerSize = 0;   // offset of entry-offset array from chunk start
    uint32_t entryCount = 0;
    uint32_t entriesStart = 0;
    // config locale
    char lang[3] = {0,0,0};
    char country[3] = {0,0,0};
    uint16_t density = 0;
    size_t chunkPos = 0; // start of this ResTable_type chunk
};

struct Arsc {
    StringPool globalPool;
    std::vector<TypeChunk> types; // all ResTable_type chunks, file order
    bool ok = false;
};

// Parse one ResTable_type sub-chunk (nested inside its ResTable_package).
void parseTypeChunk(const uint8_t* arsc, size_t len, size_t pos, uint8_t pkgId,
                    Arsc& a) {
    Cursor cc{arsc, len}; cc.pos = pos;
    uint16_t ct = cc.u16();
    uint16_t chs = cc.u16();
    uint32_t cs = cc.u32();
    (void)ct;
    if (cs < 28 || pos + cs > len) return;
    TypeChunk t;
    t.pkgId = pkgId;
    t.chunkPos = pos;
    t.headerSize = chs;
    Cursor tc{arsc, len}; tc.pos = pos + 8;
    t.typeId = tc.u8();            // +8  id
    tc.u8(); tc.u16();             // +9 res0, +10 res1
    t.entryCount = tc.u32();       // +12
    t.entriesStart = tc.u32();     // +16
    // ResTable_config at +20: size u32, mcc/mnc, locale, screen
    Cursor cfg{arsc, len}; cfg.pos = pos + 20;
    uint32_t cfgSize = cfg.u32();  // +20
    cfg.u16(); cfg.u16();          // +24 mcc/mnc
    for (int i = 0; i < 2; i++) t.lang[i] = (char)cfg.u8();     // +28
    for (int i = 0; i < 2; i++) t.country[i] = (char)cfg.u8();  // +30
    cfg.u8(); cfg.u8();            // +32 orientation, touchscreen
    // density lives at config offset +14 (2 bytes). Only read it if the
    // declared config size covers that field; otherwise leave density=0
    // (which scores as "default/any density" anyway).
    t.density = 0;
    if (cfgSize >= 16) {
        t.density = cfg.u16();         // config+14 density
    }
    if (t.entryCount > kMaxPoolStrings) return;
    a.types.push_back(t);
}

void parseArsc(const uint8_t* arsc, size_t len, Arsc& a) {
    Cursor c{arsc, len};
    uint16_t type = c.u16();
    uint16_t hdr = c.u16();
    uint32_t size = c.u32();
    uint32_t pkgCount = c.u32();
    if (type != 0x0002 || size > len || pkgCount == 0) return;
    (void)hdr;
    c.pos = 12;
    while (c.pos + 8 <= size) {
        Cursor cc{arsc, len}; cc.pos = c.pos;
        uint16_t ct = cc.u16();
        uint16_t chs = cc.u16(); (void)chs;
        uint32_t cs = cc.u32();
        if (cs < 8 || c.pos + cs > size) return;
        if (ct == 0x0001) { // top-level global string pool
            cc.pos = c.pos;
            if (!parseStringPool(cc, a.globalPool)) return;
        } else if (ct == 0x0200) {
            // ResTable_package: TYPE/TYPE_SPEC live INSIDE this chunk.
            // header: +8 id u32, +12 name[128]u16, +268 typeStrings u32,
            //         +276 keyStrings u32; body runs to c.pos+cs.
            size_t pkgStart = c.pos;
            if (pkgStart + 12 > len) return;
            Cursor pc{arsc, len}; pc.pos = pkgStart + 8;
            uint32_t pkgId = pc.u32();
            // sub-chunks start after the 288-byte package header; walk inside
            // the package body boundary only.
            size_t sub = pkgStart + 288;
            size_t pkgEnd = pkgStart + cs;
            while (sub + 8 <= pkgEnd && sub + 8 <= len) {
                Cursor sc{arsc, len}; sc.pos = sub;
                uint16_t st = sc.u16();
                uint16_t shs = sc.u16(); (void)shs;
                uint32_t ssz = sc.u32();
                if (ssz < 8 || sub + ssz > pkgEnd || sub + ssz > len) return;
                if (st == 0x0201) { // nested ResTable_type
                    parseTypeChunk(arsc, len, sub, (uint8_t)(pkgId & 0xFF), a);
                }
                // 0x0202 TYPE_SPEC / nested string pools are skipped by size.
                sub += ssz;
            }
        }
        c.pos += cs;
    }
    a.ok = true;
}

// Score a config vs desired locale. Higher = better.
int configScore(const TypeChunk& t) {
    bool langZh = (t.lang[0] == 'z' && t.lang[1] == 'h');
    bool cn = (t.country[0] == 'C' && t.country[1] == 'N');
    bool defaultLocale = (t.lang[0] == 0 && t.country[0] == 0);
    int s;
    if (langZh && cn) s = 100;
    else if (langZh) s = 60;
    else if (defaultLocale) s = 40;
    else s = 10;
    if (t.density == 0) s += 1; // prefer density-unspecified ties
    return s;
}

// Proper arsc resolver that keeps the raw buffer. See resolveEntryWithBuffer.
std::string resolveEntryWithBuffer(const uint8_t* arsc, size_t len, const Arsc& a,
                                   uint32_t refId, int depth) {
    if (depth > kMaxResolveDepth) return "";
    if (refId == 0 || refId == 0xFFFFFFFFu) return "";
    uint32_t pkg = (refId >> 24) & 0xFF;
    uint32_t tid = (refId >> 16) & 0xFF;
    uint32_t eid = refId & 0xFFFF;
    if (pkg != 0x7f) return "";

    // Collect all TypeChunks matching pkg+tid, with their config score.
    // Real-world APKs (快手/王者荣耀) often define the label string in the
    // default/zh config but leave the zh-CN entry as 0xFFFFFFFF (not defined).
    // We must try chunks in score-descending order and skip entries that are
    // absent (0xFFFFFFFF), falling back to the next-best config.
    struct Candidate { const TypeChunk* tc; int score; };
    std::vector<Candidate> cands;
    for (const TypeChunk& t : a.types) {
        if (t.pkgId != (uint8_t)pkg) continue;
        if (t.typeId != tid) continue;
        cands.push_back({&t, configScore(t)});
    }
    // Sort by score descending (stable for equal scores).
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& x, const Candidate& y) { return x.score > y.score; });

    Cursor c{arsc, len};
    for (const Candidate& cd : cands) {
        const TypeChunk& best = *cd.tc;
        if (eid >= best.entryCount) continue;
        size_t offPos = best.chunkPos + best.headerSize;
        if (best.headerSize < 28 || offPos + 4ull * eid + 4 > len) continue;
        c.pos = offPos + 4ull * eid;
        uint32_t entryOff = c.u32();
        if (entryOff == 0xFFFFFFFFu) continue; // entry not defined in this config
        size_t entryPos = best.chunkPos + best.entriesStart + entryOff;
        if (entryPos + 8 > len) continue;
        c.pos = entryPos;
        uint16_t eSize = c.u16();
        uint16_t eFlags = c.u16();
        c.u32(); // key
        if (eSize < 8) continue;
        if (eFlags & 0x0001) continue; // complex bag: not a scalar string
        if (entryPos + eSize > len) continue;
        c.pos = entryPos + 8;
        uint16_t vSize = c.u16();
        c.u8();
        uint8_t vType = c.u8();
        uint32_t vData = c.u32();
        (void)vSize;
        if (vType == 0x03) { // string
            return poolAt(a.globalPool, vData);
        }
        if (vType == 0x01) { // reference -> chain; try this chunk, fall through if chain fails
            std::string chained = resolveEntryWithBuffer(arsc, len, a, vData, depth + 1);
            if (!chained.empty()) return chained;
            // chain resolved to empty: try next config chunk
        }
        // other types: skip to next config
    }
    return "";
}

// ===========================================================================
// 6. Internal path helpers
// ===========================================================================

// Directory name like "com.foo.bar-9Kx2==" or "~~abc==". Match if name == pkg
// or name starts with pkg followed by '-'.
bool dirNameMatchesPkg(const std::string& dir, const std::string& pkg) {
    if (dir == pkg) return true;
    if (dir.size() > pkg.size() && dir.compare(0, pkg.size(), pkg) == 0 && dir[pkg.size()] == '-')
        return true;
    return false;
}

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // anonymous namespace

// ===========================================================================
// Public API (namespace aatpe::apklabel)
// ===========================================================================

bool packageListHasPackage(const std::string& text, const std::string& pkg) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        size_t a = pos;
        while (a < nl && (text[a] == ' ' || text[a] == '\t')) a++;
        size_t b = a;
        while (b < nl && text[b] != ' ' && text[b] != '\t') b++;
        if (b > a && text.compare(a, b - a, pkg) == 0 && (b - a) == pkg.size()) {
            return true;
        }
        pos = nl + 1;
    }
    return false;
}

std::vector<std::string> baseApkCandidates(const std::string& pkg,
                                           const std::string& packagesListText,
                                           const std::string& appsRoot) {
    std::vector<std::string> out;
    if (!packageListHasPackage(packagesListText, pkg)) return out;
    DIR* d0 = ::opendir(appsRoot.c_str());
    if (!d0) return out;
    size_t iter = 0;
    while (dirent* e0 = ::readdir(d0)) {
        if (++iter > kMaxChildrenPerDir) break;
        if (e0->d_name[0] == '.') continue;
        std::string l1 = appsRoot + "/" + e0->d_name;
        DIR* d1 = ::opendir(l1.c_str());
        if (!d1) continue;
        size_t iter2 = 0;
        while (dirent* e1 = ::readdir(d1)) {
            if (++iter2 > kMaxChildrenPerDir) break;
            if (e1->d_name[0] == '.') continue;
            if (!dirNameMatchesPkg(e1->d_name, pkg)) continue;
            std::string base = l1 + "/" + e1->d_name + "/base.apk";
            if (fileExists(base)) out.push_back(base);
        }
        ::closedir(d1);
    }
    ::closedir(d0);
    return out;
}

std::string readLabelFromApk(const std::string& apkPath, std::string* err) {
    std::vector<uint8_t> manifest, arsc;
    if (!readApkEntry(apkPath, "AndroidManifest.xml", manifest)) {
        setErr(err, "no AndroidManifest.xml in apk");
        return "";
    }
    LabelResult lr;
    parseAxmlLabel(manifest.data(), manifest.size(), lr);
    if (!lr.hasAttr) { setErr(err, "no <application android:label>"); return ""; }
    if (!lr.direct.empty()) return lr.direct;
    if (lr.refId == 0) { setErr(err, "label attr has no resolvable value"); return ""; }
    if (!readApkEntry(apkPath, "resources.arsc", arsc)) {
        setErr(err, "label is a resource ref but resources.arsc missing");
        return "";
    }
    Arsc a;
    parseArsc(arsc.data(), arsc.size(), a);
    std::string label = resolveEntryWithBuffer(arsc.data(), arsc.size(), a, lr.refId, 0);
    if (label.empty()) setErr(err, "resource id did not resolve to a string");
    return label;
}

namespace {

// Shared TTL label cache. Positive labels live 300s; negative (empty) entries
// live 15s so a transient read failure is retried quickly instead of sticking
// on a blank name for 5 minutes. Guarded by sLabelCacheMtx.
struct LabelCacheEntry {
    std::string label;
    std::chrono::steady_clock::time_point at;
};
std::mutex sLabelCacheMtx;
std::unordered_map<std::string, LabelCacheEntry> sLabelCache;

constexpr long kLabelTtlPositiveSec = 300;
constexpr long kLabelTtlNegativeSec = 15;

long entryTtlSec(const LabelCacheEntry& e) {
    return e.label.empty() ? kLabelTtlNegativeSec : kLabelTtlPositiveSec;
}

} // namespace

std::string peekCachedLabel(const std::string& pkg) {
    if (pkg.empty()) return "";
    std::lock_guard<std::mutex> lk(sLabelCacheMtx);
    auto it = sLabelCache.find(pkg);
    if (it == sLabelCache.end()) return "";
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second.at).count();
    if (age >= entryTtlSec(it->second)) return ""; // expired (incl. negative TTL)
    return it->second.label;
}

std::string labelForPackage(const std::string& pkg,
                            const std::string& packagesListPath,
                            const std::string& appsRoot) {
    if (pkg.empty()) return "";
    // fast path: fresh cache hit (positive or still-valid negative)
    {
        std::lock_guard<std::mutex> lk(sLabelCacheMtx);
        auto it = sLabelCache.find(pkg);
        if (it != sLabelCache.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - it->second.at).count();
            if (age < entryTtlSec(it->second)) return it->second.label;
        }
    }
    std::string result;
    std::vector<uint8_t> listBuf;
    if (!readFileWhole(packagesListPath, listBuf, size_t(4u) * 1024u * 1024u)) {
        result = "";
    } else {
        std::string listText(listBuf.begin(), listBuf.end());
        std::vector<std::string> cands = baseApkCandidates(pkg, listText, appsRoot);
        for (const std::string& base : cands) {
            std::string label = readLabelFromApk(base);
            if (!label.empty()) { result = label; break; }
            // base did not resolve the resource: try sibling split_*.apk
            // resources for the manifest's resource reference.
            std::vector<uint8_t> manifest, arsc;
            if (!readApkEntry(base, "AndroidManifest.xml", manifest)) continue;
            LabelResult lr;
            parseAxmlLabel(manifest.data(), manifest.size(), lr);
            if (!lr.refId) continue;
            std::string dir = base.substr(0, base.rfind('/'));
            DIR* dd = ::opendir(dir.c_str());
            if (!dd) continue;
            std::vector<std::string> splits;
            while (dirent* de = ::readdir(dd)) {
                std::string n = de->d_name;
                // real split APKs are named split_config.*.apk / split_*.apk;
                // base.apk never starts with "split".
                if (n.rfind("split", 0) == 0 && n.size() > 4 &&
                    n.compare(n.size() - 4, 4, ".apk") == 0)
                    splits.push_back(dir + "/" + n);
            }
            ::closedir(dd);
            std::sort(splits.begin(), splits.end());
            for (const std::string& sp : splits) {
                if (!readApkEntry(sp, "resources.arsc", arsc)) continue;
                Arsc a; parseArsc(arsc.data(), arsc.size(), a);
                std::string sl = resolveEntryWithBuffer(arsc.data(), arsc.size(), a, lr.refId, 0);
                if (!sl.empty()) { label = sl; break; }
            }
            if (!label.empty()) { result = label; break; }
        }
    }
    {
        std::lock_guard<std::mutex> lk(sLabelCacheMtx);
        sLabelCache[pkg] = {result, std::chrono::steady_clock::now()};
    }
    return result;
}

// ===========================================================================
// R7: Multi-user package enumeration
// ===========================================================================

int userIdFromUid(long uid) {
    if (uid < 10000) return 0;
    return (int)(uid / 100000);
}

std::vector<std::string> listInstalledPackagesAllUsers(
    const std::string& packagesListPath,
    const std::string& appsRoot,
    const std::string& dataUserRoot) {
    (void)dataUserRoot; // informational only; global list already has all users
    std::set<std::string> all;

    // 1. Read global packages.list with proper line handling (no fgets truncation).
    FILE* f = ::fopen(packagesListPath.c_str(), "r");
    if (f) {
        std::string line;
        char buf[8192];
        while (::fgets(buf, sizeof(buf), f)) {
            line = buf;
            // If line doesn't end with newline and buffer wasn't full, it's EOF — fine.
            // If buffer was exactly filled and no newline, it's a truncated long line:
            // discard it (continue) to avoid treating fragments as package names.
            bool truncated = (line.size() == sizeof(buf) - 1 &&
                              line.back() != '\n');
            if (truncated) {
                // Drain the rest of this long line
                int c;
                while ((c = ::fgetc(f)) != EOF && c != '\n') {}
                continue;
            }
            // Trim trailing newline/whitespace
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                     line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty()) continue;

            // First token = package name, second = uid
            size_t sp = 0;
            while (sp < line.size() && (line[sp] == ' ' || line[sp] == '\t')) sp++;
            size_t sp2 = sp;
            while (sp2 < line.size() && line[sp2] != ' ' && line[sp2] != '\t') sp2++;
            if (sp2 <= sp) continue;
            std::string pkg = line.substr(sp, sp2 - sp);

            // Third token onward: uid
            size_t up = sp2;
            while (up < line.size() && (line[up] == ' ' || line[up] == '\t')) up++;
            size_t up2 = up;
            while (up2 < line.size() && line[up2] >= '0' && line[up2] <= '9') up2++;
            long uid = 0;
            bool haveUid = (up2 > up);
            if (haveUid) {
                try { uid = std::stol(line.substr(up, up2 - up)); }
                catch (...) { haveUid = false; }
            }

            // Filter: only third-party apps (uid >= 10000). System apps excluded.
            if (haveUid && uid < 10000) continue;

            // Sanitize package name: must look like a Java package name
            // (letters, digits, dots, underscores, dashes). Reject ../, spaces,
            // absolute paths, control chars, overly long names.
            if (pkg.size() > 256) continue;
            bool ok = !pkg.empty() && pkg[0] >= 'a' && pkg[0] <= 'z';
            for (char c : pkg) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                    ok = false; break;
                }
            }
            if (!ok) continue;

            all.insert(pkg);
        }
        ::fclose(f);
    }

    // 2. Bounded /data/app scan: supplement packages not in the list.
    //    Two-level walk, each level bounded to 4096 entries.
    DIR* d0 = ::opendir(appsRoot.c_str());
    if (d0) {
        size_t iter = 0;
        while (dirent* e0 = ::readdir(d0)) {
            if (++iter > 4096) break;
            if (e0->d_name[0] == '.') continue;
            std::string l1 = appsRoot + "/" + e0->d_name;
            DIR* d1 = ::opendir(l1.c_str());
            if (!d1) continue;
            size_t iter2 = 0;
            while (dirent* e1 = ::readdir(d1)) {
                if (++iter2 > 4096) break;
                if (e1->d_name[0] == '.') continue;
                std::string dn = e1->d_name;
                // dir name is like "com.foo.bar-XyZ==" — strip the -random suffix
                size_t dash = dn.find('-');
                if (dash != std::string::npos) dn = dn.substr(0, dash);
                // Validate package name
                if (dn.size() > 256 || dn.empty()) continue;
                if (dn[0] < 'a' || dn[0] > 'z') continue;
                bool ok = true;
                for (char c : dn) {
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                        ok = false; break;
                    }
                }
                if (ok) all.insert(dn);
            }
            ::closedir(d1);
        }
        ::closedir(d0);
    }

    return std::vector<std::string>(all.begin(), all.end());
}

} // namespace apklabel
} // namespace aatpe
