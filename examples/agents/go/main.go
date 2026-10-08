package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"github.com/godbus/dbus/v5"
)

const (
	service   = "org.lamellix.vAuth"
	path      = dbus.ObjectPath("/org/lamellix/vAuth")
	iface     = "org.lamellix.vAuth.UserInteraction1"
	stateName = iface + ".StateChanged"
	busName   = "org.freedesktop.DBus"
	busPath   = dbus.ObjectPath("/org/freedesktop/DBus")
)

var presenceTerminals = map[string]bool{"presence_approved": true, "presence_denied": true}
var verificationTerminals = map[string]bool{"verification_succeeded": true, "verification_failed": true}
var commonTerminals = map[string]bool{"cancelled": true, "timed_out": true}
var progressStates = map[string]bool{
	"verification_started": true, "verification_information": true,
	"verification_error": true, "secret_required": true,
}

type interaction struct {
	generation, requestID, latestRequest, promptID, latestPrompt uint64
	state                                                        string
	presence, replied, cancelled                                 bool
}

func (i *interaction) invalidate() {
	i.requestID, i.promptID, i.state = 0, 0, ""
	i.replied, i.cancelled = false, false
}

func (i *interaction) accept(generation, requestID, promptID uint64, state string) (bool, error) {
	if generation != i.generation {
		i.invalidate()
		return false, nil
	}
	start := state == "presence_required" || state == "verification_started"
	terminal := presenceTerminals[state] || verificationTerminals[state] || commonTerminals[state]
	if requestID == 0 || (!start && !terminal && !progressStates[state]) ||
		((state == "secret_required") != (promptID != 0)) {
		i.invalidate()
		return false, errors.New("invalid interaction event")
	}
	if i.requestID == 0 {
		if !start || requestID <= i.latestRequest {
			return false, nil
		}
		i.requestID, i.latestRequest, i.latestPrompt = requestID, requestID, 0
		i.presence = state == "presence_required"
	} else if requestID != i.requestID {
		return false, nil
	} else if state == "presence_required" && i.presence {
		return false, nil
	}
	if (i.presence && state != "presence_required" && !presenceTerminals[state] && !commonTerminals[state]) ||
		(!i.presence && !progressStates[state] && !verificationTerminals[state] && !commonTerminals[state]) {
		i.invalidate()
		return false, errors.New("invalid interaction transition")
	}
	if state == "secret_required" {
		if promptID <= i.latestPrompt {
			return false, nil
		}
		i.latestPrompt = promptID
	}
	i.state, i.promptID = state, promptID
	if terminal {
		i.invalidate()
	} else if !i.presence {
		i.replied = false
	}
	return true, nil
}

func (i *interaction) canReply(generation, requestID uint64, state string, promptID uint64) bool {
	return generation == i.generation && requestID != 0 && requestID == i.requestID &&
		i.state == state && !i.replied && !i.cancelled &&
		(state != "secret_required" || (promptID != 0 && promptID == i.promptID))
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx); err != nil {
		fmt.Fprintln(os.Stderr, "vAuth agent example: stopped after a connection or interaction error")
		os.Exit(1)
	}
}

type consoleLine struct {
	text string
	err  error
}

func readConsole(ctx context.Context, lines chan<- consoleLine) {
	reader := bufio.NewScanner(os.Stdin)
	reader.Buffer(make([]byte, 128), 128)
	for reader.Scan() {
		select {
		case lines <- consoleLine{text: reader.Text()}:
		case <-ctx.Done():
			return
		}
	}
	select {
	case lines <- consoleLine{err: errors.New("console closed or input too long")}:
	case <-ctx.Done():
	}
}

