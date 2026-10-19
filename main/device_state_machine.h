#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "device_state.h"


class DeviceStateMachine {
public:
    DeviceStateMachine();
    ~DeviceStateMachine();

    DeviceStateMachine(const DeviceStateMachine&) = delete;
    DeviceStateMachine& operator=(const DeviceStateMachine&) = delete;

    DeviceState GetCurrentState() const {
        return current_state_.load();
    }
    const char* GetStateName(DeviceState state);

    bool TransitionToState(DeviceState new_state);

    bool CanTransitionTo(DeviceState target) const;

    using StateCallback = std::function<void(DeviceState, DeviceState)>;

    int AddStateCallback(StateCallback callback);

    void RemoveStateChangeListener(int listener_id); 
    
    void NotifyStateChange(DeviceState old_state, DeviceState new_state);
private:


    std::atomic<DeviceState> current_state_{kDeviceStateUnknown};
    std::vector<std::pair<int, StateCallback>> listeners_;
    int next_listener_id_{0};
    std::mutex mutex_;

    void NotifyStateChanfe(DeviceState old_state,DeviceState new_state);

    bool IsValidTransition(DeviceState from,DeviceState to) const;
};
