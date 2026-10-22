#!/usr/bin/env python3
"""App lab: scripted users on the real app stack (pp-app-probe) over the real relay.

Each user is one pp-app-probe process (its own profile dir) talking to the lab relay
(packaging/pp-node/docker-compose.app-lab.yml) with the local hop as its only mesh seed. Scenarios
press the same buttons people press, the way they press them when a call will not get through:
both calling at once, calling again and again, an app restart while ringing. Every scenario ends
by checking nothing is left stuck and the call history says what happened.

  scripts/test/pp_app_lab.py --probe build/src/app/node/pp-app-probe [--only first_contact]

(`pp_local_test.sh run --suite app` brings the lab up and runs this.)

With --nat each user runs in its own container behind its own NAT gateway
(packaging/pp-node/docker-compose.app-lab-nat.yml): no two users share a network, so everything
between them goes through the hop. `pp_local_test.sh run --suite app-nat` brings that up and runs it.
"""

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid

PIN = "246810"
# Answer → audio both ways on one machine is ~2 s; past this the call "isn't getting through" for a
# person, who hangs up and calls again.
MAX_ANSWER_TO_AUDIO_S = 8.0
# The relay allows 10 registrations per client IP per hour (in memory); every probe is 127.0.0.1.
REGISTRATIONS_PER_RELAY_START = 10
registrations = REGISTRATIONS_PER_RELAY_START  # unknown at start: the first user restarts the gateway


class ProbeError(Exception):
    pass


