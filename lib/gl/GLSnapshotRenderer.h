#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <utility>

/**
 * GLSnapshotRenderer<T> — Base class template for double-buffered OpenGL renderers.
 *
 * Encapsulates the thread-safe snapshot pattern used by all overlay and display
 * renderers in the scope application:
 *
 *   1. **Data capture (UI thread):** Call setSnapshot() to capture the latest
 *      render parameters. The data is stored in m_pending and m_newData is set.
 *
 *   2. **Snapshot swap (GL thread):** Call swapSnapshot() at the start of draw()
 *      to atomically move m_pending → m_render and clear m_newData.
 *
 *   3. **Rendering (GL thread):** Read from m_render to draw the latest frame.
 *
 * Thread safety:
 *   - UI thread calls setSnapshot() — protected by SpinLock
 *   - GL thread calls swapSnapshot() and reads m_render — protected by SpinLock
 *   - m_newData flag coordination ensures no data is lost between frames
 *   - No blocking calls; suitable for real-time audio GL rendering
 *
 * Usage:
 * @code
 * class MyRenderer : protected GLSnapshotRenderer<MyRenderer::Snapshot> {
 *     struct Snapshot {
 *         float value = 0.0f;
 *         int   count = 0;
 *     };
 *
 *     void setData(float value, int count) {
 *         Snapshot s = { value, count };
 *         setSnapshot(s);  // UI thread
 *     }
 *
 *     void draw() {
 *         if (!swapSnapshot()) return;  // GL thread
 *         // Use m_render.value, m_render.count for drawing
 *     }
 * };
 * @endcode
 *
 * @tparam SnapshotType Struct or class containing all render parameters.
 *                      Should be copy-constructible and move-constructible.
 */
template<typename SnapshotType>
class GLSnapshotRenderer {
  protected:
    mutable juce::SpinLock m_lock;          ///< Protects m_pending, m_render, m_newData
    SnapshotType   m_pending;               ///< Latest data from UI thread
    SnapshotType   m_render;                ///< Data for current GL frame
    bool           m_newData = false;       ///< Flag: m_pending has new data

    /**
     * Swap the pending snapshot into the render snapshot (GL thread only).
     *
     * Call this at the start of your draw() method to atomically acquire
     * the latest UI thread data. After swapSnapshot() returns:
     *   - If true:  m_render now contains the latest data; render and clear flag
     *   - If false: m_render unchanged; no new UI data this frame (use previous frame)
     *
     * Thread safety: Acquires and releases m_lock briefly.
     *
     * @return true if m_newData was set (snapshot was swapped);
     *         false if no new data (m_render unchanged from previous frame).
     */
    bool swapSnapshot() {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        if (m_newData) {
            m_render  = m_pending;
            m_newData = false;
            return true;
        }
        return false;
    }

    /**
     * Discard any pending snapshot and reset the new-data flag (GL thread only).
     *
     * Use this during context cleanup or when resetting renderer state.
     * After calling this, swapSnapshot() will return false until setSnapshot()
     * is called again from the UI thread.
     */
    void clearPendingSnapshot() {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        m_newData = false;
    }

    /**
     * Set a new snapshot for the next frame (UI thread only).
     *
     * Thread safety: Acquires and releases m_lock briefly.
     *
     * @param snapshot The render parameters to use in the next frame.
     */
    void setSnapshot(const SnapshotType& snapshot) {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        m_pending = snapshot;
        m_newData = true;
    }

    /**
     * Set a new snapshot via move semantics (UI thread only).
     * Use this for large snapshot types to avoid unnecessary copying.
     *
     * Thread safety: Acquires and releases m_lock briefly.
     *
     * @param snapshot R-value snapshot to move into m_pending.
     */
    void setSnapshot(SnapshotType&& snapshot) {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        m_pending = std::move(snapshot);
        m_newData = true;
    }

    /**
     * Get a copy of the current render snapshot (GL thread safe).
     *
     * Useful for accessing snapshot data outside of the draw() method
     * while maintaining thread safety.
     *
     * @return A copy of m_render (briefly acquires m_lock).
     */
    SnapshotType getRenderSnapshot() const {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        return m_render;
    }

  public:
    GLSnapshotRenderer() = default;
    virtual ~GLSnapshotRenderer() = default;

    GLSnapshotRenderer(const GLSnapshotRenderer&) = delete;
    GLSnapshotRenderer& operator=(const GLSnapshotRenderer&) = delete;
};
