#include "dbus/peer_credentials.hpp"
#include "session_validation.hpp"
#include "test_runner.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) do { \
    if(!(condition)) { \
        std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ \
                  << ": " #condition "\n"; \
        return false; \
    } \
} while(false)

constexpr std::string_view SERVICE =
    "org.lamellix.vAuth.PeerCredentialsTest";
constexpr std::string_view OBJECT =
    "/org/lamellix/vAuth/PeerCredentialsTest";
constexpr std::string_view INTERFACE =
    "org.lamellix.vAuth.PeerCredentialsTest";
constexpr std::string_view LOGIN_SERVICE = "org.freedesktop.login1";
constexpr std::string_view LOGIN_MANAGER_PATH =
    "/org/freedesktop/login1";
constexpr std::string_view LOGIN_MANAGER_INTERFACE =
    "org.freedesktop.login1.Manager";
constexpr std::string_view LOGIN_SESSION_PATH =
    "/org/freedesktop/login1/session/test";
constexpr std::string_view LOGIN_SESSION_INTERFACE =
    "org.freedesktop.login1.Session";

constexpr std::string_view USER_PATH = "/org/freedesktop/login1/user/test";
constexpr std::string_view DISPLAY_PATH = "/org/freedesktop/login1/session/display";
constexpr std::string_view USER_INTERFACE = "org.freedesktop.login1.User";

vauth::LoginSessionProperties local_session() {
    return {
        .uid = geteuid(), .active = true, .remote = false,
        .sessionClass = "user", .type = "wayland", .seat = "seat0"
    };
}

sdbus::Slot publish_session(
    sdbus::IObject& object,
    std::string id,
    vauth::LoginSessionProperties properties
) {
    return object.addVTable(
        sdbus::registerProperty("Id").withGetter([id] { return id; }),
        sdbus::registerProperty("User").withGetter([properties] {
            return sdbus::Struct<uint32_t, sdbus::ObjectPath>{
                static_cast<uint32_t>(properties.uid), sdbus::ObjectPath{USER_PATH.data()}
            };
        }),
        sdbus::registerProperty("Active").withGetter([properties] { return properties.active; }),
        sdbus::registerProperty("Remote").withGetter([properties] { return properties.remote; }),
        sdbus::registerProperty("Class").withGetter([properties] { return properties.sessionClass; }),
        sdbus::registerProperty("Type").withGetter([properties] { return properties.type; }),
        sdbus::registerProperty("Seat").withGetter([properties] {
            return sdbus::Struct<std::string, sdbus::ObjectPath>{
                properties.seat, sdbus::ObjectPath{"/"}
            };
        })
    ).forInterface(std::string(LOGIN_SESSION_INTERFACE), sdbus::return_slot);
}

struct ResolutionOptions {
    std::string lookupError;
    std::string userLookupError;
    vauth::LoginSessionProperties direct = local_session();
    vauth::LoginSessionProperties display = local_session();
    uint32_t userUid = static_cast<uint32_t>(geteuid());
    std::string displayId = "display-session";
    sdbus::ObjectPath displayPath{DISPLAY_PATH.data()};
    std::string directId = "test-session";
    bool missingClass = false;
    sdbus::ObjectPath directPath{LOGIN_SESSION_PATH.data()};
};

