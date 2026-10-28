cmake_minimum_required(VERSION 3.22)
project(LoudnessMeter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    set(LOUDNESS_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE")
    add_subdirectory(third_party/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
    set(LOUDNESS_JUCE_SOURCE_DIR "${juce_SOURCE_DIR}")
endif()
message(STATUS "Using JUCE source: ${LOUDNESS_JUCE_SOURCE_DIR}")

include(CTest)

function(loudness_enable_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(loudness_core STATIC Source/LoudnessCore.cpp Source/PeakAnalyzer.cpp)
target_include_directories(loudness_core PUBLIC Source)
loudness_enable_strict_warnings(loudness_core)

juce_add_plugin(LoudnessMeter
    PRODUCT_NAME "Loudness Meter"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.loudnessmeter"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Ldmt
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})
juce_add_binary_data(LoudnessMeterAssets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)
target_sources(LoudnessMeter PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(LoudnessMeter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(LoudnessMeter PRIVATE
    loudness_core
    LoudnessMeterAssets
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    juce::juce_recommended_lto_flags)

# Keep the readable notices in the actual plug-in bundles. BinaryData is intentionally limited to
# the two runtime fonts: a notice that is only linked data may be discarded by LTO and is not an
# accessible licence copy. LINK_DEPENDS makes edits to any notice rebuild and reseal the bundle.
set(LOUDNESS_COMMON_NOTICE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/fonts/Inter-LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/fonts/JetBrainsMono-LICENSE.txt"
    "${LOUDNESS_JUCE_SOURCE_DIR}/LICENSE.md")
set(LOUDNESS_VST3_NOTICE_SOURCES
    "${LOUDNESS_JUCE_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt"
    "${LOUDNESS_JUCE_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK/VST3_License_Agreement.pdf")
set(LOUDNESS_AU_NOTICE_SOURCE
    "${LOUDNESS_JUCE_SOURCE_DIR}/modules/juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt")

function(loudness_add_bundle_notices target include_audio_unit_sdk)
    if(APPLE)
        set(notice_directory "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources/Notices")
    else()
        # JUCE's Windows and Linux VST3 binary lives in Contents/<architecture>-<platform>.
        set(notice_directory "$<TARGET_FILE_DIR:${target}>/../Resources/Notices")
    endif()

    set(notice_sources ${LOUDNESS_COMMON_NOTICE_SOURCES})
    if(include_audio_unit_sdk)
        list(APPEND notice_sources "${LOUDNESS_AU_NOTICE_SOURCE}")
    else()
        list(APPEND notice_sources ${LOUDNESS_VST3_NOTICE_SOURCES})
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${notice_sources}")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${notice_directory}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${notice_directory}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${notice_directory}/THIRD_PARTY_NOTICES.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/fonts/Inter-LICENSE.txt"
            "${notice_directory}/Inter-OFL.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/fonts/JetBrainsMono-LICENSE.txt"
            "${notice_directory}/JetBrainsMono-OFL.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${LOUDNESS_JUCE_SOURCE_DIR}/LICENSE.md"
            "${notice_directory}/JUCE-LICENSE.md"
        VERBATIM)

    if(include_audio_unit_sdk)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LOUDNESS_AU_NOTICE_SOURCE}"
                "${notice_directory}/AudioUnitSDK-LICENSE.txt"
            VERBATIM)
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LOUDNESS_JUCE_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt"
                "${notice_directory}/VST3-SDK-LICENSE.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LOUDNESS_JUCE_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK/VST3_License_Agreement.pdf"
                "${notice_directory}/VST3-License-Agreement.pdf"
            VERBATIM)
    endif()

    if(APPLE)
        # JUCE's helper may seal the VST3 before later post-build resources are copied, and the
        # linker signs only the Mach-O. Seal each completed local bundle after every notice copy.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

if(TARGET LoudnessMeter_AU)
    loudness_add_bundle_notices(LoudnessMeter_AU TRUE)
endif()
if(TARGET LoudnessMeter_VST3)
    loudness_add_bundle_notices(LoudnessMeter_VST3 FALSE)
endif()
if(BUILD_TESTING)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    find_package(Threads REQUIRED)
    add_executable(loudness_reference_tests tests/reference_tests.cpp)
    add_executable(loudness_core_tests tests/core_tests.cpp)
    add_executable(loudness_peak_tests tests/peak_tests.cpp)
    juce_add_console_app(loudness_processor_tests PRODUCT_NAME "Loudness Meter Processor Tests")
    target_sources(loudness_processor_tests PRIVATE
        tests/processor_tests.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(loudness_processor_tests PRIVATE Source tests)
    target_compile_definitions(loudness_processor_tests PRIVATE
        JucePlugin_Name="Loudness Meter"
        LOUDNESS_METER_TESTING=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(loudness_core_tests PRIVATE loudness_core Threads::Threads)
    target_link_libraries(loudness_peak_tests PRIVATE loudness_core)
    target_link_libraries(loudness_processor_tests PRIVATE
        loudness_core
        LoudnessMeterAssets
        juce::juce_audio_processors
        juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    juce_add_console_app(loudness_snapshot PRODUCT_NAME "Loudness Meter Snapshot")
    target_sources(loudness_snapshot PRIVATE
        tests/snapshot.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(loudness_snapshot PRIVATE Source)
    target_compile_definitions(loudness_snapshot PRIVATE
        JucePlugin_Name="Loudness Meter"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(loudness_snapshot PRIVATE
        loudness_core
        LoudnessMeterAssets
        juce::juce_audio_processors
        juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    foreach(test_target IN ITEMS
            loudness_reference_tests
            loudness_core_tests
            loudness_peak_tests
            loudness_processor_tests
            loudness_snapshot)
        loudness_enable_strict_warnings(${test_target})
    endforeach()
    add_test(NAME production_core COMMAND loudness_core_tests)
    add_test(NAME production_peaks COMMAND loudness_peak_tests)
    add_test(NAME production_processor COMMAND loudness_processor_tests)
    add_test(NAME reference_cpp COMMAND loudness_reference_tests)
    add_test(NAME reference_python
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_tests.py")
    add_test(NAME reference_cross_check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cross_check.py"
                "$<TARGET_FILE:loudness_reference_tests>")
    set_tests_properties(reference_cpp reference_python reference_cross_check
        PROPERTIES LABELS "reference-only;not-product-conformance")
endif()
