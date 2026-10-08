# Custom agent examples

These minimal console programs demonstrate version 1 of the vAuth
user-interaction API:

- [`cpp/`](cpp/) uses sdbus-c++.
- [`python/`](python/) uses dbus-next.
- [`go/`](go/) uses godbus/dbus.

Run only one agent at a time, unprivileged, from an active local logind session.
Desktop launches through the same user's systemd user manager are supported
when the daemon can bind them to an eligible active local graphical session;
see the API's [trust model](../../docs/agent-api.md#trust-model).
Each example
asks before approving `presence_required`, tracks the registration generation
and request and prompt IDs, and cancels secret prompts by default. It includes a
secret-submission helper to show correct Unix-file-descriptor transfer, but a
real UI must supply that helper with a bounded mutable buffer from a protected
input widget and erase the buffer after the call.

For presence, type the full generation/request token printed by the program
followed by one action: for example, `7:12 y` approves generation 7's request 12,
`7:12 n` denies it, and `7:12 c` cancels it. An answer without the matching token
is ignored. Input is bounded; terminal events continue to be processed while
the console waits, so a delayed answer cannot approve a replacement request.
Unknown future operations are displayed as generic operation context, with no
additional authority inferred from their names. Terminal control characters
in presentation text are escaped or replaced.

The examples verify the daemon's unique bus owner, install signal subscriptions
before registration, filter early events using the returned generation, and
reject stale request/prompt IDs and duplicate replies. Cancellation and replies
are one-shot. Method errors, unknown states, invalid transitions, daemon-owner
changes, and bus disconnects invalidate local interaction state. Restart the
example after registration or session errors; it does not automatically
re-register or move an existing interaction to another session.

The secret helpers require the captured generation, current request ID, current
prompt ID, and a mutable buffer. They enforce the current `secret_required`
state, the 1024-byte bound and NUL prohibition, close the pipe writer before
transfer, close owned descriptors, and erase buffers on success and failure.
They do not collect secrets from stdin. A production UI must clear its input
widget before calling the helper and integrate login-session changes with its
UI lifecycle; version 1 has no separate registration-revocation signal.

These programs are educational starting points, not hardened user interfaces.
Read the complete [`agent API and security requirements`](../../docs/agent-api.md)
before adapting one.

## Regression checks

Each language directory documents its unit tests. The cross-language checks use
`dbus-daemon`, Python with the Python example's `dbus-next` dependency, and
prebuilt C++ and Go examples. From the repository root:

```sh
python3 examples/agents/tests/check_agents.py \
  --cpp examples/agents/cpp/build/vauth-agent-example \
  --go examples/agents/go/vauth-agent-example
```

The harness creates a private bus and fake daemon. It never registers an agent
with the live system service and needs no root, TPM, PAM, or desktop session.
It checks early events, malformed wire signatures and states, explicit presence
decisions, duplicate replies, expired input, prompt replay, method failures,
foreign senders, generation changes, and daemon/bus loss.
