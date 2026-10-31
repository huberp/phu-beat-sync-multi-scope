# Reusable dependency fetching for phu plugins.
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#   include(PhuDependencies)
#   phu_fetch_juce(TAG 8.0.15)
#   phu_fetch_audio_lib(TAG main)
#
# Options (all optional):
#   TAG         Branch or tag to fetch (default: see function)
#   REPOSITORY  Override git URL
#   SOURCE_DIR  Override checkout dir (default: <project>/<name>)

include_guard(GLOBAL)
include(FetchContent)

macro(_phu_fetch_dependency NAME DEFAULT_REPO DEFAULT_TAG EXCLUDE)
    unset(ARG_TAG)
    unset(ARG_REPOSITORY)
    unset(ARG_SOURCE_DIR)
    cmake_parse_arguments(ARG "" "TAG;REPOSITORY;SOURCE_DIR" "" ${ARGN})
    # Macro (not function) so variables from the fetched projects stay visible to the caller
    if(NOT ARG_TAG)
        set(ARG_TAG "${DEFAULT_TAG}")
    endif()
    if(NOT ARG_REPOSITORY)
        set(ARG_REPOSITORY "${DEFAULT_REPO}")
    endif()
    if(NOT ARG_SOURCE_DIR)
        set(ARG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}")
    endif()

    set(_extra "")
    if(${EXCLUDE})
        set(_extra EXCLUDE_FROM_ALL)
    endif()

    FetchContent_Declare(
        ${NAME}
        GIT_REPOSITORY ${ARG_REPOSITORY}
        GIT_TAG        ${ARG_TAG}
        GIT_SHALLOW    ON
        SOURCE_DIR     ${ARG_SOURCE_DIR}
        ${_extra}
    )
    FetchContent_MakeAvailable(${NAME})
endmacro()

macro(phu_fetch_juce)
    _phu_fetch_dependency(JUCE "https://github.com/juce-framework/JUCE.git" "8.0.15" FALSE ${ARGN})
endmacro()

macro(phu_fetch_audio_lib)
    _phu_fetch_dependency(phu-audio-lib "https://github.com/huberp/phu-audio-lib.git" "main" TRUE ${ARGN})
endmacro()
