#include "engine/EnvelopeEdit.h"
#include "engine/FmsImport.h"
#include "engine/InstrumentDocument.h"
#include "engine/NesEngine.h"
#include "engine/Preset.h"
#include "engine/TripleBuffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

Envelope parsed(const std::string& text, int type = envelopeVolume)
{
    Envelope env;
    std::string error;
    const auto ok = parseEnvelope(text, type, env, error);
    check(ok, "parses \"" + text + "\"" + (ok ? "" : ": " + error));
    return env;
}

void rejects(const std::string& text, int type, const std::string& expectedInError)
{
    Envelope env;
    env.length = 3;
    std::string error;
    const auto ok = parseEnvelope(text, type, env, error);
    check(! ok && error.find(expectedInError) != std::string::npos && env.length == 3,
          "rejects \"" + text + "\" with \"" + error + "\"");
}

bool sameInstrument(const Instrument& a, const Instrument& b)
{
    for (size_t t = 0; t < numEnvelopeTypes; ++t)
    {
        const auto& x = a.envelopes[t];
        const auto& y = b.envelopes[t];

        if (x.length != y.length || x.loop != y.loop || x.release != y.release || x.relative != y.relative)
            return false;

        for (int i = 0; i < x.length; ++i)
            if (x.values[(size_t) i] != y.values[(size_t) i])
                return false;
    }

    return true;
}

void testEnvelopeText()
{
    std::puts("envelope text");

    auto env = parsed("15 12 | 9 8 / 4 2");
    check(env.length == 6 && env.loop == 2 && env.release == 4 && env.values[1] == 12 && env.values[5] == 2, "values and markers land where written");
    check(formatEnvelope(env) == "15 12 | 9 8 / 4 2", "formats back to the same text");

    env = parsed("15,12,  9");
    check(env.length == 3 && env.loop == -1, "commas and extra spaces are fine");

    env = parsed("15 12 / 4 2");
    check(env.loop == 1 && env.release == 2, "a release with no loop holds on the step before it");

    env = parsed("| 0 4 7", envelopeArpeggio);
    check(env.loop == 0 && env.values[2] == 7, "loop can start at the first step");
    env = parsed("-12 0 63", envelopeArpeggio);
    check(env.values[0] == -12, "negative values work");

    env = parsed("");
    check(env.length == 0, "empty text clears the envelope");

    Envelope relative;
    relative.relative = true;
    std::string error;
    parseEnvelope("1 2", envelopePitch, relative, error);
    check(relative.relative, "parsing keeps the relative setting");

    rejects("16", envelopeVolume, "outside 0 to 15");
    rejects("-1", envelopeVolume, "outside");
    rejects("4", envelopeDutyCycle, "outside 0 to 3");
    rejects("64", envelopePitch, "outside -64 to 63");
    rejects("4", envelopeS5bMixer, "outside 0 to 3");
    rejects("32", envelopeS5bNoise, "outside 0 to 31");
    {
        Envelope vrc6Duty;
        std::string error;
        check(parseEnvelope("0 7", envelopeDutyCycle, vrc6Duty, error, envelopeDutyMaxVrc6) && vrc6Duty.values[1] == 7, "VRC6 duty goes up to 7");
    }
    rejects("abc", envelopeVolume, "Unexpected");
    rejects("1 | 2 | 3", envelopeVolume, "one loop");
    rejects("1 / 2 / 3", envelopeVolume, "one release");
    rejects("1 2 |", envelopeVolume, "after it");
    rejects("1 2 /", envelopeVolume, "after it");
    rejects("/ 1 2", envelopeVolume, "before it");
    rejects("1 / 2 | 3", envelopeVolume, "before the release");
    rejects("1 / 2", envelopePitch, "Only volume");

    std::string tooLong;
    for (int i = 0; i < 257; ++i)
        tooLong += "1 ";
    rejects(tooLong, envelopeVolume, "at most 256");
    rejects("99999999999999999999", envelopeVolume, "outside");
}

