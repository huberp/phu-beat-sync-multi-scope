# Beat-sync multi-scope support library

# Add OpenGL utility library subdirectory
add_subdirectory(gl)

add_library(BeatSyncLib INTERFACE)

target_include_directories(BeatSyncLib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(BeatSyncLib INTERFACE
    # Audio components
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/RawSampleBuffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/BucketSet.h

    # Event system components
    ${CMAKE_CURRENT_SOURCE_DIR}/events/Event.h
    ${CMAKE_CURRENT_SOURCE_DIR}/events/EventSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/events/SyncGlobals.h
    ${CMAKE_CURRENT_SOURCE_DIR}/events/SyncGlobalsListener.h

    # Network components
    ${CMAKE_CURRENT_SOURCE_DIR}/network/MulticastBroadcasterBase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/SampleBroadcaster.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/CtrlBroadcaster.h

    # Debug components (debug builds only)
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/EditorLogger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/DebugLogEventQueue.h
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/DebugLogSink.h
)

target_link_libraries(BeatSyncLib INTERFACE
    GLUtils
)
