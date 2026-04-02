#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace phu {
namespace network {

// Forward declare socket type to avoid including platform headers
#ifdef _WIN32
    using mcast_socket_t = unsigned long long; // SOCKET type on Windows
#else
    using mcast_socket_t = int;
#endif

/**
 * MulticastBroadcasterBase: common UDP multicast infrastructure shared by
 * SampleBroadcaster and any future broadcaster subclasses.
 *
 * Provides:
 * - Socket creation, multicast join/leave, and cleanup
 * - WSA initialization on Windows (reference-counted, shared across instances)
 * - Instance ID generation
 * - Lifecycle management (initialize / shutdown)
 * - Receiver thread management
 *
 * Subclasses must implement:
 * - receiverThreadRun()  — the loop that reads from recvSocket
 * - onShutdown()         — optional cleanup hook (called during shutdown)
 *
 * LOCALHOST-ONLY ASSUMPTION:
 * All plugin instances are assumed to run on the same machine (loopback interface).
 * The loopback MTU on Linux and macOS is 65535 bytes, so packet size is not
 * constrained by the Ethernet MTU (1500 bytes). Subclasses may therefore send
 * large packets (e.g., ~6 KB for 1470 float samples) without fragmentation concerns.
 * Do not use this infrastructure for cross-machine multicast without first adding
 * MTU-aware packet splitting.
 */
class MulticastBroadcasterBase {
  public:
    /** Default multicast group (administratively scoped, local org). */
    static constexpr const char* DEFAULT_MULTICAST_GROUP = "239.255.42.1";

    MulticastBroadcasterBase(const char* multicastGroup, int port);
    virtual ~MulticastBroadcasterBase();

    // Non-copyable / non-movable
    MulticastBroadcasterBase(const MulticastBroadcasterBase&) = delete;
    MulticastBroadcasterBase& operator=(const MulticastBroadcasterBase&) = delete;

    // ---- Lifecycle --------------------------------------------------------

    /**
     * Initialize networking (sockets, multicast group membership) and start
     * the receiver thread.  Safe to call multiple times.
     */
    bool initialize();

    /**
     * Shutdown networking, stop receiver thread, call onShutdown().
     * Blocks until the receiver thread has stopped.
     */
    void shutdown();

    /** Whether the broadcaster is initialized and the receiver thread running. */
    bool isRunning() const { return running.load(); }

    /** This instance's unique ID (randomly generated on construction). */
    uint32_t getInstanceID() const { return instanceID; }

    /** Override the instance ID. Must be called before initialize(). */
    void setInstanceID(uint32_t id) { instanceID = id; }

    /** User-assigned channel index [1, 8].  Stamped into every outgoing packet
     *  so receivers can map packets to a deterministic display slot. */
    uint8_t getInstanceIndex() const { return m_instanceIndex.load(std::memory_order_relaxed); }
    void    setInstanceIndex(uint8_t idx) { m_instanceIndex.store(idx, std::memory_order_relaxed); }

  protected:
    // ---- Subclass hooks ---------------------------------------------------

    /** Receiver thread main loop.  Must honour running flag. */
    virtual void receiverThreadRun() = 0;

    /** Called during shutdown() after the receiver thread has joined.
     *  Override to clear subclass-specific state. Default is a no-op. */
    virtual void onShutdown() {}

    // ---- Networking state (accessible to subclasses) ----------------------

    mcast_socket_t sendSocket;
    mcast_socket_t recvSocket;
    void* multicastAddr;     ///< sockaddr_in* (opaque pointer)
    bool networkInitialized;

    // Instance identification
    uint32_t instanceID;
    std::atomic<uint8_t> m_instanceIndex{1};

    // Thread management
    std::atomic<bool> running{false};
    std::unique_ptr<std::thread> receiverThread;

    // ---- Utilities --------------------------------------------------------

    /** Milliseconds since epoch (steady_clock). */
    static int64_t getCurrentTimeMs();

  private:
    const char* multicastGroup;
    int multicastPort;

    // Socket helpers
    bool initializeSockets();
    void cleanupSockets();
    static uint32_t generateInstanceID();

#ifdef _WIN32
    static bool wsaInitialized;
    static int wsaRefCount;
    static std::mutex wsaMutex;
    bool initializeWSA();
    void cleanupWSA();
#endif
};

} // namespace network
} // namespace phu