class User:
    """One person with the app: a pp-app-probe process over a profile dir that outlives it."""

    def __init__(self, lab, name, container=None):
        self.lab = lab
        self.name = name
        self.container = container  # --nat: the user's own NAT'd container (None: a host process)
        self.dir = os.path.join(lab.root, name)
        os.makedirs(self.dir, exist_ok=True)
        self.config = os.path.join(self.dir, "config.json")
        with open(self.config, "w") as f:
            json.dump(lab.config_for(self.dir), f, indent=2)
        self.proc = None
        self.next_id = 1
        self.replies = {}
        self.events = queue.Queue()
        self.surface = {"kind": "none"}
        self.history = []  # every event, for the failure report
        self.account = ""
        self.lock = threading.Condition()
        self.runs = 0

    # --- process --------------------------------------------------------------------------------

    def start(self):
        self.runs += 1
        log = os.path.join(self.dir, "probe-%d.log" % self.runs)
        argv = ["--config", self.config, "--pin", PIN, "--log-file", log]
        if self.container:
            # The probe's pid is kept so a crash kills it, not only the `docker exec` beside it.
            script = 'echo $$ > "$0"; exec /probes/%s "$@"' % os.path.basename(self.lab.probe)
            cmd = ["docker", "exec", "-i", "--user", "%d:%d" % (os.getuid(), os.getgid()), "-e", "HOME=/tmp",
                   self.container, "sh", "-c", script, self.pidfile()] + argv
        else:
            cmd = [self.lab.probe] + argv
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()
        self.wait_event(lambda e: e.get("event") in ("booted", "boot_failed"), 60, "boot")
        if self.last_event.get("event") == "boot_failed":
            raise ProbeError("%s: boot failed: %s" % (self.name, self.last_event.get("error")))

    def _read(self):
        proc = self.proc
        for line in proc.stdout:
            if not line.startswith("@@ "):
                continue
            try:
                msg = json.loads(line[3:])
            except ValueError:
                continue
            with self.lock:
                self.history.append((time.time(), msg))
                if "id" in msg and "event" not in msg:
                    self.replies[msg["id"]] = msg
                else:
                    if msg.get("event") == "surface":
                        self.surface = msg
                    self.events.put(msg)
                self.lock.notify_all()
        with self.lock:
            self.history.append((time.time(), {"event": "process_exit", "code": proc.wait()}))
            self.lock.notify_all()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def quit(self, timeout=20):
        if not self.alive():
            return
        try:
            self.cmd("quit", timeout=5)
        except ProbeError:
            pass
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.crash()
            raise ProbeError("%s: quit hung (killed)" % self.name)

    def pidfile(self):
        return os.path.join(self.dir, "probe.pid")

    def crash(self):
        """The app dies with no goodbye (force-quit, OOM kill)."""
        if self.container:
            subprocess.run(["docker", "exec", self.container, "sh", "-c", 'kill -9 "$(cat "$0")" 2>/dev/null',
                            self.pidfile()], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if self.alive():
            self.proc.kill()
            self.proc.wait()

    def restart(self):
        self.crash()
        self.surface = {"kind": "none"}
        self.start()
        self.wait_ready()

    # --- commands -------------------------------------------------------------------------------

    def cmd(self, name, timeout=30, check=True, **args):
        if not self.alive():
            raise ProbeError("%s: not running (%s)" % (self.name, name))
        req_id = self.next_id
        self.next_id += 1
        args.update({"id": req_id, "cmd": name})
        self.proc.stdin.write(json.dumps(args) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        with self.lock:
            while req_id not in self.replies:
                left = deadline - time.time()
                if left <= 0 or not self.alive():
                    raise ProbeError("%s: no reply to %s" % (self.name, name))
                self.lock.wait(min(left, 0.5))
            reply = self.replies.pop(req_id)
        if check and not reply.get("ok"):
            raise ProbeError("%s: %s failed: %s" % (self.name, name, reply.get("error")))
        return reply

    def wait_event(self, pred, timeout, what):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise ProbeError("%s: timed out waiting for %s (surface=%s)" % (self.name, what, self.surface))
            try:
                event = self.events.get(timeout=min(left, 0.5))
            except queue.Empty:
                if not self.alive():
                    raise ProbeError("%s: exited while waiting for %s" % (self.name, what))
                continue
            self.last_event = event
            if pred(event):
                return event

    def wait_surface(self, kind, timeout=30, call_id=None):
        """Wait until the call surface shows `kind` (now or later)."""
        deadline = time.time() + timeout
        while True:
            s = self.surface
            if s.get("kind") == kind and (call_id is None or s.get("call_id") == call_id):
                return s
            left = deadline - time.time()
            if left <= 0:
                raise ProbeError("%s: surface never became %s%s (is %s)" % (
                    self.name, kind, "" if call_id is None else " " + call_id, s))
            with self.lock:
                self.lock.wait(min(left, 0.25))
            if not self.alive():
                raise ProbeError("%s: exited while waiting for surface %s" % (self.name, kind))

    def wait_ready(self, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            who = self.cmd("whoami")
            if who.get("messaging_ready"):
                self.account = who.get("account") or self.account
                return who
            time.sleep(0.25)
        raise ProbeError("%s: messaging never ready" % self.name)

    def wait_mesh(self, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.cmd("whoami").get("mesh_ready"):
                return
            time.sleep(0.25)
        raise ProbeError("%s: mesh never ready" % self.name)

    def state(self):
        return self.cmd("state")

    def call_summaries(self, thread):
        rows = self.cmd("messages", thread=thread)["messages"]
        out = []
        for m in rows:
            if m["type"] != "system":
                continue
            try:
                payload = json.loads(m["payload"]) if m["payload"] else {}
            except ValueError:
                continue
            if payload.get("control_type") == "call_summary":
                detail = payload.get("detail")
                if isinstance(detail, str):
                    try:
                        detail = json.loads(detail)
                    except ValueError:
                        detail = {}
                out.append(detail or {})
        return out


class Lab:
    def __init__(self, args):
        self.probe = os.path.abspath(args.probe)
        self.relay = args.relay.rstrip("/")
        self.hop = args.hop
        # --nat: users run in these containers, one each in turn; they reach the relay at user_relay.
        self.containers = args.nat_containers.split(",") if args.nat else []
        self.user_relay = args.nat_relay.rstrip("/") if args.nat else self.relay
        if args.nat:
            os.makedirs(args.nat_share, exist_ok=True)
        self.root = tempfile.mkdtemp(prefix="pp-app-lab-", dir=args.nat_share if args.nat else None)
        # --nat: each container's network as it starts, so a scenario that changes it is undone.
        self.networks = {}
        for i, container in enumerate(self.containers):
            address, gw = container_network(container)
            if not address or not gw:  # left broken by an earlier run: the compose file's own
                address, gw = "10.119.%d.10/24" % (i + 1), "10.119.%d.254" % (i + 1)
                restore_network(container, address, gw)
            self.networks[container] = (address, gw)
        self.keep = args.keep
        self.relay_container = args.relay_container
        self.hop_container = args.hop_container
        self.hop_status = args.hop_status
        self.users = []

    def config_for(self, data_dir):
        return {
            "config_version": 1,
            "data_dir": data_dir,
            "relay": {"base_url": self.user_relay},
            "directory": {"base_url": self.user_relay},
            "registration": {"base_url": self.user_relay},
            "mesh": {
                "node_enabled": False,
                "mesh_enabled": True,
                "amp_udp_port": 0,
                "bootstrap_peers": [self.hop],
                "capabilities": {"dht": False},
            },
        }

    def wait_hop(self, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if subprocess.run(["curl", "-fsS", "-m", "2", self.hop_status + "/healthz"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0:
                return
            time.sleep(0.5)
        raise ProbeError("lab hop did not come back")

    def fresh_registration(self):
        """Restart the lab relay's gateway when this run has used up its registration allowance."""
        global registrations
        if registrations < REGISTRATIONS_PER_RELAY_START:
            registrations += 1
            return
        if not self.relay_container:
            raise ProbeError("relay registration allowance used up (pass --relay-container)")
        subprocess.run(["docker", "restart", self.relay_container], check=True, stdout=subprocess.DEVNULL)
        health = self.relay.split("/api/")[0] + "/healthz"
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                if subprocess.run(["curl", "-fsS", "-m", "2", health], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0:
                    registrations = 1
                    return
            except OSError:
                pass
            time.sleep(0.5)
        raise ProbeError("relay did not come back after restart")

    def user(self, name):
        self.fresh_registration()
        container = self.containers[len(self.users) % len(self.containers)] if self.containers else None
        u = User(self, "%s-%s" % (name, uuid.uuid4().hex[:6]), container)
        self.users.append(u)
        u.start()
        u.wait_ready()
        u.cmd("register", nickname=u.name, timeout=60)
        u.account = u.cmd("whoami")["account"]
        if not u.account:
            raise ProbeError("%s: no account after register" % u.name)
        return u

    def close(self, ok):
        for u in self.users:
            try:
                u.quit()
            except ProbeError:
                u.crash()
        for container, (address, gw) in self.networks.items():
            restore_network(container, address, gw)
        if ok and not self.keep:
            shutil.rmtree(self.root, ignore_errors=True)
        self.users = []


# --- outcome checks -------------------------------------------------------------------------------

def assert_idle(*users, timeout=30):
    """Nobody is left ringing or in a call. The screen clears on the End click (as in the app);
    the call stack follows a moment later."""
    for u in users:
        u.wait_surface("none", timeout)
        deadline = time.time() + 10
        while True:
            s = u.state()
            if s.get("phase") in ("Idle", None, ""):
                break
            if time.time() > deadline:
                raise ProbeError("%s: phase %s after the call" % (u.name, s.get("phase")))
            time.sleep(0.2)


def assert_media_flows(*users, timeout=30):
    for u in users:
        deadline = time.time() + timeout
        while True:
            s = u.state()
            if s.get("kind") == "in_call" and s.get("rx_audio_frames", 0) > 0 and s.get("tx_audio_frames", 0) > 0:
                break
            if time.time() > deadline:
                raise ProbeError("%s: no audio both ways (%s)" % (u.name, s))
            time.sleep(0.25)


def connect(a, a_thread, b, timeout=40):
    """a calls b, b answers: both in the same call with audio. Returns the call id and seconds."""
    started = time.time()
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", timeout, call_id=call_id)
    rang = time.time() - started
    b.cmd("accept_voice")
    answered = time.time()
    a.wait_surface("in_call", timeout, call_id=call_id)
    b.wait_surface("in_call", timeout, call_id=call_id)
    assert_media_flows(a, b, timeout=timeout)
    return call_id, {"ring_s": round(rang, 2), "answer_to_audio_s": check_answer_time(time.time() - answered)}


def check_answer_time(seconds):
    if seconds > MAX_ANSWER_TO_AUDIO_S:
        raise ProbeError("answer → audio took %.1f s (> %.0f s)" % (seconds, MAX_ANSWER_TO_AUDIO_S))
    return round(seconds, 2)


def open_pair(lab):
    a = lab.user("alice")
    b = lab.user("bob")
    a.wait_mesh()
    b.wait_mesh()
    a_thread = a.cmd("open_chat", account=b.account)["thread"]
    b_thread = b.cmd("open_chat", account=a.account)["thread"]
    return a, a_thread, b, b_thread


def summaries_settle(u, thread, want, timeout=20):
    deadline = time.time() + timeout
    while True:
        got = u.call_summaries(thread)
        if len(got) >= want or time.time() > deadline:
            return got
        time.sleep(0.5)


# --- scenarios ------------------------------------------------------------------------------------

def scenario_first_contact(lab):
    """Bob has never heard of Alice: her call is the first thing he gets from her."""
    a = lab.user("alice")
    b = lab.user("bob")
    a.wait_mesh()
    b.wait_mesh()
    a_thread = a.cmd("open_chat", account=b.account)["thread"]
    started = time.time()
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    rang = time.time() - started
    b.cmd("accept_voice")
    answered = time.time()
    a.wait_surface("in_call", 40, call_id=call_id)
    b.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(a, b)
    took = check_answer_time(time.time() - answered)
    a.cmd("leave")
    assert_idle(a, b)
    got = summaries_settle(a, a_thread, 1)
    if len(got) != 1 or got[0].get("outcome") != "completed":
        raise ProbeError("alice's history: %s" % got)
    b_threads = [t for t in b.cmd("threads")["threads"] if t["peer"] == a.account]
    if len(b_threads) != 1:
        raise ProbeError("bob has %d chats with alice" % len(b_threads))
    got = summaries_settle(b, b_threads[0]["id"], 1)
    if len(got) != 1:
        raise ProbeError("bob's history: %s" % got)
    return {"ring_s": round(rang, 2), "answer_to_audio_s": took}


def scenario_calling_each_other_at_once(lab):
    """Both press call at the same moment, then try again: one call must get through."""
    a, a_thread, b, b_thread = open_pair(lab)
    results = {}

    def press(u, thread):
        try:
            results[u.name] = u.cmd("call", thread=thread, check=False)
        except ProbeError as e:
            results[u.name] = {"ok": False, "error": str(e)}

    ts = [threading.Thread(target=press, args=(a, a_thread)), threading.Thread(target=press, args=(b, b_thread))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    # Whatever the glare resolves to, within a short while each side either rings or is calling;
    # answering the ring (if any) must connect both. Otherwise both hang up and Alice calls again.
    time.sleep(3)
    seen = {u.name.split("-")[0]: u.surface.get("kind") for u in (a, b)}
    ringing = [u for u in (a, b) if u.surface.get("kind") == "ringing"]
    if ringing:
        callee = ringing[0]
        callee.cmd("accept_voice")
        caller = b if callee is a else a
        call_id = callee.surface.get("call_id")
        caller.wait_surface("in_call", 40, call_id=call_id)
        callee.wait_surface("in_call", 40, call_id=call_id)
        assert_media_flows(a, b)
        a.cmd("leave")
    else:
        for u in (a, b):
            if u.surface.get("kind") != "none":
                u.cmd("leave", check=False)
    assert_idle(a, b)
    _, took = connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    took["after_glare"] = seen
    return took


def press_together(*actions):
    """Run (user, cmd, args) presses at the same moment; replies by user name."""
    results = {}

    def press(u, cmd, args):
        try:
            results[u.name] = u.cmd(cmd, check=False, **args)
        except ProbeError as e:
            results[u.name] = {"ok": False, "error": str(e)}

    ts = [threading.Thread(target=press, args=a) for a in actions]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return results


def in_one_call(a, b, timeout=30):
    deadline = time.time() + timeout
    while a.surface.get("kind") != "in_call" or b.surface.get("kind") != "in_call" or \
            a.surface.get("call_id") != b.surface.get("call_id"):
        if time.time() > deadline:
            raise ProbeError("not in one call: alice=%s bob=%s" % (a.surface, b.surface))
        time.sleep(0.25)
    assert_media_flows(a, b)


def scenario_both_answer_each_other(lab):
    """Both call at once and anyone whose phone rings taps Answer at the same moment: they must end
    up in one call together, not each in the call the other just left."""
    a, a_thread, b, b_thread = open_pair(lab)
    started = time.time()
    press_together((a, "call", {"thread": a_thread}), (b, "call", {"thread": b_thread}))
    time.sleep(2)
    ringing = [(u, "accept_voice", {}) for u in (a, b) if u.surface.get("kind") == "ringing"]
    if ringing:
        press_together(*ringing)
    in_one_call(a, b)
    together = time.time() - started
    a.cmd("leave")
    assert_idle(a, b)
    # One call happened: one history entry each side (the call that gave way leaves none).
    time.sleep(3)
    for u, thread in ((a, a_thread), (b, b_thread)):
        got = u.call_summaries(thread)
        if len(got) != 1 or got[0].get("outcome") != "completed":
            raise ProbeError("%s history: %s" % (u.name, got))
    return {"together_s": round(together, 2), "rang": [u.name.split("-")[0] for u, _, _ in ringing]}


def scenario_call_back_while_ringing(lab):
    """Alice's call rings Bob; instead of Answer he taps Call on her chat: they talk."""
    a, a_thread, b, b_thread = open_pair(lab)
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    started = time.time()
    b.cmd("call", thread=b_thread)
    in_one_call(a, b)
    together = time.time() - started
    if a.surface.get("call_id") != call_id:
        raise ProbeError("not alice's call: %s" % a.surface)
    a.cmd("leave")
    assert_idle(a, b)
    return {"together_s": round(together, 2)}


def scenario_redial_storm(lab):
    """Alice calls, gives up after a second, calls again — five times — then Bob answers."""
    a, a_thread, b, b_thread = open_pair(lab)
    for _ in range(5):
        call_id = a.cmd("call", thread=a_thread)["call_id"]
        time.sleep(1.0)
        a.cmd("leave", check=False)
    # Bob may still be ringing for the last one; it must stop.
    assert_idle(a, b, timeout=40)
    call_id, took = connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    return took


def scenario_restart_while_ringing(lab):
    """Bob's app dies while his phone rings; he opens it again; Alice keeps trying."""
    a, a_thread, b, b_thread = open_pair(lab)
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    b.restart()
    b.wait_mesh()
    time.sleep(2)
    a.cmd("leave", check=False)
    assert_idle(a, b, timeout=40)
    call_id, took = connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    return took


def wait_state(u, pred, timeout, what):
    deadline = time.time() + timeout
    while True:
        s = u.state()
        if pred(s):
            return s
        if time.time() > deadline:
            raise ProbeError("%s: never %s (%s)" % (u.name, what, s))
        time.sleep(0.25)


def scenario_caller_restarts_mid_call(lab):
    """Alice's app dies in a call and she opens it again: Bob hears the line drop (Reconnecting…),
    then the call ends for both — nothing left behind — and she can call again."""
    a, a_thread, b, b_thread = open_pair(lab)
    connect(a, a_thread, b)
    a.crash()
    died = time.time()
    wait_state(b, lambda s: s.get("media_status") == "Reconnecting", 8, "Reconnecting")
    reconnecting = time.time() - died
    a.start()
    a.wait_ready()
    b.wait_surface("none", 30)
    ended = time.time() - died
    a.wait_mesh()
    assert_idle(a, b, timeout=40)
    _, took = connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    took.update({"bob_reconnecting_s": round(reconnecting, 2), "bob_ended_s": round(ended, 2)})
    return took


def scenario_peer_vanishes_mid_call(lab):
    """Alice's phone drops off the earth mid-call: Bob sees Reconnecting… within seconds and the
    call fails within the reconnect window instead of hanging on a dead line."""
    a, a_thread, b, b_thread = open_pair(lab)
    connect(a, a_thread, b)
    a.crash()
    died = time.time()
    wait_state(b, lambda s: s.get("media_status") == "Reconnecting", 8, "Reconnecting")
    reconnecting = time.time() - died
    wait_state(b, lambda s: s.get("phase") in ("ConnectFailed", "Idle"), 45, "failed or ended")
    failed = time.time() - died
    if b.surface.get("kind") != "none":
        b.cmd("leave")
    assert_idle(b)
    return {"bob_reconnecting_s": round(reconnecting, 2), "bob_failed_s": round(failed, 2)}


def scenario_decline_then_call_back(lab):
    """Bob declines, then calls Alice back."""
    a, a_thread, b, b_thread = open_pair(lab)
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    b.cmd("decline")
    assert_idle(a, b)
    connect(b, b_thread, a)
    b.cmd("leave")
    assert_idle(a, b)
    got = summaries_settle(a, a_thread, 2)
    outcomes = sorted(s.get("outcome") for s in got)
    if outcomes != ["completed", "declined"]:
        raise ProbeError("alice's history: %s" % got)


def assert_incoming_call_notice(u, caller):
    """The ring announced itself (an "Incoming call" notification where the user is not looking)."""
    deadline = time.time() + 5
    while time.time() < deadline:
        with u.lock:
            seen = any(m.get("event") == "notice" and m.get("kind") == "incoming_call" and
                       m.get("identity") == caller.account for _, m in u.history)
        if seen:
            return
        time.sleep(0.2)
    raise ProbeError("%s: no incoming-call notification for %s's ring" % (u.name, caller.name))


def scenario_minimized_still_rings(lab):
    """Bob's desktop window is minimized: Alice's call still rings (a notification, as any
    messenger's would); he restores the window and answers."""
    a, a_thread, b, b_thread = open_pair(lab)
    if b.cmd("minimize").get("attentive"):
        raise ProbeError("bob: a minimized window reads as attended")
    started = time.time()
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 20, call_id=call_id)
    rang = time.time() - started
    assert_incoming_call_notice(b, a)
    b.cmd("restore")
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(a, b)
    a.cmd("leave")
    assert_idle(a, b)
    return {"ring_s": round(rang, 2)}


def scenario_push_wakes_background_callee(lab):
    """Bob's phone has the app in the background (its work paused): Alice's call reaches him by push.
    The push wakes the app, which rings; he opens it and answers."""
    a, a_thread, b, b_thread = open_pair(lab)
    b.cmd("background")
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    time.sleep(2)  # the push takes a moment to arrive
    woke = time.time()
    b.cmd("push_wake")
    b.wait_surface("ringing", 20, call_id=call_id)
    rang = time.time() - woke
    assert_incoming_call_notice(b, a)
    b.cmd("foreground")  # he taps the call notification
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(a, b)
    a.cmd("leave")
    assert_idle(a, b)
    return {"ring_after_push_s": round(rang, 2)}


# The app's background wake window (MessagingLimits.h kBackgroundWakeWindowMs).
BACKGROUND_WAKE_WINDOW_S = 20
# After a call ends in the background the app keeps working this long (AppLifecycle kCallEndGraceMs).
CALL_END_GRACE_S = 5


def scenario_background_is_push_only(lab):
    """Bob's phone app is in the background; a push wakes it with nothing to deliver. Once the wake
    window is over it sleeps again (push only, for power): Alice's call does not ring until its own
    push, or until Bob opens the app — then it rings and he answers."""
    a, a_thread, b, b_thread = open_pair(lab)
    b.cmd("background")
    b.cmd("push_wake")
    time.sleep(BACKGROUND_WAKE_WINDOW_S + 3)
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    try:
        b.wait_surface("ringing", 8, call_id=call_id)
        raise ProbeError("bob: rang with the app asleep in the background (no push for it)")
    except ProbeError as e:
        if "rang with" in str(e):
            raise
    opened = time.time()
    b.cmd("foreground")
    b.wait_surface("ringing", 15, call_id=call_id)
    rang = time.time() - opened
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(a, b)
    a.cmd("leave")
    assert_idle(a, b)
    return {"ring_after_open_s": round(rang, 2)}


def scenario_stay_connected_rings_in_background(lab):
    """Bob's phone has no push service, so he turned on "Stay connected in the background": with the
    app in the background, Alice's call still rings (and notifies) — no push needed."""
    a, a_thread, b, b_thread = open_pair(lab)
    if not b.cmd("stay_connected", on=True).get("stay_connected"):
        raise ProbeError("bob: the setting did not take")
    b.cmd("background")
    time.sleep(BACKGROUND_WAKE_WINDOW_S + 3)  # past any wake window: only the setting keeps it awake
    started = time.time()
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 20, call_id=call_id)
    rang = time.time() - started
    assert_incoming_call_notice(b, a)
    b.cmd("foreground")
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(a, b)
    a.cmd("leave")
    assert_idle(a, b)
    b.cmd("stay_connected", on=False)
    return {"ring_s": round(rang, 2)}


def scenario_peer_hangs_up_while_backgrounded(lab):
    """Alice's phone app is in the background mid-call when Bob hangs up: her call ends then (a call
    keeps the app working in the background), not when she next opens the app. Once it is over the
    app sleeps again: Bob's next call reaches her by push only."""
    a, a_thread, b, b_thread = open_pair(lab)
    connect(a, a_thread, b)
    a.cmd("background")
    time.sleep(BACKGROUND_WAKE_WINDOW_S + 3)  # past any wake window: only the call keeps it awake
    for u in (a, b):
        assert_hears(u, 1, timeout=10)
    b.cmd("leave")
    assert_idle(a, b)
    time.sleep(CALL_END_GRACE_S + 3)
    call_id = b.cmd("call", thread=b_thread)["call_id"]
    try:
        a.wait_surface("ringing", 8, call_id=call_id)
        raise ProbeError("alice: rang with the app asleep in the background (no push for it)")
    except ProbeError as e:
        if "rang with" in str(e):
            raise
    a.cmd("push_wake")
    a.wait_surface("ringing", 20, call_id=call_id)
    a.cmd("foreground")
    a.cmd("decline")
    assert_idle(a, b)


def scenario_answered_in_background(lab):
    """Bob's phone app is in the background; Alice's call wakes it by push and he answers without
    opening the app (the phone's own call screen, CallKit on iPhone). The call outlasts the push's
    wake window, and ends when Alice hangs up."""
    a, a_thread, b, b_thread = open_pair(lab)
    b.cmd("background")
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    time.sleep(2)  # the push takes a moment to arrive
    b.cmd("push_wake")
    b.wait_surface("ringing", 20, call_id=call_id)
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(a, b)
    time.sleep(BACKGROUND_WAKE_WINDOW_S + 3)
    for u in (a, b):
        assert_hears(u, 1, timeout=10)
    a.cmd("leave")
    assert_idle(a, b)


def scenario_background_mid_call(lab):
    """Alice's phone app goes to the background mid-call (she looks something up) and comes back:
    the call goes on both ways the whole time."""
    a, a_thread, b, b_thread = open_pair(lab)
    call_id, _ = connect(a, a_thread, b)
    a.cmd("background")
    for _ in range(3):
        for u in (a, b):
            assert_hears(u, 1, timeout=10)
        if a.surface.get("call_id") != call_id or b.surface.get("call_id") != call_id:
            raise ProbeError("the call did not survive the background (%s / %s)" % (a.surface, b.surface))
    a.cmd("foreground")
    for u in (a, b):
        assert_hears(u, 1, timeout=10)
    a.cmd("leave")
    assert_idle(a, b)


# --- the user's network (--nat: each user is a container behind its own NAT) ---------------------

def container_network(container):
    """(address/prefix, default gateway) of a user container's eth0."""
    r = subprocess.run(["docker", "exec", "--user", "0", container, "sh", "-c",
                        "ip -4 -o addr show eth0 | awk '{print $4; exit}'; "
                        "ip route show default | awk '{print $3; exit}'"],
                       capture_output=True, text=True, check=True)
    address, gw = (r.stdout.split() + ["", ""])[:2]
    return address, gw


def restore_network(container, address, gw):
    """The container's link up with its own address and route again (after net_drop / change_address)."""
    subprocess.run(["docker", "exec", "--user", "0", container, "sh", "-c",
                    "ip link set eth0 up; ip addr flush dev eth0; ip addr add %s dev eth0; "
                    "ip route replace default via %s" % (address, gw)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def net(u, script):
    """Run `script` as root in the user's container (its network namespace is the app's)."""
    if not u.container:
        raise ProbeError("%s: network changes need --nat (a container per user)" % u.name)
    r = subprocess.run(["docker", "exec", "--user", "0", u.container, "sh", "-c", script],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise ProbeError("%s: %s: %s" % (u.name, script, r.stderr.strip()))
    return r.stdout.strip()


def net_drop(u, seconds, during=None):
    """The user's link goes down (Wi-Fi drops) for `seconds`, then comes back. `during` runs halfway."""
    gw = net(u, "ip route show default | awk '{print $3; exit}'")
    net(u, "ip link set eth0 down")
    time.sleep(seconds / 2)
    if during:
        during()
    time.sleep(seconds / 2)
    net(u, "ip link set eth0 up && ip route replace default via %s" % gw)


def not_connected_while_down(u):
    """With its network down, the user's call must not read Connected (nothing flows)."""
    def check():
        s = u.state()
        if s.get("kind") == "in_call" and s.get("status") == "connected":
            raise ProbeError("%s: the call reads Connected with the network down (%s)" % (u.name, s))
    return check


def change_address(u):
    """The user's address on its LAN changes (a DHCP renewal, another access point): its NAT maps it
    anew, so the outside sees it from another port."""
    old = net(u, "ip -4 -o addr show eth0 | awk '{print $4; exit}'")  # e.g. 10.119.1.10/24
    gw = net(u, "ip route show default | awk '{print $3; exit}'")
    ip, prefix = old.split("/")
    parts = ip.split(".")
    new = ".".join(parts[:3] + [str(20 + (int(parts[3]) + 1) % 200)])
    # Delete first: deleting the primary address also deletes the secondaries on its subnet.
    net(u, "ip addr del %s dev eth0 && ip addr add %s/%s dev eth0 && ip route replace default via %s src %s"
        % (old, new, prefix, gw, new))
    return new


def still_in_call(users, call_id, timeout):
    """The same call comes back for everyone: in the call, hearing the others."""
    for u in users:
        u.wait_surface("in_call", timeout, call_id=call_id)
    for u in users:
        assert_hears(u, len(users) - 1, timeout=timeout)


def scenario_network_drops_mid_call(lab):
    """Bob's network drops for 6 s mid-call (behind his NAT) and comes back: the same call recovers,
    audio both ways."""
    a, a_thread, b, b_thread = open_pair(lab)
    call_id, _ = connect(a, a_thread, b)
    started = time.time()
    net_drop(b, 6, during=not_connected_while_down(b))
    still_in_call((a, b), call_id, 45)
    recovered = time.time() - started - 6
    a.cmd("leave")
    assert_idle(a, b)
    return {"recovered_after_up_s": round(recovered, 1)}


def scenario_address_changes_mid_call(lab):
    """Bob's address changes mid-call (his NAT maps him anew): the same call goes on, audio both ways."""
    a, a_thread, b, b_thread = open_pair(lab)
    call_id, _ = connect(a, a_thread, b)
    started = time.time()
    change_address(b)
    still_in_call((a, b), call_id, 45)
    recovered = time.time() - started
    a.cmd("leave")
    assert_idle(a, b)
    return {"recovered_s": round(recovered, 1)}


def scenario_group_member_network_drops(lab):
    """Carol's network drops for 6 s in a three-way call (on the hop) and comes back: everyone hears
    everyone again in the same call."""
    (a, b, c), threads = open_group(lab)
    call_id = group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    started = time.time()
    net_drop(c, 6, during=not_connected_while_down(c))
    still_in_call((a, b, c), call_id, 60)
    recovered = time.time() - started - 6
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)
    return {"recovered_after_up_s": round(recovered, 1)}


# Past a seed link's liveness: Amp keeps a hot link alive every 20 s and drops it after missed echoes.
SEED_LINK_GONE_S = 45


def stop_hop(lab):
    subprocess.run(["docker", "stop", "-t", "1", lab.hop_container], check=True, stdout=subprocess.DEVNULL)


def start_hop(lab):
    subprocess.run(["docker", "start", lab.hop_container], check=True, stdout=subprocess.DEVNULL)
    lab.wait_hop()


def scenario_seed_down(lab):
    """The relay seed goes away (crashed, unreachable) after both are online: the call still
    connects directly, and the answer does not sit out the whole seed-park wait."""
    a, a_thread, b, b_thread = open_pair(lab)
    stop_hop(lab)
    try:
        _, took = connect(a, a_thread, b, timeout=40)
        a.cmd("leave")
        assert_idle(a, b)
    finally:
        start_hop(lab)
    return took


def scenario_seed_dies_in_use(lab):
    """The seed carried the pair's calls, then dies: a call right away (its link still reads up) and
    one after the link timed out both connect without sitting out the seed park."""
    a, a_thread, b, b_thread = open_pair(lab)
    connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    stop_hop(lab)
    try:
        _, inside = connect(a, a_thread, b, timeout=40)
        a.cmd("leave")
        assert_idle(a, b)
        time.sleep(SEED_LINK_GONE_S)
        _, after = connect(b, b_thread, a, timeout=40)
        b.cmd("leave")
        assert_idle(a, b)
    finally:
        start_hop(lab)
    return {"inside_link_window": inside, "after_link_gone": after}


# --- group calls ----------------------------------------------------------------------------------
# docs/architecture/CALLS.md (V050): the first answer is a 1:1 call; the third person moves the call
# onto the planned hop (media_relay). Hostless: whoever placed it may leave and the rest go on.


def open_group(lab, n=3, title="lab group"):
    """n people; the first creates a group with the others, who accept its invites."""
    names = ["alice", "bob", "carol", "dave", "erin"][:n]
    users = [lab.user(name) for name in names]
    for u in users:
        u.wait_mesh()
    owner, members = users[0], users[1:]
    for m in members:
        owner.cmd("open_chat", account=m.account)
        m.cmd("open_chat", account=owner.account)
    created = owner.cmd("create_group", title=title, accounts=[m.account for m in members])
    group_id = created["group_id"]
    threads = {owner.name: created["thread"]}
    for m in members:
        invite = wait_for(lambda: next((i for i in m.cmd("group_invites")["invites"] if i["group_id"] == group_id),
                                       None), 40, "%s: the group invite" % m.name)
        threads[m.name] = m.cmd("accept_group_invite", nonce=invite["nonce"], group_id=group_id)["thread"]
        if not threads[m.name]:
            raise ProbeError("%s: no group thread after accepting" % m.name)
    everyone = sorted(u.account for u in users)
    for u in users:
        wait_for(lambda: sorted(u.cmd("group_roster", group_id=group_id)["members"]) == everyone or None, 40,
                 "%s: the whole roster" % u.name)
    return users, threads


def wait_for(fn, timeout, what):
    deadline = time.time() + timeout
    while True:
        got = fn()
        if got:
            return got
        if time.time() > deadline:
            raise ProbeError("timed out waiting for %s" % what)
        time.sleep(0.5)


def live_streams(u):
    return {row["stream_id"]: row["rx_frames"] for row in u.state().get("streams", [])}


def assert_hears(u, speakers, timeout=30):
    """`u` is in a call and hears `speakers` others: that many streams keep receiving."""
    first = live_streams(u)
    deadline = time.time() + timeout
    while True:
        time.sleep(1.0)
        now = live_streams(u)
        growing = [sid for sid, rx in now.items() if rx > first.get(sid, 0)]
        s = u.surface
        if s.get("kind") == "in_call" and len(growing) >= speakers:
            return
        if time.time() > deadline:
            raise ProbeError("%s: hears %d of %d (streams %s, surface %s)" % (u.name, len(growing), speakers, now, s))
        first = now


def group_call(caller, thread, answerers, timeout=40, **call_args):
    """`caller` calls the group; each of `answerers` answers in turn. Returns the call id."""
    call_id = caller.cmd("call", thread=thread, **call_args)["call_id"]
    for u in answerers:
        u.wait_surface("ringing", timeout, call_id=call_id)
    for u in answerers:
        u.cmd("accept_voice")
        u.wait_surface("in_call", timeout, call_id=call_id)
    caller.wait_surface("in_call", timeout, call_id=call_id)
    return call_id


def assert_group_history(u, thread, outcome, timeout=20):
    got = summaries_settle(u, thread, 1, timeout)
    if len(got) != 1 or got[0].get("outcome") != outcome:
        raise ProbeError("%s's history: %s (wanted one %s)" % (u.name, got, outcome))


def scenario_group_call_three(lab):
    """Alice calls the group; Bob and Carol answer: everyone hears both others."""
    (a, b, c), threads = open_group(lab)
    started = time.time()
    group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    took = round(time.time() - started, 2)
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)
    for u in (a, b, c):
        assert_group_history(u, threads[u.name], "completed")
    return {"all_hear_all_s": took}


def scenario_group_one_declines(lab):
    """Carol declines; Alice and Bob talk. Carol's history says she declined."""
    (a, b, c), threads = open_group(lab)
    call_id = a.cmd("call", thread=threads[a.name])["call_id"]
    for u in (b, c):
        u.wait_surface("ringing", 40, call_id=call_id)
    c.cmd("decline")
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
        assert_hears(u, 1)
    c.wait_surface("none", 20)
    a.cmd("leave")
    a.wait_surface("none", 20)
    time.sleep(2)
    if b.state().get("call_id") != call_id:
        raise ProbeError("%s: left the group call when Alice did (the last one in stays)" % b.name)
    b.cmd("leave")
    assert_idle(a, b, c)
    assert_group_history(c, threads[c.name], "declined")
    assert_group_history(a, threads[a.name], "completed")


def scenario_group_leave_one_by_one(lab):
    """Bob leaves a three-way call: Alice and Carol go on, still on the hop. Then they leave."""
    (a, b, c), threads = open_group(lab)
    group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    b.cmd("leave")
    b.wait_surface("none", 20)
    for u in (a, c):
        assert_hears(u, 1)
        if not u.state().get("sfu_mode"):
            raise ProbeError("%s: left the hop when Bob left (N→2 stays on the hop)" % u.name)
    a.cmd("leave")
    c.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_caller_leaves_first(lab):
    """Alice placed the call and leaves first: Bob and Carol go on (no one hosts the call)."""
    (a, b, c), threads = open_group(lab)
    group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    a.cmd("leave")
    a.wait_surface("none", 20)
    for u in (b, c):
        assert_hears(u, 1)
    b.cmd("leave")
    c.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_add_someone_mid_call(lab):
    """Alice calls Bob only; mid-call she adds Carol, who answers: the call moves onto the hop."""
    (a, b, c), threads = open_group(lab)
    call_id = group_call(a, threads[a.name], [b], accounts=[b.account])
    for u in (a, b):
        assert_hears(u, 1)
    a.cmd("invite_to_call", account=c.account)
    c.wait_surface("ringing", 40, call_id=call_id)
    c.cmd("accept_voice")
    c.wait_surface("in_call", 40, call_id=call_id)
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_member_crashes(lab):
    """Bob's app dies mid-call: Alice and Carol keep talking; Bob comes back to nothing stuck."""
    (a, b, c), threads = open_group(lab)
    group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    b.crash()
    for u in (a, c):
        assert_hears(u, 1)
    b.start()
    b.wait_ready()
    b.wait_surface("none", 40)
    for u in (a, c):
        assert_hears(u, 1)
    a.cmd("leave")
    c.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_hop_down_mid_call(lab):
    """The hop carrying a three-way call goes down and comes back: each person ends up either back
    in the call hearing the others or out of it — never in a call that carries nothing."""
    (a, b, c), threads = open_group(lab)
    group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    stop_hop(lab)
    try:
        time.sleep(5)
    finally:
        start_hop(lab)
    outcome = {}
    for u in (a, b, c):
        deadline = time.time() + 90
        while True:
            if u.surface.get("kind") == "none":
                outcome[u.name.split("-")[0]] = "ended"
                break
            try:
                assert_hears(u, 2, timeout=5)
                outcome[u.name.split("-")[0]] = "recovered"
                break
            except ProbeError:
                if time.time() > deadline:
                    raise ProbeError("%s: still in a call that carries nothing (%s)" % (u.name, u.state()))
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)
    return outcome


# --- V053: join while it lasts, stay when alone ----------------------------------------------------


def join(u, thread, call_id=None, timeout=40):
    """The call button on the group: joins the call that goes on (no ring)."""
    got = u.cmd("call", thread=thread)["call_id"]
    if call_id is not None and got != call_id:
        raise ProbeError("%s: the call button placed a new call %s instead of joining %s" % (u.name, got, call_id))
    u.wait_surface("in_call", timeout, call_id=got)
    return got


def scenario_group_decline_then_join(lab):
    """Carol declines, then thinks better of it: the group's call button gets her into that call."""
    (a, b, c), threads = open_group(lab)
    call_id = a.cmd("call", thread=threads[a.name])["call_id"]
    for u in (b, c):
        u.wait_surface("ringing", 40, call_id=call_id)
    c.cmd("decline")
    b.cmd("accept_voice")
    for u in (a, b):
        assert_hears(u, 1, timeout=40)
    c.wait_surface("none", 20)
    started = time.time()
    join(c, threads[c.name], call_id)
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    took = round(time.time() - started, 2)
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)
    return {"join_to_all_hear_s": took}


def scenario_group_leave_and_rejoin(lab):
    """Bob drops out of a three-way call and comes back to the same call."""
    (a, b, c), threads = open_group(lab)
    call_id = group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    b.cmd("leave")
    b.wait_surface("none", 20)
    for u in (a, c):
        assert_hears(u, 1)
    join(b, threads[b.name], call_id)
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_last_one_waits(lab):
    """Alice and Bob leave; Carol stays in the call, waiting. Bob comes back: the two talk again."""
    (a, b, c), threads = open_group(lab)
    call_id = group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    a.cmd("leave")
    b.cmd("leave")
    for u in (a, b):
        u.wait_surface("none", 20)
    time.sleep(3)
    s = c.state()
    if s.get("kind") != "in_call" or s.get("call_id") != call_id:
        raise ProbeError("%s: left the call when she was the last one in (%s)" % (c.name, s))
    join(b, threads[b.name], call_id)
    for u in (b, c):
        assert_hears(u, 1, timeout=40)
    for u in (b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_late_starter(lab):
    """Carol's app is closed through the whole ring. She opens it later and joins the call that is
    still going (her ring has long expired)."""
    (a, b, c), threads = open_group(lab)
    c.quit()
    call_id = a.cmd("call", thread=threads[a.name])["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    b.cmd("accept_voice")
    for u in (a, b):
        assert_hears(u, 1, timeout=40)
    time.sleep(65)  # past the 60 s ring
    c.start()
    c.wait_ready()
    # Her app catches up (the invite comes in too late to ring): the group offers Join.
    offered = wait_for(lambda: c.cmd("joinable", thread=threads[c.name])["call_id"] or None, 60,
                       "%s: the group's call offered to join" % c.name)
    if offered != call_id:
        raise ProbeError("%s: offered %s, the call is %s" % (c.name, offered, call_id))
    if c.state().get("kind") != "none":
        raise ProbeError("%s: a ring that ran out rang anyway (%s)" % (c.name, c.state()))
    join(c, threads[c.name], call_id, timeout=60)
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_member_removed(lab):
    """Alice (the owner) removes Carol from the group mid-call: Carol is out of the call and cannot
    join it again; Alice and Bob go on."""
    (a, b, c), threads = open_group(lab)
    call_id = group_call(a, threads[a.name], [b, c])
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    a.cmd("remove_member", thread=threads[a.name], account=c.account)
    c.wait_surface("none", 30)
    for u in (a, b):
        wait_for(lambda: len(u.state().get("participants", [])) == 2 or None, 20,
                 "%s: Carol out of the call" % u.name)
        assert_hears(u, 1)
    if c.cmd("joinable", thread=threads[c.name])["call_id"]:
        raise ProbeError("%s: offered to join the call of a group she was removed from" % c.name)
    for u in (a, b):
        if u.state().get("call_id") != call_id:
            raise ProbeError("%s: not in the call any more (%s)" % (u.name, u.state()))
        u.cmd("leave", check=False)
    assert_idle(a, b, c)


def scenario_group_calling_at_once(lab):
    """Alice and Bob press Call on the group at the same moment: they end up in one call (the same
    one on each side), and Carol answers that call."""
    (a, b, c), threads = open_group(lab)
    ids = [a.cmd("call", thread=threads[a.name])["call_id"], b.cmd("call", thread=threads[b.name])["call_id"]]
    winner = min(ids)
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=winner)
    c.wait_surface("ringing", 40, call_id=winner)
    c.cmd("accept_voice")
    c.wait_surface("in_call", 40, call_id=winner)
    for u in (a, b, c):
        assert_hears(u, 2, timeout=40)
    for u in (a, b, c):
        u.cmd("leave", check=False)
    assert_idle(a, b, c)
    return {"two_calls_placed": ids[0] != ids[1]}


SCENARIOS = [
    ("first_contact", scenario_first_contact),
    ("calling_each_other_at_once", scenario_calling_each_other_at_once),
    ("both_answer_each_other", scenario_both_answer_each_other),
    ("call_back_while_ringing", scenario_call_back_while_ringing),
    ("redial_storm", scenario_redial_storm),
    ("restart_while_ringing", scenario_restart_while_ringing),
    ("caller_restarts_mid_call", scenario_caller_restarts_mid_call),
    ("peer_vanishes_mid_call", scenario_peer_vanishes_mid_call),
    ("decline_then_call_back", scenario_decline_then_call_back),
    ("minimized_still_rings", scenario_minimized_still_rings),
    ("push_wakes_background_callee", scenario_push_wakes_background_callee),
    ("background_mid_call", scenario_background_mid_call),
    ("peer_hangs_up_while_backgrounded", scenario_peer_hangs_up_while_backgrounded),
    ("answered_in_background", scenario_answered_in_background),
    ("background_is_push_only", scenario_background_is_push_only),
    ("stay_connected_rings_in_background", scenario_stay_connected_rings_in_background),
    ("network_drops_mid_call", scenario_network_drops_mid_call),
    ("address_changes_mid_call", scenario_address_changes_mid_call),
    ("group_member_network_drops", scenario_group_member_network_drops),
    ("seed_down", scenario_seed_down),
    ("seed_dies_in_use", scenario_seed_dies_in_use),
    ("group_call_three", scenario_group_call_three),
    ("group_one_declines", scenario_group_one_declines),
    ("group_leave_one_by_one", scenario_group_leave_one_by_one),
    ("group_caller_leaves_first", scenario_group_caller_leaves_first),
    ("group_add_someone_mid_call", scenario_group_add_someone_mid_call),
    ("group_member_crashes", scenario_group_member_crashes),
    ("group_hop_down_mid_call", scenario_group_hop_down_mid_call),
    ("group_decline_then_join", scenario_group_decline_then_join),
    ("group_leave_and_rejoin", scenario_group_leave_and_rejoin),
    ("group_last_one_waits", scenario_group_last_one_waits),
    ("group_late_starter", scenario_group_late_starter),
    ("group_member_removed", scenario_group_member_removed),
    ("group_calling_at_once", scenario_group_calling_at_once),
]

# Scenarios run only when named (--only).
OPT_IN = set()

# Scenarios that take the hop away: behind NATs there is then no path between users at all, so with
# --nat they run only when named.
NEEDS_DIRECT_PATH = {"seed_down", "seed_dies_in_use", "group_hop_down_mid_call"}
# Scenarios that change a user's network: they need a container per user (--nat).
NEEDS_NAT = {"network_drops_mid_call", "address_changes_mid_call", "group_member_network_drops"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--relay", default="http://127.0.0.1:18719/api/relay", help="lab relay base url")
    ap.add_argument("--hop", help="mesh seed multiaddr (default: the lab hop, from its /status)")
    ap.add_argument("--hop-status", default="http://127.0.0.1:18718")
    ap.add_argument("--hop-ip", default="198.51.100.10", help="the lab hop's address on the lab network")
    ap.add_argument("--relay-container", default="pp-app-lab-www-1",
                    help="lab relay gateway container, restarted when registrations run out")
    ap.add_argument("--hop-container", default="pp-app-lab-hop-1", help="the lab seed (seed_down stops it)")
    ap.add_argument("--nat", action="store_true",
                    help="each user in its own container behind its own NAT (docker-compose.app-lab-nat.yml)")
    ap.add_argument("--nat-containers", default="pp-app-lab-user-a-1,pp-app-lab-user-b-1,pp-app-lab-user-c-1")
    ap.add_argument("--nat-relay", default="http://198.51.100.20:9097/api/relay",
                    help="the relay as users behind the NATs reach it")
    ap.add_argument("--nat-share", default=os.environ.get("PP_APP_LAB_NAT_SHARE", "/tmp/pp-app-lab-nat"),
                    help="profile dirs, mounted at the same path in the user containers")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--keep", action="store_true", help="keep profile dirs and logs of passing runs")
    args = ap.parse_args()
    if not args.hop:
        import urllib.request
        with urllib.request.urlopen(args.hop_status + "/status", timeout=5) as r:
            peer = json.load(r)["peer_id"]
        args.hop = "/ip4/%s/udp/18517/adp/1.0.0/p2p/%s" % (args.hop_ip, peer)

    skipped = OPT_IN | (NEEDS_DIRECT_PATH if args.nat else NEEDS_NAT)
    selected = [(n, f) for n, f in SCENARIOS if n in args.only or (not args.only and n not in skipped)]
    if not selected:
        sys.exit("no scenario matches %s" % args.only)
    failures = 0
    for round_no in range(args.repeat):
        for name, fn in selected:
            lab = Lab(args)
            started = time.time()
            ok = False
            try:
                extra = fn(lab) or {}
                ok = True
                print("PASS %-30s %5.1fs %s" % (name, time.time() - started, json.dumps(extra) if extra else ""))
            except Exception as e:  # noqa: BLE001 — report every way a scenario can go wrong
                failures += 1
                print("FAIL %-30s %5.1fs %s" % (name, time.time() - started, e))
                if not isinstance(e, ProbeError):
                    traceback.print_exc()
                for u in lab.users:
                    tail = [m for _, m in u.history[-12:]]
                    print("  %s surface=%s" % (u.name, u.surface))
                    for m in tail:
                        print("    %s" % json.dumps(m))
                print("  profiles and logs kept in %s" % lab.root)
            finally:
                lab.close(ok)
            sys.stdout.flush()
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
