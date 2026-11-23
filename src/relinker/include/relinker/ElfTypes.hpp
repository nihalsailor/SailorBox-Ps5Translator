#pragma once

#include "common/Types.hpp"

namespace Sailor {

constexpr u8 ELF_MAGIC[4] = {0x7F, 'E', 'L', 'F'};

constexpr u16 ET_EXEC         = 2;
constexpr u16 ET_DYN          = 3;
constexpr u16 ET_SCE_EXEC     = 0xFE00;
constexpr u16 ET_SCE_DYNAMIC  = 0xFE10;

constexpr u32 PT_NULL         = 0;
constexpr u32 PT_LOAD         = 1;
constexpr u32 PT_DYNAMIC      = 2;
constexpr u32 PT_INTERP       = 3;
constexpr u32 PT_NOTE         = 4;
constexpr u32 PT_SHLIB        = 5;
constexpr u32 PT_PHDR         = 6;
constexpr u32 PT_TLS          = 7;
constexpr u32 PT_SCE_PROGLIB  = 0x61000000;
constexpr u32 PT_SCE_RELA     = 0x60000000;
constexpr u32 PT_SCE_COMMENT  = 0x6FFFFF00;
constexpr u32 PT_SCE_VERSION  = 0x6FFFFF01;

constexpr u32 PF_X            = 1;
constexpr u32 PF_W            = 2;
constexpr u32 PF_R            = 4;

constexpr i64 DT_INIT           = 12;
constexpr i64 DT_FINI           = 13;
constexpr i64 DT_INIT_ARRAY     = 25;
constexpr i64 DT_FINI_ARRAY     = 26;
constexpr i64 DT_INIT_ARRAYSZ   = 27;
constexpr i64 DT_FINI_ARRAYSZ   = 28;

// Sony PlayStation 5 Dynamic Tags
constexpr i64 DT_SCE_NEEDED     = 0x6100000D;
constexpr i64 DT_SCE_EXPORT_LIB = 0x61000013;
constexpr i64 DT_SCE_IMPORT_LIB = 0x61000015;
constexpr i64 DT_SCE_JMPREL     = 0x61000029;
constexpr i64 DT_SCE_PLTGOT     = 0x6100002B;
constexpr i64 DT_SCE_PLTRELSZ   = 0x6100002D;
constexpr i64 DT_SCE_RELA       = 0x6100002F;
constexpr i64 DT_SCE_RELASZ     = 0x61000031;
constexpr i64 DT_SCE_RELAENT    = 0x61000033;
constexpr i64 DT_SCE_STRTAB     = 0x61000035;
constexpr i64 DT_SCE_STRSZ      = 0x61000037;
constexpr i64 DT_SCE_SYMTAB     = 0x61000039;
constexpr i64 DT_SCE_SYMENT     = 0x6100003B;
constexpr i64 DT_SCE_SYMTABSZ   = 0x6100003F;

#pragma pack(push, 1)

struct Elf64_Ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct Elf64_Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

struct Elf64_Shdr {
    u32 sh_name;
    u32 sh_type;
    u64 sh_flags;
    u64 sh_addr;
    u64 sh_offset;
    u64 sh_size;
    u32 sh_link;
    u32 sh_info;
    u64 sh_addralign;
    u64 sh_entsize;
};

struct Elf64_Dyn {
    i64 d_tag;
    union {
        u64 d_val;
        u64 d_ptr;
    } d_un;
};

struct Elf64_Rela {
    u64 r_offset;
    u64 r_info;
    i64 r_addend;
};

struct Elf64_Sym {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
};

#pragma pack(pop)

} // namespace Sailor
