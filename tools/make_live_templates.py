#!/usr/bin/env python3
"""Builds Live Set templates with a Chipmunk device on each track.

Live's file format isn't documented, so this starts from a real Set that already holds a Chipmunk
AU device, saved by the same Live version. It removes all of that Set's clips and automation,
then copies one of its tracks for each channel in the lineup.

    dump_factory_presets <presets folder>
    python3 -I tools/make_live_templates.py <base.als> <presets folder> templates
"""

import base64
import copy
import gzip
import pathlib
import re
import struct
import sys
import textwrap
import xml.etree.ElementTree as ET

LINEUPS = {
    "Chipmunk 2A03": [
        ("Pulse 1", "Hero Lead", 25),
        ("Pulse 2", "Pluck Lead", 25),
        ("Triangle", "Triangle Bass", 17),
        ("Noise", "Snare", 19),
        ("DPCM", "Synth Drum Kit", 3),
    ],
}

LINEUPS["Chipmunk All Chips"] = LINEUPS["Chipmunk 2A03"] + [
    ("VRC6 Pulse 1", "VRC6 Soft Lead", 5),
    ("VRC6 Pulse 2", "VRC6 Arp", 5),
    ("VRC6 Saw", "Saw Bass", 5),
    ("MMC5 Pulse 1", "MMC5 Lead", 9),
    ("MMC5 Pulse 2", "MMC5 Arp", 9),
    ("S5B 1", "S5B Lead", 13),
    ("S5B 2", "S5B Arp", 13),
    ("S5B 3", "S5B Growl Bass", 13),
]

CHANNELS = ["pulse", "triangle", "noise", "dpcm", "vrc6pulse", "vrc6saw", "mmc5pulse", "s5b",
            "fds", "n163", "vrc7", "epsmsquare", "epsmfm", "epsmrhythm", "drumkit"]

# Elements whose Id is unique across the whole Set, which copies of a track must not share.
GLOBAL_ID_TAG = re.compile(r"(AutomationTarget|ModulationTarget|Pointee|ControllerTargets\.\d+|AuPreset|RemoteableFloat|RemoteableBool)$")

SILENT_SEND = "0.0003162277571"


def read_preset(folder, name):
    text = (pathlib.Path(folder) / f"{name}.chmk").read_text()
    values = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    return text, values


def plugin_state(text, values):
    """The same bytes as AudioProcessor::copyXmlToBinary would give for this sound."""
    params = {
        "authenticGrid": 0,
        "bendRange": int(values["bendRange"]),
        "channel": CHANNELS.index(values["channel"]),
        "duty": int(values["duty"]),
        "gain": float(values.get("gain", 6)),
        "glide": int(values["glide"]),
        "legato": int(values["legato"]),
        "machine": 0,
        "n163Channels": int(values.get("n163Channels", 1)),
        "velocity": int(values["velocity"]),
        "vibratoSpeed": int(values["vibratoSpeed"]),
        "voices": int(values.get("voices", 1)),
    }
    root = ET.Element("Chipmunk")
    for key, value in params.items():
        ET.SubElement(root, "PARAM", id=key, value=f"{float(value)}")
    ET.SubElement(root, "Instrument", preset=text)
    xml = '<?xml version="1.0" encoding="UTF-8"?> ' + ET.tostring(root, encoding="unicode").replace("\n", "&#10;")
    body = xml.encode() + b"\0"
    return b"VC2!" + struct.pack("<I", len(body)) + body


def set_buffer(device, state):
    buffer = device.find(".//AuPreset/Buffer")
    indent = re.match(r"\s*", buffer.text).group(0)
    plist = bytes.fromhex(re.sub(r"\s", "", buffer.text)).decode()
    encoded = "\n\t".join(textwrap.wrap(base64.b64encode(state).decode(), 68))
    plist, count = re.subn(r"(<key>jucePluginState</key>\s*<data>).*?(</data>)", lambda m: f"{m.group(1)}\n\t{encoded}\n\t{m.group(2)}", plist, flags=re.S)
    assert count == 1
    lines = textwrap.wrap(plist.encode().hex().upper(), 80)
    buffer.text = indent + indent.join(lines) + indent[:-1]