void testEnvelopeRules()
{
    std::puts("envelope rules");

    Envelope env;
    setEnvelopeValue(env, envelopeVolume, 3, 9);
    check(env.length == 4 && env.values[3] == 9, "drawing past the end extends the envelope");
    check(env.values[0] == 15, "new steps in an empty volume envelope start at full volume");

    setEnvelopeValue(env, envelopeVolume, 1, 99);
    check(env.values[1] == 15, "values are clamped to the type's range");
    setEnvelopeValue(env, envelopeVolume, 500, 1);
    check(env.length == 4, "out-of-range steps are ignored");

    setEnvelopeLength(env, envelopeVolume, 6);
    check(env.values[5] == 9, "lengthening repeats the last value");

    setEnvelopeLoop(env, 2);
    setEnvelopeRelease(env, envelopeVolume, 4);
    check(env.loop == 2 && env.release == 4, "loop and release are set");

    setEnvelopeLoop(env, 5);
    check(env.loop == 3, "the loop can't pass the release");
    setEnvelopeRelease(env, envelopeVolume, 1);
    check(env.release == 4, "the release can't pass the loop");

    setEnvelopeLength(env, envelopeVolume, 4);
    check(env.release == -1 && env.loop == 3, "shortening removes markers that fell off the end");

    setEnvelopeLength(env, envelopeVolume, 2);
    check(env.loop == -1, "the loop goes too");

    Envelope noLoop = parsed("1 2 3 4");
    setEnvelopeRelease(noLoop, envelopeVolume, 2);
    check(noLoop.loop == 1 && noLoop.release == 2, "adding a release adds the loop it needs");
    setEnvelopeLoop(noLoop, -1);
    check(noLoop.loop == -1 && noLoop.release == -1, "removing the loop removes the release");

    Envelope arp = parsed("1 2 3", envelopeArpeggio);
    setEnvelopeRelease(arp, envelopeArpeggio, 2);
    check(arp.release == -1, "only volume envelopes can release");

    Envelope bad;
    bad.length = 500;
    bad.loop = 400;
    bad.release = 2;
    bad.values[0] = 100;
    sanitizeEnvelope(bad, envelopeVolume);
    check(bad.length == 256 && bad.loop == -1 && bad.release == -1 && bad.values[0] == 15, "sanitizing repairs bad data");
}

void testPresets()
{
    std::puts("presets");

    Preset preset;
    preset.name = "My Sound";
    preset.channel = ChannelType::noise;
    preset.voice.dutyCycle = 1;
    preset.voice.velocitySensitive = false;
    preset.voice.legato = false;
    preset.voice.glideFrames = 12;
    preset.voice.bendRangeSemitones = 7;
    preset.voice.vibratoSpeed = 3;
    preset.instrument.envelopes[envelopeVolume] = parsed("15 | 10 / 5 0");
    preset.instrument.envelopes[envelopeArpeggio] = parsed("| 0 12", envelopeArpeggio);
    preset.instrument.envelopes[envelopePitch] = parsed("1 2 -3", envelopePitch);
    preset.instrument.envelopes[envelopePitch].relative = true;
    preset.instrument.envelopes[envelopeDutyCycle] = parsed("0 1 2 3", envelopeDutyCycle);
    preset.instrument.envelopes[envelopeS5bMixer] = parsed("0 1 2 3", envelopeS5bMixer);
    preset.instrument.envelopes[envelopeS5bNoise] = parsed("| 31 0", envelopeS5bNoise);
    preset.instrument.sawMasterVolume = 2;
    preset.instrument.s5bEnvelopeShape = 8;
    preset.instrument.s5bAutoPitch = false;
    preset.instrument.s5bAutoPitchOctave = -8;
    preset.instrument.s5bEnvelopePeriod = 65535;

    Preset loaded;
    std::string error;
    const auto text = formatPreset(preset);
    check(parsePreset(text, loaded, error), "reads its own output");
    check(loaded.name == preset.name && loaded.channel == preset.channel && loaded.voice.dutyCycle == 1
              && ! loaded.voice.velocitySensitive && ! loaded.voice.legato && loaded.voice.glideFrames == 12
              && loaded.voice.bendRangeSemitones == 7 && loaded.voice.vibratoSpeed == 3,
          "all settings survive");
    check(sameInstrument(loaded.instrument, preset.instrument), "all envelopes survive");
    check(loaded.instrument.sawMasterVolume == 2 && loaded.instrument.s5bEnvelopeShape == 8 && ! loaded.instrument.s5bAutoPitch
              && loaded.instrument.s5bAutoPitchOctave == -8 && loaded.instrument.s5bEnvelopePeriod == 65535, "chip settings survive");
    check(formatPreset(loaded) == text, "saving again gives identical text");

    check(! parsePreset("hello", loaded, error), "rejects other files");
    check(parsePreset("VaroNES preset 1\nname=Old\n", loaded, error) && loaded.name == "Old", "presets from before the rename still load");
    check(! parsePreset("Chipmunk preset 1\nvolume=99\n", loaded, error) && error.find("volume") != std::string::npos, "reports which envelope is bad");
    check(parsePreset("Chipmunk preset 1\nfutureThing=7\nname=A\n", loaded, error) && loaded.name == "A", "ignores keys from newer versions");

    const auto& factory = factoryPresets();
    check(factory.size() >= 15, "factory presets exist");

    bool factoryOk = true;
    for (const auto& p : factory)
    {
        Preset again;
        auto ok = parsePreset(formatPreset(p), again, error) && sameInstrument(again.instrument, p.instrument);

        // FM patches and drums shape their own sound.
        const auto shapesItself = (int) p.channel >= (int) ChannelType::vrc7Fm1 && ((int) p.channel < (int) ChannelType::epsmSquare1 || (int) p.channel >= (int) ChannelType::epsmFm1);

        if (p.name != "Init" && p.name != "Triangle Bass" && p.channel != ChannelType::dpcm && ! shapesItself)
            ok = ok && ! p.instrument.envelopes[envelopeVolume].isEmpty(envelopeVolume);

        if (! ok)
            std::printf("  bad factory preset: %s (%s)\n", p.name.c_str(), error.c_str());

        factoryOk = factoryOk && ok;
    }
    check(factoryOk, "every factory preset parsed with its envelopes intact");

    // Each channel gets a real choice of sounds, and names have to stay unique to work as favorites.
    std::map<ChannelType, int> perChannel;
    std::set<std::string> names;
    for (const auto& p : factory)
    {
        ++perChannel[p.channel];
        names.insert(p.name);
    }

    check(names.size() == factory.size(), "factory preset names are unique");
    check(perChannel.size() == 15, "every channel type has factory sounds");

    auto enoughSounds = true;
    for (const auto& [type, count] : perChannel)
        enoughSounds = enoughSounds && count >= (type == ChannelType::dpcm ? 2 : type == ChannelType::epsmRhythm1 || type == ChannelType::drumKit ? 1 : 4);
    check(enoughSounds, "every channel type has a real choice of sounds");

    const auto kit = std::find_if(factory.begin(), factory.end(), [](const Preset& p) { return p.name == "Synth Drum Kit"; });
    check(kit != factory.end() && kit->kit != nullptr && kit->kit->mappings[36 - 23].sample != nullptr && kit->kit->mappings[38 - 23].sample != nullptr && kit->kit->mappings[42 - 23].sample != nullptr,
          "the synth kit maps kick, snare and hat to their General MIDI keys");

    Preset kitAgain;
    check(parsePreset(formatPreset(*kit), kitAgain, error) && kitAgain.kit != nullptr && kitAgain.kit->mappings[36 - 23].sample->data == kit->kit->mappings[36 - 23].sample->data,
          "the synth kit saves and loads");
}

