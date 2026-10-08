#!/usr/bin/env python3
"""Exercise all console agents on a private bus, without vAuth, PAM or logind."""

import argparse
import asyncio
import contextlib
import os
from pathlib import Path
import sys
import unittest

from dbus_next import Message, MessageType
from dbus_next.aio import MessageBus

SERVICE = "org.lamellix.vAuth"
PATH = "/org/lamellix/vAuth"
INTERFACE = SERVICE + ".UserInteraction1"
COMMANDS = []


async def close_bus(bus):
    if bus.connected:
        bus.disconnect()
    with contextlib.suppress(Exception):
        await bus.wait_for_disconnect()
    # dbus-next 0.2.3 shuts down but does not close its socket/stream. Explicit
    # fixture ownership avoids leaking descriptors across repeated test buses.
    bus._stream.close()
    bus._sock.close()


class Harness:
    def __init__(self, command, early=False):
        self.command = command
        self.early = early
        self.agent_name = ""
        self.calls = asyncio.Queue()
        self.lines = []
        self.output_changed = asyncio.Condition()
        self.reply_error = False
        self.reply_payload = False
        self.process = self.bus_process = self.bus = self.reader = None

    async def __aenter__(self):
        try:
            self.bus_process = await asyncio.create_subprocess_exec(
                "dbus-daemon", "--session", "--nofork", "--print-address=1",
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
            )
            address = (await asyncio.wait_for(self.bus_process.stdout.readline(), 3)).decode().strip()
            self.address = address
            self.bus = await MessageBus(bus_address=address, negotiate_unix_fd=True).connect()
            await self.bus.request_name(SERVICE)
            self.bus.add_message_handler(self.on_call)
            environment = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address, PYTHONDONTWRITEBYTECODE="1")
            self.process = await asyncio.create_subprocess_exec(
                *self.command, env=environment, stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
            )
            self.reader = asyncio.create_task(self.read_output())
            await self.wait_line("registered generation 7")
            return self
        except BaseException:
            await self.__aexit__(None, None, None)
            raise

    async def __aexit__(self, *unused):
        if self.process is not None and self.process.returncode is None:
            self.process.terminate()
            try:
                await asyncio.wait_for(self.process.wait(), 3)
            except asyncio.TimeoutError:
                self.process.kill()
                await self.process.wait()
        if self.reader is not None:
            await self.reader
        if self.bus is not None:
            await close_bus(self.bus)
        if self.bus_process is not None and self.bus_process.returncode is None:
            self.bus_process.terminate()
            await self.bus_process.wait()

    def on_call(self, message):
        if message.message_type != MessageType.METHOD_CALL or message.path != PATH or message.interface != INTERFACE:
            return None
        if message.member == "RegisterAgent":
            self.agent_name = message.sender
            if message.signature != "":
                return Message.new_error(message, "org.freedesktop.DBus.Error.InvalidArgs", "wrong signature")
            if self.early:
                self.send_state(1, "presence_required")
                self.send_state(1, "presence_required")
            return Message.new_method_return(message, "t", [7])
        if message.member == "UnregisterAgent":
            return Message.new_method_return(message)
        self.calls.put_nowait((message.member, message.signature, message.body))
        if self.reply_error:
            return Message.new_error(message, SERVICE + ".Error.InvalidInteraction", "injected stale registration")
        if self.reply_payload:
            return Message.new_method_return(message, "s", ["unexpected"])
        return Message.new_method_return(message)

    def send_state(self, request, state, prompt=0, generation=7, bus=None, extra=False):
        body = [generation, request, prompt, state, "future_operation", "example.com", ""]
        if extra:
            body.append("unexpected")
        return (bus or self.bus).send(Message(
            message_type=MessageType.SIGNAL, destination=self.agent_name,
            path=PATH, interface=INTERFACE, member="StateChanged",
            signature="tttsssss" if extra else "tttssss", body=body,
        ))

    async def read_output(self):
        while line := await self.process.stdout.readline():
            async with self.output_changed:
                self.lines.append(line.decode(errors="replace").rstrip())
                self.output_changed.notify_all()

    async def wait_line(self, text):
        async def wait():
            async with self.output_changed:
                await self.output_changed.wait_for(lambda: any(text in line for line in self.lines))
        try:
            await asyncio.wait_for(wait(), 3)
        except asyncio.TimeoutError:
            raise AssertionError(f"missing output {text!r}; received {self.lines!r}") from None

    async def answer(self, text):
        self.process.stdin.write((text + "\n").encode())
        await self.process.stdin.drain()

    async def next_call(self):
        return await asyncio.wait_for(self.calls.get(), 3)

    async def no_call(self, test):
        with test.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(self.calls.get(), 0.15)

    async def expect_exit(self, test):
        await asyncio.wait_for(self.process.wait(), 3)
        test.assertNotEqual(self.process.returncode, 0)


