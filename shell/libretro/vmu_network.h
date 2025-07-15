#pragma once

#ifdef _WIN32
    // Prevent Windows header pollution and conflicts
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    
    // Include Windows headers in correct order
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    
    // Clean up problematic macros that conflict with C++
    #ifdef min
        #undef min
    #endif
    #ifdef max
        #undef max
    #endif
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
#include "types.h"  // For u8, u32 types

// Use the canonical MapleMsg from dreamlink.h
#include "../../core/sdl/dreamlink.h"

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
    
    // DreamPotato protocol methods
    bool sendMapleMessage(const MapleMsg& msg);
    bool receiveMapleMessage(MapleMsg& msg);
    
private:
    bool sendRawMessage(const std::string& message);
    bool receiveRawMessage(std::string& response);
};

extern std::unique_ptr<VmuNetworkClient> g_vmu_network_client;