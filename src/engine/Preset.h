#pragma once

#include "engine/ChannelEngine.h"
#include "engine/Dpcm.h"
#include "engine/Instrument.h"
#include "engine/VoiceController.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A complete sound. Stored as readable text in .chmk files and inside Live Sets.
struct Preset
{
    std::string name = "Init";
    ChannelType channel = ChannelType::pulse1;
    VoiceController::Settings voice;
    int voices = 1;
    Instrument instrument;
    std::shared_ptr<const DpcmKit> kit;
};

std::string formatPreset(const Preset&);

// Unknown keys are ignored, so newer files still load. Fills "error" if the text is invalid.
bool parsePreset(const std::string& text, Preset& out, std::string& error);

// Reads the envelopes and DPCM samples of a 2A03 FamiTracker instrument file (.fti, version 2.4).
bool importFti(const uint8_t* data, size_t size, Preset& out, std::string& error);

const std::vector<Preset>& factoryPresets();
