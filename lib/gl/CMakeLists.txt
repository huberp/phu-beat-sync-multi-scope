# OpenGL utility library
add_library(GLUtils STATIC
    GLSLShaderBuilder.cpp
    GLSLShaderBuilder.h
    GLSnapshotRenderer.h
    GLRendererBase.h
)

target_include_directories(GLUtils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(GLUtils PUBLIC
    juce::juce_opengl
    juce::juce_gui_basics
)

# Enable C++17 features
target_compile_features(GLUtils PRIVATE cxx_std_17)

# Suppress warnings for JUCE headers (if needed on your platform)
if(MSVC)
    target_compile_options(GLUtils PRIVATE /W4)
else()
    target_compile_options(GLUtils PRIVATE -Wall -Wextra)
endif()
