#pragma once

#include <cstring>

namespace phu {

/**
 * @brief Utility functions for safe string operations.
 *        Avoids C4996 warnings from MSVC when using strncpy.
 */
class StringUtil {
public:
    /**
     * @brief Portable, safe string copy that always null-terminates.
     *        Prevents buffer overflows and ensures null termination.
     *
     * @param dest       Destination buffer
     * @param src        Source string to copy
     * @param dest_size  Size of destination buffer (in bytes)
     */
    static inline void safe_strncpy(char *dest, const char *src, size_t dest_size) {
        if (dest_size == 0) return; // No space to copy

        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0'; // Explicit null termination
    }
};

} // namespace phu
