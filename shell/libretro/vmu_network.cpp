#include "vmu_network.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <libretro.h>
#include "hw/maple/maple_if.h" // For MDCF_* and MDRS_* constants
#include "log/LogManager.h" // For INFO_LOG, WARN_LOG macros
#ifndef _WIN32
    #include <fcntl.h>  // For fcntl() in setSocketNonBlocking()
#endif
#include <thread>
#ifndef MDCF_BlockWrite
#define MDCF_BlockWrite  0x0C
#define MDCF_BlockRead   0x0B
#define MDRS_DataTransfer 0x08
#define MDRS_DeviceReply 0x07
#define MFID_1_Storage   0x02000000
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

void VmuNetworkClient::workerThreadMain() {

    while (worker_running.load()) {
        processCommands();

        // Brief sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

}

void VmuNetworkClient::processCommands() {
    std::unique_lock<std::mutex> lock(queue_mutex);

    while (!command_queue.empty()) {
        NetworkCommand cmd = std::move(command_queue.front());
        command_queue.pop();
        lock.unlock();

        bool success = false;

        switch (cmd.type) {
            case NetworkCommand::CONNECT:
                success = connect();
                thread_safe_connected.store(success);
                break;

            case NetworkCommand::DISCONNECT:
                disconnect();
                thread_safe_connected.store(false);
                success = true;
                break;

            case NetworkCommand::SEND_MESSAGE:
                success = sendMapleMessage(cmd.message);
                break;

            case NetworkCommand::SYNC_FLASH_BLOCK:
                success = syncFlashBlock(cmd.block_number, cmd.flash_data);
                break;

            case NetworkCommand::SHUTDOWN:
                worker_running.store(false);
                success = true;
                break;
        }

        if (cmd.result_promise) {
            cmd.result_promise->set_value(success);
        }

        lock.lock();
    }
}

std::future<bool> VmuNetworkClient::submitCommand(NetworkCommand cmd) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    cmd.result_promise = promise;

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        command_queue.push(std::move(cmd));
    }
    queue_cv.notify_one();

    return future;
}

void VmuNetworkClient::submitFireAndForgetCommand(NetworkCommand cmd) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        command_queue.push(std::move(cmd));
    }
    queue_cv.notify_one();
}

// Enhanced VmuNetworkClient with better disconnect detection
bool VmuNetworkClient::isConnected() const
{
    if (std::this_thread::get_id() == worker_thread_id)
    {
        if (!connected)
            return false;

        // Thorough connection test
        char test_byte;
#ifdef _WIN32
        int result = recv(socket_fd, &test_byte, 1, MSG_PEEK);
        int error = WSAGetLastError();

        if (result == 0)
        {
            connected = false;
            thread_safe_connected.store(false);
            return false;
        }

        if (result == SOCKET_ERROR)
        {
            if (error == 10053 || error == 10054 || error == 10057)
            {
                connected = false;
                thread_safe_connected.store(false);
                return false;
            }
            else if (error != WSAEWOULDBLOCK)
            {
                connected = false;
                thread_safe_connected.store(false);
                return false;
            }
        }
#endif
        return connected;
    }
    else
    {
        return thread_safe_connected.load();
    }
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
            thread_safe_connected.store(false);
            return false;
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            connected = false;
            thread_safe_connected.store(false);
            return false;
        }
#endif

        auto now = std::chrono::steady_clock::now();
        if (now - start_time > TIMEOUT_MS) {
            ERROR_LOG(MAPLE, "🔌 VmuNetworkClient: Send timeout - marking disconnected");
            connected = false;
            thread_safe_connected.store(false);
            return false;
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

    // Start worker thread and capture its ID
    worker_running.store(true);
    worker_thread = std::thread([this]() {
        worker_thread_id = std::this_thread::get_id(); // Capture worker thread ID
        workerThreadMain();
    });
    ERROR_LOG(MAPLE, "VmuNetworkClient: Constructor completed, worker thread started");
}