std::vector<uint8_t> makeFti(int instrumentType = 1)
{
    std::vector<uint8_t> f;
    const auto put32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) f.push_back((uint8_t) ((uint32_t) v >> (i * 8))); };
    const auto seq = [&](std::vector<int8_t> values, int loop, int release) {
        f.push_back(1);
        put32((int32_t) values.size());
        put32(loop);
        put32(release);
        put32(0);
        for (auto v : values)
            f.push_back((uint8_t) v);
    };

    for (char c : std::string("FTI2.4"))
        f.push_back((uint8_t) c);

    f.push_back((uint8_t) instrumentType);
    put32(4);
    for (char c : std::string("Lead"))
        f.push_back((uint8_t) c);

    f.push_back(5);
    seq({ 15, 12, 9, 6, 3 }, -1, 2);      // volume, release with no loop
    seq({ 0, 4, 7 }, 0, -1);              // arpeggio
    seq({ 1, -1 }, 0, -1);                // pitch
    seq({ 9, 9 }, -1, -1);                // hi-pitch, unsupported
    seq({ 0, 1, 2, 3, 9 }, -1, 1);        // duty, with a bad value and a release it can't have
    put32(0);                             // DPCM assignments, ignored for now
    put32(0);
    return f;
}

void testFtiImport()
{
    std::puts(".fti import");

    Preset preset;
    std::string error;
    const auto file = makeFti();

    check(importFti(file.data(), file.size(), preset, error), "imports a 2A03 instrument");
    check(preset.name == "Lead", "keeps its name");
    check(preset.voice.dutyCycle == 0, "stays at duty 0 as in FamiTracker");

    const auto& vol = preset.instrument.envelopes[envelopeVolume];
    check(vol.length == 5 && vol.loop == 2 && vol.release == 3, "converts FamiTracker's release as FamiStudio does");

    const auto& arp = preset.instrument.envelopes[envelopeArpeggio];
    check(arp.length == 3 && arp.loop == 0 && arp.values[2] == 7, "reads the arpeggio");
    check(preset.instrument.envelopes[envelopePitch].relative, "pitch envelopes are relative");

    const auto& duty = preset.instrument.envelopes[envelopeDutyCycle];
    check(duty.length == 5 && duty.values[4] == 3 && duty.release == -1, "duty is read after skipping hi-pitch, with bad data repaired");

    const auto vrc6 = makeFti(2);
    check(! importFti(vrc6.data(), vrc6.size(), preset, error) && error.find("2A03") != std::string::npos, "rejects other chips clearly");

    auto wrongVersion = makeFti();
    wrongVersion[5] = '1';
    check(! importFti(wrongVersion.data(), wrongVersion.size(), preset, error) && error.find("version") != std::string::npos, "rejects other versions");

    const uint8_t junk[] = { 1, 2, 3 };
    check(! importFti(junk, sizeof(junk), preset, error), "rejects other files");

    bool survivesTruncation = true;
    for (size_t size = 0; size < file.size() - 8; ++size)
        survivesTruncation = survivesTruncation && ! importFti(file.data(), size, preset, error);
    check(survivesTruncation, "every truncated file is rejected safely");

    std::mt19937 rng(5);
    for (int i = 0; i < 2000; ++i)
    {
        auto corrupt = file;
        for (int j = 0; j < 6; ++j)
            corrupt[rng() % corrupt.size()] = (uint8_t) rng();
        importFti(corrupt.data(), corrupt.size(), preset, error);
    }
    check(true, "2000 corrupted files don't crash");
}

