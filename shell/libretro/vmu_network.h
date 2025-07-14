#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include <string>

class VmuNetworkClient {
private:
    static constexpr int DEFAULT_PORT = 37393;
    static constexpr const char* DEFAULT_HOST = "127.0.0.1";
    
    SOCKET socket_fd = INVALID_SOCKET;
    bool connected = false;
    
public:
    VmuNetworkClient();
    ~VmuNetworkClient();
    
    bool connect();
    void disconnect();
    bool isConnected() const { return connected; }
    
    bool sendMessage(const std::string& message);
    bool receiveMessage(std::string& response);
};

extern VmuNetworkClient* g_vmu_network_client;