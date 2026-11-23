/**
 * ============================================================================
 * Tool: framebuffer_extractor
 * Description: Scans process memory for committed graphics framebuffers
 *              (1080p, 4K, 720p, etc.), inspects pixel content, and dumps
 *              valid framebuffers into 32-bit uncompressed BMP images.
 * Usage: framebuffer_extractor.exe <process_name_or_pid> [width] [height] [address_hex]
 * ============================================================================
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>

#pragma pack(push, 1)
struct BMPHeader {
    uint16_t bfType{0x4D42}; // "BM"
    uint32_t bfSize{0};
    uint16_t bfReserved1{0};
    uint16_t bfReserved2{0};
    uint32_t bfOffBits{54};
    uint32_t biSize{40};
    int32_t  biWidth{1920};
    int32_t  biHeight{-1080}; // negative for top-down bitmap
    uint16_t biPlanes{1};
    uint16_t biBitCount{32};
    uint32_t biCompression{0};
    uint32_t biSizeImage{0};
    int32_t  biXPelsPerMeter{0};
    int32_t  biYPelsPerMeter{0};
    uint32_t biClrUsed{0};
    uint32_t biClrImportant{0};
};
#pragma pack(pop)

static DWORD FindProcessId(const char* nameOrPid) {
    char* end = NULL;
    DWORD pid = strtoul(nameOrPid, &end, 10);
    if (end && *end == '\0') return pid;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    DWORD foundPid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, nameOrPid) == 0) {
                foundPid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return foundPid;
}

static bool SaveBMP(const char* filename, const uint8_t* data, int32_t width, int32_t height) {
    FILE* f = fopen(filename, "wb");
    if (!f) return false;

    uint32_t imageSize = (uint32_t)(width * height * 4);
    BMPHeader hdr;
    hdr.bfSize = sizeof(BMPHeader) + imageSize;
    hdr.biWidth = width;
    hdr.biHeight = -height; // top-down
    hdr.biSizeImage = imageSize;

    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(data, 1, imageSize, f);
    fclose(f);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <process_name_or_pid> [width] [height] [address_hex]\n", argv[0]);
        printf("Examples:\n");
        printf("  Auto-scan 1080p framebuffers: %s game.exe\n", argv[0]);
        printf("  Capture specific address:     %s game.exe 1920 1080 0x116130000\n", argv[0]);
        return 1;
    }

    DWORD pid = FindProcessId(argv[1]);
    if (!pid) {
        printf("[ERROR] Could not find process: %s\n", argv[1]);
        return 1;
    }

    int width = (argc >= 3) ? atoi(argv[2]) : 1920;
    int height = (argc >= 4) ? atoi(argv[3]) : 1080;
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    uint64_t targetAddr = 0;
    if (argc >= 5) {
        targetAddr = strtoull(argv[4], NULL, 16);
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        printf("[ERROR] Failed to open process PID %lu (error %lu)\n", pid, GetLastError());
        return 1;
    }

    size_t fbSize = (size_t)width * height * 4;
    std::vector<uint64_t> candidateAddrs;

    if (targetAddr != 0) {
        candidateAddrs.push_back(targetAddr);
    } else {
        printf("[SCANNER] Scanning committed virtual memory in PID %lu for %dx%d buffers (~%zu KB)...\n",
            pid, width, height, fbSize / 1024);

        MEMORY_BASIC_INFORMATION mbi;
        uint8_t* pAddr = NULL;
        while (VirtualQueryEx(hProc, pAddr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
                // Typical game swapchains allocate matching buffers with a slight alignment overhead
                if (mbi.RegionSize >= fbSize && mbi.RegionSize <= fbSize + 0x400000) {
                    candidateAddrs.push_back((uint64_t)mbi.BaseAddress);
                    printf("  -> Found buffer candidate at 0x%p (Region Size: 0x%zX)\n",
                        mbi.BaseAddress, mbi.RegionSize);
                }
            }
            pAddr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        }
    }

    if (candidateAddrs.empty()) {
        printf("[SCANNER] No matching framebuffers found in process memory.\n");
        CloseHandle(hProc);
        return 0;
    }

    uint8_t* fbBuffer = (uint8_t*)malloc(fbSize);
    if (!fbBuffer) {
        CloseHandle(hProc);
        return 1;
    }

    int savedIndex = 0;
    for (uint64_t addr : candidateAddrs) {
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProc, (void*)addr, fbBuffer, fbSize, &bytesRead) && bytesRead == fbSize) {
            // Statistical sampling to detect content
            int nonZeroSamples = 0;
            int totalSamples = 10000;
            size_t step = fbSize / totalSamples;
            uint32_t sampleColor = 0;

            for (int s = 0; s < totalSamples; ++s) {
                uint32_t pixel = *(uint32_t*)&fbBuffer[s * step];
                if (pixel != 0) {
                    nonZeroSamples++;
                    sampleColor = pixel;
                }
            }

            char outFilename[128];
            snprintf(outFilename, sizeof(outFilename), "framebuffer_capture_%d.bmp", savedIndex++);

            if (SaveBMP(outFilename, fbBuffer, width, height)) {
                printf("[SAVED] Framebuffer from 0x%llX -> %s\n", addr, outFilename);
                printf("  Pixel Content: %d / %d non-zero samples (Sample Color: 0x%08X)\n",
                    nonZeroSamples, totalSamples, sampleColor);
            }
        } else {
            printf("[WARN] Could not read 0x%llX (error %lu)\n", addr, GetLastError());
        }
    }

    free(fbBuffer);
    CloseHandle(hProc);
    printf("[SCANNER] Framebuffer capture complete.\n");
    return 0;
}