// A minimal project in FamiStudio's format, with instruments used on different channels.
std::vector<uint8_t> makeProject(size_t extraBytesBeforeInstruments = 0)
{
    std::vector<uint8_t> f;
    const auto put8 = [&](int v) { f.push_back((uint8_t) v); };
    const auto put16 = [&](int v) { put8(v & 0xff); put8((v >> 8) & 0xff); };
    const auto put32 = [&](int v) { for (int i = 0; i < 4; ++i) put8((v >> (i * 8)) & 0xff); };
    const auto putString = [&](const std::u16string& text) {
        put32((int) text.size() * 2);
        for (auto c : text)
            put16(c);
    };

    put32(1000);               // next id
    put32(0);                  // sorting flags
    put32(-1); put32(-1); put32(-1); // name, author, copyright
    put32(0);                  // expansion mask
    put32(1);                  // N163 channels
    put32(0);                  // tempo mode
    put8(0);                   // PAL

    // Stands in for the export settings and samples that real projects keep here.
    std::mt19937 noise(3);
    for (size_t i = 0; i < extraBytesBeforeInstruments; ++i)
        put8((int) (noise() & 0xff));

    const auto instrument = [&](int id, const std::u16string& name, std::vector<int> volume) {
        put32(id);
        putString(name);
        put32(0);              // color
        put32(0);              // expansion
        put16(volume.empty() ? 0 : 1);
        if (! volume.empty())
        {
            put32((int) volume.size());
            put32(-1); put32(-1);
            put8(0);
            put32((int) volume.size());
            for (auto v : volume)
                put8(v);
        }
        put32(-1);             // folder
        put32(0);              // DPCM mappings
    };

    put32(5);
    instrument(1, u"Lead", { 15, 10 });
    instrument(2, u"B\u00e1jo \u266b", { 15 });
    instrument(3, u"Hat", { 8, 0 });
    instrument(4, u"Unused", { 3 });
    instrument(5, u"Mixed", { 9 });

    put32(0);                  // arpeggios
    put32(1);                  // songs

    put32(50); put32(16); put32(1); put32(4);
    putString(u"Song");
    put32(150); put32(6); put32(0); put32(0); put32(10);
    put32(1); put32(10);       // groove
    put32(1);                  // groove padding
    put8(0); put32(0); put32(0); put32(0); put32(-1); put32(0); // custom settings
    put32(-1);                 // folder

    const auto channel = [&](int type, std::vector<int> instruments) {
        put32(50);
        put32(1);              // patterns
        put32(type);
        put32(100 + type);
        putString(u"Pattern");
        put32(type); put32(0); put32(50);
        put32((int) instruments.size());
        int time = 0;
        for (auto id : instruments)
        {
            put16(time++);
            put8(40); put8(0); // value, flags
            put8(0); put32(id); put16(1); put16(0);
            put16(0);          // effects
            put32(-1);         // arpeggio
        }
        for (int i = 0; i < 256; ++i)
            put32(i == 0 ? 100 + type : -1);
    };

    channel(0, { 1, 1, 5 });
    channel(1, { 1 });
    channel(2, { 2, 2, 5 });
    channel(3, { 3, 5 });
    channel(4, {});

    return f;
}

