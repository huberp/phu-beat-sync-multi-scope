#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <atomic>
#include <cstdint>
#include "WaveformGLRenderer.h"
#include "GridPlayheadGLRenderer.h"
#include "RmsOverlayRenderer.h"
#include "CancellationOverlayRenderer.h"

/**
 * ScopeGLCoordinator — thin OpenGL coordinator for the ScopeDisplay.
 *
 * Owns the juce::OpenGLContext and four self-contained sub-renderers:
 *   - WaveformGLRenderer          — oscilloscope waveform lines
 *   - GridPlayheadGLRenderer      — amplitude grid + beat-position playhead
 *   - RmsOverlayRenderer          — RMS amplitude bar overlay
 *   - CancellationOverlayRenderer — inter-instance cancellation-index bar
 *
 * As the sole juce::OpenGLRenderer registered on the context, this class
 * drives the GL lifecycle callbacks and delegates all actual drawing to the
 * sub-renderers. Each sub-renderer owns its shader, VBOs, and snapshot.
 *
 * Architecture & Code Reuse:
 *   All sub-renderers benefit from lib/gl utility classes:
 *
 *   - **GLSnapshotRenderer<T>** — Template base for double-buffered snapshots.
 *     Provides thread-safe data hand-off: UI thread calls setSnapshot(), GL thread
 *     calls swapSnapshot() to atomically acquire the latest data. Eliminates ~40 lines
 *     of boilerplate per renderer (SpinLock + pending/render fields).
 *
 *   - **GLSLShaderBuilder** — Static utility for GLSL version detection, shader
 *     assembly, and compilation with standard error handling. Centralizes the
 *     repeated pattern of version checking, fragment shader assembly for
 *     compatibility, and compile error reporting.
 *
 *   - **GLRendererBase** — Abstract base for renderer lifecycle. Provides create()
 *     and release() contract, compileShader() helper, and isReady() checks.
 *     Ensures consistent initialization order and resource cleanup.
 *
 * Thread safety:
 *   - setWaveformData / setGridPlayheadData / setRmsData / setCancellationData
 *     are called from the UI thread. Each forwards to the corresponding
 *     sub-renderer's setData() method which is internally guarded by a SpinLock
 *     (see GLSnapshotRenderer<T>::setSnapshot()).
 *   - newOpenGLContextCreated / renderOpenGL / openGLContextClosing are called
 *     from the GL thread (see GLSnapshotRenderer<T>::swapSnapshot()).
 *
 * @see lib/gl/GLSnapshotRenderer.h
 * @see lib/gl/GLSLShaderBuilder.h
 * @see lib/gl/GLRendererBase.h
 */
class ScopeGLCoordinator : public juce::OpenGLRenderer {
  public:
    /** Convenience alias so callers don't need to include WaveformGLRenderer.h. */
    using WaveformInstanceData = WaveformGLRenderer::InstanceData;

    ScopeGLCoordinator()  = default;
    ~ScopeGLCoordinator() override;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /** Attach the OpenGL context to the given component. */
    void attachTo(juce::Component& component);

    /** Detach and release all OpenGL resources (blocks until GL thread finishes). */
    void detach();

    /** Returns true once the OpenGL context has been successfully created. */
    bool isAvailable() const { return m_available.load(std::memory_order_acquire); }

    // -------------------------------------------------------------------------
    // Typed data setters (UI thread)
    // -------------------------------------------------------------------------

    /**
     * Upload waveform display data.  Also captures the current viewport
     * dimensions from the target component for use by renderOpenGL().
     */
    void setWaveformData(
        const std::array<WaveformInstanceData, WaveformGLRenderer::MAX_INSTANCES>& instances,
        float ampScale,
        bool  showLocal,
        bool  showRemote,
        bool  broadcastOnly,
        int   localSlotIndex);

    /** Upload grid and playhead parameters. */
    void setGridPlayheadData(double displayRangeBeats, double currentPpq, bool broadcastOnly);

    /** Upload RMS overlay data.
     *  @param barBoundaries  Normalised [0,1] left-edge per bar + trailing 1.0
     *                        (numBars+1 values). Nullptr falls back to even spacing.
     */
    void setRmsData(const float* values, int numBars, float ampScale, bool show,
                    const float* barBoundaries = nullptr);

    /** Upload cancellation overlay data. */
    void setCancellationData(const float* values, int numSlots, bool show);

    // -------------------------------------------------------------------------
    // juce::OpenGLRenderer (GL thread)
    // -------------------------------------------------------------------------

    void newOpenGLContextCreated() override;
    void renderOpenGL()            override;
    void openGLContextClosing()    override;

  private:
    juce::OpenGLContext m_glContext;
    std::atomic<bool>   m_available { false };
    juce::Component*    m_targetComponent = nullptr;

    WaveformGLRenderer          m_waveform;
    GridPlayheadGLRenderer      m_gridPlayhead;
    RmsOverlayRenderer          m_rms;
    CancellationOverlayRenderer m_cancellation;

    // Viewport dimensions packed as (logicalW << 32 | logicalH) for a single
    // lock-free atomic read/write across threads.
    // Written on UI thread (release ordering), read on GL thread (acquire ordering).
    std::atomic<uint64_t> m_vpSize { 0 };

    /** Capture current component size and store it atomically for the GL thread. */
    void captureViewport();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeGLCoordinator)
};
