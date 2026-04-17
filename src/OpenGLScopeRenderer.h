#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <array>
#include <vector>

/**
 * OpenGLScopeRenderer — GPU-accelerated waveform renderer for ScopeDisplay.
 *
 * Implements juce::OpenGLRenderer to draw waveform data using OpenGL shaders
 * and per-instance VBOs.  The displayBins array from each InstanceSlot is
 * uploaded to a GPU buffer once per frame; the vertex shader then maps each
 * bin to its correct screen position.
 *
 * Lifecycle:
 *   1. Construct and call attachTo(component) to attempt OpenGL initialisation.
 *   2. After newOpenGLContextCreated() fires, isAvailable() returns true.
 *   3. If the context fails to create (no GPU / driver issue), isAvailable()
 *      remains false and the caller should fall back to software rendering.
 *   4. Call detach() or destroy to release OpenGL resources.
 *
 * Thread safety:
 *   - setWaveformData() copies display data from the UI thread.
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
        const float*    bins   = nullptr;  // pointer to displayBins (DISPLAY_BINS floats)
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

    // Shader program
    std::unique_ptr<juce::OpenGLShaderProgram> m_shader;

    // Per-instance VBOs (each holds DISPLAY_BINS * 2 floats: x, y interleaved)
    GLuint m_vbos[MAX_INSTANCES] {};
    bool   m_vbosCreated = false;

    // Grid VBO
    GLuint m_gridVbo = 0;
    int    m_gridVertexCount = 0;

    // Playhead VBO
    GLuint m_playheadVbo = 0;

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
    };

    juce::SpinLock m_dataLock;
    FrameSnapshot  m_pendingData;
    FrameSnapshot  m_renderData;
    bool           m_newDataAvailable = false;

    // Component pointer (for size queries on GL thread)
    juce::Component* m_targetComponent = nullptr;

    // -------------------------------------------------------------------------
    // GL helpers
    // -------------------------------------------------------------------------

    bool createShaders();
    void createVBOs();
    void deleteVBOs();
    void uploadWaveform(int index, const float* bins, int numBins, float ampScale);
    void drawGrid(int width, int height, double displayRangeBeats);
    void drawPlayhead(int width, int height, double displayRangeBeats, double currentPpq);
    void drawWaveform(int index, int numBins, float r, float g, float b, float a);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenGLScopeRenderer)
};
