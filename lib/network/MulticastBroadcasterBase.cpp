#include "MulticastBroadcasterBase.h"

#include <chrono>
#include <cstring>
#include <random>

// Socket headers must be included before any namespace block.
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define MCAST_INVALID_SOCKET INVALID_SOCKET
    #define MCAST_SOCKET_ERROR   SOCKET_ERROR
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define MCAST_INVALID_SOCKET (-1)
    #define MCAST_SOCKET_ERROR   (-1)
    #define closesocket close
#endif

namespace phu {
namespace network {

#ifdef _WIN32
bool       MulticastBroadcasterBase::wsaInitialized = false;
int        MulticastBroadcasterBase::wsaRefCount    = 0;
std::mutex MulticastBroadcasterBase::wsaMutex;
#endif

// ============================================================================
// Construction / Destruction
// ============================================================================

MulticastBroadcasterBase::MulticastBroadcasterBase(const char* group, int port)
    : sendSocket(MCAST_INVALID_SOCKET),
      recvSocket(MCAST_INVALID_SOCKET),
      multicastAddr(nullptr),
      networkInitialized(false),
      instanceID(0),
      multicastGroup(group),
      multicastPort(port) {
    multicastAddr = new sockaddr_in();
    std::memset(multicastAddr, 0, sizeof(sockaddr_in));
    instanceID = generateInstanceID();
}

MulticastBroadcasterBase::~MulticastBroadcasterBase() {
    shutdown();
    if (multicastAddr) {
        delete static_cast<sockaddr_in*>(multicastAddr);
        multicastAddr = nullptr;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

bool MulticastBroadcasterBase::initialize() {
    if (networkInitialized)
        return true;

#ifdef _WIN32
    if (!initializeWSA())
        return false;
#endif

    if (!initializeSockets()) {
        shutdown();
        return false;
    }

    running.store(true);
    receiverThread = std::make_unique<std::thread>(
        &MulticastBroadcasterBase::receiverThreadRun, this);

    networkInitialized = true;
    return true;
}

void MulticastBroadcasterBase::shutdown() {
    running.store(false);
    if (receiverThread && receiverThread->joinable())
        receiverThread->join();
    receiverThread.reset();

    cleanupSockets();

#ifdef _WIN32
    cleanupWSA();
#endif

    networkInitialized = false;

    // Let subclass clear its own state
    onShutdown();
}

// ============================================================================
// Socket Setup
// ============================================================================

bool MulticastBroadcasterBase::initializeSockets() {
    // --- send socket -------------------------------------------------------
    sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sendSocket == MCAST_INVALID_SOCKET)
        return false;

    // Multicast destination address
    auto* addr = static_cast<sockaddr_in*>(multicastAddr);
    std::memset(addr, 0, sizeof(sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_port   = htons(static_cast<uint16_t>(multicastPort));
    inet_pton(AF_INET, multicastGroup, &addr->sin_addr);

    // Enable loopback (filtered by instanceID on receive)
    int loopback = 1;
    setsockopt(sendSocket, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loopback), sizeof(loopback));

    // TTL = 1 (local network only)
    int ttl = 1;
    setsockopt(sendSocket, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    // --- receive socket ----------------------------------------------------
    recvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (recvSocket == MCAST_INVALID_SOCKET) {
        closesocket(sendSocket);
        sendSocket = MCAST_INVALID_SOCKET;
        return false;
    }

    // Address reuse (allow multiple instances on same machine)
    int reuse = 1;
    setsockopt(recvSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(recvSocket, SOL_SOCKET, SO_REUSEPORT,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

    // Bind to multicast port
    struct sockaddr_in bindAddr{};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_port        = htons(static_cast<uint16_t>(multicastPort));
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(recvSocket, reinterpret_cast<struct sockaddr*>(&bindAddr),
             sizeof(bindAddr)) == MCAST_SOCKET_ERROR) {
        closesocket(sendSocket);
        closesocket(recvSocket);
        sendSocket = MCAST_INVALID_SOCKET;
        recvSocket = MCAST_INVALID_SOCKET;
        return false;
    }

    // Join multicast group
    struct ip_mreq mreq{};
    inet_pton(AF_INET, multicastGroup, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(recvSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq),
                   sizeof(mreq)) == MCAST_SOCKET_ERROR) {
        closesocket(sendSocket);
        closesocket(recvSocket);
        sendSocket = MCAST_INVALID_SOCKET;
        recvSocket = MCAST_INVALID_SOCKET;
        return false;
    }

    // Receive timeout 100ms (so receiver thread can check the running flag)
#ifdef _WIN32
    DWORD timeout = 100;
    setsockopt(recvSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(recvSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    return true;
}

void MulticastBroadcasterBase::cleanupSockets() {
    if (sendSocket != MCAST_INVALID_SOCKET) {
        closesocket(sendSocket);
        sendSocket = MCAST_INVALID_SOCKET;
    }
    if (recvSocket != MCAST_INVALID_SOCKET) {
        closesocket(recvSocket);
        recvSocket = MCAST_INVALID_SOCKET;
    }
}

// ============================================================================
// Helpers
// ============================================================================

uint32_t MulticastBroadcasterBase::generateInstanceID() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;
    return dis(gen);
}

int64_t MulticastBroadcasterBase::getCurrentTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// Windows-specific WSA initialization
// ============================================================================

#ifdef _WIN32
bool MulticastBroadcasterBase::initializeWSA() {
    std::lock_guard<std::mutex> lock(wsaMutex);
    if (!wsaInitialized) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            return false;
        wsaInitialized = true;
    }
    ++wsaRefCount;
    return true;
}

void MulticastBroadcasterBase::cleanupWSA() {
    std::lock_guard<std::mutex> lock(wsaMutex);
    if (wsaInitialized && --wsaRefCount == 0) {
        WSACleanup();
        wsaInitialized = false;
    }
}
#endif

} // namespace network
} // namespace phu
