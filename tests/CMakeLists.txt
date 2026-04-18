add_executable(test_audio_buffers test_audio_buffers.cpp)

target_include_directories(test_audio_buffers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../phu-audio-lib
)

target_compile_features(test_audio_buffers PRIVATE cxx_std_17)

enable_testing()
add_test(NAME audio_buffer_tests COMMAND test_audio_buffers)
