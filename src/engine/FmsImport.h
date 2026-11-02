#pragma once

#include "engine/Preset.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct FmsImportResult
{
    std::vector<Preset> presets;
    int skippedExpansion = 0; // Instruments for chips Chipmunk can't play. There are none left now.
    int skippedDpcm = 0;      // Sample instruments from projects too old to read samples from.

    // FamiStudio's id for each preset, parallel to "presets".
    std::vector<int> instrumentIds;
};

// Reads the 2A03 instruments and DPCM samples from a FamiStudio project. "data" is the project's contents after
// the file header has been decompressed, and "version" is the format version from that header.
// Each sound's channel comes from where the project's songs use it most.
bool importFmsProject(const uint8_t* data, size_t size, int version, FmsImportResult& out, std::string& error);
