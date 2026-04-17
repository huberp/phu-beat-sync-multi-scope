#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <vector>
#include <gl/GLRendererBase.h>
#include <gl/GLSnapshotRenderer.h>

namespace gl_detail {
struct GridPlayheadSnapshot {
  double displayRangeBeats = 4.0;
  double currentPpq        = 0.0;
  bool   broadcastOnly     = false;
};
}

/**
 * GridPlayheadGLRenderer — self-contained GPU renderer for the amplitude grid
 * and beat-position playhead.
 *
 * Uses a simple vec2 pass-through shader.  The grid VBO is rebuilt lazily
 * when displayRangeBeats changes.  The playhead VBO is updated every frame.
 *
 * Owns its own shader, VBOs, and double-buffered data snapshot.
 *
 * Thread safety:
 *   - setData() is called from the UI thread.
 *   - draw()    is called from the GL thread.
 *   - A SpinLock protects the pending → render snapshot swap.
 */
class GridPlayheadGLRenderer : public GLRendererBase,
                               protected GLSnapshotRenderer<gl_detail::GridPlayheadSnapshot> {
  public:
    GridPlayheadGLRenderer() { m_gridStaging.reserve(128); }
    ~GridPlayheadGLRenderer() { release(); }

    // -------------------------------------------------------------------------
    // Lifecycle (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Compile the utility shader and allocate grid / playhead VBOs.
     * Must be called on the GL thread inside newOpenGLContextCreated().
     * @return true on success; false if GLSL < 1.30 or shader compilation fails.
     */
    bool create(juce::OpenGLContext& ctx);

    /** Release all GL resources. Must be called on the GL thread. */
    void release();

    // -------------------------------------------------------------------------
    // Data upload (UI thread)
    // -------------------------------------------------------------------------

    /**
     * Snapshot the current grid/playhead parameters for the next GL frame.
     * Must be called from the UI thread.
     */
    void setData(double displayRangeBeats, double currentPpq, bool broadcastOnly);

    // -------------------------------------------------------------------------
    // Drawing (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Swap the latest snapshot and draw the grid and playhead.
     * Must be called from the GL thread.
     * The caller is responsible for setting glViewport before this call.
     */
    void draw();

  private:
    using Snapshot = gl_detail::GridPlayheadSnapshot;

    GLint  m_aPositionLoc = -1;  ///< attribute: vec2 aPosition
    GLint  m_uColourLoc   = -1;  ///< uniform:   vec4 uColour

    GLuint m_gridVbo     = 0;
    GLuint m_playheadVbo = 0;

    // Grid VBO rebuild state (GL thread only — no lock needed)
    int    m_gridVertexCount    = 0;
    double m_lastGridRangeBeats = -1.0;
    std::vector<float> m_gridStaging;  ///< Reusable staging buffer (avoids per-frame allocation)

    void drawGrid();
    void drawPlayhead();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GridPlayheadGLRenderer)
};