void testProjectChannels()
{
    std::puts("project import: channels and names");

    const auto file = makeProject();

    FmsImportResult result;
    std::string error;
    check(importFmsProject(file.data(), file.size(), 19, result, error), "reads a minimal project: " + error);
    check(result.presets.size() == 5, "finds every instrument");

    if (result.presets.size() == 5)
    {
        check(result.presets[0].channel == ChannelType::pulse1, "used on pulse: pulse");
        check(result.presets[1].channel == ChannelType::triangle, "used only on triangle: triangle");
        check(result.presets[2].channel == ChannelType::noise, "used only on noise: noise");
        check(result.presets[3].channel == ChannelType::pulse1, "unused: pulse");
        check(result.presets[4].channel == ChannelType::pulse1, "tied: pulse");
        check(result.presets[1].name == "B\xc3\xa1jo \xe2\x99\xab", "non-English names survive");
        check(formatEnvelope(result.presets[0].instrument.envelopes[envelopeVolume]) == "15 10", "envelopes are read");
    }

    bool truncatedFails = true;
    for (size_t size = 0; size < file.size(); ++size)
    {
        FmsImportResult partial;
        truncatedFails = truncatedFails && ! importFmsProject(file.data(), size, 19, partial, error);
    }
    check(truncatedFails, "every truncated project is rejected safely");

    // A real project can hold a lot of samples ahead of the instruments.
    const auto large = makeProject(2 * 1024 * 1024);
    FmsImportResult largeResult;
    const auto started = std::chrono::steady_clock::now();
    const auto largeOk = importFmsProject(large.data(), large.size(), 19, largeResult, error);
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - started;
    std::printf("  finding instruments past 2 MB of other data took %.3f s\n", took.count());
    check(largeOk && largeResult.presets.size() == 5, "instruments are found past a lot of other data");
    check(took.count() < 1.0, "without a noticeable wait");

    auto extra = file;
    extra.push_back(0);
    FmsImportResult withExtra;
    check(! importFmsProject(extra.data(), extra.size(), 19, withExtra, error), "trailing garbage is rejected");

    std::mt19937 rng(9);
    for (int i = 0; i < 3000; ++i)
    {
        auto corrupt = file;
        for (int j = 0; j < 4; ++j)
            corrupt[rng() % corrupt.size()] = (uint8_t) rng();
        FmsImportResult damaged;
        importFmsProject(corrupt.data(), corrupt.size(), 19, damaged, error);
    }
    check(true, "3000 corrupted projects don't crash");
}

double correlation(const std::vector<float>& a, const std::vector<float>& b)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return ab / std::sqrt(aa * bb + 1e-12);
}

void testDpcmConversion()
{
    std::puts("DPCM conversion");

    constexpr int rate = 44100;
    std::vector<int16_t> wave(rate / 5);
    for (size_t i = 0; i < wave.size(); ++i)
        wave[i] = (int16_t) (12000.0 * std::sin(2.0 * M_PI * 110.0 * (double) i / rate) + 0.5);

    const auto converted = convertWaveToDpcm("Test", wave, rate);
    check(converted.sample != nullptr && converted.rate == 15 && ! converted.wasTruncated, "a short sound uses the fastest rate");

    const auto decoded = dpcmToWave(converted.sample->data, converted.sample->initialValueDiv2);
    const auto hz = dpcmRateHz(converted.rate);
    std::printf("  %zu bytes, %.3f s from a %.3f s sound\n", converted.sample->data.size(), decoded.size() / (double) hz, wave.size() / (double) rate);
    check(std::abs(decoded.size() / (double) hz - wave.size() / (double) rate) < 0.01, "the converted length matches the source");

    // Compare the decoded audio with the original, after normalizing and at matching positions.
    std::vector<float> original, roundTrip;
    for (size_t i = 200; i < decoded.size(); ++i)
    {
        const auto t = (double) i / hz;
        const auto index = (size_t) (t * rate);
        if (index >= wave.size())
            break;
        original.push_back((float) wave[index]);
        roundTrip.push_back((float) decoded[i]);
    }
    const auto match = correlation(original, roundTrip);
    std::printf("  correlation with the source: %.3f\n", match);
    check(match > 0.95, "the decoded sound follows the source");

    std::vector<int16_t> withSilence(rate / 2);
    for (size_t i = rate / 4; i < rate / 4 + rate / 10; ++i)
        withSilence[i] = (int16_t) (20000.0 * std::sin(2.0 * M_PI * 200.0 * (double) i / rate));
    const auto trimmed = convertWaveToDpcm("Gap", withSilence, rate);
    const auto trimmedSeconds = trimmed.sample->data.size() * 8.0 / dpcmRateHz(trimmed.rate);
    check(trimmedSeconds < 0.12, "silence around a sound is removed, leaving " + std::to_string(trimmedSeconds) + " s");

    std::vector<int16_t> quiet(rate / 10);
    for (size_t i = 0; i < quiet.size(); ++i)
        quiet[i] = (int16_t) (800.0 * std::sin(2.0 * M_PI * 200.0 * (double) i / rate));
    const auto normalized = dpcmToWave(convertWaveToDpcm("Quiet", quiet, rate).sample->data, dpcmDefaultInitialValueDiv2);
    int peak = 0;
    for (auto s : normalized)
        peak = std::max(peak, std::abs((int) s));
    check(peak > 15000, "quiet sounds are brought up to use the chip's range");

    const std::vector<int16_t> longWave(rate * 4, 8000);
    const auto medium = convertWaveToDpcm("Medium", longWave, rate);
    check(medium.rate < 15 && (int) medium.sample->data.size() <= dpcmMaxSampleBytes && ! medium.wasTruncated, "longer sounds pick a slower rate that fits");

    const std::vector<int16_t> tooLong(rate * 30, 8000);
    const auto huge = convertWaveToDpcm("Huge", tooLong, rate);
    check(huge.rate == 0 && (int) huge.sample->data.size() == dpcmMaxSampleBytes && huge.wasTruncated, "very long sounds are cut at the limit and flagged");

    const auto empty = convertWaveToDpcm("Empty", {}, rate);
    check(empty.sample != nullptr && empty.sample->data.empty(), "an empty sound is handled");

    check(midiNoteName(60) == "C3" && midiNoteName(61) == "C#3" && midiNoteName(24) == "C0", "key names match Live's");
}

