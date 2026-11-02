// Writes each factory preset as a .chmk file, for tools/make_live_templates.py.

#include "engine/Preset.h"

#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::puts("usage: dump_factory_presets <output folder>");
        return 1;
    }

    for (const auto& preset : factoryPresets())
    {
        std::ofstream file(std::string(argv[1]) + "/" + preset.name + ".chmk");
        file << formatPreset(preset);
    }

    return 0;
}