def set_live_parameters(device, values):
    """Live keeps its own copy of each parameter's value, which has to agree with the state."""
    manual = {
        "Channel": CHANNELS.index(values["channel"]),
        "Duty": int(values["duty"]),
        "Output": (float(values.get("gain", 6)) + 24) / 48,
        "Glide": int(values["glide"]) / 64,
        "Bend Range": int(values["bendRange"]) / 24,
        "Legato": int(values["legato"]),
        "Vibrato Speed": (int(values["vibratoSpeed"]) - 1) / 11,
        "Authentic Grid": 0,
        "Velocity": int(values["velocity"]),
    }
    found = set()
    for parameter in device.find("ParameterList"):
        name = parameter.find("ParameterName").get("Value")
        if name in manual:
            parameter.find("ParameterValue/Manual").set("Value", f"{manual[name]}")
            if name == "Channel":
                parameter.find("LastItemCount").set("Value", str(len(CHANNELS)))
            found.add(name)
    assert found == set(manual), set(manual) - found


def clean_track(track):
    """Removes everything that belongs to the source Set's song."""
    for slot in track.iter("ClipSlot"):
        value = slot.find("Value")
        if value is not None:
            for child in list(value):
                value.remove(child)
    for events in track.iterfind(".//ArrangerAutomation/Events"):
        for child in list(events):
            events.remove(child)
    envelopes = track.find("AutomationEnvelopes/Envelopes")
    for child in list(envelopes):
        envelopes.remove(child)
    lanes = track.find("DeviceChain/AutomationLanes/AutomationLanes")
    for child in list(lanes)[1:]:
        lanes.remove(child)
    for take_lanes in track.iterfind("TakeLanes/TakeLanes"):
        for child in list(take_lanes):
            take_lanes.remove(child)

    mixer = track.find("DeviceChain/Mixer")
    mixer.find("Volume/Manual").set("Value", "1")
    mixer.find("Pan/Manual").set("Value", "0")
    mixer.find("Speaker/Manual").set("Value", "true")
    mixer.find("SoloSink").set("Value", "false")
    for manual in mixer.iterfind("Sends/TrackSendHolder/Send/Manual"):
        manual.set("Value", SILENT_SEND)
    track.find("DeviceChain/MainSequencer/Recorder/IsArmed").set("Value", "false")


def build(base, presets, lineup):
    """Each track in the lineup is (name, sound, colour). "presets" is a folder of sound files, or a
    function that returns the text and values of a sound."""
    root = copy.deepcopy(base)
    live_set = root.find("LiveSet")
    tracks = live_set.find("Tracks")

    prototype = next(t for t in tracks.findall("MidiTrack") if t.find(".//AuPluginDevice") is not None and any(
        p.find("ParameterName").get("Value") == "Channel" for p in t.find(".//AuPluginDevice/ParameterList")))
    prototype = copy.deepcopy(prototype)
    clean_track(prototype)

    returns = tracks.findall("ReturnTrack")
    for track in list(tracks):
        tracks.remove(track)

    next_id = int(live_set.find("NextPointeeId").get("Value"))
    track_id = 100

    for name, preset_name, colour in lineup:
        track = copy.deepcopy(prototype)
        track.set("Id", str(track_id))
        track_id += 1

        for element in track.iter():
            if "Id" in element.attrib and GLOBAL_ID_TAG.search(element.tag):
                element.set("Id", str(next_id))
                next_id += 1

        names = track.find("Name")
        names.find("EffectiveName").set("Value", name)
        names.find("UserName").set("Value", name)
        names.find("MemorizedFirstClipName").set("Value", "")
        track.find("Color").set("Value", str(colour))

        text, values = presets(preset_name) if callable(presets) else read_preset(presets, preset_name)
        device = track.find(".//AuPluginDevice")
        set_buffer(device, plugin_state(text, values))
        set_live_parameters(device, values)
        tracks.append(track)

    for track in returns:
        tracks.append(track)

    live_set.find("NextPointeeId").set("Value", str(next_id))
    live_set.find("Transport/CurrentTime").set("Value", "0")
    live_set.find("HighlightedTrackIndex").set("Value", "0")
    live_set.find("Annotation").set("Value", "")

    grooves = live_set.find("GroovePool/Grooves")
    for child in list(grooves):
        grooves.remove(child)

    for scene in live_set.find("Scenes"):
        scene.find("Name").set("Value", "")

    text = ET.tostring(root, encoding="unicode")
    for leftover in ("<MidiClip", "<AudioClip", "<MidiNoteEvent", "<SampleRef", "<Groove "):
        assert leftover not in text, leftover

    return '<?xml version="1.0" encoding="UTF-8"?>\n' + text + "\n"


def main():
    base_path, presets, output = sys.argv[1:4]
    base = ET.fromstring(gzip.open(base_path).read())
    output = pathlib.Path(output)
    output.mkdir(exist_ok=True)

    for title, lineup in LINEUPS.items():
        path = output / f"{title}.als"
        path.write_bytes(gzip.compress(build(base, presets, lineup).encode(), mtime=0))
        print(f"{path}: {len(lineup)} tracks, {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
