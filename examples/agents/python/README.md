# Python example

This example requires Python 3.9 or later and dbus-next:

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 agent.py
```

Run it unprivileged from an active local login session (including an eligible
desktop launch through the systemd user manager). It handles presence requests and
cancels secret requests. The `submit_secret` helper demonstrates D-Bus Unix
file-descriptor transfer for integration with a protected input widget.

Use the full token and action printed by the prompt, for example `7:12 y`,
`7:12 n`, or `7:12 c`. Console input uses the asyncio event loop so terminal
events invalidate pending decisions immediately. The bus connection explicitly
enables Unix descriptor negotiation. No password is read from the console.
See the [shared example rules](../README.md) before adapting it.

With the virtual environment active, run the regression tests:

```sh
python3 -m unittest -v test_agent
```
