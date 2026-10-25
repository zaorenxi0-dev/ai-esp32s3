#include "device_state_machine.h"

#include <algorithm>

#include <esp_log.h>
static const char* TAG = "DeviceStateMachine";

// State name strings for logging
static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "wifi_configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

DeviceStateMachine::DeviceStateMachine() {
    current_state_.store(kDeviceStateUnknown);
}

DeviceStateMachine::~DeviceStateMachine() {
}

const char* DeviceStateMachine::GetStateName(DeviceState state) {
    if (state >= 0 && state <= kDeviceStateFatalError) {
        return STATE_STRINGS[state];
    }
    return STATE_STRINGS[kDeviceStateFatalError + 1];
}

bool DeviceStateMachine::IsValidTransition(DeviceState from,DeviceState to) const{
       // Allow transition to the same state (no-op)
    if (from == to) {
        return true;
    }

    // Define valid state transitions based on the state diagram
    switch (from) {
        case kDeviceStateUnknown:
            // Can only go to starting
            return to == kDeviceStateStarting;

        case kDeviceStateStarting:
            // Can go to wifi configuring or activating
            return to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateActivating;

        case kDeviceStateWifiConfiguring:
            // Can go to activating (after wifi connected) or audio testing
            return to == kDeviceStateActivating ||
                   to == kDeviceStateAudioTesting;

        case kDeviceStateAudioTesting:
            // Can go back to wifi configuring
            return to == kDeviceStateWifiConfiguring;

        case kDeviceStateActivating:
            // Can go to upgrading, idle, or back to wifi configuring (on error)
            return to == kDeviceStateUpgrading ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateUpgrading:
            // Can go to idle (upgrade failed) or activating
            return to == kDeviceStateIdle ||
                   to == kDeviceStateActivating;

        case kDeviceStateIdle:
            // Can go to connecting, listening (manual mode), speaking, activating, upgrading, or wifi configuring
            return to == kDeviceStateConnecting ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateActivating ||
                   to == kDeviceStateUpgrading ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateConnecting:
            // Can go to idle (failed) or listening (success)
            return to == kDeviceStateIdle ||
                   to == kDeviceStateListening;

        case kDeviceStateListening:
            // Can go to speaking or idle
            return to == kDeviceStateSpeaking ||
                   to == kDeviceStateIdle;

        case kDeviceStateSpeaking:
            // Can go to listening or idle
            return to == kDeviceStateListening ||
                   to == kDeviceStateIdle;

        case kDeviceStateFatalError:
            // Cannot transition out of fatal error
            return false;

        default:
            return false;
    }
}

bool DeviceStateMachine::CanTransitionTo(DeviceState target) const {
    return IsValidTransition(current_state_.load(), target);
}


bool DeviceStateMachine::TransitionToState(DeviceState new_state) {
    DeviceState old_state = current_state_.load();
    if (old_state == new_state) {
        return true;
    }
    if(!IsValidTransition(old_state, new_state)) {
        // Log invalid transition attempt
        ESP_LOGI(TAG, "Invalid state transition from %s to %s", GetStateName(old_state), GetStateName(new_state));
        return false;
    }
    current_state_.store(new_state);
    ESP_LOGI(TAG, "Transitioned from %s to %s", GetStateName(old_state), GetStateName(new_state));
    
    NotifyStateChange(old_state, new_state);
    return true;
}

int DeviceStateMachine::AddStateCallback(StateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int listener_id = next_listener_id_++;
    listeners_.emplace_back(listener_id, std::move(callback));
    return listener_id;
}   

void DeviceStateMachine::RemoveStateChangeListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
        [listener_id](const std::pair<int, StateCallback>& listener) {
            return listener.first == listener_id;
        }), listeners_.end());
}

void DeviceStateMachine::NotifyStateChange(DeviceState old_state,DeviceState new_state){
    std::vector<StateCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy.reserve(listeners_.size());
        for (const auto& [id, cb] : listeners_) {
            callbacks_copy.push_back(cb);
        }
    }
    for (const auto& cb : callbacks_copy) {
        cb(old_state, new_state);
    }

}





