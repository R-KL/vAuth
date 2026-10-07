#include "peer_credentials.hpp"
#include "session_validation.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <tuple>
#include <utility>

namespace vauth::dbus {
namespace {

constexpr std::string_view LOGIN_SERVICE = "org.freedesktop.login1";
constexpr std::string_view LOGIN_MANAGER_PATH =
    "/org/freedesktop/login1";
constexpr std::string_view LOGIN_MANAGER_INTERFACE =
    "org.freedesktop.login1.Manager";
constexpr std::string_view LOGIN_SESSION_INTERFACE =
    "org.freedesktop.login1.Session";

constexpr std::string_view LOGIN_USER_INTERFACE = "org.freedesktop.login1.User";

vauth::LoginSessionProperties session_properties(sdbus::IProxy& session) {
    const auto property = [&](const char* name) {
        return session.getProperty(name)
            .onInterface(std::string(LOGIN_SESSION_INTERFACE));
    };
    const auto user = property("User").get<sdbus::Struct<uint32_t, sdbus::ObjectPath>>();
    const auto seat = property("Seat").get<sdbus::Struct<std::string, sdbus::ObjectPath>>();
    return {
        .uid = static_cast<uid_t>(std::get<0>(user)),
        .active = property("Active").get<bool>(),
        .remote = property("Remote").get<bool>(),
        .sessionClass = property("Class").get<std::string>(),
        .type = property("Type").get<std::string>(),
        .seat = std::get<0>(seat)
    };
}

}

AuthenticatedBusPeer authenticate_bus_peer(const sdbus::MethodCall& call) {
    const char* sender = call.getSender();
    if(sender == nullptr || sender[0] != ':')
        throw std::runtime_error("Agent has no authenticated unique bus name");

    // The bus authenticates the effective UID directly. Requesting the real
    // UID may require a /proc lookup, which is intentionally unavailable under
    // ProtectProc=invisible when the agent belongs to another user.
    const uid_t effective_uid = call.getCredsEuid();
    const pid_t pid = call.getCredsPid();
    if(pid <= 0)
        throw std::runtime_error("Agent has no authenticated process ID");

    return {
        .effectiveUid = effective_uid,
        .pid = pid,
        .uniqueName = sender
    };
}

AgentLoginSession login_session_for_bus_peer(
    sdbus::IConnection& connection,
    const AuthenticatedBusPeer& peer
) {
    const pid_t pid = peer.pid;
    if(
        pid <= 0 ||
        static_cast<uintmax_t>(pid) > std::numeric_limits<uint32_t>::max()
    ) {
        throw std::invalid_argument("Agent process ID is out of range");
    }

    auto manager = sdbus::createProxy(
        connection,
        sdbus::ServiceName{std::string(LOGIN_SERVICE)},
        sdbus::ObjectPath{std::string(LOGIN_MANAGER_PATH)}
    );
    sdbus::ObjectPath session_path;
    bool no_direct_session = false;
    try {
        manager->callMethod("GetSessionByPID")
            .onInterface(std::string(LOGIN_MANAGER_INTERFACE))
            .withArguments(static_cast<uint32_t>(pid))
            .storeResultsTo(session_path);
    } catch(const sdbus::Error& error) {
        if(error.getName() != "org.freedesktop.login1.NoSessionForPID")
            throw;
        no_direct_session = true;
    }

    const auto session_proxy = [&connection](const sdbus::ObjectPath& path) {
        if(path.empty() || path == "/")
            throw std::runtime_error("Agent login session path is missing");
        return sdbus::createProxy(
            connection,
            sdbus::ServiceName{std::string(LOGIN_SERVICE)},
            path
        );
    };
    if(!no_direct_session) {
        auto session = session_proxy(session_path);
        const auto properties = session_properties(*session);
        vauth::validate_active_local_session(properties, peer.effectiveUid);
        if(properties.sessionClass != "manager") {
            std::string id = session->getProperty("Id")
                .onInterface(std::string(LOGIN_SESSION_INTERFACE))
                .get<std::string>();
            vauth::validate_agent_session(id, properties, peer.effectiveUid, std::nullopt);
            return {.id = std::move(id)};
        }
    }

    // Only logind's absence-of-session result or a manager session permits this
    // fallback. A remote or inactive direct login must never be rebound.
    sdbus::ObjectPath user_path;
    manager->callMethod("GetUserByPID")
        .onInterface(std::string(LOGIN_MANAGER_INTERFACE))
        .withArguments(static_cast<uint32_t>(pid))
        .storeResultsTo(user_path);
    if(user_path.empty() || user_path == "/")
        throw std::runtime_error("Agent has no logind user");
    auto user = sdbus::createProxy(
        connection, sdbus::ServiceName{std::string(LOGIN_SERVICE)}, user_path
    );
    const uint32_t uid = user->getProperty("UID")
        .onInterface(std::string(LOGIN_USER_INTERFACE)).get<uint32_t>();
    if(static_cast<uid_t>(uid) != peer.effectiveUid)
        throw std::runtime_error("Logind user does not match agent UID");
    const auto display = user->getProperty("Display")
        .onInterface(std::string(LOGIN_USER_INTERFACE))
        .get<sdbus::Struct<std::string, sdbus::ObjectPath>>();
    const std::string display_id = std::get<0>(display);
    auto session = session_proxy(std::get<1>(display));
    std::string session_id = session->getProperty("Id")
        .onInterface(std::string(LOGIN_SESSION_INTERFACE))
        .get<std::string>();
    vauth::validate_agent_session(
        session_id, session_properties(*session), peer.effectiveUid, display_id
    );
    return {.id = std::move(session_id), .usesDisplaySession = true};
}

}