// Private-bus logind fixture: no dependency on a desktop session or /proc.
class LoginFixture {
public:
    LoginFixture(const std::string& address, const ResolutionOptions& options)
        : connection_(sdbus::createSessionBusConnectionWithAddress(address)) {
        connection_->requestName(sdbus::ServiceName{std::string(LOGIN_SERVICE)});
        manager_ = sdbus::createObject(*connection_, sdbus::ObjectPath{LOGIN_MANAGER_PATH.data()});
        direct_ = sdbus::createObject(*connection_, sdbus::ObjectPath{LOGIN_SESSION_PATH.data()});
        display_ = sdbus::createObject(*connection_, sdbus::ObjectPath{DISPLAY_PATH.data()});
        user_ = sdbus::createObject(*connection_, sdbus::ObjectPath{USER_PATH.data()});
        slots_.push_back(manager_->addVTable(
            sdbus::registerMethod("GetSessionByPID").implementedAs([this, options](uint32_t pid) {
                queriedPid.store(pid);
                if(!options.lookupError.empty())
                    throw sdbus::Error{sdbus::Error::Name{options.lookupError}, "session lookup failed"};
                return options.directPath;
            }),
            sdbus::registerMethod("GetUserByPID").implementedAs([this, options](uint32_t pid) {
                queriedUserPid.store(pid);
                if(!options.userLookupError.empty())
                    throw sdbus::Error{sdbus::Error::Name{options.userLookupError}, "user lookup failed"};
                return sdbus::ObjectPath{USER_PATH.data()};
            })
        ).forInterface(std::string(LOGIN_MANAGER_INTERFACE), sdbus::return_slot));
        if(!options.missingClass)
            slots_.push_back(publish_session(*direct_, options.directId, options.direct));
        slots_.push_back(publish_session(*display_, "display-session", options.display));
        slots_.push_back(user_->addVTable(
            sdbus::registerProperty("UID").withGetter([options] { return options.userUid; }),
            sdbus::registerProperty("Display").withGetter([options] {
                return sdbus::Struct<std::string, sdbus::ObjectPath>{options.displayId, options.displayPath};
            })
        ).forInterface(std::string(USER_INTERFACE), sdbus::return_slot));
        connection_->enterEventLoopAsync();
    }

    ~LoginFixture() {
        try {
            connection_->leaveEventLoop();
        } catch(...) {
        }
    }

    std::atomic<uint32_t> queriedPid = 0;
    std::atomic<uint32_t> queriedUserPid = 0;

private:
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> manager_, direct_, display_, user_;
    std::vector<sdbus::Slot> slots_;
};

bool test_authenticated_peer_credentials(const std::string& address) {
    LoginFixture logind(address, {});

    auto service = sdbus::createSessionBusConnectionWithAddress(address);
    service->requestName(sdbus::ServiceName{std::string(SERVICE)});
    auto object = sdbus::createObject(
        *service,
        sdbus::ObjectPath{std::string(OBJECT)}
    );

    auto inspect = sdbus::registerMethod("Inspect");
    inspect.outputSignature = sdbus::Signature{"uuss"};
    inspect.outputParamNames = {
        "effectiveUid", "pid", "uniqueName", "sessionId"
    };
    inspect.callbackHandler = [&](sdbus::MethodCall call) {
        const auto peer = vauth::dbus::authenticate_bus_peer(call);
        const auto session =
            vauth::dbus::login_session_for_bus_peer(*service, peer);
        auto reply = call.createReply();
        reply << static_cast<uint32_t>(peer.effectiveUid)
              << static_cast<uint32_t>(peer.pid)
              << peer.uniqueName
              << session.id;
        reply.send();
    };
    auto slot = object->addVTable(std::move(inspect)).forInterface(
        std::string(INTERFACE),
        sdbus::return_slot
    );
    service->enterEventLoopAsync();

    auto client = sdbus::createSessionBusConnectionWithAddress(address);
    auto proxy = sdbus::createProxy(
        *client,
        sdbus::ServiceName{std::string(SERVICE)},
        sdbus::ObjectPath{std::string(OBJECT)}
    );
    uint32_t effective_uid = 0;
    uint32_t pid = 0;
    std::string unique_name;
    std::string session;
    proxy->callMethod("Inspect")
        .onInterface(std::string(INTERFACE))
        .storeResultsTo(effective_uid, pid, unique_name, session);

    service->leaveEventLoop();
    static_cast<void>(slot);
    CHECK(effective_uid == static_cast<uint32_t>(geteuid()));
    CHECK(pid == static_cast<uint32_t>(getpid()));
    CHECK(logind.queriedPid.load() == pid);
    CHECK(logind.queriedUserPid.load() == 0);
    CHECK(!unique_name.empty());
    CHECK(unique_name.front() == ':');
    CHECK(session == "test-session");
    return true;
}

bool check_resolution(
    const std::string& address,
    ResolutionOptions options,
    bool accepted,
    bool fallback,
    bool user_lookup
) {
    LoginFixture logind(address, options);
    auto client = sdbus::createSessionBusConnectionWithAddress(address);
    const vauth::dbus::AuthenticatedBusPeer peer{
        .effectiveUid = geteuid(), .pid = getpid(), .uniqueName = ":1.42"
    };
    bool succeeded = false;
    try {
        const auto result = vauth::dbus::login_session_for_bus_peer(*client, peer);
        CHECK(result.usesDisplaySession == fallback);
        CHECK(result.id == (fallback ? "display-session" : options.directId));
        succeeded = true;
    } catch(const std::exception&) {
    }
    CHECK(succeeded == accepted);
    CHECK(logind.queriedPid.load() == static_cast<uint32_t>(getpid()));
    CHECK(logind.queriedUserPid.load() == (user_lookup ? static_cast<uint32_t>(getpid()) : 0));
    return true;
}

