/*
    Copyright 2024 flyinghead

    This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "dreamlink.h"

// Platform-specific includes and implementations
#ifdef USE_DREAMCASTCONTROLLER

#include "dreamconn.h"
#include "dreampicoport.h"
#include "hw/maple/maple_devs.h"
#include "ui/gui.h"
#include <cfg/option.h>
#include <SDL.h>
#include <asio.hpp>
#include <iomanip>
#include <sstream>
#include <optional>
#include <thread>
#include <list>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Platform-specific includes
#if defined(__linux__) || (defined(__APPLE__) && defined(TARGET_OS_MAC))
#include <dirent.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#endif

// Include SDLGamepad for SDL builds only
#ifndef LIBRETRO
#include "input/sdl_gamepad.h"
#endif

// Global manager instance definition
std::unique_ptr<DreamLinkManager> g_dreamlink_manager = nullptr;

// Manager initialization functions
void initializeDreamLinkManager() {
    if (!g_dreamlink_manager) {
        #if defined(LIBRETRO)
            g_dreamlink_manager = std::make_unique<LibretroDreamLinkManager>();
        #else
            g_dreamlink_manager = std::make_unique<SDLDreamLinkManager>();
        #endif
    }
}

void shutdownDreamLinkManager() {
    g_dreamlink_manager.reset();
}

// SDL-specific DreamLinkGamepad implementation
#ifndef LIBRETRO

class DreamLinkGamepad : public SDLGamepad {
private:
    std::shared_ptr<DreamLink> dreamlink;
    std::string device_guid;
    bool startPressed = false;
    bool ltrigPressed = false;
    bool rtrigPressed = false;
    u32 leftTrigger = 0;
    u32 rightTrigger = 0;

public:
    static bool isDreamcastController(int deviceIndex);
    
    DreamLinkGamepad(int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick);
    virtual ~DreamLinkGamepad();
    
    void set_maple_port(int port) override;
    void registered() override;
    bool gamepad_btn_input(u32 code, bool pressed) override;
    bool gamepad_axis_input(u32 code, int value) override;
    void resetMappingToDefault(bool arcade, bool gamepad) override;
    const char *get_button_name(u32 code) override;
    const char *get_axis_name(u32 code) override;
    std::shared_ptr<InputMapping> getDefaultMapping() override;
    
private:
    static void handleEvent(Event event, void *arg);
    void checkKeyCombo();
};

bool DreamLinkGamepad::isDreamcastController(int deviceIndex) {
    char guid_str[33] {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(deviceIndex), guid_str, sizeof(guid_str));
    NOTICE_LOG(INPUT, "GUID: %s VID:%c%c%c%c PID:%c%c%c%c", guid_str,
            guid_str[10], guid_str[11], guid_str[8], guid_str[9],
            guid_str[18], guid_str[19], guid_str[16], guid_str[17]);

    // DreamConn VID:4457 PID:4443
    // Dreamcast Controller USB VID:1209 PID:2f07
    const char* pid_vid_guid_str = guid_str + 8;
    // TODO hack: just assume every controller is a DreamLinkGamepad
    if (true ||
        memcmp(DreamConn::VID_PID_GUID, pid_vid_guid_str, 16) == 0 ||
        memcmp(DreamPicoPort::VID_PID_GUID, pid_vid_guid_str, 16) == 0) {
        NOTICE_LOG(INPUT, "Dreamcast controller found!");
        return true;
    }
    return false;
}

DreamLinkGamepad::DreamLinkGamepad(int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick)
    : SDLGamepad(maple_port, joystick_idx, sdl_joystick) {
    
    // Ensure manager is initialized
    if (!g_dreamlink_manager) {
        initializeDreamLinkManager();
    }
    
    char guid_str[33] {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(joystick_idx), guid_str, sizeof(guid_str));
    device_guid = guid_str;

    // DreamConn VID:4457 PID:4443
    // Dreamcast Controller USB VID:1209 PID:2f07
    // TODO hack: assume the connected gamepad is a DreamConn.
    if (true || memcmp(DreamConn::VID_PID_GUID, guid_str + 8, 16) == 0) {
        // TODO: can't we just attempt to make a DreamConn elsewhere, in order to decouple the concept from gamepad
        dreamlink = std::make_shared<DreamConn>(maple_port);
    }
    else if (memcmp(DreamPicoPort::VID_PID_GUID, guid_str + 8, 16) == 0) {
        dreamlink = std::make_shared<DreamPicoPort>(maple_port, joystick_idx, sdl_joystick);
    }

    if (dreamlink) {
        // Use manager instead of global vector
        if (g_dreamlink_manager) {
            g_dreamlink_manager->addDreamLink(dreamlink);
        }
        
        _name = dreamlink->getName();
        int defaultBus = dreamlink->getDefaultBus();
        if (defaultBus >= 0 && defaultBus < 4) {
            set_maple_port(defaultBus);
        }

        std::string uniqueId = dreamlink->getUniqueId();
        if (!uniqueId.empty()) {
            this->_unique_id = uniqueId;
        }
    }

    EventManager::listen(Event::Start, handleEvent, this);
    EventManager::listen(Event::LoadState, handleEvent, this);
    EventManager::listen(Event::Terminate, handleEvent, this);

    loadMapping();
}

DreamLinkGamepad::~DreamLinkGamepad() {
    EventManager::unlisten(Event::Start, handleEvent, this);
    EventManager::unlisten(Event::LoadState, handleEvent, this);
    EventManager::unlisten(Event::Terminate, handleEvent, this);
    
    if (dreamlink) {
        tearDownDreamLinkDevices(dreamlink);
        
        // Remove from manager instead of global vector
        if (g_dreamlink_manager) {
            g_dreamlink_manager->removeDreamLink(dreamlink);
        }
        
        dreamlink.reset();

        // Make sure settings are open in case disconnection happened mid-game
        if (!gui_is_open()) {
            gui_open_settings();
        }
    }
}

void DreamLinkGamepad::set_maple_port(int port) {
    if (dreamlink) {
        if (port < 0 || port >= 4) {
            dreamlink->disconnect();
        }
        else if (dreamlink->getBus() != port) {
            dreamlink->changeBus(port);
            if (is_registered()) {
                dreamlink->connect();
            }
        }
    }
    SDLGamepad::set_maple_port(port);
}

void DreamLinkGamepad::registered() {
    if (dreamlink) {
        dreamlink->connect();
        // Create DreamLink Maple Devices here just in case game is already running
        createDreamLinkDevices(dreamlink, false);
    }
}

void DreamLinkGamepad::handleEvent(Event event, void *arg) {
    DreamLinkGamepad *gamepad = static_cast<DreamLinkGamepad*>(arg);
    if (gamepad->dreamlink != nullptr && event != Event::Terminate) {
        createDreamLinkDevices(gamepad->dreamlink, event == Event::Start);
    }

    if (gamepad->dreamlink != nullptr && event == Event::Terminate) {
        gamepad->dreamlink->gameTermination();
    }
}

bool DreamLinkGamepad::gamepad_btn_input(u32 code, bool pressed) {
    if (!is_detecting_input() && input_mapper) {
        DreamcastKey key = input_mapper->get_button_id(0, code);
        if (key == DC_BTN_START) {
            startPressed = pressed;
            checkKeyCombo();
        }
    }
    else {
        startPressed = false;
    }
    return SDLGamepad::gamepad_btn_input(code, pressed);
}

bool DreamLinkGamepad::gamepad_axis_input(u32 code, int value) {
    if (!is_detecting_input()) {
        if (code == leftTrigger) {
            ltrigPressed = value > 0;
            checkKeyCombo();
        }
        else if (code == rightTrigger) {
            rtrigPressed = value > 0;
            checkKeyCombo();
        }
    }
    else {
        ltrigPressed = false;
        rtrigPressed = false;
    }
    return SDLGamepad::gamepad_axis_input(code, value);
}

void DreamLinkGamepad::resetMappingToDefault(bool arcade, bool gamepad) {
    SDLGamepad::resetMappingToDefault(arcade, gamepad);
    if (input_mapper && dreamlink) {
        dreamlink->setDefaultMapping(input_mapper);
    }
}

const char *DreamLinkGamepad::get_button_name(u32 code) {
    if (dreamlink) {
        const char* name = dreamlink->getButtonName(code);
        if (name) {
            return name;
        }
    }
    return SDLGamepad::get_button_name(code);
}

const char *DreamLinkGamepad::get_axis_name(u32 code) {
    if (dreamlink) {
        const char* name = dreamlink->getAxisName(code);
        if (name) {
            return name;
        }
    }
    return SDLGamepad::get_axis_name(code);
}

std::shared_ptr<InputMapping> DreamLinkGamepad::getDefaultMapping() {
    std::shared_ptr<InputMapping> mapping = SDLGamepad::getDefaultMapping();
    if (mapping && dreamlink) {
        dreamlink->setDefaultMapping(mapping);
    }
    return mapping;
}

void DreamLinkGamepad::checkKeyCombo() {
    if (ltrigPressed && rtrigPressed && startPressed)
        gui_open_settings();
}

// SDL Manager Implementation
void SDLDreamLinkManager::processVblank() {
    // Check for configuration reloads
    for (auto& link : getDreamLinks()) {
        if (link) {
            link->reloadConfigurationIfNeeded();
        }
    }
}

void SDLDreamLinkManager::handleReconnect() {
    auto reconnectLink = getReconnectCandidate();
    if (reconnectLink) {
        tearDownDevices(reconnectLink, false);
        createDevices(reconnectLink, false);
        clearReconnectCandidate();
    }
}

void SDLDreamLinkManager::reloadAllConfigurations() {
    for (auto& link : getDreamLinks()) {
        if (link) {
            link->reloadConfigurationIfNeeded();
        }
    }
}

void SDLDreamLinkManager::createDevices(std::shared_ptr<DreamLink> link, bool gameStart) {
    if (!link) return;
    
    int bus = link->getBus();
    if (bus < 0 || bus >= 4) return;

    // Create VMU device if supported
    if (link->getFunctionCode(1) != 0) {
        // VMU logic here
        NOTICE_LOG(INPUT, "Creating VMU device for DreamLink bus %d", bus);
    }
    
    // Create rumble device if supported  
    if (link->getFunctionCode(2) != 0) {
        // Rumble logic here
        NOTICE_LOG(INPUT, "Creating rumble device for DreamLink bus %d", bus);
    }
}

void SDLDreamLinkManager::tearDownDevices(std::shared_ptr<DreamLink> link) {
    if (!link) return;
    
    int bus = link->getBus();
    NOTICE_LOG(INPUT, "Tearing down DreamLink devices for bus %d", bus);
    
    // Teardown logic here
}

std::shared_ptr<DreamLink> SDLDreamLinkManager::createDreamLink(const std::string& type, const std::string& config) {
    // Factory method for future config-driven creation
    if (type == "dreamconn") {
        // Parse config for bus number
        int bus = 0; // Default or parse from config
        return std::make_shared<DreamConn>(bus);
    }
    // Add other types as needed
    return nullptr;
}

#else // USE_DREAMCASTCONTROLLER not defined

// Stub implementations for builds without DreamLink support
class DreamLinkGamepad : public SDLGamepad {
public:
    static bool isDreamcastController(int deviceIndex) { return false; }
    DreamLinkGamepad(int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick) : SDLGamepad(maple_port, joystick_idx, sdl_joystick) {}
    virtual ~DreamLinkGamepad() {}
    void set_maple_port(int port) override { SDLGamepad::set_maple_port(port); }
    void registered() override {}
    bool gamepad_btn_input(u32 code, bool pressed) override { return SDLGamepad::gamepad_btn_input(code, pressed); }
    bool gamepad_axis_input(u32 code, int value) override { return SDLGamepad::gamepad_axis_input(code, value); }
    void resetMappingToDefault(bool arcade, bool gamepad) override { SDLGamepad::resetMappingToDefault(arcade, gamepad); }
    const char *get_button_name(u32 code) override { return SDLGamepad::get_button_name(code); }
    const char *get_axis_name(u32 code) override { return SDLGamepad::get_axis_name(code); }
    std::shared_ptr<InputMapping> getDefaultMapping() override { return SDLGamepad::getDefaultMapping(); }
};

#endif // USE_DREAMCASTCONTROLLER

#endif // !LIBRETRO

// LIBRETRO IMPLEMENTATIONS
#if defined(LIBRETRO)

// LibRetro Manager Implementation (No-op for most functions)
void LibretroDreamLinkManager::processVblank() {
    // LibRetro handles device updates through RetroArch
    // Could add network VMU status checks here in the future
}

void LibretroDreamLinkManager::handleReconnect() {
    // LibRetro handles reconnection through core options
    // Could trigger VMU network reconnection here
}

void LibretroDreamLinkManager::reloadAllConfigurations() {
    // LibRetro configurations are handled via core options
    // No action needed for current implementation
}

void LibretroDreamLinkManager::createDevices(std::shared_ptr<DreamLink> link, bool gameStart) {
    // No-op for libretro - DreamLink devices not supported
    // Physical controller integration happens through RetroArch's input system
}

void LibretroDreamLinkManager::tearDownDevices(std::shared_ptr<DreamLink> link) {
    // No-op for libretro - DreamLink devices not supported
}

#endif // defined(LIBRETRO)