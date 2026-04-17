#include "RmsOverlayRenderer.h"

using namespace juce::gl;

// ============================================================================
// GL-thread lifecycle
// ============================================================================

bool RmsOverlayRenderer::create(juce::OpenGLContext& ctx) {
    m_ctx = &ctx;

    // gl_VertexID requires GLSL 1.30+ (OpenGL 3.0)
    const double glslVersion = juce::OpenGLShaderProgram::getLanguageVersion();
    if (glslVersion < 1.30) {
        DBG("RmsOverlayRenderer: GLSL " + juce::String(glslVersion)
            + " too old (need 1.30+ for gl_VertexID)");
        return false;
    }

    // Select GLSL version directive (macOS Core Profile requires >= 150)
    juce::String versionLine;
    bool         useModernOutput = false;

    if (glslVersion >= 3.30) {
        versionLine     = "#version 330\n";
        useModernOutput = true;
    } else if (glslVersion >= 1.50) {
        versionLine = "#version 150\n";
    } else {
        versionLine = "#version 130\n";
    }

    // ---- Vertex shader -------------------------------------------------------
    // Geometry is computed entirely from gl_VertexID — no VBO/attributes needed.
    // Each bar occupies 6 vertices (2 CCW triangles forming a rectangle):
    //
    //   Triangle 1 vertices:  0=TL  1=TR  2=BL
    //   Triangle 2 vertices:  3=TR  4=BR  5=BL
    //
    // X: bar left/right edges are evenly spaced over clip-space [-1, 1].
    // Y: centred on clamp(rmsValue * ampScale, -1, 1); half-height set per draw.
    // -------------------------------------------------------------------------
    const juce::String vertSrc = versionLine + R"(
        uniform float uRmsValues[128];
        uniform int   uNumBars;
        uniform float uAmpScale;
        uniform float uHalfH;
        void main() {
            int barIndex = gl_VertexID / 6;
            int corner   = gl_VertexID % 6;

            // CCW winding:  TL(0) TR(1) BL(2)  |  TR(3) BR(4) BL(5)
            bool isRight = (corner == 1 || corner == 3 || corner == 4);
            bool isTop   = (corner == 0 || corner == 1 || corner == 3);

            int  n      = max(uNumBars, 1);
            float xLeft  = float(barIndex)     / float(n) * 2.0 - 1.0;
            float xRight = float(barIndex + 1) / float(n) * 2.0 - 1.0;
            float x      = isRight ? xRight : xLeft;

            float yCenter = clamp(uRmsValues[barIndex] * uAmpScale, -1.0, 1.0);
            float y       = yCenter + (isTop ? uHalfH : -uHalfH);

            gl_Position = vec4(x, y, 0.0, 1.0);
        }
    )";

    // ---- Fragment shader (flat colour) --------------------------------------
    const juce::String fragSrc = versionLine
        + (useModernOutput ? "out vec4 fragColor;\n" : "")
        + R"(
        uniform vec4 uColour;
        void main() {
        )" + (useModernOutput ? "    fragColor = uColour;\n" : "    gl_FragColor = uColour;\n")
        + "}\n";

    m_shader = std::make_unique<juce::OpenGLShaderProgram>(ctx);

    if (!m_shader->addVertexShader(vertSrc)) {
        DBG("RmsOverlayRenderer: vertex shader failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }
    if (!m_shader->addFragmentShader(fragSrc)) {
        DBG("RmsOverlayRenderer: fragment shader failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }
    if (!m_shader->link()) {
        DBG("RmsOverlayRenderer: shader link failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }

    // Cache uniform locations
    const GLuint prog = m_shader->getProgramID();
    m_uRmsValuesLoc = ctx.extensions.glGetUniformLocation(prog, "uRmsValues[0]");
    m_uNumBarsLoc   = ctx.extensions.glGetUniformLocation(prog, "uNumBars");
    m_uAmpScaleLoc  = ctx.extensions.glGetUniformLocation(prog, "uAmpScale");
    m_uHalfHLoc     = ctx.extensions.glGetUniformLocation(prog, "uHalfH");
    m_uColourLoc    = ctx.extensions.glGetUniformLocation(prog, "uColour");

    return true;
}

void RmsOverlayRenderer::release() {
    m_shader.reset();
    m_uRmsValuesLoc = -1;
    m_uNumBarsLoc   = -1;
    m_uAmpScaleLoc  = -1;
    m_uHalfHLoc     = -1;
    m_uColourLoc    = -1;
    m_ctx           = nullptr;
}

// ============================================================================
// Data Upload (UI thread)
// ============================================================================

void RmsOverlayRenderer::setData(const float* values, int numBars,
                                  float ampScale, bool show) {
    const juce::SpinLock::ScopedLockType lock(m_lock);
    m_pending.show     = show;
    m_pending.ampScale = ampScale;
    m_pending.numBars  = 0;

    if (show && values != nullptr && numBars > 0) {
        const int bars = juce::jmin(numBars, MAX_RMS_BARS);
        std::memcpy(m_pending.rmsValues, values, static_cast<size_t>(bars) * sizeof(float));
        m_pending.numBars = bars;
    }

    m_newData = true;
}

// ============================================================================
// Drawing (GL thread)
// ============================================================================

void RmsOverlayRenderer::draw(int vpHeightPx) {
    if (!m_shader) return;

    // Swap in the latest snapshot
    {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        if (m_newData) {
            m_render  = m_pending;
            m_newData = false;
        }
    }

    if (!m_render.show || m_render.numBars <= 0 || vpHeightPx <= 0)
        return;

    const int bars      = m_render.numBars;
    const float ampScale = m_render.ampScale;

    m_shader->use();

    // Upload RMS values and shared uniforms
    glUniform1fv(m_uRmsValuesLoc, bars, m_render.rmsValues);
    glUniform1i (m_uNumBarsLoc,   bars);
    glUniform1f (m_uAmpScaleLoc,  ampScale);

    // NDC half-height: halfH_ndc = pixelHeight / (vpHeightPx / 2)
    //                            = 2.0f * pixelHeight / vpHeightPx
    const float invHalfVp = 2.0f / static_cast<float>(vpHeightPx);

    // 3 concentric layers — widest/most transparent first (painter's order)
    struct Layer { float halfPx; float r, g, b, a; };
    static constexpr Layer kLayers[] = {
        // JUCE Colour(0x1A44AAFF) → A=0x1A R=0x44 G=0xAA B=0xFF
        { 5.0f, 0x44 / 255.0f, 0xAA / 255.0f, 0xFF / 255.0f, 0x1A / 255.0f },
        // JUCE Colour(0x5566CCFF) → A=0x55 R=0x66 G=0xCC B=0xFF
        { 3.0f, 0x66 / 255.0f, 0xCC / 255.0f, 0xFF / 255.0f, 0x55 / 255.0f },
        // JUCE Colour(0xEEAAEEFF) → A=0xEE R=0xAA G=0xEE B=0xFF
        { 2.0f, 0xAA / 255.0f, 0xEE / 255.0f, 0xFF / 255.0f, 0xEE / 255.0f },
    };

    for (const auto& layer : kLayers) {
        glUniform1f(m_uHalfHLoc,  layer.halfPx * invHalfVp);
        glUniform4f(m_uColourLoc, layer.r, layer.g, layer.b, layer.a);
        glDrawArrays(GL_TRIANGLES, 0, bars * 6);
    }
}
