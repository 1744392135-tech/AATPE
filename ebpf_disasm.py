#!/usr/bin/env python3
"""Minimal eBPF bytecode disassembler for AATPE sched_events_embedded.h.

BPF instruction encoding (64-bit little-endian):
  byte0  opcode
  byte1  dst:src  (src = high nibble, dst = low nibble)
  bytes2-3  offset (s16)
  bytes4-7  imm (s32)
  16-byte wide instruction for LD_IMM (0x18).
"""
import re, sys

# G4-013: resolve the embedded header relative to this script (ebpf/) instead of a
# stale absolute dev-machine path; allow an optional CLI override.
import os, sys as _sys
_HERE = os.path.dirname(os.path.abspath(__file__))
SRC = _sys.argv[1] if len(_sys.argv) > 1 else os.path.join(_HERE, "ebpf", "sched_events_embedded.h")

# ---- BPF opcode tables ----
# ALU operations (class = BPF_ALU(4) / BPF_ALU64(7)); src = BPF_K(0)/BPF_X(8)
ALU_OP = {
    0x00: "add", 0x10: "sub", 0x20: "mul", 0x30: "div",
    0x40: "or",  0x50: "and", 0x60: "lsh", 0x70: "rsh",
    0x80: "neg", 0x90: "mod", 0xa0: "xor", 0xb0: "mov", 0xc0: "arsh",
}
# Memory/ld/st: size modifier
MEM_SZ = {0x00: "w", 0x08: "h", 0x10: "b", 0x18: "dw"}  # BPF_W/H/B/DW
# Mode bits for class BPF_LD(BPF_MEM=6)/BPF_ST(0): BPF_MEM=0x60
JMP_OP = {
    0x00: "ja",   0x10: "eq",   0x20: "gt",   0x30: "ge",
    0x40: "set",  0x50: "neq",  0x60: "sgt",  0x70: "sge",
    0x80: "call", 0x90: "exit", 0xa0: "lt",   0xb0: "slt",
    0xc0: "le",   0xd0: "be",   # jmp32 variants use class 6
}
REG = ["r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10"]

# Helper IDs as USED BY THIS FIRMWARE (from sched_events.bpf.c macros)
HELPER_FW = {
    1:  "map_lookup_elem",
    2:  "map_update_elem",
    3:  "map_delete_elem",
    4:  "probe_read",
    5:  "ktime_get_ns",
    8:  "get_smp_processor_id",
    14: "get_current_pid_tgid (FW-claimed)",
    16: "get_current_comm (FW-claimed)",
    25: "perf_event_output (FW-claimed)",
}
# Canonical upstream 4.14 enum bpf_func_id (for cross-check) — filled after web verify
HELPER_UPSTREAM_414 = {
    1:"map_lookup_elem",2:"map_update_elem",3:"map_delete_elem",4:"probe_read",
    5:"ktime_get_ns",6:"trace_printk",7:"get_prandom_u32",8:"get_smp_processor_id",
}

def parse_arrays(text):
    """Extract name -> bytes for each kEmbeddedProg_* array."""
    progs = {}
    pat = re.compile(r"static const uint8_t (kEmbeddedProg_\w+)\[\]\s*=\s*\{([^}]*)\};", re.S)
    for m in pat.finditer(text):
        name = m.group(1)
        body = m.group(2)
        nums = re.findall(r"0x([0-9a-fA-F]{2})", body)
        progs[name] = bytes(int(x,16) for x in nums)
    return progs