void testKitsInPresets()
{
    std::puts("kits in presets");

    auto shared = std::make_shared<DpcmSampleData>();
    shared->name = "Kick drum, deep";
    shared->initialValueDiv2 = 20;
    for (int i = 0; i < 4081; ++i)
        shared->data.push_back((uint8_t) (i * 7));

    auto small = std::make_shared<DpcmSampleData>();
    small->name = "x";
    small->data = { 1, 2 };

    auto kit = std::make_shared<DpcmKit>();
    kit->mappings[37].sample = shared;
    kit->mappings[37].pitch = 9;
    kit->mappings[37].loop = true;
    kit->mappings[38].sample = shared;
    kit->mappings[38].overrideInitialValue = true;
    kit->mappings[38].initialValueDiv2 = 5;
    kit->mappings[50].sample = small;

    Preset preset;
    preset.channel = ChannelType::dpcm;
    preset.kit = kit;

    const auto text = formatPreset(preset);
    Preset loaded;
    std::string error;
    check(parsePreset(text, loaded, error), "reads its own output: " + error);
    check(loaded.channel == ChannelType::dpcm && loaded.kit != nullptr, "channel and kit survive");

    if (loaded.kit != nullptr)
    {
        const auto& a = loaded.kit->mappings[37];
        const auto& b = loaded.kit->mappings[38];
        check(a.sample->data == shared->data && a.sample->name == shared->name && a.sample->initialValueDiv2 == 20, "sample data and name survive");
        check(a.pitch == 9 && a.loop && ! a.overrideInitialValue, "key settings survive");
        check(b.sample == a.sample, "keys sharing a sample still share it");
        check(b.overrideInitialValue && b.initialValueDiv2 == 5, "overrides survive");
        check(loaded.kit->mappings[50].sample->data.size() == 2, "short samples round-trip");
        check(loaded.kit->mappings[1].sample == nullptr, "other keys stay empty");
    }

    check(formatPreset(loaded) == text, "saving again gives identical text");

    Preset bad;
    check(! parsePreset("Chipmunk preset 1\nsample=32 ***\n", bad, error), "a damaged sample is rejected");
    check(! parsePreset("Chipmunk preset 1\nkey=40 3 15 0 0 32\n", bad, error), "a key with no sample is rejected");
    check(! parsePreset("Chipmunk preset 1\nsample=32 AAAA x\nkey=400 0 15 0 0 32\n", bad, error), "a key out of range is rejected");
}

void testDocumentKits()
{
    std::puts("document with kits");

    InstrumentDocument document;
    check(document.kit() == nullptr, "starts without a kit");

    auto sample = std::make_shared<DpcmSampleData>();
    document.editKit([&](DpcmKit& kit) { kit.mappings[40].sample = sample; });
    const auto first = document.kit();
    check(first != nullptr && first->mappings[40].sample == sample, "editing creates one");

    document.editKit([&](DpcmKit& kit) { kit.mappings[40].pitch = 3; });
    check(document.kit() != first && first->mappings[40].pitch == 15, "edits never change a kit in use");
    check(document.kit()->mappings[40].sample == sample, "and share the sample data");

    document.undo();
    check(document.kit() == first, "undo restores the kit");
    document.undo();
    check(document.kit() == nullptr, "all the way back");
    document.redo();
    check(document.kit() == first, "redo works too");

    int kitChanges = 0;
    InstrumentDocument::KitPointer last;
    document.setListener([&](const Instrument&, const InstrumentDocument::KitPointer& kit) {
        kitChanges += kit != last ? 1 : 0;
        last = kit;
    });
    check(kitChanges == 1, "listeners hear the kit");
    document.edit([](Instrument&) {});
    check(kitChanges == 1, "envelope edits leave it alone");
}

