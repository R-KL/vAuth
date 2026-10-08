#!/usr/bin/env python3
"""Console presence agent. Secret prompts are cancelled, never read from stdin."""

import asyncio
import os
import sys
from collections import deque
from dataclasses import dataclass
from typing import Optional

from dbus_next import BusType, Message, MessageType
from dbus_next.aio import MessageBus

SERVICE = "org.lamellix.vAuth"
PATH = "/org/lamellix/vAuth"
INTERFACE = "org.lamellix.vAuth.UserInteraction1"
BUS_SERVICE = "org.freedesktop.DBus"
BUS_PATH = "/org/freedesktop/DBus"
START_STATES = {"presence_required", "verification_started"}
PRESENCE_TERMINALS = {"presence_approved", "presence_denied"}
VERIFICATION_TERMINALS = {"verification_succeeded", "verification_failed"}
TERMINAL_STATES = PRESENCE_TERMINALS | VERIFICATION_TERMINALS | {"cancelled", "timed_out"}
PROGRESS_STATES = {
    "verification_started", "verification_information", "verification_error", "secret_required",
}
KNOWN_STATES = START_STATES | TERMINAL_STATES | PROGRESS_STATES


@dataclass
class Interaction:
    generation: int = 0
    request_id: int = 0
    latest_request: int = 0
    state: str = ""
    prompt_id: int = 0
    latest_prompt: int = 0
    replied: bool = False
    cancelled: bool = False
    presence: bool = False

    def invalidate(self) -> None:
        self.request_id = self.prompt_id = 0
        self.state = ""
        self.replied = self.cancelled = False

    def accept(self, generation: int, request_id: int, prompt_id: int, state: str) -> bool:
        if generation != self.generation:
            self.invalidate()
            return False
        if request_id == 0 or state not in KNOWN_STATES:
            self.invalidate()
            raise ValueError("invalid interaction state")
        if (state == "secret_required") != (prompt_id != 0):
            self.invalidate()
            raise ValueError("invalid prompt ID")
        if self.request_id == 0:
            if state not in START_STATES or request_id <= self.latest_request:
                return False
            self.request_id = self.latest_request = request_id
            self.latest_prompt = 0
            self.presence = state == "presence_required"
        elif request_id != self.request_id:
            return False
        elif state == "presence_required" and self.presence:
            return False
        if self.presence:
            if state != "presence_required" and state not in PRESENCE_TERMINALS | {"cancelled", "timed_out"}:
                self.invalidate()
                raise ValueError("invalid presence transition")
        elif state not in PROGRESS_STATES | VERIFICATION_TERMINALS | {"cancelled", "timed_out"}:
            self.invalidate()
            raise ValueError("invalid verification transition")
        if state == "secret_required":
            if prompt_id <= self.latest_prompt:
                return False
            self.latest_prompt = prompt_id
        self.state = state
        self.prompt_id = prompt_id
        if state in TERMINAL_STATES:
            self.invalidate()
        elif not self.presence:
            self.replied = False
        return True

    def can_reply(self, generation: int, request_id: int, state: str, prompt_id: int = 0) -> bool:
        return (
            generation == self.generation and request_id != 0
            and request_id == self.request_id and self.state == state
            and not self.replied and not self.cancelled
            and (state != "secret_required" or prompt_id == self.prompt_id != 0)
        )


