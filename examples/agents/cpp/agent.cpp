#include "interaction.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string.h>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr char SERVICE[] = "org.lamellix.vAuth";
constexpr char PATH[] = "/org/lamellix/vAuth";
constexpr char INTERFACE[] = "org.lamellix.vAuth.UserInteraction1";
constexpr char BUS_SERVICE[] = "org.freedesktop.DBus";

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() {
        if(fd_ >= 0)
            static_cast<void>(close(fd_));
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset() noexcept {
        if(fd_ >= 0)
            static_cast<void>(close(std::exchange(fd_, -1)));
    }
private:
    int fd_;
};

void write_all(int fd, std::span<const uint8_t> bytes) {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if(count > 0)
            offset += static_cast<std::size_t>(count);
        else if(count < 0 && errno == EINTR)
            continue;
        else
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "write secret pipe");
    }
}

class SecretClearGuard {
public:
    explicit SecretClearGuard(std::vector<uint8_t>& secret) noexcept : secret_(secret) {}
    ~SecretClearGuard() {
        if(!secret_.empty())
            explicit_bzero(secret_.data(), secret_.size());
        secret_.clear();
    }
    SecretClearGuard(const SecretClearGuard&) = delete;
    SecretClearGuard& operator=(const SecretClearGuard&) = delete;
private:
    std::vector<uint8_t>& secret_;
};

// RP IDs and operation strings are untrusted terminal presentation text.
std::string console_text(std::string_view value) {
    std::string result;
    for(const unsigned char byte : value) {
        result += byte < 0x20 || byte == 0x7f ? '?' : static_cast<char>(byte);
    }
    return result;
}

class Agent {
public:
    Agent() : connection_(sdbus::createSystemBusConnection()) {
        busProxy_ = sdbus::createProxy(
            *connection_, sdbus::ServiceName{BUS_SERVICE}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
        );
        ownerSlot_ = busProxy_->uponSignal("NameOwnerChanged").onInterface(BUS_SERVICE).call(
            [this](const std::string& name, const std::string&, const std::string& next) {
                if(name == SERVICE && !owner_.empty() && next != owner_)
                    stop();
            }, sdbus::return_slot
        );
        busProxy_->callMethod("GetNameOwner").onInterface(BUS_SERVICE)
            .withArguments(std::string(SERVICE)).storeResultsTo(owner_);
        if(owner_.empty() || owner_.front() != ':')
            throw std::runtime_error("Daemon has no unique bus name");
        proxy_ = sdbus::createProxy(
            *connection_, sdbus::ServiceName{owner_}, sdbus::ObjectPath{PATH}
        );
        stateSlot_ = proxy_->registerSignalHandler(
            sdbus::InterfaceName{INTERFACE}, sdbus::SignalName{"StateChanged"},
            [this](sdbus::Signal signal) {
                if(stopped_)
                    return;
                if(signal.getSender() == nullptr || std::string_view(signal.getSender()) != owner_)
                    return;
                try {
                    Event event{};
                    signal >> event.generation >> event.requestId >> event.promptId
                           >> event.state >> event.operation >> event.relyingPartyId >> event.message;
                    if(!signal.isAtEnd(true) || events_.size() == 32) {
                        stop();
                        return;
                    }
                    events_.push_back(std::move(event));
                } catch(...) {
                    stop();
                }
            }, sdbus::return_slot
        );
        auto request = proxy_->createMethodCall(
            sdbus::InterfaceName{INTERFACE}, sdbus::MethodName{"RegisterAgent"}
        );
        auto reply = proxy_->callMethod(request);
        reply >> interaction_.generation;
        if(interaction_.generation == 0 || !reply.isAtEnd(true))
            throw std::runtime_error("Daemon returned an invalid generation");
        std::string current_owner;
        busProxy_->callMethod("GetNameOwner").onInterface(BUS_SERVICE)
            .withArguments(std::string(SERVICE)).storeResultsTo(current_owner);
        if(stopped_ || current_owner != owner_)
            throw std::runtime_error("Daemon owner changed during registration");
        std::cout << "registered generation " << interaction_.generation << std::endl;
    }

