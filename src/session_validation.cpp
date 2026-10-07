#include "session_validation.hpp"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <systemd/sd-login.h>

namespace vauth {
namespace {

struct FreeDeleter {
    void operator()(char* value) const noexcept {
        std::free(value);
    }
};

using SystemdString = std::unique_ptr<char, FreeDeleter>;

[[noreturn]] void throw_login_error(int result, const char* operation) {
    throw std::system_error(
        -result,
        std::generic_category(),
        operation
    );
}

} // namespace

LoginSessionProperties query_login_session(const std::string& session_id) {
    if(session_id.empty())
        throw std::invalid_argument("Login session ID must not be empty");

    uid_t uid = 0;
    const int uid_result = sd_session_get_uid(session_id.c_str(), &uid);
    if(uid_result < 0)
        throw_login_error(uid_result, "resolve login session user");

    const int active = sd_session_is_active(session_id.c_str());
    if(active < 0)
        throw_login_error(active, "inspect login session activity");

    const int remote = sd_session_is_remote(session_id.c_str());
    if(remote < 0)
        throw_login_error(remote, "inspect login session locality");

    const auto property = [&](auto query, const char* operation) {
        char* raw_value = nullptr;
        const int result = query(session_id.c_str(), &raw_value);
        SystemdString value{raw_value};
        if(result < 0)
            throw_login_error(result, operation);
        if(!value)
            throw std::runtime_error("Login session property is missing");
        return std::string(value.get());
    };

    return {
        .uid = uid,
        .active = active > 0,
        .remote = remote > 0,
        .sessionClass = property(sd_session_get_class, "inspect login session class"),
        .type = property(sd_session_get_type, "inspect login session type"),
        .seat = [&] {
            char* raw_seat = nullptr;
            const int result = sd_session_get_seat(session_id.c_str(), &raw_seat);
            SystemdString seat{raw_seat};
            if(result == -ENODATA)
                return std::string{};
            if(result < 0)
                throw_login_error(result, "inspect login session seat");
            if(!seat)
                throw std::runtime_error("Login session seat is missing");
            return std::string(seat.get());
        }()
    };
}

std::string primary_display_session(uid_t uid) {
    char* raw_session = nullptr;
    const int result = sd_uid_get_display(uid, &raw_session);
    SystemdString session{raw_session};
    if(result < 0)
        throw_login_error(result, "resolve primary graphical login session");
    if(!session || session.get()[0] == '\0')
        throw std::runtime_error("User has no primary login session");
    return session.get();
}

void validate_agent_session(
    const std::string& session_id,
    const LoginSessionProperties& session,
    uid_t expected_uid,
    const std::optional<std::string>& display_session
) {
    if(session_id.empty())
        throw std::runtime_error("Agent login session is empty");
    validate_active_local_session(session, expected_uid);
    if(session.sessionClass != "user")
        throw std::runtime_error("Agent session is not a normal login session");
    if(display_session) {
        if(display_session->empty() || *display_session != session_id)
            throw std::runtime_error("Agent primary display session changed");
        if(session.type != "wayland" && session.type != "x11")
            throw std::runtime_error("Agent display session is not graphical");
        if(session.seat.empty())
            throw std::runtime_error("Agent display session has no local seat");
    }
}

bool is_agent_session_current(
    const std::string& session_id,
    uid_t expected_uid,
    bool uses_display_session
) noexcept {
    try {
        const auto display = uses_display_session
            ? std::optional<std::string>{primary_display_session(expected_uid)}
            : std::nullopt;
        validate_agent_session(
            session_id, query_login_session(session_id), expected_uid, display
        );
        return true;
    } catch(...) {
        return false;
    }
}

void validate_active_local_session(
    const LoginSessionProperties& session,
    uid_t expected_uid
) {
    if(session.uid != expected_uid)
        throw std::runtime_error("Login session belongs to another user");
    if(!session.active)
        throw std::runtime_error("Login session is not active");
    if(session.remote)
        throw std::runtime_error("Remote login sessions are not allowed");
}

void require_active_local_session(
    const std::string& session_id,
    uid_t expected_uid
) {
    validate_active_local_session(
        query_login_session(session_id),
        expected_uid
    );
}

bool is_active_local_session(
    const std::string& session_id,
    uid_t expected_uid
) noexcept {
    try {
        require_active_local_session(session_id, expected_uid);
        return true;
    } catch(...) {
        return false;
    }
}

} // namespace vauth
