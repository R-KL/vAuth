#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <string>
#include <sys/types.h>

namespace vauth::dbus {

struct AuthenticatedBusPeer {
    uid_t effectiveUid;
    pid_t pid;
    std::string uniqueName;
};

struct AgentLoginSession {
    std::string id;
    bool usesDisplaySession = false;
};

[[nodiscard]] AuthenticatedBusPeer authenticate_bus_peer(
    const sdbus::MethodCall& call
);

[[nodiscard]] AgentLoginSession login_session_for_bus_peer(
    sdbus::IConnection& connection,
    const AuthenticatedBusPeer& peer
);

}