class Agent:
    def __init__(self) -> None:
        self.bus: Optional[MessageBus] = None
        self.owner = ""
        self.interaction = Interaction()
        self.early_events = deque()
        self.stopped = asyncio.Event()
        self.failure: Optional[str] = None
        self.input_buffer = bytearray()
        self.input_oversized = False
        self.pending_calls = set()
        self.disconnect_task = None

    def stop(self, reason: Optional[str] = None) -> None:
        self.interaction.invalidate()
        self.input_buffer.clear()
        self.early_events.clear()
        self.failure = self.failure or reason
        self.stopped.set()

    async def bus_call(self, member: str, signature: str, body: list) -> Message:
        reply = await self.bus.call(Message(
            destination=BUS_SERVICE, path=BUS_PATH, interface=BUS_SERVICE,
            member=member, signature=signature, body=body,
        ))
        if reply is None or reply.message_type != MessageType.METHOD_RETURN:
            raise RuntimeError("D-Bus setup failed")
        return reply

    async def connect(self) -> None:
        # dbus-next defaults to disabling ancillary Unix descriptor transfer.
        self.bus = await MessageBus(bus_type=BusType.SYSTEM, negotiate_unix_fd=True).connect()
        self.bus.add_message_handler(self.on_message)
        for rule in (
            f"type='signal',sender='{SERVICE}',path='{PATH}',interface='{INTERFACE}',member='StateChanged'",
            f"type='signal',sender='{BUS_SERVICE}',path='{BUS_PATH}',interface='{BUS_SERVICE}',member='NameOwnerChanged',arg0='{SERVICE}'",
        ):
            await self.bus_call("AddMatch", "s", [rule])
        owner_reply = await self.bus_call("GetNameOwner", "s", [SERVICE])
        self.owner = owner_reply.body[0]
        if not self.owner.startswith(":"):
            raise RuntimeError("daemon has no unique bus name")
        reply = await self.call_method("RegisterAgent")
        if reply.signature != "t" or reply.body[0] == 0:
            raise RuntimeError("daemon returned an invalid generation")
        self.interaction.generation = reply.body[0]
        # Detect a daemon replacement during the registration round trip.
        if (await self.bus_call("GetNameOwner", "s", [SERVICE])).body[0] != self.owner:
            raise RuntimeError("daemon owner changed during registration")
        if self.stopped.is_set():
            raise RuntimeError(self.failure or "registration invalidated")
        while self.early_events:
            self.handle_state(*self.early_events.popleft())
        if self.stopped.is_set():
            raise RuntimeError(self.failure or "registration invalidated")
        print(f"registered generation {self.interaction.generation}", flush=True)

    async def call_method(self, member: str, signature: str = "", body=None, unix_fds=None) -> Message:
        reply = await self.bus.call(Message(
            destination=self.owner, path=PATH, interface=INTERFACE, member=member,
            signature=signature, body=body or [], unix_fds=unix_fds or [],
        ))
        if reply is None or reply.message_type != MessageType.METHOD_RETURN or reply.sender != self.owner:
            self.stop("daemon method failed")
            raise RuntimeError("daemon method failed")
        if member != "RegisterAgent" and reply.signature != "":
            self.stop("invalid daemon reply")
            raise RuntimeError("invalid daemon reply")
        return reply

    def on_message(self, message: Message) -> None:
        if message.message_type != MessageType.SIGNAL:
            return
        if (
            message.sender == BUS_SERVICE and message.path == BUS_PATH
            and message.interface == BUS_SERVICE and message.member == "NameOwnerChanged"
            and message.signature == "sss" and message.body[0] == SERVICE
        ):
            if self.owner and message.body[2] != self.owner:
                self.stop("daemon owner changed")
            return
        if (
            message.sender != self.owner or message.path != PATH
            or message.interface != INTERFACE or message.member != "StateChanged"
        ):
            return
        if message.signature != "tttssss":
            self.stop("invalid StateChanged signature")
            return
        if self.interaction.generation == 0:
            if len(self.early_events) == 32:
                self.stop("too many registration events")
            elif not self.stopped.is_set():
                self.early_events.append(message.body)
        else:
            self.handle_state(*message.body)

    def handle_state(self, generation, request_id, prompt_id, state, operation, rp_id, message) -> None:
        if self.stopped.is_set():
            return
        try:
            if not self.interaction.accept(generation, request_id, prompt_id, state):
                if not self.interaction.request_id:
                    self.input_buffer.clear()
                return
        except ValueError:
            self.stop("invalid interaction event")
            return
        print(f"{state}: operation {operation!r} for RP {rp_id!r}", flush=True)
        if message:
            print(repr(message), flush=True)
        if state == "presence_required":
            print(f"Reply {generation}:{request_id} y|n|c:", flush=True)
        elif state == "secret_required" and not self.interaction.cancelled:
            # A real UI may replace this cancellation with submit_secret().
            self.queue_reply(generation, request_id, "c")
        if state in TERMINAL_STATES:
            self.input_buffer.clear()

    def queue_reply(self, generation: int, request_id: int, answer: str) -> None:
        if answer == "c":
            valid = request_id == self.interaction.request_id and request_id != 0 and not (
                self.interaction.replied or self.interaction.cancelled
            ) and generation == self.interaction.generation
        else:
            valid = self.interaction.can_reply(generation, request_id, "presence_required")
        if not valid:
            return
        self.interaction.replied = True
        self.interaction.cancelled = answer == "c"
        self.input_buffer.clear()

        async def send() -> None:
            # A terminal event may have arrived before this task got CPU time.
            if self.stopped.is_set() or self.interaction.request_id != request_id:
                return
            try:
                if answer == "c":
                    await self.call_method("CancelInteraction", "tt", [generation, request_id])
                else:
                    await self.call_method("RespondToPresence", "ttb", [generation, request_id, answer == "y"])
            except Exception:
                self.stop("interaction reply failed")

        task = asyncio.create_task(send())
        self.pending_calls.add(task)
        task.add_done_callback(self.pending_calls.discard)

    def on_input(self) -> None:
        try:
            chunk = os.read(sys.stdin.fileno(), 256)
        except OSError:
            self.stop("console input failed")
            return
        if not chunk:
            self.stop()
            return
        for value in chunk:
            if value == 10:
                if not self.input_oversized:
                    fields = self.input_buffer.decode("ascii", errors="replace").split()
                    token = f"{self.interaction.generation}:{self.interaction.request_id}"
                    if len(fields) == 2 and fields[0] == token and fields[1] in ("y", "n", "c"):
                        self.queue_reply(self.interaction.generation, self.interaction.request_id, fields[1])
                self.input_buffer.clear()
                self.input_oversized = False
            elif not self.input_oversized:
                if len(self.input_buffer) == 128:
                    self.input_buffer.clear()
                    self.input_oversized = True
                else:
                    self.input_buffer.append(value)

    async def submit_secret(self, generation: int, request_id: int, prompt_id: int, secret: bytearray) -> None:
        """Submit only the current one-shot prompt, then erase all mutable bytes."""
        read_fd = write_fd = -1
        try:
            if self.bus is None or not self.interaction.can_reply(
                generation, request_id, "secret_required", prompt_id
            ):
                raise RuntimeError("secret prompt is not active")
            if len(secret) > 1024 or 0 in secret:
                raise ValueError("secret must be at most 1024 bytes without NUL")
            read_fd, write_fd = os.pipe2(os.O_CLOEXEC)
            written = 0
            while written < len(secret):
                count = os.write(write_fd, memoryview(secret)[written:])
                if count <= 0:
                    raise RuntimeError("secret pipe made no write progress")
                written += count
            os.close(write_fd)
            write_fd = -1
            self.interaction.replied = True
            await self.call_method(
                "SubmitSecret", "ttth", [generation, request_id, prompt_id, 0], [read_fd]
            )
        except Exception:
            self.stop("secret submission failed")
            raise
        finally:
            try:
                if write_fd >= 0:
                    os.close(write_fd)
            finally:
                try:
                    if read_fd >= 0:
                        os.close(read_fd)
                finally:
                    secret[:] = b"\0" * len(secret)
                    secret.clear()

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        loop.add_reader(sys.stdin.fileno(), self.on_input)
        disconnected = asyncio.create_task(self.bus.wait_for_disconnect())
        self.disconnect_task = disconnected
        stopped = asyncio.create_task(self.stopped.wait())
        try:
            done, _ = await asyncio.wait((disconnected, stopped), return_when=asyncio.FIRST_COMPLETED)
            if disconnected in done:
                self.stop("bus disconnected")
                try:
                    disconnected.result()
                except Exception:
                    pass
            if self.failure:
                raise RuntimeError(self.failure)
        finally:
            loop.remove_reader(sys.stdin.fileno())
            stopped.cancel()
            await asyncio.gather(stopped, return_exceptions=True)

    async def close(self) -> None:
        self.interaction.invalidate()
        self.input_buffer.clear()
        for task in list(self.pending_calls):
            task.cancel()
        await asyncio.gather(*self.pending_calls, return_exceptions=True)
        if self.bus is not None:
            try:
                if self.bus.connected and self.interaction.generation and self.owner:
                    await self.call_method("UnregisterAgent")
            except Exception:
                pass
            self.bus.disconnect()
            if self.disconnect_task is not None:
                await asyncio.gather(self.disconnect_task, return_exceptions=True)
            else:
                try:
                    await self.bus.wait_for_disconnect()
                except Exception:
                    pass


async def main() -> None:
    agent = Agent()
    try:
        await agent.connect()
        await agent.run()
    finally:
        await agent.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    except Exception:
        print("vAuth agent example: stopped after a connection or interaction error", file=sys.stderr)
        sys.exit(1)
