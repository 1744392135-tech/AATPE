#!/usr/bin/env python3
"""Generate embedded eBPF bytecode header from compiled .o file.
Extracts program bytecode, map definitions, and relocation refs.
Supports tracepoint/, kprobe/, kretprobe/ section types.
"""
import struct
import sys
import os

def c_escape(s):
    """R9-019 / LED-151: escape a Python str for safe embedding inside a C "..."
    literal. A license string or BPF section name containing a double quote, a
    backslash, or a control byte would otherwise break the generated header."""
    out = []
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20 or ord(ch) > 0x7e:
            out.append('\\x%02x' % ord(ch))
        else:
            out.append(ch)
    return ''.join(out)

def read_elf(path):
    with open(path, 'rb') as f:
        return f.read()

def parse_elf64(data):
    """Parse ELF64 header and section headers."""
    if data[:4] != b'\x7fELF':
        raise ValueError("Not ELF")
    # R9-011 / LED-142: validate header bounds up-front so a truncated / malformed
    # .o produces a clear diagnostic instead of an ugly struct.unpack IndexError or
    # a data.index() ValueError deep in the parser.
    if len(data) < 64:
        raise ValueError("Truncated ELF: %d bytes < 64-byte ELF64 header" % len(data))
    e_shoff = struct.unpack_from('<Q', data, 40)[0]
    e_shentsize = struct.unpack_from('<H', data, 58)[0]
    e_shnum = struct.unpack_from('<H', data, 60)[0]
    e_shstrndx = struct.unpack_from('<H', data, 62)[0]
    if e_shentsize < 64:
        raise ValueError("Bad e_shentsize=%d" % e_shentsize)
    if e_shoff == 0 or e_shnum == 0:
        raise ValueError("No section header table (e_shoff=%d e_shnum=%d)" % (e_shoff, e_shnum))
    if e_shoff + e_shnum * e_shentsize > len(data):
        raise ValueError("Section header table overruns file (shoff=%d num=%d entsz=%d filesz=%d)"
                         % (e_shoff, e_shnum, e_shentsize, len(data)))
    if e_shstrndx >= e_shnum:
        raise ValueError("Bad e_shstrndx=%d >= e_shnum=%d" % (e_shstrndx, e_shnum))

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name = struct.unpack_from('<I', data, off)[0]
        sh_type = struct.unpack_from('<I', data, off + 4)[0]
        sh_offset = struct.unpack_from('<Q', data, off + 24)[0]
        sh_size = struct.unpack_from('<Q', data, off + 32)[0]
        sh_link = struct.unpack_from('<I', data, off + 40)[0]
        sh_info = struct.unpack_from('<I', data, off + 44)[0]
        sh_entsize = struct.unpack_from('<Q', data, off + 56)[0]
        sections.append({
            'name_off': sh_name, 'type': sh_type, 'offset': sh_offset,
            'size': sh_size, 'link': sh_link, 'info': sh_info,
            'entsize': sh_entsize
        })

    # Section name string table. R9-011: guard the NUL search so a corrupt strtab
    # raises a clear error rather than a bare ValueError.
    shstr = sections[e_shstrndx]
    for s in sections:
        start = shstr['offset'] + s['name_off']
        if start >= len(data):
            s['name'] = ''
            continue
        try:
            name_end = data.index(b'\x00', start)
        except ValueError:
            name_end = len(data)
        s['name'] = data[start:name_end].decode('ascii', errors='replace')

    return sections

