#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <array>
#include <vector>

/**
 * OpenGLScopeRenderer — GPU-accelerated waveform renderer for ScopeDisplay.
 *
 * Uses two shader programs:
 *
 *   1. **Waveform shader** — The vertex shader takes a single float Y value per
 *      vertex and computes X from `gl_VertexID`.  This allows the existing
 *      displayBins array (Y-only floats) to be uploaded directly to the VBO
 *      without any CPU-side transformation or staging buffer.
 *
 *   2. **Utility shader** — A simple vec2 pass-through used for grid lines and
 *      the playhead, which require explicit (x, y) coordinates.
 *
 * Lifecycle:
 *   1. Construct and call attachTo(component) to attempt OpenGL initialisation.
 *   2. After newOpenGLContextCreated() fires, isAvailable() returns true.
 *   3. If the context fails to create (no GPU / driver issue), isAvailable()
 *      remains false and the caller should fall back to software rendering.
 *   4. Call detach() or destroy to release OpenGL resources.
 *
 * Thread safety:
 *   - setFrameData() copies display data from the UI thread.
 *   - renderOpenGL() runs on the GL thread and reads the latest snapshot.
 *   - A SpinLock protects the data hand-off.
 */
class OpenGLScopeRenderer : public juce::OpenGLRenderer {
  public:
    static constexpr int MAX_INSTANCES = 8;
    static constexpr int DISPLAY_BINS  = 4096;

    OpenGLScopeRenderer();
    ~OpenGLScopeRenderer() override;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /** Attempt to attach an OpenGL context to the given component. */
    void attachTo(juce::Component& component);

    /** Detach and release all OpenGL resources. */
    void detach();

    /** Returns true once the OpenGL context has been successfully created. */
    bool isAvailable() const { return m_available.load(std::memory_order_acquire); }

    // -------------------------------------------------------------------------
    // Data upload (called from UI thread before repaint)
    // -------------------------------------------------------------------------

    struct WaveformInstance {
        bool            active = false;
        bool            isLocal = false;
        juce::Colour    colour { juce::Colours::green };
        float           alpha  = 1.0f;
        const float*    bins   = nullptr;  // pointer to displayBins (DISPLAY_BINS floats, Y-only)
    };

    /**
     * Snapshot the current frame data for the GL renderer.
     * Must be called from the UI thread.
     */
    void setFrameData(const std::array<WaveformInstance, MAX_INSTANCES>& instances,
                      float amplitudeScale,
                      double displayRangeBeats,
                      double currentPpq,
                      bool showLocal,
                      bool showRemote,
                      bool broadcastOnlyOverlay,
                      int localSlotIndex);

    // -------------------------------------------------------------------------
    // OpenGLRenderer overrides (called on GL thread)
    // -------------------------------------------------------------------------

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

  private:
    juce::OpenGLContext m_glContext;
    std::atomic<bool>   m_available { false };

    // --- Waveform shader (Y-only input, X from gl_VertexID) ---
    std::unique_ptr<juce::OpenGLShaderProgram> m_waveShader;
    GLint m_wave_aYValueLoc  = -1;   // attribute: float yValue
    GLint m_wave_uColourLoc  = -1;   // uniform:   vec4  uColour
    GLint m_wave_uNumBinsLoc = -1;   // uniform:   int   uNumBins
    GLint m_wave_uAmpScaleLoc = -1;  // uniform:   float uAmpScale

    // --- Utility shader (vec2 position pass-through for grid/playhead) ---
    std::unique_ptr<juce::OpenGLShaderProgram> m_utilShader;
    GLint m_util_aPositionLoc = -1;  // attribute: vec2 aPosition
    GLint m_util_uColourLoc   = -1;  // uniform:   vec4 uColour

    // Per-instance VBOs (each holds DISPLAY_BINS floats — raw Y values)
    GLuint m_vbos[MAX_INSTANCES] {};
    bool   m_vbosCreated = false;

    // Grid VBO (interleaved x, y pairs)
    GLuint m_gridVbo = 0;
    int    m_gridVertexCount = 0;
    double m_lastGridRangeBeats = -1.0;

    // Playhead VBO (2 vertices = 4 floats)
    GLuint m_playheadVbo = 0;

    // Pre-allocated staging buffer for grid (avoid per-frame heap allocation)
    std::vector<float> m_gridStaging;

    // Frame data snapshot (protected by spin lock for UI→GL thread handoff)
    struct FrameSnapshot {
        struct InstanceData {
            bool         active = false;
            bool         isLocal = false;
            float        r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            float        bins[DISPLAY_BINS] {};
        };
        std::array<InstanceData, MAX_INSTANCES> instances;
        float  amplitudeScale    = 1.0f;
        double displayRangeBeats = 4.0;
        double currentPpq        = 0.0;
        bool   showLocal         = true;
        bool   showRemote        = true;
        bool   broadcastOnly     = false;
        int    localSlotIndex    = 0;
        int    viewportWidth     = 0;
        int    viewportHeight    = 0;
    };

    juce::SpinLock m_dataLock;
    FrameSnapshot  m_pendingData;
    FrameSnapshot  m_renderData;
    bool           m_newDataAvailable = false;

    // Component pointer (for attachTo/detach lifecycle only — not accessed on GL thread)
    juce::Component* m_targetComponent = nullptr;

    // -------------------------------------------------------------------------
    // GL helpers
    // -------------------------------------------------------------------------

    bool createShaders();
    void createVBOs();
    void deleteVBOs();
    void uploadWaveform(int index, const float* bins, int numBins);
    void drawGrid(double displayRangeBeats);
    void drawPlayhead(double displayRangeBeats, double currentPpq);
    void drawWaveform(int index, int numBins, float ampScale,
                      float r, float g, float b, float a);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenGLScopeRenderer)
};
