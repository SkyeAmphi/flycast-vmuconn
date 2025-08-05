#include "vmu_network.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <libretro.h>
#include "hw/maple/maple_if.h" // For MDCF_* and MDRS_* constants
#include "log/LogManager.h" // For INFO_LOG, WARN_LOG macros
#ifndef _WIN32
    #include <fcntl.h>  // For fcntl() in setSocketNonBlocking()
#endif

void VmuNetworkClient::setSocketNonBlocking() { // Set the socket to non-blocking mode
    if (socket_fd < 0) return;  // Ensure socket is valid

    // Use platform-specific methods to set non-blocking mode
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_fd, FIONBIO, &mode);
#else
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool VmuNetworkClient::performHandshake() {
    MapleMsg handshake;
    handshake.command = 0xFF;
    handshake.destAP = 0x01;
    handshake.originAP = 0x00;
    handshake.size = 2;
    const char* handshake_str = "DP_HANDSHAKE";
    memcpy(handshake.data, handshake_str, std::min<size_t>(handshake.size * 4, strlen(handshake_str)));

    if (!sendMapleMessage(handshake)) {
        ERROR_LOG(MAPLE, "VmuNetworkClient: Failed to send handshake");
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        connected = false;
        return false;
    }

    MapleMsg response;
    if (!receiveMapleMessage(response) || response.command != 0xFE) {
        ERROR_LOG(MAPLE, "VmuNetworkClient: Handshake ACK not received");
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        connected = false;
        return false;
    }

    ERROR_LOG(MAPLE, "VmuNetworkClient: Handshake completed, connection established");
    return true;
}

// Enhanced VmuNetworkClient with better disconnect detection
bool VmuNetworkClient::isConnected() const {
    if (!connected) return false;

    // Test socket health with non-blocking peek
    char test_byte;
#ifdef _WIN32
    // On Windows, use standard recv with MSG_PEEK (socket should already be non-blocking)
    int result = recv(socket_fd, &test_byte, 1, MSG_PEEK);
    int error = WSAGetLastError();
    if (result == 0) {
        connected = false;
        return false;
    } else if (result == SOCKET_ERROR && error != WSAEWOULDBLOCK) {
        connected = false;
        return false;
    }
#else
    // On Unix systems, use MSG_DONTWAIT for non-blocking peek
    int result = recv(socket_fd, &test_byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) {
        connected = false;
        return false;
    } else if (result == SOCKET_ERROR && errno != EAGAIN && errno != EWOULDBLOCK) {
        connected = false;
        return false;
    }
#endif
    return true;
}

