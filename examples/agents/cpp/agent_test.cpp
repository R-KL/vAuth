#include "agent.cpp"

#define CHECK(condition) do { if(!(condition)) { \
    std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition "\n"; return 1; \
} } while(false)

Event event(uint64_t request, std::string state, uint64_t prompt = 0) {
    return {7, request, prompt, std::move(state), "get_assertion", "example.com", ""};
}

int main() {
    Interaction presence{.generation = 7};
    CHECK(presence.accept(event(1, "presence_required")));
    CHECK(presence.can_reply(7, 1, "presence_required"));
    CHECK(!presence.can_reply(8, 1, "presence_required"));
    CHECK(!presence.can_reply(7, 2, "presence_required"));
    presence.replied = true;
    CHECK(!presence.accept(event(1, "presence_required")));
    CHECK(!presence.can_reply(7, 1, "presence_required"));
    CHECK(presence.accept(event(1, "timed_out")));
    CHECK(!presence.can_reply(7, 1, "presence_required"));
    CHECK(!presence.accept(event(1, "presence_required")));
    CHECK(!presence.accept(event(1, "timed_out")));

    Interaction verification{.generation = 7};
    CHECK(verification.accept(event(2, "verification_started")));
    CHECK(verification.accept(event(2, "secret_required", 10)));
    CHECK(verification.can_reply(7, 2, "secret_required", 10));
    CHECK(!verification.can_reply(7, 2, "secret_required", 9));
    verification.replied = true;
    CHECK(!verification.can_reply(7, 2, "secret_required", 10));
    CHECK(verification.accept(event(2, "verification_error")));
    CHECK(!verification.accept(event(2, "secret_required", 10)));
    CHECK(!verification.accept(event(2, "secret_required", 9)));
    CHECK(verification.accept(event(2, "secret_required", 11)));
    verification.cancelled = true;
    CHECK(verification.accept(event(2, "verification_information")));
    CHECK(verification.accept(event(2, "secret_required", 12)));
    CHECK(!verification.can_reply(7, 2, "secret_required", 12));
    CHECK(verification.accept(event(2, "cancelled")));
    CHECK(!verification.accept(event(2, "verification_started")));

    for(const auto& invalid : {
        event(0, "presence_required"), event(3, "future_state"),
        event(3, "presence_required", 1), event(3, "secret_required")
    }) {
        Interaction tracker{.generation = 7};
        CHECK(tracker.accept(event(3, "verification_started")));
        bool rejected = false;
        try { static_cast<void>(tracker.accept(invalid)); }
        catch(const std::runtime_error&) { rejected = true; }
        CHECK(rejected);
        CHECK(tracker.requestId == 0);
    }
    Interaction invalid_transition{.generation = 7};
    CHECK(invalid_transition.accept(event(3, "presence_required")));
    bool rejected = false;
    try { static_cast<void>(invalid_transition.accept(event(3, "verification_succeeded"))); }
    catch(const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    CHECK(invalid_transition.requestId == 0);

    Interaction foreign_generation{.generation = 7};
    CHECK(foreign_generation.accept(event(4, "presence_required")));
    auto foreign = event(4, "presence_required");
    foreign.generation = 8;
    CHECK(!foreign_generation.accept(foreign));
    CHECK(foreign_generation.requestId == 0);
    CHECK(!foreign_generation.accept(event(4, "presence_required")));

    CHECK(console_text("example.com\x1b[2J\n") == "example.com?[2J?");
    std::vector<uint8_t> secret{1, 2, 3};
    { SecretClearGuard guard(secret); }
    CHECK(secret.empty());
    return 0;
}
