#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

struct Event {
    uint64_t generation;
    uint64_t requestId;
    uint64_t promptId;
    std::string state;
    std::string operation;
    std::string relyingPartyId;
    std::string message;
};

inline bool presence_terminal(std::string_view state) {
    return state == "presence_approved" || state == "presence_denied";
}

inline bool verification_terminal(std::string_view state) {
    return state == "verification_succeeded" || state == "verification_failed";
}

inline bool common_terminal(std::string_view state) {
    return state == "cancelled" || state == "timed_out";
}

inline bool verification_progress(std::string_view state) {
    return state == "verification_started" || state == "verification_information" ||
        state == "verification_error" || state == "secret_required";
}

struct Interaction {
    uint64_t generation = 0;
    uint64_t requestId = 0;
    uint64_t latestRequest = 0;
    uint64_t promptId = 0;
    uint64_t latestPrompt = 0;
    std::string state;
    bool presence = false;
    bool replied = false;
    bool cancelled = false;

    void invalidate() noexcept {
        requestId = promptId = 0;
        state.clear();
        replied = cancelled = false;
    }

    bool accept(const Event& event) {
        if(event.generation != generation) {
            invalidate();
            return false;
        }
        const auto& next = event.state;
        const bool start = next == "presence_required" || next == "verification_started";
        const bool terminal = presence_terminal(next) || verification_terminal(next) || common_terminal(next);
        if(event.requestId == 0 || (!start && !terminal && !verification_progress(next)) ||
            ((next == "secret_required") != (event.promptId != 0))) {
            invalidate();
            throw std::runtime_error("Invalid interaction event");
        }
        if(requestId == 0) {
            if(!start || event.requestId <= latestRequest)
                return false;
            requestId = latestRequest = event.requestId;
            latestPrompt = 0;
            presence = next == "presence_required";
        } else if(requestId != event.requestId) {
            return false;
        } else if(next == "presence_required" && presence) {
            return false;
        }
        if((presence && next != "presence_required" && !presence_terminal(next) && !common_terminal(next)) ||
            (!presence && !verification_progress(next) && !verification_terminal(next) && !common_terminal(next))) {
            invalidate();
            throw std::runtime_error("Invalid interaction transition");
        }
        if(next == "secret_required") {
            if(event.promptId <= latestPrompt)
                return false;
            latestPrompt = event.promptId;
        }
        state = next;
        promptId = event.promptId;
        if(terminal)
            invalidate();
        else if(!presence)
            replied = false;
        return true;
    }

    bool can_reply(uint64_t expected_generation, uint64_t request_id,
        std::string_view expected_state, uint64_t prompt_id = 0) const noexcept {
        return expected_generation == generation && request_id != 0 && request_id == requestId &&
            state == expected_state && !replied && !cancelled &&
            (expected_state != "secret_required" || (prompt_id != 0 && prompt_id == promptId));
    }
};
