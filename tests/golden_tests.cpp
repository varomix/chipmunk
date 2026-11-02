// Replays FamiStudio's test songs through ChannelEngine and compares every register write
// with the trace FamiStudio itself produced. A local script, fms_to_script.py, generates the inputs.
#include "SongScript.h"
#include "engine/FmsImport.h"

#include <zlib.h>

#include <cstdio>
#include <memory>
#include <cstring>
#include <iterator>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct TraceSink final : RegisterSink
{
    std::vector<std::string>& out;
    int frame = 0;

    explicit TraceSink(std::vector<std::string>& o) : out(o) {}

    void writeRegister(int reg, int data) override
    {
        char line[64];
        std::snprintf(line, sizeof(line), "Frame %d Register %04X %02X", frame, reg, data);
        out.emplace_back(line);
    }

    void skipCycles(int) override {}
};

// Reads the song's DPCM samples and mappings through the plugin's own project importer.
std::map<int, std::shared_ptr<const DpcmKit>> loadKits(const std::string& name)
{
    std::map<int, std::shared_ptr<const DpcmKit>> kits;

    std::ifstream file(std::string(CHIPMUNK_GOLDEN_DIR "/projects/") + name + ".fms", std::ios::binary);
    const std::string compressed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    int version = 0;
    uint32_t size = 0;
    std::memcpy(&version, compressed.data() + 4, 4);
    std::memcpy(&size, compressed.data() + 8, 4);

    std::vector<uint8_t> data(size);
    z_stream stream {};
    inflateInit2(&stream, -15);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data() + 12));
    stream.avail_in = (uInt) compressed.size() - 12;
    stream.next_out = data.data();
    stream.avail_out = size;
    inflate(&stream, Z_FINISH);
    inflateEnd(&stream);

    FmsImportResult result;
    std::string error;

    if (! importFmsProject(data.data(), data.size(), version, result, error))
        std::printf("%s: can't import: %s\n", name.c_str(), error.c_str());

    for (size_t i = 0; i < result.presets.size(); ++i)
        if (result.presets[i].kit != nullptr)
            kits[result.instrumentIds[i]] = result.presets[i].kit;

    return kits;
}

bool runSong(const std::string& name)
{
    SongScript song;
    {
        std::ifstream script(std::string(CHIPMUNK_GOLDEN_DIR "/") + name + ".script");
        readSongScript(script, song);
    }

    // Older scripts get their samples through the plugin's own project importer.
    if (song.kits.empty())
        for (const auto& [id, kit] : loadKits(name))
            song.kits[id] = std::const_pointer_cast<DpcmKit>(kit);

    std::vector<std::string> actual, expected;
    TraceSink sink(actual);
    SongChannels channels(song.channelFileTypes, [&](size_t) -> RegisterSink& { return sink; }, true);

    for (int frame = 0; frame < song.totalFrames; ++frame)
    {
        sink.frame = frame;
        channels.playFrame(song, frame);
    }

    gzFile trace = gzopen((std::string(CHIPMUNK_GOLDEN_DIR "/") + name + ".trace.gz").c_str(), "rb");
    char traceLine[128];
    std::string line;

    while (trace != nullptr && gzgets(trace, traceLine, sizeof(traceLine)) != nullptr)
    {
        line = traceLine;
        line.erase(line.find_last_not_of("\r\n") + 1);
        expected.push_back(line);
    }

    if (trace != nullptr)
        gzclose(trace);

    size_t mismatches = 0, first = expected.size();

    for (size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
    {
        if (actual[i] != expected[i])
        {
            ++mismatches;
            first = std::min(first, i);
        }
    }

    std::printf("%s: %zu writes, reference %zu, mismatched %zu\n", name.c_str(), actual.size(), expected.size(), mismatches);

    if (mismatches > 0 || actual.size() != expected.size())
    {
        if (first == expected.size())
            first = std::min(actual.size(), expected.size());

        for (size_t i = first > 3 ? first - 3 : 0; i < first + 6 && i < expected.size(); ++i)
            std::printf("  %zu: expected [%s] got [%s]\n", i, expected[i].c_str(), i < actual.size() ? actual[i].c_str() : "");

        return false;
    }

    return true;
}
} // namespace

int main()
{
    bool ok = true;

    for (const auto* name : { "TestBase", "TestFamiTrackerTempo", "TestVRC6", "TestMMC5", "TestS5B", "TestFDS", "TestN163", "TestVRC7", "TestEPSM" })
        ok = runSong(name) && ok;

    std::puts(ok ? "All golden traces match FamiStudio" : "Golden traces differ");
    return ok ? 0 : 1;
}
