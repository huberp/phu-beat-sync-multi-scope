#include "RmsOverlayRenderer.h"

using namespace juce::gl;

// ============================================================================
// GL-thread lifecycle
// ============================================================================

bool RmsOverlayRenderer::create(juce::OpenGLContext& ctx) {
    m_ctx = &ctx;

    if (!GLSLShaderBuilder::validateGLSLVersion("RmsOverlayRenderer")) {
        return false;
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
    const juce::String vertSrc = GLSLShaderBuilder::getVersionDirective() + R"(
        uniform float uRmsValues[128];
        uniform float uBarBoundaries[129];
        uniform int   uNumBars;
        uniform float uAmpScale;
        uniform float uHalfH;
        void main() {
            int barIndex = gl_VertexID / 6;
            int corner   = gl_VertexID % 6;

            // CCW winding:  TL(0) TR(1) BL(2)  |  TR(3) BR(4) BL(5)
            bool isRight = (corner == 1 || corner == 3 || corner == 4);
            bool isTop   = (corner == 0 || corner == 1 || corner == 3);

            // Use actual bucket boundary positions to match the beat-position grid.
            // uBarBoundaries[i] = bucket(i).startIdx / N  (normalised 0..1)
            // uBarBoundaries[numBars] = 1.0
            float normLeft  = uBarBoundaries[barIndex];
            float normRight = uBarBoundaries[barIndex + 1];
            float xLeft  = normLeft  * 2.0 - 1.0;
            float xRight = normRight * 2.0 - 1.0;
            float x      = isRight ? xRight : xLeft;

            float yCenter = clamp(uRmsValues[barIndex] * uAmpScale, -1.0, 1.0);
            float y       = yCenter + (isTop ? uHalfH : -uHalfH);

            gl_Position = vec4(x, y, 0.0, 1.0);
        }
    )";

    const juce::String fragSrc = GLSLShaderBuilder::buildFragmentShader(
        "uniform vec4 uColour;\n",
        "    fragColor = uColour;\n");

    if (!compileShader(vertSrc, fragSrc, "RmsOverlayRenderer")) {
        return false;
    }

    // Cache uniform locations
    const GLuint prog = m_shader->getProgramID();
    m_uRmsValuesLoc     = ctx.extensions.glGetUniformLocation(prog, "uRmsValues[0]");
    m_uBarBoundariesLoc = ctx.extensions.glGetUniformLocation(prog, "uBarBoundaries[0]");
    m_uNumBarsLoc       = ctx.extensions.glGetUniformLocation(prog, "uNumBars");
    m_uAmpScaleLoc      = ctx.extensions.glGetUniformLocation(prog, "uAmpScale");
    m_uHalfHLoc         = ctx.extensions.glGetUniformLocation(prog, "uHalfH");
    m_uColourLoc        = ctx.extensions.glGetUniformLocation(prog, "uColour");

    return true;
}

void RmsOverlayRenderer::release() {
    m_uRmsValuesLoc     = -1;
    m_uBarBoundariesLoc = -1;
    m_uNumBarsLoc       = -1;
    m_uAmpScaleLoc      = -1;
    m_uHalfHLoc         = -1;
    m_uColourLoc        = -1;
    clearPendingSnapshot();
    GLRendererBase::release();
}

// ============================================================================
// Data Upload (UI thread)
// ============================================================================

void RmsOverlayRenderer::setData(const float* values, int numBars,
                                  float ampScale, bool show,
                                  const float* barBoundaries) {
    Snapshot snapshot;
    snapshot.show     = show;
    snapshot.ampScale = ampScale;
    snapshot.numBars  = 0;

    if (show && values != nullptr && numBars > 0) {
        const int bars = juce::jmin(numBars, MAX_RMS_BARS);
        std::memcpy(snapshot.rmsValues, values, static_cast<size_t>(bars) * sizeof(float));
        snapshot.numBars = bars;

        if (barBoundaries != nullptr) {
            // numBars+1 boundary values (left edges of each bar + trailing 1.0)
            std::memcpy(snapshot.barBoundaries, barBoundaries,
                        static_cast<size_t>(bars + 1) * sizeof(float));
        } else {
            // Fallback: evenly-spaced boundaries
            for (int i = 0; i <= bars; ++i)
                snapshot.barBoundaries[i] = static_cast<float>(i) / static_cast<float>(bars);
        }
    }

    setSnapshot(snapshot);
}

// ============================================================================
// Drawing (GL thread)
// ============================================================================

void RmsOverlayRenderer::draw(int vpHeightPx) {
    if (!isReady()) return;

    swapSnapshot();

    if (!m_render.show || m_render.numBars <= 0 || vpHeightPx <= 0)
        return;

    const int bars      = m_render.numBars;
    const float ampScale = m_render.ampScale;

    m_shader->use();

    // Upload RMS values, boundary positions, and shared uniforms
    glUniform1fv(m_uRmsValuesLoc,     bars,       m_render.rmsValues);
    glUniform1fv(m_uBarBoundariesLoc, bars + 1,   m_render.barBoundaries);
    glUniform1i (m_uNumBarsLoc,       bars);
    glUniform1f (m_uAmpScaleLoc,      ampScale);

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