// Enhanced error handling in communication methods
bool VmuNetworkClient::sendRawMessage(const std::string& message) {
    if (!connected) return false;

    setSocketNonBlocking();

    size_t total_sent = 0;
    auto start_time = std::chrono::steady_clock::now();
    constexpr auto TIMEOUT_MS = std::chrono::milliseconds(5); // 5 ms timeout

    while (total_sent < message.length()) {
        int result = send(socket_fd, message.c_str() + total_sent, 
                         message.length() - total_sent, 0);

        if (result > 0) {
            total_sent += result;
            continue;
        }

        if (result == 0) {
            connected = false;
            return false;
        }

#ifdef _WIN32
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            connected = false;
            return false;
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            connected = false;
            return false;
        }
#endif

        auto now = std::chrono::steady_clock::now();
        if (now - start_time > TIMEOUT_MS) {
            return false; // Quick timeout
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

bool VmuNetworkClient::receiveRawMessage(std::string& response) {
    if (!connected) return false;

    setSocketNonBlocking();
    response.clear();

    auto start_time = std::chrono::steady_clock::now();
    constexpr auto TIMEOUT_MS = std::chrono::milliseconds(5); // 5 ms timeout

    char ch;
    while (true) {
        int result = recv(socket_fd, &ch, 1, 0);

        if (result > 0) {
            response += ch;
            if (response.length() >= 2 && 
                response.substr(response.length() - 2) == "\r\n") {
                response.erase(response.length() - 2);
                return true;
            }

            if (response.length() > 1024) { // Smaller buffer for faster failure detection
                connected = false;
                return false;
            }
            continue; // Reset timeout on successful data
        }

        if (result == 0) {
            connected = false;
            return false;
        }

#ifdef _WIN32
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            connected = false;
            return false;
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            connected = false;
            return false;
        }
#endif

        auto now = std::chrono::steady_clock::now();
        if (now - start_time > TIMEOUT_MS) {
            return false; // Quick fallback to file VMU
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// NetworkVmuManager Implementation
NetworkVmuManager::NetworkVmuManager(retro_environment_t env_cb) 
    : environ_cb(env_cb) {
    enterState(NetworkVmuState::DISABLED);
}

void NetworkVmuManager::enterState(NetworkVmuState new_state) {
    current_state = new_state;
    state_entered_time = std::chrono::steady_clock::now();
}

int NetworkVmuManager::getTimeInCurrentState() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
        now - state_entered_time).count();
}

bool NetworkVmuManager::shouldCheckHealth() const {
    auto now = std::chrono::steady_clock::now();
    auto time_since_check = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_health_check).count();
    return time_since_check >= HEALTH_CHECK_INTERVAL_SECONDS;
}

bool NetworkVmuManager::isConnectionHealthy() {
    last_health_check = std::chrono::steady_clock::now();
    return client && client->isConnected();
}

bool NetworkVmuManager::attemptConnection() {
    if (!client) {
        client = std::make_unique<VmuNetworkClient>();
    }
    
    return client->connect();
}

void NetworkVmuManager::showConnectionMessage(const char* message, unsigned int duration) {
    // Categorize messages by importance
    bool is_connection_established = strstr(message, "connected") != nullptr && strstr(message, "disconnected") == nullptr;
    bool is_disconnection = strstr(message, "disconnected") != nullptr;
    bool is_reconnection = strstr(message, "reconnected") != nullptr;
    
    // Always log significant state changes
    if (is_connection_established || is_disconnection || is_reconnection) {
        INFO_LOG(MAPLE, "🔗 Network VMU: %s", message);
        
        // Show user message for initial connection and disconnection only
        if (environ_cb && (is_connection_established || is_disconnection)) {
            struct retro_message msg = {message, duration};
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
    } else {
        // Less important messages - debug level only
        DEBUG_LOG(MAPLE, "Network VMU: %s", message);
    }
}

void NetworkVmuManager::setEnabled(bool enable) {
    enabled = enable;
    if (!enabled && current_state != NetworkVmuState::DISABLED) {
        if (client) {
            client->disconnect();
            client.reset();
        }
        enterState(NetworkVmuState::DISABLED);
    } else if (enabled && current_state == NetworkVmuState::DISABLED) {
        enterState(NetworkVmuState::DISCONNECTED);
    }
}

bool NetworkVmuManager::isConnected() const {
    return current_state == NetworkVmuState::CONNECTED;
}

void NetworkVmuManager::update() {

    static NetworkVmuState last_state = NetworkVmuState::DISABLED;
    if (current_state != last_state) {
        last_state = current_state;
    }

    static int update_count = 0;
    if (++update_count % 300 == 0) {  // Every 5 seconds at 60fps
                 update_count, (int)current_state, enabled ? "true" : "false");
    }

    switch (current_state) {
        case NetworkVmuState::DISABLED:
            if (enabled) {
                enterState(NetworkVmuState::DISCONNECTED);
            }
            break;
            
        case NetworkVmuState::DISCONNECTED:
            if (!enabled) {
                enterState(NetworkVmuState::DISABLED);
            } else {
                enterState(NetworkVmuState::CONNECTING);
            }
            break;
            
        case NetworkVmuState::CONNECTING:
            if (!enabled) {
                enterState(NetworkVmuState::DISABLED);
            } else if (attemptConnection()) {
                enterState(NetworkVmuState::CONNECTED);
                backoff_seconds = 1; // Reset backoff on success
                showConnectionMessage("Network VMU A1 connected to DreamPotato", 180);
            } else {
                // If stuck in CONNECTING for >10s, forcibly reset client
                if (getTimeInCurrentState() > 10) {
                    if (client) {
                        client->disconnect();
                        client.reset();
                    }
                    enterState(NetworkVmuState::DISCONNECTED);
                }
                // else: stay in CONNECTING and keep trying
            }
            break;
            
        case NetworkVmuState::CONNECTED:
            if (!enabled) {
                if (client) {
                    client->disconnect();
                    client.reset();
                }
                enterState(NetworkVmuState::DISABLED);
            } else if (shouldCheckHealth() && !isConnectionHealthy()) {
                showConnectionMessage("Network VMU A1 disconnected from DreamPotato", 120);
                enterState(NetworkVmuState::RECONNECTING);
            }
            break;
            
        case NetworkVmuState::RECONNECTING:
            if (!enabled) {
                enterState(NetworkVmuState::DISABLED);
            } else if (getTimeInCurrentState() >= backoff_seconds) {
                if (attemptConnection()) {
                    enterState(NetworkVmuState::CONNECTED);
                    backoff_seconds = 1; // Reset backoff
                    showConnectionMessage("Network VMU A1 reconnected to DreamPotato", 120);
                } else {
                    // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 30s (max)
                    backoff_seconds = std::min(backoff_seconds * 2, MAX_BACKOFF_SECONDS);
                    enterState(NetworkVmuState::RECONNECTING); // Reset timer
                }
            }
            break;
    }
}

// Global manager instance
static std::unique_ptr<NetworkVmuManager> g_network_vmu_manager;

// Updated wrapper functions to maintain API compatibility
void initNetworkVmuSystem(retro_environment_t env_cb) {
    g_network_vmu_manager = std::make_unique<NetworkVmuManager>(env_cb);
}

void updateNetworkVmuEnabled(bool enabled) {
    if (g_network_vmu_manager) {
        g_network_vmu_manager->setEnabled(enabled);
    }
}

void checkNetworkVmuConnection() {
    if (!g_network_vmu_manager || !g_network_vmu_manager->isEnabled())
        return;
    g_network_vmu_manager->update(); // All logic happens here
}

void shutdownNetworkVmu() {
    g_network_vmu_manager.reset();
}

// Update global client access for maple integration
std::unique_ptr<VmuNetworkClient> g_vmu_network_client = nullptr; // Keep for compatibility

// Add compatibility function for maple_devs.cpp
VmuNetworkClient* getNetworkVmuClient() {
    return g_network_vmu_manager ? g_network_vmu_manager->getClient() : nullptr;
}

VmuNetworkClient::VmuNetworkClient() : socket_fd(INVALID_SOCKET), connected(false) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

VmuNetworkClient::~VmuNetworkClient() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool VmuNetworkClient::connect() {
    
    if (connected && isConnected()) return true; // Add isConnected() check

    // Check existing socket completion
    if (socket_fd != INVALID_SOCKET) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_fd, &write_set);
        struct timeval timeout = {0, 0};
        
        int result = select(socket_fd + 1, nullptr, &write_set, nullptr, &timeout);
        if (result > 0 && FD_ISSET(socket_fd, &write_set)) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len) == 0) {
                if (performHandshake()) {
                    connected = true;
                    return true;
                } else {
                    return false;
                }
            }
        } else if (result == 0) {
            if (connect_start_time.time_since_epoch().count() > 0) {
                auto now = std::chrono::steady_clock::now();
                if (now - connect_start_time > std::chrono::seconds(5)) {
                    closesocket(socket_fd);
                    socket_fd = INVALID_SOCKET;
                    connected = false;
                    connect_start_time = {};
                    // Fall through to create a new socket below
                } else {
                    return false; // Still connecting
                }
            } else {
                return false;
            }
        } else {
            closesocket(socket_fd);
            socket_fd = INVALID_SOCKET;
            connected = false;
            connect_start_time = {};
            return false;
        }
    }

    // Create new socket and attempt connection
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == INVALID_SOCKET) {
        ERROR_LOG(MAPLE, "VmuNetworkClient: Failed to create socket");
        return false;
    }
    
    setSocketNonBlocking();
    connect_start_time = std::chrono::steady_clock::now();
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(DEFAULT_HOST);
#else
    inet_pton(AF_INET, DEFAULT_HOST, &addr.sin_addr);
