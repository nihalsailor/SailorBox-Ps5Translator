#include "common/Nid.hpp"
#include <iostream>
#include <iomanip>

using namespace Sailor;

int main() {
    auto printNid = [](const char* name) {
        Nid nid = Nid::Compute(name);
        std::cout << name << " -> NID: 0x" << std::hex << nid.Value() << " (Encoded: " << nid.ToString() << ")\n";
    };

    printNid("sceVideoOutSubmitFlip");
    printNid("sceVideoOutRegisterBuffers");
    printNid("sceVideoOutRegisterBuffers2");
    printNid("sceVideoOutSetBufferAttribute2");
    printNid("sceVideoOutGetResolutionStatus");
    printNid("sceVideoOutRegisterFlipEventQueue");
    printNid("sceVideoOutAddFlipEvent");
    printNid("sceVideoOutOpen");
    printNid("sceVideoOutClose");
    printNid("sceVideoOutSetFlipRate");
    return 0;
}
