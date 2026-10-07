#pragma once

#include <optional>
#include <string>
#include <sys/types.h>

namespace vauth {

struct LoginSessionProperties {
    uid_t uid;
    bool active;
    bool remote;
    std::string sessionClass;
    std::string type;
    std::string seat;
};

[[nodiscard]] LoginSessionProperties query_login_session(
    const std::string& session_id
);

[[nodiscard]] std::string primary_display_session(uid_t uid);

// A display binding is required only for processes shared by the user manager.
void validate_agent_session(
    const std::string& session_id,
    const LoginSessionProperties& session,
    uid_t expected_uid,
    const std::optional<std::string>& display_session
);

[[nodiscard]] bool is_agent_session_current(
    const std::string& session_id,
    uid_t expected_uid,
    bool uses_display_session
) noexcept;

void validate_active_local_session(
    const LoginSessionProperties& session,
    uid_t expected_uid
);

void require_active_local_session(
    const std::string& session_id,
    uid_t expected_uid
);

[[nodiscard]] bool is_active_local_session(
    const std::string& session_id,
    uid_t expected_uid
) noexcept;

} // namespace vauth