#endif

    int result = ::connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr));

    if (result == 0) {
        // Handshake call
        if (performHandshake()) {
            connected = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // avoid race conditions
            return true;
        } else {
            return false;
        }
    } else {
    #ifdef _WIN32
        int error = WSAGetLastError();
    #else
        int error = errno;
    #endif
        connected = false;
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        return false;
    }

#ifdef _WIN32
    int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        return false; // Will complete asynchronously
    } else {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        return false;
    }
#else
    if (errno == EINPROGRESS) {
        return false; // Will complete asynchronously  
    } else {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        return false;
    }
#endif
}

void VmuNetworkClient::disconnect() {
    if (socket_fd != INVALID_SOCKET) {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
    }
    connected = false;
}

bool VmuNetworkClient::sendMapleMessage(const MapleMsg& msg) {
    std::lock_guard<std::mutex> lock(client_mutex);
    if (!connected) return false;

    // Single-stream hex formatting
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

        NetworkCommand cmd(NetworkCommand::SEND_MESSAGE);
        cmd.message = msg;
        submitFireAndForgetCommand(cmd);

        return true; // Successfully queued (doesn't mean sent successfully)
    }
    else
    {
        std::lock_guard<std::mutex> lock(client_mutex);
        if (!connected)
            return false;

        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');

        // Send header: command, destAP, originAP, size
        oss << std::setw(2) << (int)msg.command << " "
            << std::setw(2) << (int)msg.destAP << " "
            << std::setw(2) << (int)msg.originAP << " "
            << std::setw(2) << (int)msg.size;

        // Send all data bytes as hex with spaces
        u32 dataSize = msg.getDataSize();
        for (u32 i = 0; i < dataSize; ++i)
        {
            oss << " " << std::setw(2) << (int)msg.data[i];
        }

        if (dataSize > 0 || true)
        { // Always add space for consistency
            oss << " ";
        }

        oss << "\r\n";

        std::string message = oss.str();

        // DreamPotato expects: "XX XX XX XX ... \r\n"
        // Each byte = "XX " (3 chars), final space before \r\n, then \r\n (2 chars)
        u32 total_bytes = 4 + dataSize; // header + data bytes
        u32 expected_length = total_bytes * 3 + 1 + 2; // each byte="XX ", extra space, \r\n

                  (int)message.length(), expected_length, total_bytes);

        return sendRawMessage(message);
    }
}

bool VmuNetworkClient::receiveMapleMessage(MapleMsg& msg) {
    std::lock_guard<std::mutex> lock(client_mutex);
    if (!connected) return false;
    std::string response;
    if (!receiveRawMessage(response)) return false;

    // Clear message first for safety
    msg = {};

    // Decode ASCII hex back to MapleMsg
    std::istringstream iss(response);
    for (size_t i = 0; i < sizeof(MapleMsg); ++i) {
        std::string byteStr;
        if (!(iss >> byteStr)) break;
        ((u8*)&msg)[i] = (u8)std::stoi(byteStr, nullptr, 16);
    }

    // Log successful save write confirmations
    if (msg.command == 0x07) { // MDRS_DeviceReply indicates success
        INFO_LOG(MAPLE, "💾 Network VMU: Save data updated via DreamPotato");
    }

    return true;
}