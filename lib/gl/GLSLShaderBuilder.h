#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <optional>
#include <memory>

/**
 * GLSLShaderBuilder — Utility class for GLSL version detection, shader assembly,
 * and compilation with standard error handling.
 *
 * Eliminates boilerplate across all OpenGL renderers by centralizing:
 *   - GLSL version detection and validation
 *   - Version-aware fragment shader assembly
 *   - Standard shader compilation error reporting
 *
 * Thread safety: All methods are thread-safe and stateless; thread context
 * (GL thread vs UI thread) is determined by the caller.
 */
class GLSLShaderBuilder {
  public:
    /** Minimum GLSL version required for modern renderer features (gl_VertexID). */
    static constexpr double MINIMUM_GLSL_VERSION = 1.30;

    // -------------------------------------------------------------------------
    // GLSL Version Detection
    // -------------------------------------------------------------------------

    /**
     * Get the GLSL version supported by the current OpenGL context.
     * @return GLSL version as a double (e.g., 1.30, 3.30), or std::nullopt
     *         if querying fails.
     */
    static std::optional<double> getGLSLVersion();

    /**
     * Check if the current OpenGL context supports the required GLSL version.
     * @param requiredVersion Minimum GLSL version; defaults to MINIMUM_GLSL_VERSION.
     * @return true if version >= requiredVersion.
     */
    static bool isGLSLVersionSupported(double requiredVersion = MINIMUM_GLSL_VERSION);

    /**
     * Get the GLSL version directive string for shader compilation.
     * Automatically selected based on context GLSL version.
     * @return "#version XXX\n" string, or empty if version < 1.30.
     *
     * Selection logic:
     *   - >= 3.30: "#version 330\n"   (modern core profile, out vec4 fragColor)
     *   - >= 1.50: "#version 150\n"   (compatibility profile)
     *   - >= 1.30: "#version 130\n"   (legacy, gl_FragColor)
     */
    static juce::String getVersionDirective();

    /**
     * Check if the current context uses modern fragment shader output
     * (GLSL >= 3.30 with "out vec4 fragColor" vs legacy "gl_FragColor").
     * @return true if GLSL >= 3.30.
     */
    static bool useModernFragmentOutput();

    // -------------------------------------------------------------------------
    // Fragment Shader Assembly
    // -------------------------------------------------------------------------

    /**
     * Build a complete fragment shader with version-aware output syntax.
     *
     * Automatically handles GLSL version differences for output variable:
     *   - GLSL >= 3.30: "out vec4 fragColor" + "fragColor = ..."
     *   - GLSL < 3.30:  "gl_FragColor = ..."
     *
     * @param uniformDeclarations  Uniform declarations (e.g., "uniform vec4 uColour;\n")
     * @param mainBody             Main function body between the braces
     *                             (without leading/trailing newlines).
     *                             E.g., "    fragColor = uColour;\n" (already indented)
     *
     * @return Complete fragment shader source with version directive.
     *
     * Example:
     *   @code
     *   auto frag = GLSLShaderBuilder::buildFragmentShader(
     *       "uniform vec4 uColour;\n",
     *       "    fragColor = uColour;\n");
     *   @endcode
     */
    static juce::String buildFragmentShader(
        const juce::String& uniformDeclarations,
        const juce::String& mainBody);

    // -------------------------------------------------------------------------
    // Shader Compilation & Linking
    // -------------------------------------------------------------------------

    /**
     * Compile and link a complete shader program with standard error reporting.
     *
     * On compilation/linking failure, logs a detailed error message and returns nullptr.
     * Error messages are formatted as:
     *   "ClassName: [vertex|fragment|link] shader failed: [GL error message]"
     *
     * @param ctx       OpenGL context (must be valid on calling thread).
     * @param vertSrc   Vertex shader source code (complete with version directive).
     * @param fragSrc   Fragment shader source code (complete with version directive).
     * @param debugName Class/component name for error messages (e.g., "WaveformGLRenderer").
     *
     * @return std::unique_ptr<juce::OpenGLShaderProgram> on success; nullptr on failure.
     *         The returned pointer is never null if the function returns non-nullptr.
     */
    static std::unique_ptr<juce::OpenGLShaderProgram> compileShaderProgram(
        juce::OpenGLContext& ctx,
        const juce::String& vertSrc,
        const juce::String& fragSrc,
        const juce::String& debugName);

    /**
     * Check GLSL version and log a warning if below the minimum.
     * Convenience helper for early validation in renderer create() methods.
     *
     * @param debugName   Renderer class name for debug output.
     * @param minVersion  Minimum required version; defaults to MINIMUM_GLSL_VERSION.
     * @return true if version >= minVersion.
     */
    static bool validateGLSLVersion(const juce::String& debugName,
                                    double minVersion = MINIMUM_GLSL_VERSION);

  private:
    GLSLShaderBuilder() = delete;  // Static utility class only
};
