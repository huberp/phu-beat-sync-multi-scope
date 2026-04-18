# Beat-sync multi-scope support library

add_library(BeatSyncLib INTERFACE)

target_include_directories(BeatSyncLib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(BeatSyncLib INTERFACE
    # Audio components (local only)
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/RawSampleBuffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/BucketSet.h
)

target_link_libraries(BeatSyncLib INTERFACE
    PhuAudioLib
    PhuNetworkLib
    PhuGLUtils
)
