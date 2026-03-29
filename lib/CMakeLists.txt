# Beat-sync multi-scope support library
add_library(BeatSyncLib INTERFACE)

target_include_directories(BeatSyncLib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(BeatSyncLib INTERFACE
    # Audio components
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/AudioSampleFifo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/AudioSampleRingBuffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/BeatSyncBuffer.h

    # Event system components
    ${CMAKE_CURRENT_SOURCE_DIR}/events/Event.h
    ${CMAKE_CURRENT_SOURCE_DIR}/events/EventSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/events/SyncGlobals.h
    ${CMAKE_CURRENT_SOURCE_DIR}/events/SyncGlobalsListener.h

    # Network components
    ${CMAKE_CURRENT_SOURCE_DIR}/network/MulticastBroadcasterBase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/SampleBroadcaster.h

    # Debug components (debug builds only)
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/EditorLogger.h
)