VmuNetworkClient::~VmuNetworkClient() {
    // Stop worker thread first
    if (worker_running.load()) {
        NetworkCommand shutdown_cmd(NetworkCommand::SHUTDOWN);
        submitFireAndForgetCommand(shutdown_cmd);

        // Wait for worker thread to finish
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }

    // Then cleanup socket
    if (socket_fd != INVALID_SOCKET) {
        closesocket(socket_fd);
        socket_fd = INVALID_SOCKET;
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

bool VmuNetworkClient::connect()
{
    if (std::this_thread::get_id() == worker_thread_id)
    {

        if (connected && isConnected())
            return true;

        // Check if we have an existing socket in progress
        if (socket_fd != INVALID_SOCKET)
        {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(socket_fd, &write_set);
            struct timeval timeout = {0, 0};

            int result = select(socket_fd + 1, nullptr, &write_set, nullptr, &timeout);
            if (result > 0 && FD_ISSET(socket_fd, &write_set))
            {
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
                if (error == 0)
                {
                    // double check connection is actually working
                    char test_byte;
                    int test_result = recv(socket_fd, &test_byte, 1, MSG_PEEK);
#ifdef _WIN32
                    int test_error = WSAGetLastError();
                    if (test_result == SOCKET_ERROR && (test_error == 10053 || test_error == 10054))
                    {
                        // Connection is actually broken
                        closesocket(socket_fd);
                        socket_fd = INVALID_SOCKET;
                        connected = false;
                        thread_safe_connected.store(false);
                        connect_start_time = {};
                        return false;
                    }
#endif
                    // Connection truly established
                    connected = true;
                    thread_safe_connected.store(true);
                    return true;
                }
                else
                {
                    closesocket(socket_fd);
                    socket_fd = INVALID_SOCKET;
                    connected = false;
                    thread_safe_connected.store(false);
                    connect_start_time = {};
                    return false;
                }
            }
            else if (result == 0)
            {
                // Check timeout
                if (connect_start_time.time_since_epoch().count() > 0)
                {
                    auto now = std::chrono::steady_clock::now();
                    if (now - connect_start_time > std::chrono::seconds(5))
                    {
                        closesocket(socket_fd);
                        socket_fd = INVALID_SOCKET;
                        connected = false;
                        thread_safe_connected.store(false);
                        connect_start_time = {};
                        return false;
                    }
                    else
                    {
                        return false; // Still connecting
                    }
                }
                else
                {
                    return false;
                }
            }
            else
            {
                closesocket(socket_fd);
                socket_fd = INVALID_SOCKET;
                connected = false;
                thread_safe_connected.store(false);
                connect_start_time = {};
                return false;
            }
        }

        // Create new socket and attempt connection
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd == INVALID_SOCKET)
        {
            return false;
        }

        ERROR_LOG(MAPLE, "VmuNetworkClient: Failed to create socket");
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

        int result = ::connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr));

        if (result == 0)
        {
            // Immediate connect success
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            connected = true;
            thread_safe_connected.store(true); // update atomic immediately
            return true;
        }

#ifdef _WIN32
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS)
        {
            return false; // Will complete asynchronously in next update
        }
        else
        {
            connected = false;
            closesocket(socket_fd);
            socket_fd = INVALID_SOCKET;
            return false;
        }
#else
        if (errno == EINPROGRESS)
        {
            return false; // Will complete asynchronously in next update
        }
        else
        {
            connected = false;
            closesocket(socket_fd);
            socket_fd = INVALID_SOCKET;
            return false;
        }
#endif
    }
    else
    {
        NetworkCommand cmd(NetworkCommand::CONNECT);
        submitFireAndForgetCommand(cmd);
        return thread_safe_connected.load();
    }
}

void VmuNetworkClient::disconnect() {
    if (std::this_thread::get_id() == worker_thread_id) {
        if (socket_fd != INVALID_SOCKET) {
            closesocket(socket_fd);
            socket_fd = INVALID_SOCKET;
        }
        connected = false;
    } else {
        // Main thread - delegate to worker thread
        NetworkCommand cmd(NetworkCommand::DISCONNECT);
        submitFireAndForgetCommand(cmd);
    }
}

