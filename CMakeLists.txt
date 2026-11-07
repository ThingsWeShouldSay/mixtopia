cmake_minimum_required(VERSION 3.22)

project(Mixtopia VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)
if (NOT EXISTS ${EXTERNAL_DIR}/tracktion_engine/modules/juce/CMakeLists.txt)
    message(FATAL_ERROR "Dependencies missing - run scripts/fetch-deps.sh first")
endif()

# No paths from the build machine in the program: source and build folder paths (in __FILE__ and any debug info)
# are written as "./..." and "build/...". Set before JUCE and Tracktion are added, so it covers all compiled code.
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
if (NOT CMAKE_BINARY_DIR MATCHES "^${CMAKE_SOURCE_DIR}/")
    add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
endif()

add_subdirectory(${EXTERNAL_DIR}/tracktion_engine/modules/juce ${CMAKE_BINARY_DIR}/juce)
add_subdirectory(${EXTERNAL_DIR}/tracktion_engine/modules ${CMAKE_BINARY_DIR}/tracktion)

juce_add_gui_app(Mixtopia
    PRODUCT_NAME "Mixtopia"
    COMPANY_NAME "Mixtopia"
    BUNDLE_ID "org.mixtopia.Mixtopia"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(Mixtopia)

target_sources(Mixtopia PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/TimelineComponent.cpp
    Source/TempoStrip.cpp
    Source/Mpg123AudioFormat.cpp
    Source/DecodedAudioCache.cpp
    Source/Project.cpp
    Source/Exporter.cpp
    Source/MasterLimiter.cpp
    Source/ClipMixer.cpp
    Source/MasterPanel.cpp
    Source/LoudnessOverview.cpp
    Source/BeatGrid.cpp
    Source/SongAnalysis.cpp
    Source/KeyDetection.cpp
    Source/Library.cpp
    Source/MixOverview.cpp
    Source/PreviewStrip.cpp
    Source/LibraryPanel.cpp
    Source/SongTags.cpp
    Source/Loudness.cpp
    Source/Loops.cpp
    Source/Chops.cpp
    Source/LoopPanel.cpp
    Source/TestHooks.cpp
    Source/SelfTests.cpp)

target_compile_features(Mixtopia PRIVATE cxx_std_20)

# Release builds are stripped of symbols and debug information.
target_link_options(Mixtopia PRIVATE $<$<CONFIG:Release>:-s>)

set_target_properties(Mixtopia PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)

# So Tracktion can find <rubberband/single/RubberBandSingle.cpp>
target_include_directories(Mixtopia PRIVATE ${EXTERNAL_DIR})

target_compile_definitions(Mixtopia PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_MP3AUDIOFORMAT=0     # MP3 is decoded with libmpg123 (Source/Mpg123AudioFormat)
    JUCE_USE_FLAC=1
    JUCE_PLUGINHOST_VST3=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_JACK=1
    JUCE_ALSA=1
    TRACKTION_ENABLE_TIMESTRETCH_RUBBERBAND=1
    TRACKTION_BUILD_RUBBERBAND=1
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    TRACKTION_ENABLE_LIBLAME=1)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)

# Key detection: libKeyFinder (GPLv3), compiled in from the pinned source (scripts/fetch-deps.sh).
add_library(keyfinder STATIC
    ${EXTERNAL_DIR}/libkeyfinder/src/audiodata.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/chromagram.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/chromatransform.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/chromatransformfactory.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/fftadapter.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/keyclassifier.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/keyfinder.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/lowpassfilter.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/lowpassfilterfactory.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/spectrumanalyser.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/temporalwindowfactory.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/toneprofiles.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/windowfunctions.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/workspace.cpp
    ${EXTERNAL_DIR}/libkeyfinder/src/constants.cpp)
target_include_directories(keyfinder PUBLIC ${EXTERNAL_DIR}/libkeyfinder/src)
target_compile_features(keyfinder PRIVATE cxx_std_17)
target_link_libraries(keyfinder PUBLIC PkgConfig::FFTW3)
set_target_properties(keyfinder PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The app's font (Inter, fetched by scripts/fetch-deps.sh), compiled in.
juce_add_binary_data(MixtopiaFonts SOURCES
    external/inter/Inter-Regular.ttf
    external/inter/Inter-Bold.ttf)

target_link_libraries(Mixtopia PRIVATE
    MixtopiaFonts
    PkgConfig::MPG123
    keyfinder
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    atomic)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(Mixtopia PRIVATE -Wno-unknown-pragmas)
endif()
