# =============================================================================
# Compiler Flags Configuration
# =============================================================================

# --- Warning Flags (optional, disabled by default for compatibility) ---
option(ENABLE_STRICT_WARNINGS "Enable strict compiler warnings" OFF)
if(ENABLE_STRICT_WARNINGS)
    if(MSVC)
        # Microsoft Visual C++
        add_compile_options(/W4)  # High warning level
        add_compile_options(/wd4100)  # Disable "unreferenced parameter" warning (common in JUCE)
        add_compile_options(/wd4127)  # Disable "conditional expression is constant" warning
        add_compile_options(/wd4244)  # Disable "conversion from 'type1' to 'type2', possible loss of data"
        add_compile_options(/wd4305)  # Disable "truncation from 'type1' to 'type2'"
        add_compile_options(/wd4996)  # Disable "deprecated function" warnings
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # GCC and Clang
        add_compile_options(-Wall -Wextra -Wpedantic)
        add_compile_options(-Wno-unused-parameter)  # Disable "unreferenced parameter" warning (common in JUCE)
        add_compile_options(-Wno-deprecated-declarations)  # Disable deprecated warnings
    endif()
endif()

# --- Sanitizers (optional) ---
option(ENABLE_SANITIZERS "Enable Address/Undefined Behavior Sanitizers" OFF)
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    elseif(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    endif()
endif()

# --- Coverage (optional) ---
option(ENABLE_COVERAGE "Enable coverage flags" OFF)
if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage)
        add_link_options(--coverage)
    endif()
endif()
