#include "vmu_network.h"

VmuNetworkClient* g_vmu_network_client = nullptr;

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
    
    // Set socket timeout for robustness
    struct timeval timeout;
    timeout.tv_sec = 5;  // 5 second timeout
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    
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

bool VmuNetworkClient::sendMessage(const std::string& message) {
    if (!connected) return false;
    
    int result = send(socket_fd, message.c_str(), message.length(), 0);
    return result != SOCKET_ERROR;
}

bool VmuNetworkClient::receiveMessage(std::string& response) {
    if (!connected) return false;
    
    char buffer[1024];
    int result = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);
    if (result <= 0) return false;
    
    buffer[result] = '\0';
    response = buffer;
    return true;
}