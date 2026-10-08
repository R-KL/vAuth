# C++ example

This example requires a C++20 compiler and sdbus-c++ development files:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/vauth-agent-example
```

Run it unprivileged from an active local login session (including an eligible
desktop launch through the systemd user manager). It handles presence requests and
cancels secret requests. The `submit_secret` method demonstrates Unix-file-
descriptor transfer for integration with a protected input widget.

Use the full token and action printed by the prompt, for example `7:12 y`,
`7:12 n`, or `7:12 c`. D-Bus and console input share one event-loop thread;
waiting for input does not delay terminal events. No password is read from
the console. See the [shared example rules](../README.md) before adapting it.

Run the isolated regression tests after building:

```sh
ctest --test-dir build --output-on-failure
```

`-DBUILD_TESTING=OFF` builds only the example.
