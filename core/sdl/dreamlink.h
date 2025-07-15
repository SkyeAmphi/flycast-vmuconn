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
#pragma once

#include "types.h"
#include "emulator.h"
#include <functional>
#include <memory>
#include <array>
#include <vector>
#include <algorithm>

// Conditional include prioritizing libretro safety
#ifndef LIBRETRO
    #include "input/mapping.h"  // Full include for SDL builds
#else
    // Forward declaration for libretro with minimal dependencies
    class InputMapping;
#endif

// Common structures available to all build targets
struct MapleMsg
{
	u8 command = 0;
	u8 destAP = 0;
	u8 originAP = 0;
	u8 size = 0;
	u8 data[1024];

	u32 getDataSize() const {
		return size * 4;
	}

	template<typename T>
	void setData(const T& p) {
		memcpy(data, &p, sizeof(T));
		this->size = (sizeof(T) + 3) / 4;
	}

	void setWord(const u32& p, int index) {
		if (index < 0 || index >= 256) {
			return;
		}
		memcpy(&data[index * 4], &p, sizeof(u32));
		if (this->size <= index) {
			this->size = index + 1;
		}
	}
};
static_assert(sizeof(MapleMsg) == 1028);

// Abstract base class for physical controller implementations
class DreamLink : public std::enable_shared_from_this<DreamLink>
{
public:
	DreamLink() = default;

	virtual ~DreamLink() = default;

	//! Sends a message to the controller, ignoring the response
	//! @note The implementation shall be thread safe
	virtual bool send(const MapleMsg& msg) = 0;

	//! Sends a message to the controller and waits for a response
	//! @note The implementation shall be thread safe
    virtual bool send(const MapleMsg& txMsg, MapleMsg& rxMsg) = 0;

	//! When called, do teardown stuff like reset screen
	virtual inline void gameTermination() {}

    //! @param[in] forPort The port number to get the function code of (1 or 2)
    //! @return the device type for the given port
    virtual u32 getFunctionCode(int forPort) const = 0;

    //! @param[in] forPort The port number to get the function definitions of (1 or 2)
	//! @return the 3 function definitions for the supported function codes
    virtual std::array<u32, 3> getFunctionDefinitions(int forPort) const = 0;

	//! @return the default bus number to select for this controller or -1 to not select a default
	virtual int getDefaultBus() const {
		return -1;
	}

	//! Allows a DreamLink device to dictate the default mapping
	virtual void setDefaultMapping(const std::shared_ptr<InputMapping>& mapping) const {
	}

	//! Allows button names to be defined by a DreamLink device
	//! @param[in] code The button code to retrieve name of
	//! @return the button name for the given code to override what is defined by the gamepad
	//! @return nullptr to fall back to gamepad definitions
	virtual const char *getButtonName(u32 code) const {
		return nullptr;
	}

	//! Allows axis names to be defined by a DreamLink device
	//! @param[in] code The axis code to retrieve name of
	//! @return the axis name for the given code to override what is defined by the gamepad
	//! @return nullptr to fall back to gamepad definitions
	virtual const char *getAxisName(u32 code) const {
		return nullptr;
	}

	//! @return a unique ID for this DreamLink device or empty string to use default
	virtual std::string getUniqueId() const {
		return std::string();
	}

	//! @return the selected bus number of the controller
	virtual int getBus() const = 0;

	//! Changes the selected maple port is changed by the user
	virtual void changeBus(int newBus) = 0;

	//! @return the display name of the controller
	virtual std::string getName() const = 0;

	//! Check if the remote device configuration has changed and update if necessary
	virtual void reloadConfigurationIfNeeded() = 0;

	//! Attempt connection to the hardware controller
	virtual void connect() = 0;

	//! Disconnect from the hardware controller
	virtual void disconnect() = 0;
};

// Complete manager interface with owned state
class DreamLinkManager {
private:
    std::vector<std::shared_ptr<DreamLink>> dreamLinks;
    std::shared_ptr<DreamLink> reconnectCandidate = nullptr;
    
public:
    virtual ~DreamLinkManager() = default;

    // Core operations (platform-specific)
    virtual void processVblank() = 0;
    virtual void handleReconnect() = 0;
    virtual void reloadAllConfigurations() = 0;
    virtual void createDevices(std::shared_ptr<DreamLink> link, bool gameStart) = 0;
    virtual void tearDownDevices(std::shared_ptr<DreamLink> link) = 0;

