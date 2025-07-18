#pragma once

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    
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
#include <memory>
#include <chrono>  // Add this for NetworkVmuManager
#include "types.h"  // For u8, u32 types
#include <mutex>

typedef bool (*retro_environment_t)(unsigned cmd, void *data);

struct MapleMsg {
    u8 command = 0;
    u8 destAP = 0; 
    u8 originAP = 0;
    u8 size = 0;
    u8 data[1024];

    u32 getDataSize() const { return size * 4; }
};

// Forward declaration for clean interfaces
class NetworkVmuManager;

class VmuNetworkClient {
private:
    static constexpr int DEFAULT_PORT = 37393;
    static constexpr const char* DEFAULT_HOST = "127.0.0.1";
    
    SOCKET socket_fd = INVALID_SOCKET;
    mutable bool connected = false;  // mutable for isConnected() const
    mutable std::mutex client_mutex; // Add mutex for thread safety

        void setSocketNonBlocking(); // we dont want to block the main thread
    
public:
    VmuNetworkClient();
    ~VmuNetworkClient();
    
    bool connect();
    void disconnect();
    bool isConnected() const;  // Remove inline implementation for better error handling
    
    bool sendMapleMessage(const MapleMsg& msg);
    bool receiveMapleMessage(MapleMsg& msg);
    
private:
    bool sendRawMessage(const std::string& message);
    bool receiveRawMessage(std::string& response);
};

enum class NetworkVmuState {
    DISABLED,        // Feature turned off
    DISCONNECTED,    // Ready to connect but not connected  
    CONNECTING,      // Actively attempting connection
    CONNECTED,       // Successfully connected and healthy
    RECONNECTING     // Attempting to restore lost connection (includes backoff)
};

class NetworkVmuManager {
private:
    NetworkVmuState current_state = NetworkVmuState::DISABLED;
    std::chrono::steady_clock::time_point state_entered_time;
    std::chrono::steady_clock::time_point last_health_check;
    
    int backoff_seconds = 1;
    static constexpr int MAX_BACKOFF_SECONDS = 30;
    static constexpr int HEALTH_CHECK_INTERVAL_SECONDS = 5;
    
    bool enabled = false;
    retro_environment_t environ_cb = nullptr;
    std::unique_ptr<VmuNetworkClient> client;
    
    // State transition helpers
    void enterState(NetworkVmuState new_state);
    int getTimeInCurrentState() const;
    bool shouldCheckHealth() const;
    bool isConnectionHealthy();
    bool attemptConnection();
    void showConnectionMessage(const char* message, int duration);
    
public:
    NetworkVmuManager(retro_environment_t env_cb);
    ~NetworkVmuManager() = default;
    
    // Main interface
    void setEnabled(bool enable);
    void update();
    
    // Query interface
    bool isConnected() const;
    bool isEnabled() const { return enabled; }
    NetworkVmuState getCurrentState() const { return current_state; }
    
    // Communication interface
    VmuNetworkClient* getClient() { return client.get(); }
};

// Public API - clean and consistent
void initNetworkVmuSystem(retro_environment_t env_cb);
void updateNetworkVmuEnabled(bool enabled);
void shutdownNetworkVmu();

// Legacy compatibility functions (for existing libretro.cpp integration)
bool attemptNetworkVmuConnection();
void checkNetworkVmuConnection();

// For maple integration (replaces global g_vmu_network_client)
VmuNetworkClient* getNetworkVmuClient();