import asyncio
import contextlib
import fcntl
import io
import os
import unittest

from dbus_next import Message, MessageType
from agent import Agent, Interaction


class InteractionTests(unittest.TestCase):
    def test_one_shot_presence_and_terminal_replay(self):
        tracker = Interaction(generation=7)
        self.assertTrue(tracker.accept(7, 1, 0, "presence_required"))
        self.assertTrue(tracker.can_reply(7, 1, "presence_required"))
        self.assertFalse(tracker.can_reply(8, 1, "presence_required"))
        tracker.replied = True
        self.assertFalse(tracker.accept(7, 1, 0, "presence_required"))
        self.assertFalse(tracker.can_reply(7, 1, "presence_required"))
        self.assertTrue(tracker.accept(7, 1, 0, "timed_out"))
        self.assertFalse(tracker.accept(7, 1, 0, "presence_required"))
        self.assertFalse(tracker.accept(7, 1, 0, "timed_out"))

    def test_prompt_history_and_cancellation(self):
        tracker = Interaction(generation=7)
        tracker.accept(7, 1, 0, "verification_started")
        self.assertTrue(tracker.accept(7, 1, 10, "secret_required"))
        tracker.replied = True
        self.assertFalse(tracker.can_reply(7, 1, "secret_required", 10))
        tracker.accept(7, 1, 0, "verification_error")
        self.assertFalse(tracker.accept(7, 1, 10, "secret_required"))
        self.assertFalse(tracker.accept(7, 1, 9, "secret_required"))
        self.assertTrue(tracker.accept(7, 1, 11, "secret_required"))
        tracker.cancelled = True
        tracker.accept(7, 1, 0, "verification_information")
        tracker.accept(7, 1, 12, "secret_required")
        self.assertFalse(tracker.can_reply(7, 1, "secret_required", 12))

    def test_invalid_event_clears_request(self):
        for request, prompt, state in (
            (0, 0, "presence_required"), (1, 0, "future_state"),
            (1, 1, "presence_required"), (1, 0, "secret_required"),
            (1, 0, "verification_succeeded"),
        ):
            tracker = Interaction(generation=7)
            tracker.accept(7, 1, 0, "presence_required")
            with self.assertRaises(ValueError):
                tracker.accept(7, request, prompt, state)
            self.assertEqual(tracker.request_id, 0)
        tracker = Interaction(generation=7)
        tracker.accept(7, 1, 0, "presence_required")
        self.assertFalse(tracker.accept(8, 1, 0, "presence_required"))
        self.assertEqual(tracker.request_id, 0)


class FakeBus:
    def __init__(self, test, fail=False):
        self.test = test
        self.fail = fail
        self.calls = []
        self.fd = -1

    async def call(self, message):
        self.calls.append(message)
        if message.member == "SubmitSecret":
            self.test.assertEqual(message.signature, "ttth")
            self.test.assertEqual(message.body, [7, 1, 10, 0])
            self.test.assertEqual(len(message.unix_fds), 1)
            self.fd = message.unix_fds[0]
            self.test.assertTrue(fcntl.fcntl(self.fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC)
            self.test.assertEqual(os.read(self.fd, 1025), b"x" * 1024)
            # Nonblocking EOF distinguishes a closed writer from a leaked one.
            os.set_blocking(self.fd, False)
            self.test.assertEqual(os.read(self.fd, 1), b"")
        if self.fail:
            return Message(message_type=MessageType.ERROR, reply_serial=1,
                           error_name="org.lamellix.vAuth.Error.InvalidInteraction")
        return Message(message_type=MessageType.METHOD_RETURN, reply_serial=1, sender=":1.42")


class ReplyTests(unittest.IsolatedAsyncioTestCase):
    def make_agent(self, fail=False):
        agent = Agent()
        agent.owner = ":1.42"
        agent.bus = FakeBus(self, fail)
        agent.interaction = Interaction(generation=7)
        return agent

    async def test_delayed_presence_callback_and_duplicates(self):
        agent = self.make_agent()
        with contextlib.redirect_stdout(io.StringIO()):
            agent.handle_state(7, 1, 0, "presence_required", "future_operation", "example.com", "")
            agent.queue_reply(7, 1, "y")
            agent.queue_reply(7, 1, "y")
            agent.handle_state(7, 1, 0, "timed_out", "future_operation", "example.com", "")
        await asyncio.gather(*agent.pending_calls)
        self.assertEqual(agent.bus.calls, [])

        agent.interaction.accept(7, 2, 0, "presence_required")
        agent.queue_reply(7, 1, "y")
        agent.queue_reply(7, 2, "n")
        agent.queue_reply(7, 2, "c")
        await asyncio.gather(*agent.pending_calls)
        self.assertEqual(len(agent.bus.calls), 1)
        self.assertEqual(agent.bus.calls[0].body, [7, 2, False])

    async def test_valid_secret_boundary_and_failure_erasure(self):
        for fail in (False, True):
            agent = self.make_agent(fail)
            agent.interaction.accept(7, 1, 0, "verification_started")
            agent.interaction.accept(7, 1, 10, "secret_required")
            secret = bytearray(b"x" * 1024)
            if fail:
                with self.assertRaises(RuntimeError):
                    await agent.submit_secret(7, 1, 10, secret)
            else:
                await agent.submit_secret(7, 1, 10, secret)
                self.assertFalse(agent.interaction.can_reply(7, 1, "secret_required", 10))
            self.assertEqual(secret, bytearray())
            with self.assertRaises(OSError):
                os.fstat(agent.bus.fd)
            if fail:
                self.assertEqual(agent.interaction.request_id, 0)

    async def test_invalid_secret_is_erased_before_any_call(self):
        for invalid in ("size", "nul", "request", "prompt", "generation", "state", "duplicate"):
            agent = self.make_agent()
            agent.interaction.accept(7, 1, 0, "verification_started")
            agent.interaction.accept(7, 1, 10, "secret_required")
            secret = bytearray(b"x")
            generation, request, prompt = 7, 1, 10
            if invalid == "size":
                secret = bytearray(b"x" * 1025)
            elif invalid == "nul":
                secret = bytearray(b"x\0")
            elif invalid == "request":
                request = 0
            elif invalid == "prompt":
                prompt = 9
            elif invalid == "generation":
                generation = 8
            elif invalid == "state":
                agent.interaction.state = "presence_required"
            else:
                agent.interaction.replied = True
            with self.assertRaises((RuntimeError, ValueError)):
                await agent.submit_secret(generation, request, prompt, secret)
            self.assertEqual(secret, bytearray())
            self.assertEqual(agent.bus.calls, [])
            self.assertEqual(agent.interaction.request_id, 0)


if __name__ == "__main__":
    unittest.main()
