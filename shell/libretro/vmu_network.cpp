#include "vmu_network.h"
#include <sstream>
#include <iomanip>
#include <libretro.h>


std::unique_ptr<VmuNetworkClient> g_vmu_network_client = nullptr;

static bool network_vmu_enabled = false;
static bool network_vmu_connected = false;
static bool network_vmu_connection_attempted = false;
static retro_environment_t environ_cb = nullptr;

VmuNetworkClient::VmuNetworkClient() {
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
    
    // Set socket timeouts
#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval timeout;
    timeout.tv_sec = 2;  // 2 second timeout
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#endif
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DEFAULT_PORT);
    inet_pton(AF_INET, DEFAULT_HOST, &server_addr.sin_addr);
    
    if (::connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
        return false;
    }
    
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

bool VmuNetworkClient::sendRawMessage(const std::string& message) {
    if (!connected) return false;
    
    int result = send(socket_fd, message.c_str(), message.length(), 0);
    return result != SOCKET_ERROR;
}

bool VmuNetworkClient::receiveRawMessage(std::string& response) {
    if (!connected) return false;
    
    // DreamPotato uses line-based protocol (\r\n terminated)
    response.clear();
    char ch;
    while (true) {
        int result = recv(socket_fd, &ch, 1, 0);
        if (result <= 0) return false;
        
        response += ch;
        if (response.length() >= 2 && 
            response.substr(response.length() - 2) == "\r\n") {
            response.erase(response.length() - 2); // Remove \r\n
            break;
        }
        
        // Prevent infinite loops on malformed messages
        if (response.length() > 4096) return false;
    }
    
    return true;
}

bool VmuNetworkClient::sendMapleMessage(const MapleMsg& msg) {
    if (!connected) return false;
    
    // Format exactly like the SDL implementation
    std::ostringstream s;
    s.fill('0');
    s << std::hex << std::uppercase
        << std::setw(2) << (u32)msg.command << " "
        << std::setw(2) << (u32)msg.destAP << " "
        << std::setw(2) << (u32)msg.originAP << " "
        << std::setw(2) << (u32)msg.size;
    
    const u32 sz = msg.getDataSize();
    for (u32 i = 0; i < sz; i++)
        s << " " << std::setw(2) << (u32)msg.data[i];
    s << "\r\n";
    
    return sendRawMessage(s.str());
}
bool VmuNetworkClient::receiveMapleMessage(MapleMsg& msg) {
    if (!connected) return false;
    
    std::string response;
    if (!receiveRawMessage(response)) return false;
    
    // Parse hex-encoded maple message
    if (sscanf(response.c_str(), "%hhx %hhx %hhx %hhx", 
               &msg.command, &msg.destAP, &msg.originAP, &msg.size) != 4)
        return false;
    
    // Ensure we have enough data for the message
    if ((msg.getDataSize() - 1) * 3 + 13 >= response.length())
        return false;
    
    // Parse data bytes
    for (unsigned i = 0; i < msg.getDataSize(); i++) {
        if (sscanf(&response[i * 3 + 12], "%hhx", &msg.data[i]) != 1)
            return false;
    }
    
    return true;
}

void initNetworkVmuSystem(retro_environment_t env_cb) {
    environ_cb = env_cb;
}

void updateNetworkVmuEnabled(bool enabled) {
    network_vmu_enabled = enabled;
}

void initializeNetworkVmu() {
    struct retro_variable var = {"flycast_vmu_network", NULL};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (strcmp(var.value, "enabled") == 0) {
            // Create the global network client using std::unique_ptr
            if (!g_vmu_network_client) {
                g_vmu_network_client = std::make_unique<VmuNetworkClient>();
                if (g_vmu_network_client->connect()) {
                    network_vmu_connected = true;
                    struct retro_message msg = {"Network VMU A1 connected to DreamPotato", 180};
                    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
                } else {
                    network_vmu_connected = false;
                    struct retro_message msg = {"Network VMU failed to connect to DreamPotato", 180};
                    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
                    g_vmu_network_client.reset();
                }
            }
            network_vmu_connection_attempted = true;
        }
    }
}

void shutdownNetworkVmu() {
    if (g_vmu_network_client) {
        g_vmu_network_client.reset();
        network_vmu_connected = false;
    }
}

bool attemptNetworkVmuConnection() {
    if (!network_vmu_enabled || network_vmu_connection_attempted) {
        return network_vmu_connected;
    }
    
    network_vmu_connection_attempted = true;
    
    if (!g_vmu_network_client) {
        g_vmu_network_client = std::make_unique<VmuNetworkClient>();
    }
    
    if (g_vmu_network_client->connect()) {
        network_vmu_connected = true;
        struct retro_message msg = {"Network VMU A1 connected to DreamPotato", 180};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    } else {
        network_vmu_connected = false;
        struct retro_message msg = {"Network VMU failed to connect to DreamPotato", 180};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
    
    return network_vmu_connected;
}

void checkNetworkVmuConnection() {
    if (!network_vmu_enabled) {
        return;
    }
    
    if (!network_vmu_connected && !network_vmu_connection_attempted) {
        attemptNetworkVmuConnection();
    }
    
    // Check if connection is still alive
    if (network_vmu_connected && g_vmu_network_client) {
        if (!g_vmu_network_client->isConnected()) {
            network_vmu_connected = false;
            struct retro_message msg = {"Network VMU A1 disconnected from DreamPotato", 120};
            environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
    }
}