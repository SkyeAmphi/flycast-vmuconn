#include "vmu_network.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <libretro.h>
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

// Enhanced VmuNetworkClient with better disconnect detection
bool VmuNetworkClient::isConnected() const {
    if (!connected) return false;

    // Test socket health with non-blocking peek
    char test_byte;

#ifdef _WIN32
    // On Windows, use standard recv with MSG_PEEK (socket should already be non-blocking)
    int result = recv(socket_fd, &test_byte, 1, MSG_PEEK);
#else
    // On Unix systems, use MSG_DONTWAIT for non-blocking peek
    int result = recv(socket_fd, &test_byte, 1, MSG_PEEK | MSG_DONTWAIT);
#endif

    if (result == 0) {
        // Connection closed by peer
        connected = false;
        return false;
    } else if (result == SOCKET_ERROR) {
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
    }

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
        // Immediate retry for best responsiveness
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
        // Immediate retry
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
    if (environ_cb) {
        struct retro_message msg = {message, duration};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
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
                enterState(NetworkVmuState::RECONNECTING);
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

bool attemptNetworkVmuConnection() {
    return g_network_vmu_manager ? g_network_vmu_manager->isConnected() : false;
}

void checkNetworkVmuConnection() {
    if (g_network_vmu_manager) {
        g_network_vmu_manager->update(); // All logic happens here
    }
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
    if (connected) return true;
    
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == INVALID_SOCKET) return false;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
    
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(DEFAULT_HOST);
#else
    inet_pton(AF_INET, DEFAULT_HOST, &addr.sin_addr);
#endif
    
    if (::connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        return false;
    }
    
    setSocketNonBlocking();
    connected = true;
    return true;
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

    // Encode MapleMsg as ASCII hex (DreamPotato expects this)
    std::ostringstream oss;
    for (size_t i = 0; i < 4 + msg.getDataSize(); ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << (int)((const u8*)&msg)[i];
        if (i < 4 + msg.getDataSize() - 1)
            oss << " ";
    }
    oss << "\r\n";
    return sendRawMessage(oss.str());
}

bool VmuNetworkClient::receiveMapleMessage(MapleMsg& msg) {
    std::lock_guard<std::mutex> lock(client_mutex);
    if (!connected) return false;
    std::string response;
    if (!receiveRawMessage(response)) return false;

    // Decode ASCII hex back to MapleMsg
    std::istringstream iss(response);
    for (size_t i = 0; i < 4 + sizeof(msg.data); ++i) {
        std::string byteStr;
        if (!(iss >> byteStr)) break;
        ((u8*)&msg)[i] = (u8)std::stoi(byteStr, nullptr, 16);
    }
    return true;
}