func run(ctx context.Context) error {
	conn, err := dbus.ConnectSystemBus()
	if err != nil {
		return err
	}
	defer conn.Close()
	signals := make(chan *dbus.Signal, 32)
	conn.Signal(signals)
	defer conn.RemoveSignal(signals)
	for _, options := range [][]dbus.MatchOption{
		{dbus.WithMatchSender(busName), dbus.WithMatchObjectPath(busPath),
			dbus.WithMatchInterface(busName), dbus.WithMatchMember("NameOwnerChanged"), dbus.WithMatchArg(0, service)},
		{dbus.WithMatchSender(service), dbus.WithMatchObjectPath(path),
			dbus.WithMatchInterface(iface), dbus.WithMatchMember("StateChanged")},
	} {
		if err := conn.AddMatchSignal(options...); err != nil {
			return err
		}
	}
	var owner string
	if err := conn.BusObject().Call(busName+".GetNameOwner", 0, service).Store(&owner); err != nil {
		return err
	}
	if !strings.HasPrefix(owner, ":") {
		return errors.New("daemon has no unique bus name")
	}
	object := conn.Object(owner, path)
	tracker := interaction{}
	registration := object.Call(iface+".RegisterAgent", 0)
	if registration.Err != nil {
		return registration.Err
	}
	if len(registration.Body) != 1 {
		return errors.New("invalid registration reply")
	}
	var valid bool
	tracker.generation, valid = registration.Body[0].(uint64)
	if !valid || tracker.generation == 0 {
		return errors.New("daemon returned an invalid generation")
	}
	defer object.Call(iface+".UnregisterAgent", 0)
	defer tracker.invalidate()
	var currentOwner string
	if err := conn.BusObject().Call(busName+".GetNameOwner", 0, service).Store(&currentOwner); err != nil || currentOwner != owner {
		return errors.New("daemon owner changed during registration")
	}
	fmt.Printf("registered generation %d\n", tracker.generation)
	lines := make(chan consoleLine, 8)
	inputCtx, stopInput := context.WithCancel(ctx)
	defer stopInput()
	go readConsole(inputCtx, lines)

	handleSignal := func(s *dbus.Signal) error {
		if s == nil {
			return errors.New("bus disconnected")
		}
		if s.Sender == busName && s.Path == busPath && s.Name == busName+".NameOwnerChanged" {
			var name, previous, next string
			if err := dbus.Store(s.Body, &name, &previous, &next); err != nil {
				return err
			}
			if name == service && next != owner {
				tracker.invalidate()
				return errors.New("daemon owner changed")
			}
			return nil
		}
		// Directed signals bypass broadcast match rules: verify their sender too.
		if s.Sender != owner || s.Path != path || s.Name != stateName {
			return nil
		}
		var generation, requestID, promptID uint64
		var state, operation, rpID, message string
		if len(s.Body) != 7 || dbus.SignatureOf(s.Body...).String() != "tttssss" {
			tracker.invalidate()
			return errors.New("invalid StateChanged signature")
		}
		if err := dbus.Store(s.Body, &generation, &requestID, &promptID, &state, &operation, &rpID, &message); err != nil {
			tracker.invalidate()
			return err
		}
		accepted, err := tracker.accept(generation, requestID, promptID, state)
		if err != nil || !accepted {
			return err
		}
		fmt.Printf("%s: operation %q for RP %q\n", state, operation, rpID)
		if message != "" {
			fmt.Printf("%q\n", message)
		}
		if state == "presence_required" {
			fmt.Printf("Reply %d:%d y|n|c:\n", generation, requestID)
		} else if state == "secret_required" && !tracker.cancelled {
			tracker.replied, tracker.cancelled = true, true
			// A real UI may replace this cancellation with submitSecret.
			if err := checkReply(object.CallWithContext(ctx, iface+".CancelInteraction", 0, generation, requestID)); err != nil {
				tracker.invalidate()
				return err
			}
		}
		return nil
	}
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-conn.Context().Done():
			return errors.New("bus disconnected")
		case s := <-signals:
			if err := handleSignal(s); err != nil {
				return err
			}
		case line := <-lines:
			// Apply queued terminal events before considering a console answer.
		drain:
			for {
				select {
				case s := <-signals:
					if err := handleSignal(s); err != nil {
						return err
					}
				default:
					break drain
				}
			}
			if line.err != nil {
				return line.err
			}
			fields := strings.Fields(line.text)
			token := fmt.Sprintf("%d:%d", tracker.generation, tracker.requestID)
			if len(fields) != 2 || fields[0] != token {
				continue
			}
			answer := fields[1]
			if answer != "y" && answer != "n" && answer != "c" {
				continue
			}
			if answer == "c" {
				if tracker.requestID == 0 || tracker.replied || tracker.cancelled {
					continue
				}
			} else if !tracker.canReply(tracker.generation, tracker.requestID, "presence_required", 0) {
				continue
			}
			tracker.replied, tracker.cancelled = true, answer == "c"
			if answer == "c" {
				err = checkReply(object.CallWithContext(ctx, iface+".CancelInteraction", 0, tracker.generation, tracker.requestID))
			} else {
				err = checkReply(object.CallWithContext(ctx, iface+".RespondToPresence", 0, tracker.generation, tracker.requestID, answer == "y"))
			}
			if err != nil {
				tracker.invalidate()
				return err
			}
		}
	}
}

func checkReply(call *dbus.Call) error {
	if call.Err != nil {
		return call.Err
	}
	if len(call.Body) != 0 {
		return errors.New("invalid daemon reply")
	}
	return nil
}

func submitSecret(
	object dbus.BusObject,
	tracker *interaction,
	generation, requestID, promptID uint64,
	secret []byte,
) (err error) {
	defer clear(secret)
	defer func() {
		if err != nil {
			tracker.invalidate()
		}
	}()
	if !tracker.canReply(generation, requestID, "secret_required", promptID) {
		return errors.New("secret prompt is not active")
	}
	if len(secret) > 1024 || bytes.IndexByte(secret, 0) >= 0 {
		return errors.New("secret must be at most 1024 bytes without NUL")
	}
	// os.Pipe creates close-on-exec descriptors on Linux.
	readEnd, writeEnd, err := os.Pipe()
	if err != nil {
		return err
	}
	defer readEnd.Close()
	defer writeEnd.Close()
	written := 0
	for written < len(secret) {
		count, writeErr := writeEnd.Write(secret[written:])
		if writeErr != nil {
			return writeErr
		}
		if count == 0 {
			return errors.New("secret pipe made no write progress")
		}
		written += count
	}
	if err = writeEnd.Close(); err != nil {
		return err
	}
	tracker.replied = true
	return checkReply(object.Call(
		iface+".SubmitSecret", 0, generation, requestID, promptID, dbus.UnixFD(readEnd.Fd()),
	))
}