bool test_resolution_failures(const std::string& address) {
    ResolutionOptions no_session;
    no_session.lookupError = "org.freedesktop.login1.NoSessionForPID";
    CHECK(check_resolution(address, no_session, true, true, true));

    ResolutionOptions manager;
    manager.direct.sessionClass = "manager";
    manager.direct.type = "unspecified";
    manager.direct.seat.clear();
    CHECK(check_resolution(address, manager, true, true, true));

    for(const auto& error : {"org.freedesktop.DBus.Error.AccessDenied", "org.freedesktop.DBus.Error.NoReply"}) {
        ResolutionOptions options;
        options.lookupError = error;
        CHECK(check_resolution(address, options, false, false, false));
    }
    for(int invalid = 0; invalid != 7; ++invalid) {
        ResolutionOptions options;
        switch(invalid) {
            case 0: options.direct.remote = true; break;
            case 1: options.direct.active = false; break;
            case 2: options.direct.uid = geteuid() + 1; break;
            case 3: options.direct.sessionClass = "greeter"; break;
            case 4: options.directId.clear(); break;
            case 5: options.missingClass = true; break;
            case 6: options.directPath = "/"; break;
        }
        CHECK(check_resolution(address, options, false, false, false));
    }
    auto foreign_manager = manager;
    foreign_manager.direct.uid = geteuid() + 1;
    CHECK(check_resolution(address, foreign_manager, false, false, false));
    auto inactive_manager = manager;
    inactive_manager.direct.active = false;
    CHECK(check_resolution(address, inactive_manager, false, false, false));
    for(int invalid = 0; invalid != 11; ++invalid) {
        auto options = no_session;
        switch(invalid) {
            case 0: options.userUid = static_cast<uint32_t>(geteuid()) + 1; break;
            case 1: options.display.remote = true; break;
            case 2: options.display.active = false; break;
            case 3: options.display.uid = geteuid() + 1; break;
            case 4: options.display.sessionClass = "manager"; break;
            case 5: options.display.type = "tty"; break;
            case 6: options.display.seat.clear(); break;
            case 7: options.displayId.clear(); break;
            case 8: options.displayPath = "/"; break;
            case 9: options.displayId = "different-session"; break;
            case 10: options.userLookupError = "org.freedesktop.login1.NoUserForPID"; break;
        }
        CHECK(check_resolution(address, options, false, false, true));
    }
    auto x11 = no_session;
    x11.display.type = "x11";
    CHECK(check_resolution(address, x11, true, true, true));
    ResolutionOptions tty;
    tty.direct.type = "tty";
    tty.direct.seat.clear();
    CHECK(check_resolution(address, tty, true, false, false));
    return true;
}

bool test_display_binding_invalidation() {
    const auto properties = local_session();
    vauth::validate_agent_session("display-session", properties, geteuid(), "display-session");
    CHECK(!vauth::is_agent_session_current("", geteuid(), false));
    CHECK(!vauth::is_agent_session_current("vauth-no-such-session", geteuid(), true));
    for(const auto& display : {"", "replacement-session"}) {
        bool rejected = false;
        try {
            vauth::validate_agent_session("display-session", properties, geteuid(), display);
        } catch(const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
    }
    return true;
}

}

int main(int argc, char** argv) {
    try {
        if(argc != 2)
            throw std::invalid_argument("Expected private D-Bus address");
        test_support::Runner runner;
        runner.run("authenticated D-Bus peer credentials", [&] {
            return test_authenticated_peer_credentials(argv[1]);
        });
        runner.run("session resolution and fail-closed fallbacks", [&] {
            return test_resolution_failures(argv[1]);
        });
        runner.run("display binding invalidation", test_display_binding_invalidation);
        return runner.finish();
    } catch(const std::exception& error) {
        std::cerr << "Test invocation failed: " << error.what() << '\n';
        return 1;
    }
}
