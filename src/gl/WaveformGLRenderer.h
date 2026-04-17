#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <array>

/**
 * WaveformGLRenderer — self-contained GPU waveform line renderer.
 *
 * Renders up to 8 waveform instances (local + remotes) using a Y-only VBO
 * technique: the float array of display bins is uploaded directly per
 * instance, and the vertex shader computes X from gl_VertexID.
 *
 * Owns its own shader, VBOs, and double-buffered data snapshot.
 *
 * Thread safety:
 *   - setData() is called from the UI thread.
 *   - draw()    is called from the GL thread.
 *   - A SpinLock protects the pending → render snapshot swap.
 */
class WaveformGLRenderer {
  public:
    static constexpr int MAX_INSTANCES = 8;
    static constexpr int DISPLAY_BINS  = 4096;

    /** Per-instance input data supplied by the UI thread. */
    struct InstanceData {
        bool         active  = false;
        bool         isLocal = false;
        juce::Colour colour  { juce::Colours::green };
        float        alpha   = 1.0f;
        const float* bins    = nullptr;  ///< DISPLAY_BINS Y-only floats; not owned.
    };

    WaveformGLRenderer()  = default;
    ~WaveformGLRenderer() { release(); }

    // -------------------------------------------------------------------------
    // Lifecycle (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Compile the waveform shader and allocate per-instance VBOs.
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
     * Snapshot the current waveform data for the next GL frame.
     * Must be called from the UI thread.
     */
    void setData(const std::array<InstanceData, MAX_INSTANCES>& instances,
                 float ampScale,
                 bool  showLocal,
                 bool  showRemote,
                 bool  broadcastOnly,
                 int   localSlotIndex);

    // -------------------------------------------------------------------------
    // Drawing (GL thread)
    // -------------------------------------------------------------------------

    /**
     * Swap the latest snapshot and draw all active waveform instances.
     * Must be called from the GL thread.
     * The caller is responsible for setting glViewport before this call.
     */
    void draw();

  private:
    juce::OpenGLContext* m_ctx = nullptr;

    std::unique_ptr<juce::OpenGLShaderProgram> m_shader;
    GLint  m_aYValueLoc   = -1;  ///< attribute: float yValue
    GLint  m_uColourLoc   = -1;  ///< uniform:   vec4  uColour
    GLint  m_uNumBinsLoc  = -1;  ///< uniform:   int   uNumBins
    GLint  m_uAmpScaleLoc = -1;  ///< uniform:   float uAmpScale

    GLuint m_vbos[MAX_INSTANCES] {};
    bool   m_vbosCreated = false;

    // Double-buffered snapshot (UI thread writes pending, GL thread reads render)
    struct Snapshot {
        struct Inst {
            bool  active  = false;
            bool  isLocal = false;
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            float bins[DISPLAY_BINS] {};
        };
        std::array<Inst, MAX_INSTANCES> instances;
        float ampScale       = 1.0f;
        bool  showLocal      = true;
        bool  showRemote     = true;
        bool  broadcastOnly  = false;
        int   localSlotIndex = 0;
    };

    juce::SpinLock m_lock;
    Snapshot       m_pending;
    Snapshot       m_render;
    bool           m_newData = false;

    void createVBOs();
    void deleteVBOs();

    /** Upload bins to the VBO for index, set shader uniforms, and draw. */
    void uploadAndDraw(int           index,
                       const float*  bins,
                       int           numBins,
                       float         ampScale,
                       float r, float g, float b, float a);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformGLRenderer)
};
