cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Chipmunk VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CHIPMUNK_BUILD_PLUGIN "Build the JUCE plugin (off for sanitizer test builds)" ON)
option(CHIPMUNK_TSAN "Build with Thread Sanitizer" OFF)

if(CHIPMUNK_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

# Emulator (LGPL, see third_party/NesSndEmu)
set(EMU third_party/NesSndEmu)
add_library(NesSndEmu STATIC
    ${EMU}/Simple_Apu.cpp
    ${EMU}/nes_apu/apu_snapshot.cpp
    ${EMU}/nes_apu/Blip_Buffer.cpp
    ${EMU}/nes_apu/Nes_Apu.cpp
    ${EMU}/nes_apu/Nes_Namco.cpp
    ${EMU}/nes_apu/Nes_Oscs.cpp
    ${EMU}/nes_apu/Nes_Vrc6.cpp
    ${EMU}/nes_apu/Nes_Vrc7.cpp
    ${EMU}/nes_apu/Nes_Fds.cpp
    ${EMU}/nes_apu/Nes_Mmc5.cpp
    ${EMU}/nes_apu/Nes_Sunsoft.cpp
    ${EMU}/nes_apu/Nes_Fme7.cpp
    ${EMU}/nes_apu/emu2413.c
    ${EMU}/nes_apu/emu2149.c
    ${EMU}/nes_apu/Nes_EPSM.cpp
    ${EMU}/nes_apu/ym3438.cpp)
target_include_directories(NesSndEmu PUBLIC ${EMU})
target_compile_features(NesSndEmu PRIVATE cxx_std_14)
set_target_properties(NesSndEmu PROPERTIES CXX_STANDARD 14)
target_compile_options(NesSndEmu PRIVATE -w)

add_library(ChipmunkEngine STATIC
    src/engine/ChannelEngine.cpp
    src/engine/Dpcm.cpp
    src/engine/EnvelopeEdit.cpp
    src/engine/FmsImport.cpp
    src/engine/InstrumentDocument.cpp
    src/engine/NesEngine.cpp
    src/engine/PolyEngine.cpp
    src/engine/NoteTables.cpp
    src/engine/Preset.cpp
    src/engine/VoiceController.cpp)
target_include_directories(ChipmunkEngine PUBLIC src)
target_link_libraries(ChipmunkEngine PUBLIC NesSndEmu)

enable_testing()
add_executable(engine_tests tests/engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE ChipmunkEngine)
# The tests rely on the emulator's asserts, so keep them on in every build type.
target_compile_options(engine_tests PRIVATE -UNDEBUG)
add_test(NAME engine_tests COMMAND engine_tests)

find_package(ZLIB REQUIRED)

add_executable(golden_tests tests/golden_tests.cpp)
target_link_libraries(golden_tests PRIVATE ChipmunkEngine ZLIB::ZLIB)
target_compile_definitions(golden_tests PRIVATE CHIPMUNK_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
add_test(NAME golden_tests COMMAND golden_tests)

add_executable(document_tests tests/document_tests.cpp)
target_link_libraries(document_tests PRIVATE ChipmunkEngine)
target_compile_options(document_tests PRIVATE -UNDEBUG)
add_test(NAME document_tests COMMAND document_tests)

# Measures the engine against FamiStudio's own WAV export of a song.
add_executable(fms_compare tools/fms_compare.cpp)
target_link_libraries(fms_compare PRIVATE ChipmunkEngine)

add_executable(dump_factory_presets tools/dump_factory_presets.cpp)
target_link_libraries(dump_factory_presets PRIVATE ChipmunkEngine)

if(CHIPMUNK_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    juce_add_plugin(Chipmunk
        COMPANY_NAME "varomix"
        BUNDLE_ID "net.varomix.chipmunk"
        PLUGIN_MANUFACTURER_CODE Vrmx
        PLUGIN_CODE VNes
        FORMATS VST3 AU Standalone
        PRODUCT_NAME "Chipmunk"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        COPY_PLUGIN_AFTER_BUILD TRUE)

    set(CHIPMUNK_PLUGIN_SOURCES
        src/plugin/PluginProcessor.cpp
        src/plugin/PresetLibrary.cpp
        src/ui/ActivityDisplay.cpp
        src/ui/DpcmEditor.cpp
        src/ui/EnvelopeEditor.cpp
        src/ui/PluginEditor.cpp
        src/ui/PresetBrowser.cpp
        src/ui/WaveEditor.cpp)

    target_sources(Chipmunk PRIVATE ${CHIPMUNK_PLUGIN_SOURCES})
    target_include_directories(Chipmunk PRIVATE src)
    target_compile_definitions(Chipmunk PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(Chipmunk
        PRIVATE
            ChipmunkEngine
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Runs the real processor and editor without a host, including mouse gestures and screenshots.
    juce_add_console_app(plugin_tests PRODUCT_NAME "plugin_tests")
    target_sources(plugin_tests PRIVATE tests/plugin_tests.cpp ${CHIPMUNK_PLUGIN_SOURCES})
    target_include_directories(plugin_tests PRIVATE src)
    target_compile_definitions(plugin_tests PRIVATE
        JucePlugin_Name="Chipmunk"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_compile_definitions(plugin_tests PRIVATE CHIPMUNK_PROJECTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/projects" CHIPMUNK_TEMPLATES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates")
    target_compile_options(plugin_tests PRIVATE -UNDEBUG)
    target_link_libraries(plugin_tests PRIVATE ChipmunkEngine juce::juce_audio_utils juce::juce_recommended_config_flags)
    add_test(NAME plugin_tests COMMAND plugin_tests)

    # Plays a Live Set's arrangement through the real processor, for sample songs.
    juce_add_console_app(render_als PRODUCT_NAME "render_als")
    target_sources(render_als PRIVATE tools/render_als.cpp ${CHIPMUNK_PLUGIN_SOURCES})
    target_include_directories(render_als PRIVATE src)
    target_compile_definitions(render_als PRIVATE
        JucePlugin_Name="Chipmunk"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(render_als PRIVATE ChipmunkEngine juce::juce_audio_utils juce::juce_recommended_config_flags)

    # Fails if a sample song has a silent track or clips.
    foreach(song "varo - First Stage" "varo - Neon Causeway" "varo - Crystal Cavern")
        add_test(NAME "song: ${song}" COMMAND render_als "${CMAKE_CURRENT_SOURCE_DIR}/examples/${song}.als" "${CMAKE_CURRENT_BINARY_DIR}/${song}.wav")
    endforeach()
endif()