def parse_symtab(data, symtab_sec, strtab_sec):
    """Parse symbol table, return list of (name, value, size, info, other, shndx)."""
    syms = []
    entsize = symtab_sec['entsize'] if symtab_sec['entsize'] else 24
    num = symtab_sec['size'] // entsize
    for i in range(num):
        off = symtab_sec['offset'] + i * entsize
        st_name = struct.unpack_from('<I', data, off)[0]
        st_info = data[off + 4]
        st_other = data[off + 5]
        st_shndx = struct.unpack_from('<H', data, off + 6)[0]
        st_value = struct.unpack_from('<Q', data, off + 8)[0]
        st_size = struct.unpack_from('<Q', data, off + 16)[0]
        name = ''
        if st_name < strtab_sec['size']:
            name_end = data.index(b'\x00', strtab_sec['offset'] + st_name)
            name = data[strtab_sec['offset'] + st_name:name_end].decode('ascii')
        syms.append({'name': name, 'value': st_value, 'size': st_size,
                     'info': st_info, 'shndx': st_shndx})
    return syms

def parse_rel(data, rel_sec):
    """Parse Elf64_Rel section, return list of (r_offset, sym_idx, type)."""
    rels = []
    entsize = rel_sec['entsize'] if rel_sec['entsize'] else 16
    num = rel_sec['size'] // entsize
    for i in range(num):
        off = rel_sec['offset'] + i * entsize
        r_offset = struct.unpack_from('<Q', data, off)[0]
        r_info = struct.unpack_from('<Q', data, off + 8)[0]
        sym_idx = r_info >> 32
        r_type = r_info & 0xffffffff
        rels.append({'offset': r_offset, 'sym_idx': sym_idx, 'type': r_type})
    return rels

def map_name_to_index(name):
    if name == 'events_map': return 0
    if name == 'event_counters': return 1
    if name == 'wakeup_cache': return 2
    if name == 'switch_sample_counter': return 3
    return -1

def format_bytecode(data, offset, size):
    """Format bytecode as C hex array, 16 bytes per line."""
    lines = []
    for i in range(0, size, 16):
        chunk = data[offset + i:offset + i + min(16, size - i)]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        lines.append(f'    {hex_str},')
    return '\n'.join(lines)

def sanitize_name(name):
    return name.replace('/', '_').replace('.', '_')

def classify_section(name):
    """Classify a program section: returns (prog_type, func_name, is_kretprobe) or None."""
    # BPF_PROG_TYPE_TRACEPOINT = 5, BPF_PROG_TYPE_KPROBE = 2
    if name.startswith('tracepoint/'):
        return (5, name, 0)
    if name.startswith('kretprobe/'):
        return (2, name[len('kretprobe/'):], 1)
    if name.startswith('kprobe/'):
        return (2, name[len('kprobe/'):], 0)
    return None

