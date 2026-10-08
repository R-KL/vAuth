package main

import (
	"bytes"
	"errors"
	"io"
	"os"
	"syscall"
	"testing"

	"github.com/godbus/dbus/v5"
)

func TestInteractionLifetime(t *testing.T) {
	i := interaction{generation: 7}
	accept := func(request, prompt uint64, state string, want bool) {
		t.Helper()
		got, err := i.accept(7, request, prompt, state)
		if err != nil || got != want {
			t.Fatalf("%s: accepted=%v, error=%v", state, got, err)
		}
	}
	accept(1, 0, "presence_required", true)
	if !i.canReply(7, 1, "presence_required", 0) || i.canReply(8, 1, "presence_required", 0) {
		t.Fatal("presence callback binding is wrong")
	}
	i.replied = true
	accept(1, 0, "presence_required", false)
	if i.canReply(7, 1, "presence_required", 0) {
		t.Fatal("duplicate presence reply")
	}
	accept(1, 0, "timed_out", true)
	accept(1, 0, "presence_required", false)
	accept(2, 0, "verification_started", true)
	accept(2, 10, "secret_required", true)
	i.replied = true
	accept(2, 0, "verification_error", true)
	accept(2, 10, "secret_required", false)
	accept(2, 9, "secret_required", false)
	accept(2, 11, "secret_required", true)
	i.cancelled = true
	accept(2, 0, "verification_information", true)
	accept(2, 12, "secret_required", true)
	if i.canReply(7, 2, "secret_required", 12) {
		t.Fatal("reply after cancellation")
	}
	accept(2, 0, "cancelled", true)
	accept(2, 0, "verification_started", false)
	accept(3, 0, "presence_required", true)
	if accepted, err := i.accept(8, 3, 0, "presence_required"); accepted || err != nil || i.requestID != 0 {
		t.Fatal("foreign generation retained active request")
	}
}

func TestInvalidStatesFailClosed(t *testing.T) {
	for _, event := range []struct {
		request, prompt uint64
		state           string
	}{
		{0, 0, "presence_required"}, {1, 0, "future_state"},
		{1, 1, "presence_required"}, {1, 0, "secret_required"},
		{1, 0, "verification_succeeded"},
	} {
		i := interaction{generation: 7}
		i.accept(7, 1, 0, "presence_required")
		if accepted, err := i.accept(7, event.request, event.prompt, event.state); accepted || err == nil || i.requestID != 0 {
			t.Fatalf("invalid %s event did not invalidate", event.state)
		}
	}
}

type secretObject struct {
	dbus.BusObject
	t     *testing.T
	calls int
	fd    int
	fail  bool
}

func (s *secretObject) Call(method string, flags dbus.Flags, args ...interface{}) *dbus.Call {
	s.t.Helper()
	s.calls++
	if method != iface+".SubmitSecret" || flags != 0 || len(args) != 4 ||
		args[0] != uint64(7) || args[1] != uint64(1) || args[2] != uint64(10) {
		s.t.Fatal("wrong SubmitSecret wire arguments")
	}
	s.fd = int(args[3].(dbus.UnixFD))
	fdFlags, _, err := syscall.Syscall(syscall.SYS_FCNTL, uintptr(s.fd), syscall.F_GETFD, 0)
	if err != 0 || fdFlags&syscall.FD_CLOEXEC == 0 {
		s.t.Fatal("pipe is not close-on-exec")
	}
	duplicate, dupErr := syscall.Dup(s.fd)
	if dupErr != nil {
		s.t.Fatal(dupErr)
	}
	file := os.NewFile(uintptr(duplicate), "secret-test-pipe")
	defer file.Close()
	// ReadAll reaches EOF only if the writer was closed before the call.
	data, readErr := io.ReadAll(file)
	if readErr != nil || !bytes.Equal(data, bytes.Repeat([]byte{'x'}, 1024)) {
		s.t.Fatal("pipe bytes are wrong")
	}
	if s.fail {
		return &dbus.Call{Err: errors.New("injected failure")}
	}
	return &dbus.Call{}
}

func TestSecretSubmissionAndErasure(t *testing.T) {
	for _, fail := range []bool{false, true} {
		i := interaction{generation: 7}
		i.accept(7, 1, 0, "verification_started")
		i.accept(7, 1, 10, "secret_required")
		object := &secretObject{t: t, fail: fail}
		secret := bytes.Repeat([]byte{'x'}, 1024)
		err := submitSecret(object, &i, 7, 1, 10, secret)
		if (err != nil) != fail || object.calls != 1 || !bytes.Equal(secret, make([]byte, 1024)) {
			t.Fatal("submission outcome or erasure is wrong")
		}
		if _, readErr := syscall.Read(object.fd, make([]byte, 1)); readErr != syscall.EBADF {
			t.Fatal("read descriptor retained")
		}
		if fail && i.requestID != 0 {
			t.Fatal("method failure did not invalidate")
		}
		if !fail && i.canReply(7, 1, "secret_required", 10) {
			t.Fatal("prompt was reusable")
		}
	}
}

func TestInvalidSecretsAreErased(t *testing.T) {
	for _, invalid := range []string{"size", "nul", "request", "prompt", "generation", "state", "duplicate"} {
		t.Run(invalid, func(t *testing.T) {
			i := interaction{generation: 7}
			i.accept(7, 1, 0, "verification_started")
			i.accept(7, 1, 10, "secret_required")
			generation, request, prompt := uint64(7), uint64(1), uint64(10)
			secret := []byte{'x'}
			switch invalid {
			case "size":
				secret = bytes.Repeat([]byte{'x'}, 1025)
			case "nul":
				secret = []byte{'x', 0}
			case "request":
				request = 0
			case "prompt":
				prompt = 9
			case "generation":
				generation = 8
			case "state":
				i.state = "presence_required"
			case "duplicate":
				i.replied = true
			}
			object := &secretObject{t: t}
			if submitSecret(object, &i, generation, request, prompt, secret) == nil || object.calls != 0 ||
				!bytes.Equal(secret, make([]byte, len(secret))) || i.requestID != 0 {
				t.Fatal("invalid secret was accepted or retained")
			}
		})
	}
}