bool VmuNetworkClient::sendMapleMessage(const MapleMsg &msg)
{
    // Always delegate to worker thread for thread safety
    if (std::this_thread::get_id() != worker_thread_id)
    {
        // Main thread - delegate to worker thread (non-blocking)
        if (!thread_safe_connected.load())
        {
            return false;
        }

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
    if (std::this_thread::get_id() == worker_thread_id) {
        // Worker thread - can block safely
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
        }

        return true;
    } else {
        // Main thread - this ideally should never happen
        // since receiveMapleMessage is only called from syncFlashBlock worker path
        return false;
    }
}

bool VmuNetworkClient::syncFlashBlock(u32 block_number, u8* local_flash_data) {
    if (std::this_thread::get_id() == worker_thread_id) {
        // Worker thread - can use ALL existing blocking logic
        std::lock_guard<std::mutex> lock(client_mutex);
        if (!connected) return false;

        auto start_time = std::chrono::steady_clock::now();
        constexpr auto TIMEOUT_MS = std::chrono::milliseconds(2000); // 2 seconds timeout

        // DreamPotato expects 4 phases of 128 bytes each to write a full 512-byte block
        for (u32 phase = 0; phase < 4; phase++) {
            // Send one phase (128 bytes)
            MapleMsg writeMsg = {};
            writeMsg.command = MDCF_BlockWrite;
            writeMsg.destAP = 0x01; // Port A, Slot 1
            writeMsg.originAP = 0;
            writeMsg.size = 34; // 2 words header + 32 words data = 34 words total

            // Format matching DreamPotato's expected format
            *(u32*)&writeMsg.data[0] = MFID_1_Storage;

            // Pack block number and phase correctly for DreamPotato
            u32 blockPhaseData = (block_number << 24) | (phase << 8) | 0; // pt = 0
            *(u32*)&writeMsg.data[4] = blockPhaseData;

            // Copy 128 bytes for this phase
            u32 phase_offset = phase * 128;
            memcpy(&writeMsg.data[8], &local_flash_data[block_number * 512 + phase_offset], 128);

            if (!sendMapleMessage(writeMsg)) {
                return false;
            }

            // Receive acknowledgment for this phase
            MapleMsg writeResponse;
            while (true) {
                if (receiveMapleMessage(writeResponse)) break;
                if (std::chrono::steady_clock::now() - start_time > TIMEOUT_MS) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10)); // OK on worker thread
            }
            if (writeResponse.command != MDRS_DeviceReply) {
                return false;
            }
        }

        // Step 2: Read back the complete updated block from DreamPotato
        MapleMsg readMsg = {};
        readMsg.command = MDCF_BlockRead;
        readMsg.destAP = 0x01; // Port A, Slot 1
        readMsg.originAP = 0;
        readMsg.size = 2; // 8 bytes / 4 = 2 words

        *(u32*)&readMsg.data[0] = MFID_1_Storage;
        u32 readBlockData = (block_number << 24) | (0 << 8) | 0; // phase 0, pt 0 for read
        *(u32*)&readMsg.data[4] = readBlockData;

        if (!sendMapleMessage(readMsg)) {
            return false;
        }

        MapleMsg response;
        if (!receiveMapleMessage(response)) {
            return false;
        }

        // Step 3: Update our local flash with DreamPotato's version
        if (response.command == MDRS_DataTransfer && response.getDataSize() >= 520) {
            // DreamPotato response format: function(4) + blockdata(4) + payload(512)
            u32 response_function = *(u32*)&response.data[0];
            u32 response_block_data = *(u32*)&response.data[4];
            u32 response_block = (response_block_data >> 24) & 0xFF;

            if (response_function == MFID_1_Storage && response_block == block_number) {
                // Update our local flash with DreamPotato's authoritative version
                memcpy(&local_flash_data[block_number * 512], &response.data[8], 512);
                return true;
            }
        }

        return false;
    } else {
        // Main thread - delegate to worker thread (non-blocking)
        if (!thread_safe_connected.load()) {
            return false;
        }

        NetworkCommand cmd(NetworkCommand::SYNC_FLASH_BLOCK);
        cmd.block_number = block_number;
        memcpy(cmd.flash_data, &local_flash_data[block_number * 512], 512);

        submitFireAndForgetCommand(cmd);

        return true; // Successfully queued (zero blocking!)
    }
}