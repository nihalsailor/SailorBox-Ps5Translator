#!/usr/bin/env python3
import struct
import subprocess
import os
import sys

def create_synthetic_ps5_elf(output_path):
    # 64-bit ELF Header
    e_ident = b"\x7fELF\x02\x01\x01\x09" + b"\x00" * 8 # FreeBSD/Sony ABI
    e_type = 0xFE00 # ET_SCE_EXEC
    e_machine = 0x3E # x86-64
    e_version = 1
    e_entry = 0x140001000
    e_phoff = 64
    e_shoff = 0
    e_flags = 0
    e_ehsize = 64
    e_phentsize = 56
    e_phnum = 2
    e_shentsize = 0
    e_shnum = 0
    e_shstrndx = 0

    elf_header = struct.pack("<16sHHIQQQIHHHHHH",
        e_ident, e_type, e_machine, e_version, e_entry,
        e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize,
        e_phnum, e_shentsize, e_shnum, e_shstrndx
    )

    # Program Header 1: PT_LOAD Code (PF_X | PF_R)
    # Return 42 in RAX:
    # mov eax, 42 (0xB8, 42, 0, 0, 0)
    # ret (0xC3)
    code_bytes = b"\xB8\x2A\x00\x00\x00\xC3" + b"\x90" * 10
    phdr1 = struct.pack("<IIQQQQQQ",
        1, # PT_LOAD
        5, # PF_R | PF_X
        0x1000, # offset
        0x140001000, # vaddr
        0x140001000, # paddr
        len(code_bytes), # filesz
        0x1000, # memsz
        0x1000  # align
    )

    # Program Header 2: PT_LOAD Data (PF_R | PF_W)
    data_bytes = b"Hello, SailorBox World!\x00"
    phdr2 = struct.pack("<IIQQQQQQ",
        1, # PT_LOAD
        6, # PF_R | PF_W
        0x2000, # offset
        0x140002000, # vaddr
        0x140002000, # paddr
        len(data_bytes), # filesz
        0x1000, # memsz
        0x1000  # align
    )

    with open(output_path, "wb") as f:
        f.write(elf_header)
        f.write(phdr1)
        f.write(phdr2)
        # Pad to 0x1000
        f.write(b"\x00" * (0x1000 - f.tell()))
        f.write(code_bytes)
        # Pad to 0x2000
        f.write(b"\x00" * (0x2000 - f.tell()))
        f.write(data_bytes)

    print(f"Created synthetic PS5 ELF at: {output_path}")

def test_relinker():
    elf_path = "test_synthetic.elf"
    exe_path = "test_synthetic.exe"
    relinker_exe = r"c:\Developments\@SailorBox\build\bin\sailor_relink.exe"

    create_synthetic_ps5_elf(elf_path)

    cmd = [relinker_exe, elf_path, exe_path]
    print(f"Running relinker: {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    print("STDOUT:", res.stdout)
    print("STDERR:", res.stderr)

    assert res.returncode == 0, f"Relinker returned {res.returncode}"
    assert os.path.exists(exe_path), f"Output {exe_path} does not exist"

    # Verify PE header
    with open(exe_path, "rb") as f:
        pe_bytes = f.read()

    assert pe_bytes[:2] == b"MZ", "Missing DOS magic MZ"
    pe_offset = struct.unpack_from("<I", pe_bytes, 0x3C)[0]
    assert pe_bytes[pe_offset:pe_offset+4] == b"PE\x00\x00", "Missing PE signature"

    print("SUCCESS: PE binary passed structural validation!")

if __name__ == "__main__":
    test_relinker()