class AgentComplianceTests(unittest.IsolatedAsyncioTestCase):
    async def test_early_events_duplicate_presence_and_late_input(self):
        for name, command in COMMANDS:
            with self.subTest(language=name):
                async with Harness(command, early=True) as agent:
                    await agent.wait_line("Reply 7:1 ")
                    self.assertEqual(sum("Reply 7:1 " in line for line in agent.lines), 1)
                    await agent.answer("7:1 y")
                    self.assertEqual(await agent.next_call(), ("RespondToPresence", "ttb", [7, 1, True]))
                    await agent.answer("7:1 n")
                    await agent.no_call(self)
                    await agent.send_state(1, "presence_approved")
                    await agent.wait_line("presence_approved:")
                    await agent.send_state(1, "presence_required")
                    await agent.answer("7:1 y")
                    await agent.no_call(self)
                    await agent.send_state(2, "presence_required")
                    await agent.wait_line("Reply 7:2 ")
                    await agent.send_state(2, "timed_out")
                    await agent.wait_line("timed_out:")
                    await agent.send_state(3, "presence_required")
                    await agent.wait_line("Reply 7:3 ")
                    await agent.answer("7:2 y")
                    await agent.answer("y")
                    await agent.no_call(self)
                    await agent.answer("7:3 n")
                    self.assertEqual(await agent.next_call(), ("RespondToPresence", "ttb", [7, 3, False]))

    async def test_prompt_history_and_one_shot_cancellation(self):
        for name, command in COMMANDS:
            with self.subTest(language=name):
                async with Harness(command) as agent:
                    await agent.send_state(1, "verification_started")
                    await agent.send_state(1, "secret_required", 10)
                    self.assertEqual(await agent.next_call(), ("CancelInteraction", "tt", [7, 1]))
                    await agent.send_state(1, "verification_error")
                    await agent.send_state(1, "secret_required", 10)
                    await agent.send_state(1, "secret_required", 9)
                    await agent.send_state(1, "secret_required", 11)
                    await agent.answer("7:1 c")
                    await agent.no_call(self)
                    self.assertEqual(sum("secret_required:" in line for line in agent.lines), 2)
                    await agent.send_state(1, "cancelled")
                    await agent.wait_line("cancelled:")
                    await agent.send_state(1, "verification_started")
                    await agent.no_call(self)

    async def test_foreign_sender_and_generation_are_rejected(self):
        for name, command in COMMANDS:
            with self.subTest(language=name):
                async with Harness(command) as agent:
                    foreign = await MessageBus(bus_address=agent.address).connect()
                    try:
                        await agent.send_state(99, "presence_required", bus=foreign)
                        await agent.send_state(1, "presence_required")
                        await agent.wait_line("Reply 7:1 ")
                        await agent.answer("7:99 y")
                        await agent.no_call(self)
                        await agent.send_state(1, "presence_required", generation=8)
                        # A later valid event is a barrier behind the mismatch.
                        await agent.send_state(2, "presence_required")
                        await agent.wait_line("Reply 7:2 ")
                        await agent.answer("7:1 y")
                        await agent.no_call(self)
                        await agent.answer("7:2 c")
                        self.assertEqual(await agent.next_call(), ("CancelInteraction", "tt", [7, 2]))
                    finally:
                        await close_bus(foreign)

    async def test_invalid_events_and_method_errors_fail_closed(self):
        for name, command in COMMANDS:
            for failure in ("unknown", "prompt", "transition", "signature", "method", "reply"):
                with self.subTest(language=name, failure=failure):
                    async with Harness(command) as agent:
                        await agent.send_state(1, "presence_required")
                        await agent.wait_line("Reply 7:1 ")
                        if failure == "unknown":
                            await agent.send_state(1, "future_state")
                        elif failure == "prompt":
                            await agent.send_state(1, "presence_required", 1)
                        elif failure == "transition":
                            await agent.send_state(1, "verification_succeeded")
                        elif failure == "signature":
                            await agent.send_state(1, "presence_required", extra=True)
                        else:
                            agent.reply_error = failure == "method"
                            agent.reply_payload = failure == "reply"
                            await agent.answer("7:1 y")
                            self.assertEqual(await agent.next_call(), ("RespondToPresence", "ttb", [7, 1, True]))
                        await agent.expect_exit(self)
                        await agent.no_call(self)

    async def test_daemon_and_bus_loss_while_waiting_for_input(self):
        for name, command in COMMANDS:
            for failure in ("owner", "bus"):
                with self.subTest(language=name, failure=failure):
                    async with Harness(command) as agent:
                        await agent.send_state(1, "presence_required")
                        await agent.wait_line("Reply 7:1 ")
                        if failure == "owner":
                            await agent.bus.release_name(SERVICE)
                        else:
                            agent.bus_process.terminate()
                            await agent.bus_process.wait()
                        await agent.expect_exit(self)
                        self.assertTrue(agent.calls.empty())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp", required=True, type=Path)
    parser.add_argument("--go", required=True, type=Path)
    arguments = parser.parse_args()
    COMMANDS.extend([
        ("C++", [str(arguments.cpp.resolve())]),
        ("Python", [sys.executable, str(Path(__file__).resolve().parents[1] / "python" / "agent.py")]),
        ("Go", [str(arguments.go.resolve())]),
    ])
    unittest.main(argv=[sys.argv[0]], verbosity=2)
