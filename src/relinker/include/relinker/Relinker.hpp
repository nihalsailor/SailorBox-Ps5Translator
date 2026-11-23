#pragma once

#include "relinker/ElfReader.hpp"
#include "relinker/PeWriter.hpp"
#include <filesystem>

namespace Sailor {

struct RelinkOptions {
    bool emitGuiSubsystem{false};
    bool isDll{false};
    u64 fixedBaseAddress{0x140000000ULL};
};

class Relinker {
public:
    Relinker() = default;

    bool Relink(const std::filesystem::path& inputElfPath,
                const std::filesystem::path& outputPePath,
                const RelinkOptions& options = {});
};

} // namespace Sailor
