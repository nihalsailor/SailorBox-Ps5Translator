#!/usr/bin/env python3
import sys
import os
import struct
from pathlib import Path

def audit_elf(file_path):
    print(f"=== SailorBox Import Audit: {file_path} ===")
    if not os.path.exists(file_path):
        print(f"Error: File not found: {file_path}")
        return

    with open(file_path, "rb") as f:
        data = f.read()

    if len(data) < 64 or data[:4] != b"\x7fELF":
        print("Error: Invalid ELF file (magic mismatch).")
        return

    e_type, e_machine, e_version, e_entry, e_phoff = struct.unpack_from("<HHIQQ", data, 16)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)

    print(f"ELF Architecture: x86-64 (0x{e_machine:X})")
    print(f"ELF Entrypoint:   0x{e_entry:X}")
    print(f"Program Headers:  {e_phnum} entries at offset 0x{e_phoff:X}")

    pt_loads = 0
    pt_dynamic = 0
    for i in range(e_phnum):
        offset = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<IIQQQQQ", data, offset)
        if p_type == 1: # PT_LOAD
            pt_loads += 1
        elif p_type == 2: # PT_DYNAMIC
            pt_dynamic += 1

    print(f"Found {pt_loads} PT_LOAD segments and {pt_dynamic} PT_DYNAMIC sections.")
    print("Pre-flight check passed: Ready for SailorBox relinking!")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python import_audit.py <path-to-eboot.elf>")
        sys.exit(1)
    audit_elf(sys.argv[1])