def disasm_insn(buf, i):
    """Decode instruction at index i (in 8-byte units). Returns (text, next_i)."""
    off = i*8
    b = buf[off:off+8]
    opcode = b[0]
    src = (b[1] >> 4) & 0xf
    dst = b[1] & 0xf
    k = int.from_bytes(b[2:4], "little", signed=True)
    imm = int.from_bytes(b[4:8], "little", signed=True)
    cls = opcode & 0x07
    name = None
    operands = ""

    if opcode == 0x00 and cls == 0:
        return f"{i:4d}: {'zero':14s}", i+1

    # ---- lddw (16-byte wide) ----
    if opcode == 0x18:
        lo = int.from_bytes(buf[off:off+8][4:8], "little", signed=False)
        hi = int.from_bytes(buf[off+8:off+16][0:8], "little", signed=False)
        imm64 = lo | (hi << 32)
        return f"{i:4d}: {'lddw':14s} {REG[dst]}, 0x{imm64 & 0xffffffffffffffff:x}  ; src_reg={src}", i+2

    # ---- ALU / ALU64 ----
    if cls in (0x04, 0x07):
        op = ALU_OP.get(opcode & 0xf0, f"op{opcode:#x}")
        sz = "64" if cls == 0x07 else "32"
        if (opcode & 0x08):  # BPF_X
            operands = f"{REG[dst]}, {REG[src]}"
        else:
            operands = f"{REG[dst]}, {imm}"
        name = f"{op}{sz}"
        return f"{i:4d}: {name:14s} {operands}", i+1

    # ---- JMP / JMP32 ----
    if cls in (0x05, 0x06):
        op = JMP_OP.get(opcode & 0xf0, f"jmp{opcode:#x}")
        if op == "call":
            h = HELPER_FW.get(imm, f"helper_{imm}")
            return f"{i:4d}: {'call':14s} {h}()", i+1
        elif op == "exit":
            return f"{i:4d}: {'exit':14s}", i+1
        elif op == "ja":
            return f"{i:4d}: {'ja':14s} +{k}", i+1
        else:
            sz = "32" if cls == 0x06 else "64"
            return f"{i:4d}: {f'j{op}{sz}':14s} {REG[dst]}, {imm}, +{k}", i+1

    # ---- LD/ST ----
    if cls == 0x01:  # BPF_LDX: dst = *(size*)(src+off)
        size = MEM_SZ.get(opcode & 0x18, "?")
        return f"{i:4d}: {f'ldx{size}':14s} {REG[dst]} = [{REG[src]}{'+' if k else ''}{k if k else ''}]", i+1
    if cls == 0x03:  # BPF_STX: *(dst+off) = src
        size = MEM_SZ.get(opcode & 0x18, "?")
        return f"{i:4d}: {f'stx{size}':14s} [{REG[dst]}{'+' if k else ''}{k if k else ''}] = {REG[src]}", i+1
    if cls == 0x02:  # BPF_ST: *(dst+off) = imm
        size = MEM_SZ.get(opcode & 0x18, "?")
        return f"{i:4d}: {f'st{size}':14s} [{REG[dst]}{'+' if k else ''}{k if k else ''}] = {imm}", i+1

    clsv = cls
    return f"{i:4d}: {f'op{opcode:#x}/cls{clsv}':14s} dst={REG[dst]} src={REG[src]} off={k} imm={imm}", i+1

def main():
    text = open(SRC).read()
    progs = parse_arrays(text)
    order = [
        "kEmbeddedProg_tracepoint_sched_sched_process_fork",
        "kEmbeddedProg_tracepoint_sched_sched_process_exit",
        "kEmbeddedProg_tracepoint_sched_sched_switch",
        "kEmbeddedProg_tracepoint_sched_sched_process_exec",
        # G4-013: these two were embedded but omitted from the disasm order list.
        "kEmbeddedProg_tracepoint_sched_sched_wakeup",
        "kEmbeddedProg_tracepoint_sched_sched_wakeup_new",
    ]
    # Disasm in declared order; any extra programs found are disassembled last.
    ordered = [n for n in order if n in progs]
    ordered += [n for n in progs if n not in ordered]
    for name in ordered:
        buf = progs[name]
        print("="*78)
        print(f"PROGRAM {name}")
        print(f"  total bytes = {len(buf)}  insns = {len(buf)//8}")
        print("="*78)
        i = 0
        while i*8 < len(buf):
            line, ni = disasm_insn(buf, i)
            print("   " + line)
            i = ni
        print()

if __name__ == "__main__":
    main()
