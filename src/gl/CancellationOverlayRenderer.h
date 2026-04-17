#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

/**
 * CancellationOverlayRenderer — OpenGL renderer for the inter-instance
 * cancellation-index bar overlay.
 *
 * Draws a 6 px tall colour-coded bar strip anchored to the bottom of the
 * viewport.  Each slot colour interpolates:
 *   ci in [0.0, 0.4] : green  (0x00BB55) -> yellow (0xFFCC00)
 *   ci in [0.4, 1.0] : yellow (0xFFCC00) -> red    (0xFF3300)
 *
 * No VBO — geometry from gl_VertexID; ci values as a uniform float array.
 * Each slot = 2 triangles = 6 vertices; alpha = 0.85.
 *
 * Thread safety:
 *   - setData()  is called from the UI thread.
 *   - draw()     is called from the GL thread.
 *   - A SpinLock protects the pending -> render snapshot swap.
 */
class CancellationOverlayRenderer {
  public:
    /** Maximum number of cancellation slots (matches ScopeDisplay::MAX_CANCEL_SLOTS). */
    static constexpr int MAX_CANCEL_SLOTS = 256;

    CancellationOverlayRenderer()  = default;
    ~CancellationOverlayRenderer() { release(); }

    // -------------------------------------------------------------------------
    // GL-thread lifecycle
    // -------------------------------------------------------------------------

    /**
     * Compile the cancellation shader and cache uniform locations.
     * Must be called on the GL thread inside newOpenGLContextCreated().
     * @return true on success; false if GLSL < 1.30 or shader compilation fails.
     */
    bool create(juce::OpenGLContext& ctx);

    /** Release all GL resources. Must be called on the GL thread. */
    void release();

    /** Returns true once create() has succeeded and release() has not been called. */
    bool isReady() const { return m_shader != nullptr; }

    // -------------------------------------------------------------------------
    // Data upload (UI thread)
    // -------------------------------------------------------------------------

    /**
     * Snapshot the current cancellation data for the next GL frame.
     * Must be called from the UI thread.
     *
     * @param values    Cancellation index per slot, range [0, 1].
     * @param numSlots  Number of valid entries in values[]; clamped to MAX_CANCEL_SLOTS.
     * @param show      Whether the overlay should be visible.
     */
    void setData(const float* values, int numSlots, bool show);

    // -------------------------------------------------------------------------
    // Drawing (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Swap the latest snapshot and draw the cancellation bar overlay.
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

    GLint m_uCancelValuesLoc = -1;  // float uCancelValues[256]
    GLint m_uNumSlotsLoc     = -1;  // int   uNumSlots
    GLint m_uBarTopNDCLoc    = -1;  // float uBarTopNDC

    struct Snapshot {
        float cancelValues[MAX_CANCEL_SLOTS] {};
        int   numSlots = 0;
        bool  show     = false;
    };

    juce::SpinLock m_lock;
    Snapshot       m_pending;
    Snapshot       m_render;
    bool           m_newData = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CancellationOverlayRenderer)
};