    // State management (base implementations)
    virtual void addDreamLink(std::shared_ptr<DreamLink> link) {
        if (link && std::find(dreamLinks.begin(), dreamLinks.end(), link) == dreamLinks.end()) {
            dreamLinks.push_back(link);
        }
    }
    
    virtual void removeDreamLink(std::shared_ptr<DreamLink> link) {
        auto it = std::find(dreamLinks.begin(), dreamLinks.end(), link);
        if (it != dreamLinks.end()) {
            dreamLinks.erase(it);
        }
    }
    
    virtual const std::vector<std::shared_ptr<DreamLink>>& getDreamLinks() const {
        return dreamLinks;
    }
    
    virtual std::vector<std::shared_ptr<DreamLink>>& getDreamLinksMutable() {
        return dreamLinks;
    }

    // Reconnection handling (base implementations)
    virtual void markForReconnect(std::shared_ptr<DreamLink> link) {
        reconnectCandidate = link;
    }
    
    virtual std::shared_ptr<DreamLink> getReconnectCandidate() const {
        return reconnectCandidate;
    }
    
    virtual void clearReconnectCandidate() {
        reconnectCandidate = nullptr;
    }
    
    // Factory method preparation
    virtual std::shared_ptr<DreamLink> createDreamLink(const std::string& type, const std::string& config = "") {
        return nullptr; // Base implementation returns null
    }
};

// Global manager instance
extern std::unique_ptr<DreamLinkManager> g_dreamlink_manager;

// Platform-specific implementations
#if !defined(LIBRETRO)
    // Forward declaration to avoid SDL dependency in header
    class SDLGamepad;
    
    class SDLDreamLinkManager : public DreamLinkManager {
    public:
        void processVblank() override;
        void handleReconnect() override;
        void reloadAllConfigurations() override;
        void createDevices(std::shared_ptr<DreamLink> link, bool gameStart) override;
        void tearDownDevices(std::shared_ptr<DreamLink> link) override;
        
    // Factory methods for SDL
    std::shared_ptr<DreamLink> createDreamLink(const std::string& type, const std::string& config = "") override;
};

// Forward declaration for DreamLinkGamepad - full definition in implementation file
class DreamLinkGamepad;

#else // LIBRETRO
    
    class LibretroDreamLinkManager : public DreamLinkManager {
    public:
        void processVblank() override;
        void handleReconnect() override;
        void reloadAllConfigurations() override;
        void createDevices(std::shared_ptr<DreamLink> link, bool gameStart) override;
        void tearDownDevices(std::shared_ptr<DreamLink> link) override;
    };
    
#endif

// Global manager instance
extern std::unique_ptr<DreamLinkManager> g_dreamlink_manager;

// Manager initialization
void initializeDreamLinkManager();
void shutdownDreamLinkManager();

// Unified API (replaces ALL global functions and variables)
inline std::vector<std::shared_ptr<DreamLink>>& getAllDreamLinks() {
    static std::vector<std::shared_ptr<DreamLink>> empty;
    return g_dreamlink_manager ? g_dreamlink_manager->getDreamLinksMutable() : empty;
}

inline std::shared_ptr<DreamLink> getDreamLinkNeedsReconnect() {
    return g_dreamlink_manager ? g_dreamlink_manager->getReconnectCandidate() : nullptr;
}

inline void setDreamLinkNeedsReconnect(std::shared_ptr<DreamLink> link) {
    if (g_dreamlink_manager) {
        g_dreamlink_manager->markForReconnect(link);
    }
}

inline void clearDreamLinkNeedsReconnect() {
    if (g_dreamlink_manager) {
        g_dreamlink_manager->clearReconnectCandidate();
    }
}

inline void createDreamLinkDevices(std::shared_ptr<DreamLink> link, bool gameStart) {
    if (g_dreamlink_manager) {
        g_dreamlink_manager->createDevices(link, gameStart);
    }
}

inline void tearDownDreamLinkDevices(std::shared_ptr<DreamLink> link) {
    if (g_dreamlink_manager) {
        g_dreamlink_manager->tearDownDevices(link);
    }
}