    ~Agent() {
        interaction_.invalidate();
        try {
            reply_method("UnregisterAgent");
        } catch(...) {
        }
    }
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    void run() {
        while(true) {
            while(connection_->processPendingEvent()) {}
            apply_events();
            if(stopped_)
                throw std::runtime_error("Agent lifecycle invalidated");
            const auto bus = connection_->getEventLoopPollData();
            if(bus.fd < 0)
                throw std::runtime_error("Bus disconnected");
            std::array<pollfd, 3> descriptors{{
                {bus.fd, bus.events, 0}, {bus.eventFd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}
            }};
            int result;
            do {
                result = poll(descriptors.data(), descriptors.size(), bus.getPollTimeout());
            } while(result < 0 && errno == EINTR);
            if(result < 0)
                throw std::system_error(errno, std::generic_category(), "poll");
            if(descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL))
                throw std::runtime_error("Bus disconnected");
            // Drain terminal events before examining any console answer.
            while(connection_->processPendingEvent()) {}
            apply_events();
            if(stopped_)
                throw std::runtime_error("Agent lifecycle invalidated");
            if(descriptors[2].revents & (POLLERR | POLLNVAL))
                throw std::runtime_error("Console input unavailable");
            if(descriptors[2].revents & (POLLIN | POLLHUP)) {
                std::array<char, 256> bytes{};
                ssize_t count;
                do {
                    count = read(STDIN_FILENO, bytes.data(), bytes.size());
                } while(count < 0 && errno == EINTR);
                if(count == 0)
                    return;
                if(count < 0)
                    throw std::system_error(errno, std::generic_category(), "read console");
                for(std::size_t index = 0; index < static_cast<std::size_t>(count); ++index)
                    accept_input(bytes[index]);
            }
        }
    }

    void submit_secret(uint64_t generation, uint64_t request_id,
        uint64_t prompt_id, std::vector<uint8_t>& secret) {
        SecretClearGuard clearOnExit(secret);
        try {
            if(!interaction_.can_reply(generation, request_id, "secret_required", prompt_id))
                throw std::runtime_error("Secret prompt is not active");
            if(secret.size() > 1024 || std::ranges::find(secret, uint8_t{0}) != secret.end())
                throw std::invalid_argument("Secret must be at most 1024 bytes without NUL");
            std::array<int, 2> descriptors{};
            if(pipe2(descriptors.data(), O_CLOEXEC) != 0)
                throw std::system_error(errno, std::generic_category(), "pipe2");
            UniqueFd readEnd(descriptors[0]);
            UniqueFd writeEnd(descriptors[1]);
            write_all(writeEnd.get(), secret);
            writeEnd.reset();
            interaction_.replied = true;
            reply_method("SubmitSecret", generation, request_id, prompt_id, sdbus::UnixFd{readEnd.get()});
        } catch(...) {
            stop();
            throw;
        }
    }

private:
    template<typename... Arguments>
    void reply_method(const char* method, const Arguments&... arguments) {
        auto request = proxy_->createMethodCall(
            sdbus::InterfaceName{INTERFACE}, sdbus::MethodName{method}
        );
        (request << ... << arguments);
        auto reply = proxy_->callMethod(request);
        if(!reply.isEmpty())
            throw std::runtime_error("Invalid daemon reply");
    }

    void stop() noexcept {
        stopped_ = true;
        interaction_.invalidate();
        input_.clear();
        events_.clear();
    }

    void apply_events() {
        while(!events_.empty()) {
            Event event = std::move(events_.front());
            events_.pop_front();
            if(stopped_)
                return;
            if(!interaction_.accept(event)) {
                if(interaction_.requestId == 0)
                    input_.clear();
                continue;
            }
            std::cout << event.state << ": operation " << console_text(event.operation)
                      << " for RP " << console_text(event.relyingPartyId) << std::endl;
            if(!event.message.empty())
                std::cout << console_text(event.message) << std::endl;
            if(event.state == "presence_required") {
                std::cout << "Reply " << event.generation << ':' << event.requestId << " y|n|c:" << std::endl;
            } else if(event.state == "secret_required" && !interaction_.cancelled) {
                // A real UI may replace this cancellation with submit_secret().
                interaction_.replied = interaction_.cancelled = true;
                reply_method("CancelInteraction", event.generation, event.requestId);
            }
            if(interaction_.requestId == 0)
                input_.clear();
        }
    }

    void accept_input(char byte) {
        if(byte != '\n') {
            if(!inputOversized_) {
                if(input_.size() == 128) {
                    input_.clear();
                    inputOversized_ = true;
                } else {
                    input_ += byte;
                }
            }
            return;
        }
        const bool oversized = std::exchange(inputOversized_, false);
        std::string line = std::exchange(input_, {});
        if(oversized)
            return;
        std::istringstream fields(line);
        std::string token, answer, extra;
        if(!(fields >> token >> answer) || (fields >> extra))
            return;
        const std::string expected = std::to_string(interaction_.generation) + ':' +
            std::to_string(interaction_.requestId);
        if(token != expected || (answer != "y" && answer != "n" && answer != "c"))
            return;
        if(answer == "c") {
            if(interaction_.requestId == 0 || interaction_.replied || interaction_.cancelled)
                return;
        } else if(!interaction_.can_reply(interaction_.generation, interaction_.requestId, "presence_required")) {
            return;
        }
        interaction_.replied = true;
        interaction_.cancelled = answer == "c";
        if(answer == "c")
            reply_method("CancelInteraction", interaction_.generation, interaction_.requestId);
        else
            reply_method("RespondToPresence", interaction_.generation, interaction_.requestId, answer == "y");
    }

    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IProxy> busProxy_;
    std::unique_ptr<sdbus::IProxy> proxy_;
    sdbus::Slot ownerSlot_;
    sdbus::Slot stateSlot_;
    std::string owner_;
    Interaction interaction_;
    std::deque<Event> events_;
    std::string input_;
    bool inputOversized_ = false;
    bool stopped_ = false;
};

} // namespace

#ifndef VAUTH_AGENT_EXAMPLE_TESTING
int main() {
    try {
        Agent agent;
        agent.run();
    } catch(...) {
        std::cerr << "vAuth agent example: stopped after a connection or interaction error\n";
        return 1;
    }
}
#endif