// Exercises the DMC through the emulator. Freed samples would show up under the address sanitizer.
void testKitSwapSafety()
{
    std::puts("replacing kits during playback");

    NesEngine engine;
    engine.prepare(48000.0);
    engine.setChannelType(ChannelType::dpcm);

    std::vector<float> audio(2048);

    const auto makeKit = [](int fill) {
        auto kit = std::make_shared<DpcmKit>();
        auto sample = std::make_shared<DpcmSampleData>();
        for (int i = 0; i < 2001; ++i)
            sample->data.push_back((uint8_t) (i * fill));
        kit->mappings[37].sample = std::move(sample);
        return kit;
    };

    auto kit = makeKit(3);
    engine.setDpcmKit(kit.get());
    engine.noteOn(60, 127);
    engine.render(audio.data(), 2048);

    for (int i = 0; i < 100; ++i)
    {
        auto next = makeKit(i + 5);
        engine.setDpcmKit(next.get());
        kit = std::move(next); // The old kit and its samples are gone now.
        engine.render(audio.data(), 512);

        if (i % 10 == 0)
            engine.noteOn(60, 127);
    }

    engine.setDpcmKit(nullptr);
    kit.reset();
    engine.render(audio.data(), 2048);
    check(true, "the chip never reads a freed sample");

    // Two engines read their own samples.
    NesEngine a, b;
    a.prepare(48000.0);
    b.prepare(48000.0);
    a.setChannelType(ChannelType::dpcm);
    b.setChannelType(ChannelType::dpcm);
    auto kitA = makeKit(3), kitB = makeKit(91);
    a.setDpcmKit(kitA.get());
    b.setDpcmKit(kitB.get());
    a.noteOn(60, 127);
    b.noteOn(60, 127);
    std::vector<float> outA(12000), outB(12000);
    a.render(outA.data(), 12000);
    b.render(outB.data(), 12000);
    check(outA != outB, "each instance plays its own samples");
}

void testDocument()
{
    std::puts("document and undo");

    InstrumentDocument document;
    int notified = 0;
    document.setListener([&](const Instrument&, const InstrumentDocument::KitPointer&) { ++notified; });
    check(notified == 1, "a new listener hears the current state");

    document.beginGesture();
    for (int i = 0; i < 5; ++i)
        document.edit([&](Instrument& inst) { setEnvelopeValue(inst.envelopes[envelopeVolume], envelopeVolume, i, 3); });
    document.endGesture();
    check(document.snapshot().envelopes[envelopeVolume].length == 5, "edits apply");
    check(notified == 6, "the listener hears every edit");

    document.edit([](Instrument& inst) { setEnvelopeLoop(inst.envelopes[envelopeVolume], 1); });

    check(document.undo() && document.snapshot().envelopes[envelopeVolume].loop == -1, "undo reverts one edit");
    check(document.undo() && document.snapshot().envelopes[envelopeVolume].length == 0, "a whole stroke is one undo step");
    check(! document.undo(), "nothing left to undo");
    check(document.redo() && document.snapshot().envelopes[envelopeVolume].length == 5, "redo restores the stroke");

    document.edit([](Instrument& inst) { setEnvelopeLength(inst.envelopes[envelopeVolume], envelopeVolume, 2); });
    check(! document.canRedo(), "a new edit clears redo");

    const auto version = document.version();
    Instrument other;
    other.envelopes[envelopeArpeggio] = parsed("1 2", envelopeArpeggio);
    document.load(other, "Other");
    check(document.name() == "Other" && document.version() != version, "loading replaces the instrument and name");
    check(document.undo() && document.snapshot().envelopes[envelopeVolume].length == 2, "loading can be undone");

    for (int i = 0; i < 500; ++i)
        document.edit([&](Instrument& inst) { inst.envelopes[envelopeArpeggio].length = i % 3; });
    int steps = 0;
    while (document.undo())
        ++steps;
    check(steps == 200, "undo history is bounded");
}

// A value whose fields must always agree, so a torn read would be detected.
struct Checked
{
    std::array<int, 64> values {};
};

