#include "CancellationOverlayRenderer.h"

using namespace juce::gl;

// =============================================================================
bool CancellationOverlayRenderer::create(juce::OpenGLContext& ctx)
{
    m_ctx = &ctx;

    if (!GLSLShaderBuilder::validateGLSLVersion("CancellationOverlayRenderer"))
    {
        return false;
    }

    // -------------------------------------------------------------------------
    // Vertex shader — geometry built from gl_VertexID; no VBO required.
    // -------------------------------------------------------------------------
    const juce::String vertSrc = GLSLShaderBuilder::getVersionDirective() + R"(
        uniform float uCancelValues[256];
        uniform int   uNumSlots;
        uniform float uBarTopNDC;
        out vec3 vColour;

        void main()
        {
            int slotIndex = gl_VertexID / 6;
            int corner    = gl_VertexID % 6;

            // Two CCW triangles per slot:
            //   triangle 0: corners 0,1,2  (bottom-left, bottom-right, top-right)
            //   triangle 1: corners 3,4,5  (bottom-left, top-right,   top-left )
            bool isRight = (corner == 1 || corner == 3 || corner == 4);
            bool isTop   = (corner == 2 || corner == 3 || corner == 5);

            int n = max(uNumSlots, 1);
            float xLeft  = float(slotIndex)     / float(n) * 2.0 - 1.0;
            float xRight = float(slotIndex + 1) / float(n) * 2.0 - 1.0;

            float x = isRight ? xRight : xLeft;
            float y = isTop   ? uBarTopNDC : -1.0;
            gl_Position = vec4(x, y, 0.0, 1.0);

            // Colour ramp: green -> yellow -> red
            float ci      = clamp(uCancelValues[slotIndex], 0.0, 1.0);
            vec3 green    = vec3(0.0,   0.733, 0.333);
            vec3 yellow   = vec3(1.0,   0.800, 0.000);
            vec3 red      = vec3(1.0,   0.200, 0.000);
            float useSec  = step(0.4, ci);
            vec3 col1     = mix(green,  yellow, clamp(ci / 0.4,         0.0, 1.0));
            vec3 col2     = mix(yellow, red,    clamp((ci - 0.4) / 0.6, 0.0, 1.0));
            vColour       = mix(col1, col2, useSec);
        }
    )";

    // -------------------------------------------------------------------------
    // Fragment shader — output colour passed from vertex stage.
    // -------------------------------------------------------------------------
    const juce::String fragSrc = GLSLShaderBuilder::buildFragmentShader(
        "in vec3 vColour;\n",
        "    fragColor = vec4(vColour, 0.85);\n");

    if (!compileShader(vertSrc, fragSrc, "CancellationOverlayRenderer"))
    {
        return false;
    }

    // Cache uniform locations — use "uCancelValues[0]" for array uniforms.
    const GLuint prog  = m_shader->getProgramID();
    m_uCancelValuesLoc = ctx.extensions.glGetUniformLocation(prog, "uCancelValues[0]");
    m_uNumSlotsLoc     = ctx.extensions.glGetUniformLocation(prog, "uNumSlots");
    m_uBarTopNDCLoc    = ctx.extensions.glGetUniformLocation(prog, "uBarTopNDC");
    return true;
}

// =============================================================================
void CancellationOverlayRenderer::release()
{
    m_uCancelValuesLoc = m_uNumSlotsLoc = m_uBarTopNDCLoc = -1;
    clearPendingSnapshot();
    GLRendererBase::release();
}

// =============================================================================
void CancellationOverlayRenderer::setData(const float* values, int numSlots, bool show)
{
    Snapshot snapshot;

    snapshot.show     = show;
    snapshot.numSlots = 0;

    if (show && values != nullptr && numSlots > 0)
    {
        const int n = juce::jmin(numSlots, MAX_CANCEL_SLOTS);
        std::memcpy(snapshot.cancelValues, values, static_cast<size_t>(n) * sizeof(float));
        snapshot.numSlots = n;
    }

    setSnapshot(snapshot);
}

// =============================================================================
void CancellationOverlayRenderer::draw(int vpHeightPx)
{
    if (!isReady()) return;

    swapSnapshot();

    if (!m_render.show || m_render.numSlots <= 0 || vpHeightPx <= 0) return;

    // Bar is 6 px tall, anchored to the bottom edge of the viewport.
    const float barTopNDC = -1.0f + 2.0f * 6.0f / static_cast<float>(vpHeightPx);

    m_shader->use();
    glUniform1fv(m_uCancelValuesLoc, m_render.numSlots, m_render.cancelValues);
    glUniform1i (m_uNumSlotsLoc,     m_render.numSlots);
    glUniform1f (m_uBarTopNDCLoc,    barTopNDC);

    glDrawArrays(GL_TRIANGLES, 0, m_render.numSlots * 6);
}
