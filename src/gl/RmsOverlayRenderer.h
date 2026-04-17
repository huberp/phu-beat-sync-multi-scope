#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

/**
 * RmsOverlayRenderer — OpenGL renderer for the beat-synced RMS bar overlay.
 *
 * Draws the RMS amplitude as a 3-layer horizontal bar per beat-slot:
 *   Layer 1 (outermost): 5px tall, dark blue   (0x1A44AAFF)
 *   Layer 2            : 3px tall, medium blue  (0x5566CCFF)
 *   Layer 3 (innermost): 2px tall, purple/pink  (0xEEAAEEFF)
 *
 * Implementation:
 *   - No VBO — bar geometry is computed entirely from gl_VertexID.
 *   - RMS values are passed as a uniform float array (max 128 elements, 512 B).
 *   - Each bar is 2 triangles = 6 vertices; total draw = numBars * 6 vertices.
 *   - 3 glDrawArrays calls per frame (one per visual layer).
 *
 * Lifecycle:
 *   1. Call create() inside newOpenGLContextCreated() on the GL thread.
 *   2. Call draw() inside renderOpenGL() on the GL thread.
 *   3. Call release() inside openGLContextClosing() on the GL thread.
 *   4. isReady() returns true only between a successful create() and release().
 */
class RmsOverlayRenderer {
  public:
    /** Maximum number of RMS bars (matches ScopeDisplay::MAX_METRIC_SLOTS). */
    static constexpr int MAX_RMS_BARS = 128;

    RmsOverlayRenderer()  = default;
    ~RmsOverlayRenderer() { release(); }

    // -------------------------------------------------------------------------
    // GL-thread lifecycle
    // -------------------------------------------------------------------------

    /**
     * Compile the RMS shader and cache uniform locations.
     * Must be called on the GL thread inside newOpenGLContextCreated().
     * @return true on success; false if GLSL < 1.30 or shader fails to compile.
     */
    bool create(juce::OpenGLContext& ctx);

    /** Release all GL resources. Must be called on the GL thread. */
    void release();

    /** Returns true once create() has succeeded and release() has not yet been called. */
    bool isReady() const { return m_shader != nullptr; }

    // -------------------------------------------------------------------------
    // Data upload (UI thread)
    // -------------------------------------------------------------------------

    /**
     * Snapshot the current RMS data for the next GL frame.
     * Must be called from the UI thread.
     *
     * @param values    Linear-amplitude RMS per bar slot, range [0, 1+].
     * @param numBars   Number of valid entries in values[]; clamped to MAX_RMS_BARS.
     * @param ampScale  Same amplitude scale applied to waveforms.
     * @param show      Whether the overlay should be visible.
     */
    void setData(const float* values, int numBars, float ampScale, bool show);

    // -------------------------------------------------------------------------
    // Drawing (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Swap the latest snapshot and draw the 3-layer RMS bar overlay.
     *
     * Blending must already be enabled by the caller (GL_SRC_ALPHA /
     * GL_ONE_MINUS_SRC_ALPHA) before this call.
     *
     * @param vpHeightPx  Physical viewport height in pixels (after renderingScale).
     */
    void draw(int vpHeightPx);

  private:
    juce::OpenGLContext* m_ctx = nullptr;

    std::unique_ptr<juce::OpenGLShaderProgram> m_shader;

    // Cached uniform locations
    GLint m_uRmsValuesLoc = -1;   // uniform float uRmsValues[MAX_RMS_BARS]
    GLint m_uNumBarsLoc   = -1;   // uniform int   uNumBars
    GLint m_uAmpScaleLoc  = -1;   // uniform float uAmpScale
    GLint m_uHalfHLoc     = -1;   // uniform float uHalfH   (NDC half-height for current layer)
    GLint m_uColourLoc    = -1;   // uniform vec4  uColour

    // Double-buffered snapshot (UI thread writes pending, GL thread reads render)
    struct Snapshot {
        float rmsValues[MAX_RMS_BARS] {};
        int   numBars  = 0;
        float ampScale = 1.0f;
        bool  show     = false;
    };

    juce::SpinLock m_lock;
    Snapshot       m_pending;
    Snapshot       m_render;
    bool           m_newData = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RmsOverlayRenderer)
};
