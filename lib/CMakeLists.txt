# Beat-sync multi-scope support library

add_library(BeatSyncLib INTERFACE)

target_link_libraries(BeatSyncLib INTERFACE
    PhuAudioLib
    PhuNetworkLib
    PhuGLUtils
)