void testTripleBuffer()
{
    std::puts("lock-free instrument hand-off");

    TripleBuffer<Checked> buffer;
    constexpr int numWrites = 300000;
    bool torn = false, ordered = true;
    int seen = 0;

    std::thread writer([&] {
        for (int i = 1; i <= numWrites; ++i)
        {
            buffer.writeBuffer().values.fill(i);
            buffer.publish();
        }
    });

    int last = 0;
    while (last < numWrites)
    {
        if (buffer.fetch())
        {
            const auto& v = buffer.readBuffer();
            for (auto x : v.values)
                torn = torn || x != v.values[0];
            ordered = ordered && v.values[0] > last;
            last = v.values[0];
            ++seen;
        }
    }

    writer.join();

    std::printf("  reader saw %d of %d updates\n", seen, numWrites);
    check(! torn, "no torn reads");
    check(ordered, "values only move forward");
    check(last == numWrites, "the final value always arrives");
    check(! buffer.fetch(), "nothing new after the last value");
}

// Checks real audio: sound level over time follows the envelope, and changes apply mid-note.
void testLiveEditing()
{
    std::puts("live instrument changes in the engine");

    const auto rms = [](const std::vector<float>& a, int from, int to) {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += a[(size_t) i] * a[(size_t) i];
        return std::sqrt(sum / (to - from));
    };

    NesEngine engine;
    engine.prepare(48000.0);

    Instrument loud;
    loud.envelopes[envelopeVolume] = parsed("15");
    engine.setInstrument(loud);

    std::vector<float> audio(48000);
    engine.noteOn(69, 127);
    engine.render(audio.data(), 12000);

    Instrument quiet;
    quiet.envelopes[envelopeVolume] = parsed("3");
    engine.setInstrument(quiet);
    engine.render(audio.data() + 12000, 12000);

    const auto before = rms(audio, 2000, 12000), after = rms(audio, 15000, 24000);
    std::printf("  rms before edit %.4f, after %.4f\n", before, after);
    check(after < before * 0.5 && after > 0.001, "an edit changes the sounding note without cutting it");

    // Long loop, then shrink everything under the running indices.
    Instrument longEnv;
    std::string text;
    for (int i = 0; i < 256; ++i)
        text += std::to_string(i % 16) + " ";
    longEnv.envelopes[envelopeVolume] = parsed("| " + text);
    longEnv.envelopes[envelopeArpeggio] = parsed("| " + text, envelopeArpeggio);
    longEnv.envelopes[envelopePitch] = parsed("| " + text, envelopePitch);
    longEnv.envelopes[envelopeDutyCycle] = parsed("| 0 1 2 3 3 2 1 0", envelopeDutyCycle);
    engine.setInstrument(longEnv);
    engine.render(audio.data(), 40000);
    check(engine.getEnvelopePosition(envelopeVolume) >= 0, "playhead reports the envelope position");

    std::mt19937 rng(11);
    bool finite = true;
    for (int round = 0; round < 300; ++round)
    {
        Instrument random;
        for (int type = 0; type < numEnvelopeTypes; ++type)
        {
            auto& env = random.envelopes[(size_t) type];
            const auto range = envelopeRange(type);
            env.length = (int) (rng() % 257);
            for (int i = 0; i < env.length; ++i)
                env.values[(size_t) i] = (int8_t) (range.min + (int) (rng() % (unsigned) (range.max - range.min + 1)));
            env.loop = (int) (rng() % 300) - 1;
            env.release = (int) (rng() % 300) - 1;
            env.relative = type == envelopePitch && (rng() & 1) != 0;
            sanitizeEnvelope(env, type);
        }
        engine.setInstrument(random);
        if (round % 7 == 0)
            engine.noteOn(40 + (int) (rng() % 40), 100);
        if (round % 11 == 0)
            engine.noteOff(40 + (int) (rng() % 40));
        engine.render(audio.data(), 400);
        for (int i = 0; i < 400; ++i)
            finite = finite && std::isfinite(audio[(size_t) i]) && std::abs(audio[(size_t) i]) <= 1.0f;
    }
    check(finite, "300 random edits under a playing note stay safe");

    engine.allNotesOff();
    engine.render(audio.data(), 100);
    check(engine.getEnvelopePosition(envelopeVolume) == -1, "playhead clears when silent");
}
} // namespace

int main()
{
    testEnvelopeText();
    testEnvelopeRules();
    testPresets();
    testFtiImport();
    testProjectChannels();
    testDpcmConversion();
    testKitsInPresets();
    testDocumentKits();
    testKitSwapSafety();
    testDocument();
    testTripleBuffer();
    testLiveEditing();

    std::printf(failures == 0 ? "\nAll document tests passed\n" : "\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
