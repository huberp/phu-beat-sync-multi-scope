#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <memory>
#include "GLSLShaderBuilder.h"

/**
 * GLRendererBase — Abstract base class for OpenGL renderers.
 *
 * Provides common lifecycle management and shader compilation utilities
 * for all OpenGL renderers (WaveformGLRenderer, GridPlayheadGLRenderer, etc.).
 *
 * Lifecycle:
 *   1. Override create(ctx) to compile shaders and allocate GPU resources.
 *   2. Call release() or let destructor clean up (safe to call multiple times).
 *   3. Check isReady() before drawing.
 *
 * Shader Compilation:
 *   - Use compileShader() to leverage GLSLShaderBuilder for standard error handling
 *   - Automatically checks GLSL version compatibility
 *   - Provides consistent debug output format
 *
 * Thread safety:
 *   - create() must be called on the GL thread (inside newOpenGLContextCreated())
 *   - release() must be called on the GL thread (inside openGLContextClosing())
 *   - isReady() is thread-safe (atomic read of m_shader pointer)
 *
 * Usage:
 * @code
 * class MyRenderer : public GLRendererBase {
 *     bool create(juce::OpenGLContext& ctx) override {
 *         if (!GLSLShaderBuilder::validateGLSLVersion("MyRenderer"))
 *             return false;
 *
 *         juce::String vert = GLSLShaderBuilder::getVersionDirective()
 *             + "in float value; void main() { ... }";
 *         juce::String frag = GLSLShaderBuilder::buildFragmentShader(
 *             "uniform vec4 uColour;\n",
 *             "    fragColor = uColour;\n");
 *
 *         return compileShader(vert, frag, "MyRenderer");
 *     }
 *
 *     void draw() {
 *         if (!isReady()) return;
 *         m_shader->use();
 *         // ... render ...
 *     }
 * };
 * @endcode
 */
class GLRendererBase {
  public:
    GLRendererBase() = default;
    virtual ~GLRendererBase() { release(); }

    GLRendererBase(const GLRendererBase&) = delete;
    GLRendererBase& operator=(const GLRendererBase&) = delete;

    // -------------------------------------------------------------------------
    // Lifecycle (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Compile shaders and allocate GPU resources.
     * Must be called on the GL thread inside newOpenGLContextCreated().
     *
     * @param ctx OpenGL context for this renderer (must be valid on GL thread).
     * @return true on success; false if GLSL version is unsupported or
     *         shader compilation fails.
     */
    virtual bool create(juce::OpenGLContext& ctx) = 0;

    /**
     * Release all OpenGL resources.
     * Must be called on the GL thread inside openGLContextClosing().
     * Safe to call multiple times; subsequent calls have no effect.
     *
     * Default implementation releases m_shader and clears m_ctx.
     * Override to release additional resources (VBOs, textures, etc.),
     * but call GLRendererBase::release() in your implementation.
     */
    virtual void release() {
        m_shader.reset();
        m_ctx = nullptr;
    }

    /**
     * Check if this renderer is ready to draw.
     * Returns true only between a successful create() and release().
     *
     * Thread safety: Safe to call from any thread (atomic check of m_shader).
     *
     * @return true if m_shader is valid and ready for use.
     */
    bool isReady() const { return m_shader != nullptr; }

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------

    /**
     * Get the associated OpenGL context.
     * @return Pointer to the OpenGL context, or nullptr if not created.
     */
    juce::OpenGLContext* getContext() { return m_ctx; }
    const juce::OpenGLContext* getContext() const { return m_ctx; }

    /**
     * Get the compiled shader program.
     * @return Pointer to the shader, or nullptr if not created or after release().
     */
    juce::OpenGLShaderProgram* getShader() { return m_shader.get(); }
    const juce::OpenGLShaderProgram* getShader() const { return m_shader.get(); }

  protected:
    juce::OpenGLContext* m_ctx = nullptr;
    std::unique_ptr<juce::OpenGLShaderProgram> m_shader;

    /**
     * Compile and link a shader program using GLSLShaderBuilder.
     *
     * Convenience method to avoid repeating shader compilation boilerplate.
     * Automatically:
     *   - Verifies GLSL version compatibility
     *   - Compiles vertex and fragment shaders
     *   - Reports standard error messages on failure
     *   - Stores the result in m_shader
     *
     * @param vertSrc   Vertex shader source (must include version directive).
     * @param fragSrc   Fragment shader source (must include version directive).
     * @param debugName Class name for error messages.
     *
     * @return true if compilation succeeded; false on error.
     *         On failure, m_shader is nullptr and errors are logged to DBG().
     *
     * Example:
     *   @code
     *   bool MyRenderer::create(juce::OpenGLContext& ctx) {
     *       m_ctx = &ctx;
     *       if (!GLSLShaderBuilder::validateGLSLVersion("MyRenderer"))
     *           return false;
     *
     *       const auto vert = GLSLShaderBuilder::getVersionDirective()
     *           + "in float yValue;\n void main() { ... }";
     *       const auto frag = GLSLShaderBuilder::buildFragmentShader(
     *           "uniform vec4 uColour;\n",
     *           "    fragColor = uColour;\n");
     *
     *       return compileShader(vert, frag, "MyRenderer");
     *   }
     *   @endcode
     */
    bool compileShader(const juce::String& vertSrc,
                       const juce::String& fragSrc,
                       const juce::String& debugName) {
        if (m_ctx == nullptr) return false;

        auto compiled = GLSLShaderBuilder::compileShaderProgram(
            *m_ctx, vertSrc, fragSrc, debugName);

        if (compiled != nullptr) {
            m_shader = std::move(compiled);
            return true;
        }
        return false;
    }
};
