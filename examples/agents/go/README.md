# Go example

This example requires Go 1.22 or later and godbus/dbus:

```sh
go mod download
go build -o vauth-agent-example .
go run .
```

Run it unprivileged from an active local login session (including an eligible
desktop launch through the systemd user manager). It handles presence requests and
cancels secret requests. The `submitSecret` function demonstrates Unix-file-
descriptor transfer for integration with a protected input widget.

Use the full token and action printed by the prompt, for example `7:12 y`,
`7:12 n`, or `7:12 c`. A bounded console reader runs separately from the signal
loop so terminal events continue to invalidate pending decisions. No password
is read from the console. See the [shared example rules](../README.md) before
adapting it.

Run the regression tests:

```sh
go test -timeout 30s ./...
```