def main():
    elf_path = sys.argv[1] if len(sys.argv) > 1 else 'sched_events.bpf.o'
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'sched_events_embedded.h'

    data = read_elf(elf_path)
    sections = parse_elf64(data)

    # Find symtab and strtab
    symtab_sec = None
    strtab_sec = None
    for s in sections:
        if s['type'] == 2 and s['name'] == '.symtab':  # SHT_SYMTAB
            symtab_sec = s
        if s['name'] == '.strtab':
            strtab_sec = s

    syms = parse_symtab(data, symtab_sec, strtab_sec) if symtab_sec and strtab_sec else []

    # Extract license string from ELF 'license' section.
    # This is REQUIRED for kernel 4.14: BPF_PROG_LOAD must receive a
    # GPL-compatible license pointer when the program uses GPL-only helpers
    # (bpf_perf_event_output, etc.). Embedding it in the header ensures the
    # loader always passes the exact license that the .o was compiled with.
    license_str = "GPL"
    for s in sections:
        if s['name'] == 'license' and s['size'] > 0:
            raw = data[s['offset']:s['offset'] + s['size']]
            license_str = raw.rstrip(b'\x00').decode('ascii', errors='replace')
            break

    # Find maps section
    maps_sec = None
    for s in sections:
        if s['name'] == 'maps':
            maps_sec = s
            break

    # Parse maps (bpf_map_def: type, key_size, value_size, max_entries, map_flags = 5 * u32 = 20 bytes)
    maps = []
    if maps_sec:
        num_maps = maps_sec['size'] // 20
        for i in range(num_maps):
            off = maps_sec['offset'] + i * 20
            mtype, key_size, value_size, max_entries, map_flags = struct.unpack_from('<IIIII', data, off)
            maps.append({'type': mtype, 'key_size': key_size, 'value_size': value_size,
                         'max_entries': max_entries, 'flags': map_flags})

    # Find program sections (tracepoint / kprobe / kretprobe) and their relocations
    programs = []
    for s in sections:
        if s['type'] != 1 or s['size'] <= 0:
            continue
        cls = classify_section(s['name'])
        if cls is None:
            continue
        prog_type, func_name, is_kretprobe = cls
        prog_name = s['name']

        # Find corresponding .rel section (.rel + full section name)
        rel_sec = None
        for rs in sections:
            if rs['name'] == '.rel' + prog_name:
                rel_sec = rs
                break

        # Parse relocations to get map indices per reference
        map_indices = []
        if rel_sec:
            rels = parse_rel(data, rel_sec)
            for r in rels:
                if r['sym_idx'] < len(syms):
                    midx = map_name_to_index(syms[r['sym_idx']]['name'])
                    map_indices.append({'offset': r['offset'], 'map_index': midx})
                else:
                    map_indices.append({'offset': r['offset'], 'map_index': -1})

        bytecode = data[s['offset']:s['offset'] + s['size']]
        # R9-012 / LED-143: every eBPF instruction is exactly 8 bytes. A program
        # section whose size is not a multiple of 8 is corrupt (or truncated) and
        # must fail the build loudly instead of being silently truncated by
        # s['size'] // 8 below (which would otherwise mis-length the program).
        if s['size'] % 8 != 0:
            raise ValueError(
                "Program section %r size=%d is not a multiple of 8 (eBPF insn = 8 bytes)"
                % (prog_name, s['size']))
        programs.append({
            'name': prog_name,
            'sanitized': sanitize_name(prog_name),
            'bytecode': bytecode,
            'size': s['size'],
            'insns': s['size'] // 8,
            'refs': map_indices,
            'prog_type': prog_type,
            'func_name': func_name,
            'is_kretprobe': is_kretprobe,
        })

    # Generate header
    lines = []
    lines.append('// AUTO-GENERATED embedded eBPF bytecode header')
    lines.append(f'// Generated from {os.path.basename(elf_path)} (license={license_str})')
    lines.append('// DO NOT EDIT MANUALLY')
    lines.append('//')
    lines.append('// v94 fix / 问题44/45/46: EXPECTED tracepoint ctx offsets (Linux 4.14 generic')
    lines.append('// TP_STRUCT__entry layout; verified on Dimensity 1100 / MT6893, kernel 4.14.186).')
    lines.append('// The BPF programs read these offsets via bpf_probe_read; they are HARDCODED in')
    lines.append('// sched_events.bpf.c. On any other kernel version / vendor BSP they MUST be')
    lines.append('// re-derived from /sys/kernel/[debug/]tracing/events/sched/<ev>/format before use:')
    lines.append('//   sched_switch       : prev_comm@8  prev_pid@24  next_comm@40  next_pid@56  next_prio@60')
    lines.append('//                         (prev_state is `long`=8B on arm64, which shifts next_* by +8)')
    lines.append('//   sched_process_fork  : child_pid@44  child_tgid@48')
    lines.append('//   sched_process_exec  : no ctx read (bpf_get_current_comm)')
    lines.append('// The daemon re-checks these at runtime and publishes ebpf_offset_verified.')
    lines.append('#ifndef AATPE_EBPF_EMBEDDED_H')
    lines.append('#define AATPE_EBPF_EMBEDDED_H')
    lines.append('')
    lines.append('#include <cstdint>')
    lines.append('')
    lines.append('namespace aatpe_ebpf {')
    lines.append('')
    # License string extracted from the ELF 'license' section.
    # MUST be GPL-compatible for kernel 4.14 when using GPL-only helpers.
    lines.append(f'// eBPF program license (from ELF license section)')
    lines.append(f'static const char kEmbeddedLicense[] = "{c_escape(license_str)}";')
    lines.append('')
    lines.append('// Map definitions (index matches order in .o maps section)')
    lines.append('struct EmbeddedMapDef {')
    lines.append('    uint32_t type;')
    lines.append('    uint32_t keySize;')
    lines.append('    uint32_t valueSize;')
    lines.append('    uint32_t maxEntries;')
    lines.append('    uint32_t flags;')
    lines.append('};')
    lines.append('')
    lines.append('static const EmbeddedMapDef kEmbeddedMaps[] = {')
    map_names = ['events_map', 'event_counters', 'wakeup_cache', 'switch_sample_counter']
    for i, m in enumerate(maps):
        mname = map_names[i] if i < len(map_names) else f'map_{i}'
        lines.append(f'    {{ {m["type"]}, {m["key_size"]}, {m["value_size"]}, {m["max_entries"]}, {m["flags"]} }},  // {mname}')
    lines.append('};')
    lines.append(f'static const int kEmbeddedMapCount = {len(maps)};')
    lines.append('')
    lines.append('// Map name -> index lookup for relocation patching')
    lines.append('struct EmbeddedMapRef {')
    lines.append('    int insnOffset;  // byte offset in program bytecode')
    lines.append('    int mapIndex;    // index into kEmbeddedMaps')
    lines.append('};')
    lines.append('')

    for prog in programs:
        type_str = 'KPROBE' if prog['prog_type'] == 2 else 'TRACEPOINT'
        ret_str = ' (kretprobe)' if prog['is_kretprobe'] else ''
        lines.append(f'// Program: {prog["name"]} [{type_str}{ret_str}] ({prog["size"]} bytes, {prog["insns"]} insns)')
        var_name = f'kEmbeddedProg_{prog["sanitized"]}'
        lines.append(f'static const uint8_t {var_name}[] = {{')
        lines.append(format_bytecode(prog['bytecode'], 0, prog['size']))
        lines.append('};')
        lines.append(f'static const int {var_name}_size = {prog["size"]};')
        lines.append('')
        lines.append(f'static const EmbeddedMapRef {var_name}_refs[] = {{')
        for ref in prog['refs']:
            mname = map_names[ref['map_index']] if 0 <= ref['map_index'] < len(map_names) else 'unknown'
            lines.append(f'    {{ {ref["offset"]}, {ref["map_index"]} }},  // ref to {mname}')
        lines.append('};')
        lines.append(f'static const int {var_name}_refCount = {len(prog["refs"])};')
        lines.append('')

    lines.append('// Program info for each program (in order)')
    lines.append('struct EmbeddedProgInfo {')
    lines.append('    const uint8_t* bytecode;')
    lines.append('    int bytecodeSize;')
    lines.append('    const EmbeddedMapRef* refs;')
    lines.append('    int refCount;')
    lines.append('    const char* sectionName;   // full section name, e.g. "kprobe/do_exit"')
    lines.append('    const char* kprobeFunc;    // kernel function to hook, e.g. "do_exit" (for kprobe type)')
    lines.append('    int progType;              // BPF_PROG_TYPE_KPROBE(2) or BPF_PROG_TYPE_TRACEPOINT(5)')
    lines.append('    int isKretprobe;           // 1 if kretprobe, 0 if kprobe/tracepoint')
    lines.append('};')
    lines.append('')
    lines.append('static const EmbeddedProgInfo kEmbeddedPrograms[] = {')
    for prog in programs:
        var_name = f'kEmbeddedProg_{prog["sanitized"]}'
        lines.append(f'    {{ {var_name}, {var_name}_size,')
        lines.append(f'      {var_name}_refs, {var_name}_refCount,')
        lines.append(f'      "{c_escape(prog["name"])}", "{c_escape(prog["func_name"])}",')
        lines.append(f'      {prog["prog_type"]}, {prog["is_kretprobe"]} }},')
    lines.append('};')
    # final-review fix: emit a table-derived count instead of a hardcoded
    # literal, so regenerating the header after adding/removing a program cannot
    # desync kEmbeddedProgramCount from kEmbeddedPrograms[].
    lines.append('static const int kEmbeddedProgramCount = '
                 '(int)(sizeof(kEmbeddedPrograms) / sizeof(kEmbeddedPrograms[0]));')
    lines.append('')
    lines.append('} // namespace aatpe_ebpf')
    lines.append('#endif // AATPE_EBPF_EMBEDDED_H')

    # M-22: write to a temp file FIRST and only atomically publish it after ALL
    # kernel-4.14 compatibility checks below pass. Previously the final header
    # was written before verification; if a check then failed the script exited
    # non-zero but an unverified (possibly incompatible) header was already left
    # on disk, which a subsequent build could compile against. On any failure we
    # remove the temp file and leave the existing on-disk header untouched.
    tmp_path = out_path + '.tmp'
    with open(tmp_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    print(f'Generated {tmp_path}: {len(programs)} programs, {len(maps)} maps')
    for prog in programs:
        type_str = 'KPROBE' if prog['prog_type'] == 2 else 'TRACEPOINT'
        ret_str = ' (kretprobe)' if prog['is_kretprobe'] else ''
        print(f'  {prog["name"]}: {prog["size"]} bytes, {len(prog["refs"])} refs [{type_str}{ret_str}] func={prog["func_name"]}')
        for ref in prog['refs']:
            mname = map_names[ref['map_index']] if 0 <= ref['map_index'] < len(map_names) else 'unknown'
            print(f'    ref @ offset {ref["offset"]} -> {mname} (idx {ref["map_index"]})')

    # Verify no BPF_ATOMIC (0xdb) or BPF_JMP32 (class=6) instructions.
    # BPF_ATOMIC requires kernel 5.1+.
    # BPF_JMP32 (opcode & 0x07 == 0x06) requires kernel 4.20+.
    # Kernel 4.14.186 supports neither — must fail the build if detected.
    print('\nVerification:')
    all_ok = True
    for prog in programs:
        has_atomic = False
        has_jmp32 = False
        for i in range(0, len(prog['bytecode']), 8):
            op = prog['bytecode'][i]
            if op == 0xdb:
                has_atomic = True
                all_ok = False
                print(f'  WARNING: {prog["name"]} has BPF_ATOMIC at offset {i} (opcode 0x{op:02x})')
            # BPF_JMP32: instruction class = (opcode & 0x07) == 0x06
            # This covers all JMP32 variants: JMP/JA/JEQ/JGT/JGE/JSET/JNE/JSGT/JSGE
            # with either K or X operands. Opcode bytes with class=6 include:
            # 0x06 (JMP32|JA), 0x16 (JMP32|JEQ|K), 0x26, 0x36, 0x46, 0x56, 0x66,
            # 0x76, 0x86, 0x96, 0xa6, 0xb6, 0xc6, 0xd6, 0xe6, 0xf6, and X variants.
            if (op & 0x07) == 0x06:
                has_jmp32 = True
                all_ok = False
                print(f'  FATAL: {prog["name"]} has BPF_JMP32 (class=6) at offset {i} (opcode 0x{op:02x}) — kernel 4.14 cannot load this!')
        if not has_atomic and not has_jmp32:
            print(f'  {prog["name"]}: no BPF_ATOMIC / BPF_JMP32 instructions (OK)')
    if all_ok:
        print('All programs: BPF_ATOMIC + BPF_JMP32 check PASSED')
        # M-22: checks passed — atomically publish the verified temp header.
        os.replace(tmp_path, out_path)
        print(f'Published verified header -> {out_path}')
    else:
        print('FATAL: Kernel-4.14-incompatible instructions detected — aborting!')
        # M-22: never leave an unverified header in place.
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        sys.exit(1)

if __name__ == '__main__':
    main()
