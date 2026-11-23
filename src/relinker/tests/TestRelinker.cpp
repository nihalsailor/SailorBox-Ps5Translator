#include "relinker/Relinker.hpp"
#include "common/Logger.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cassert>

namespace {

void CreateSyntheticElf(const std::filesystem::path& path) {
    std::ofstream f(path, std::ios::binary);

    // 64-bit ELF Header
    Sailor::Elf64_Ehdr ehdr{};
    ehdr.e_ident[0] = 0x7F;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2; // 64-bit
    ehdr.e_ident[5] = 1; // Little endian
    ehdr.e_ident[6] = 1; // Version
    ehdr.e_ident[7] = 9; // FreeBSD / Sony
    ehdr.e_type = Sailor::ET_SCE_EXEC;
    ehdr.e_machine = 0x3E; // x86-64
    ehdr.e_version = 1;
    ehdr.e_entry = 0x140001000ULL;
    ehdr.e_phoff = sizeof(Sailor::Elf64_Ehdr);
    ehdr.e_ehsize = sizeof(Sailor::Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Sailor::Elf64_Phdr);
    ehdr.e_phnum = 2;

    f.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));

    // Code: return 42
    // mov eax, 42 (B8 2A 00 00 00), ret (C3)
    const std::vector<Sailor::u8> code = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
    const std::vector<Sailor::u8> data = { 'H', 'e', 'l', 'l', 'o', '!', 0 };

    // Phdr 1: Code (.text)
    Sailor::Elf64_Phdr phdr1{};
    phdr1.p_type = Sailor::PT_LOAD;
    phdr1.p_flags = Sailor::PF_R | Sailor::PF_X;
    phdr1.p_offset = 0x1000;
    phdr1.p_vaddr = 0x140001000ULL;
    phdr1.p_paddr = 0x140001000ULL;
    phdr1.p_filesz = code.size();
    phdr1.p_memsz = 0x1000;
    phdr1.p_align = 0x1000;

    // Phdr 2: Data (.data)
    Sailor::Elf64_Phdr phdr2{};
    phdr2.p_type = Sailor::PT_LOAD;
    phdr2.p_flags = Sailor::PF_R | Sailor::PF_W;
    phdr2.p_offset = 0x2000;
    phdr2.p_vaddr = 0x140002000ULL;
    phdr2.p_paddr = 0x140002000ULL;
    phdr2.p_filesz = data.size();
    phdr2.p_memsz = 0x1000;
    phdr2.p_align = 0x1000;

    f.write(reinterpret_cast<const char*>(&phdr1), sizeof(phdr1));
    f.write(reinterpret_cast<const char*>(&phdr2), sizeof(phdr2));

    // Pad to 0x1000
    std::size_t cur = static_cast<std::size_t>(f.tellp());
    std::vector<char> pad1(0x1000 - cur, 0);
    f.write(pad1.data(), pad1.size());
    f.write(reinterpret_cast<const char*>(code.data()), code.size());

    // Pad to 0x2000
    cur = static_cast<std::size_t>(f.tellp());
    std::vector<char> pad2(0x2000 - cur, 0);
    f.write(pad2.data(), pad2.size());
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace

int main() {
    std::filesystem::path inputElf = "test_synthetic.elf";
    std::filesystem::path outputExe = "test_synthetic.exe";

    std::cout << "[TEST] Creating synthetic PS5 ELF binary...\n";
    CreateSyntheticElf(inputElf);

    std::cout << "[TEST] Running SailorBox Relinker...\n";
    Sailor::Relinker relinker;
    bool success = relinker.Relink(inputElf, outputExe);
    assert(success);

    // Verify output PE
    std::ifstream peFile(outputExe, std::ios::binary | std::ios::ate);
    assert(peFile.is_open());
    std::size_t peSize = static_cast<std::size_t>(peFile.tellg());
    peFile.seekg(0, std::ios::beg);

    std::vector<Sailor::u8> peBytes(peSize);
    peFile.read(reinterpret_cast<char*>(peBytes.data()), peSize);

    assert(peBytes[0] == 'M' && peBytes[1] == 'Z');
    Sailor::u32 peOffset = *reinterpret_cast<const Sailor::u32*>(peBytes.data() + 0x3C);
    assert(peBytes[peOffset] == 'P' && peBytes[peOffset + 1] == 'E');

    std::cout << "[TEST] SUCCESS: Transmutation produced valid 64-bit Windows PE (" << peSize << " bytes)\n";
    return 0;
}
