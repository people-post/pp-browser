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
import base64
import contextlib
import json
import os
import queue
import random
import re
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


# A directory nobody answers (the discard port): lookups fail at once.
NO_DIRECTORY = "http://127.0.0.1:9/api/relay"


class User:
    """One person with the app: a pp-app-probe process over a profile dir that outlives it."""

    def __init__(self, lab, name, container=None, directory=True, reachable=False, seeds=None):
        self.lab = lab
        self.name = name
        self.container = container  # --nat: the user's own NAT'd container (None: a host process)
        self.dir = os.path.join(lab.root, name)
        os.makedirs(self.dir, exist_ok=True)
        self.config = os.path.join(self.dir, "config.json")
        with open(self.config, "w") as f:
            json.dump(lab.config_for(self.dir, directory, reachable, seeds), f, indent=2)
        self.proc = None
        self.next_id = 1
        self.replies = {}
        self.events = queue.Queue()
        self.surface = {"kind": "none"}
        self.history = []  # every event, for the failure report
        self.account = ""
        self.peer_id = ""  # this device's, as last seen ready (a restore after a crash compares against it)
        self.lock = threading.Condition()
        self.runs = 0

    # --- process --------------------------------------------------------------------------------

    def start(self, restore_phrase_file=None, saved_copy_file=None):
        self.runs += 1
        log = os.path.join(self.dir, "probe-%d.log" % self.runs)
        argv = ["--config", self.config, "--pin", PIN, "--log-file", log]
        if restore_phrase_file:
            argv += ["--restore-phrase-file", restore_phrase_file]
        if saved_copy_file:
            argv += ["--restore-saved-copy", saved_copy_file]
        if self.lab.synthetic_video:
            argv += ["--synthetic-video"]
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

    def uninstall(self):
        """The app and its data dir gone (all but the lab's config and the logs of earlier runs)."""
        self.quit()
        for entry in os.listdir(self.dir):
            if entry == "config.json" or re.match(r"probe-\d+\.log$", entry):
                continue
            path = os.path.join(self.dir, entry)
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            else:
                os.remove(path)

    def restore_from_phrase(self, phrase, saved_copy=None):
        """Every device is gone; a fresh install restores the account from its recovery phrase (multi-device
        M023): a new device of the same account, its state from the locker — and from `saved_copy` (an exported
        locker file's text) when given. The app may already be dead (a crashed or lost phone)."""
        before = self.cmd("whoami") if self.alive() else {"account": self.account, "peer_id": self.peer_id}
        self.uninstall()
        phrase_file = os.path.join(self.dir, "restore-phrase.txt")
        with open(phrase_file, "w") as f:
            f.write(phrase + "\n")
        copy_file = None
        if saved_copy is not None:
            copy_file = os.path.join(self.dir, "saved-copy.json")
            with open(copy_file, "w") as f:
                f.write(saved_copy)
        self.surface = {"kind": "none"}
        self.start(restore_phrase_file=phrase_file, saved_copy_file=copy_file)
        os.remove(phrase_file)
        if copy_file:
            os.remove(copy_file)
        who = self.wait_ready()
        if who.get("account") != before.get("account") or who.get("peer_id") == before.get("peer_id"):
            raise ProbeError("%s: restored as %s device %s (was %s device %s)" % (
                self.name, who.get("account"), who.get("peer_id"), before.get("account"), before.get("peer_id")))

    def log_text(self):
        """Everything this user's app has logged, every run."""
        out = []
        for run in range(1, self.runs + 1):
            try:
                with open(os.path.join(self.dir, "probe-%d.log" % run), errors="replace") as f:
                    out.append(f.read())
            except OSError:
                pass
        return "".join(out)

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
                self.peer_id = who.get("peer_id") or self.peer_id
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
        self.hop2 = args.hop2  # the second seed (call_path_*_two_hops)
        self.hop2_status = args.hop2_status
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
        # Video scenarios set it before their users start: the probes take SyntheticVideo's camera and codec.
        self.synthetic_video = False
        self.users = []

    def config_for(self, data_dir, directory=True, reachable=False, seeds=None):
        return {
            "config_version": 1,
            "data_dir": data_dir,
            "relay": {"base_url": self.user_relay},
            # directory=False: a directory nobody answers (an empty one means the built-in default).
            "directory": {"base_url": self.user_relay if directory else NO_DIRECTORY},
            "registration": {"base_url": self.user_relay},
            "mesh": {
                "node_enabled": False,
                "mesh_enabled": True,
                "amp_udp_port": 0,
                # seeds: the ones the user bootstraps from (default the lab hop alone; the two-hop rows add hop2).
                "bootstrap_peers": seeds or [self.hop],
                "capabilities": {"dht": False},
                # Me → Network → Reachable without the relay: keep a reservation on the seed (the lab hop).
                "reachable_without_relay": reachable,
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

    def user(self, name, directory=True, reachable=False, beside=None, seeds=None):
        """A registered user; directory=False: one whose directory never answers; reachable=True: one that
        stays reachable without the relay (parked on the lab's seed); beside=<user>: --nat, on that user's
        network (the same Wi-Fi), not behind a NAT of its own; seeds=[...]: the seeds it bootstraps from
        (default the lab hop alone)."""
        self.fresh_registration()
        if beside is not None:
            container = beside.container
        else:
            container = self.containers[len(self.users) % len(self.containers)] if self.containers else None
        u = User(self, "%s-%s" % (name, uuid.uuid4().hex[:6]), container, directory, reachable, seeds)
        self.users.append(u)
        u.start()
        u.wait_ready()
        u.cmd("register", nickname=u.name, timeout=60)
        u.account = u.cmd("whoami")["account"]
        if not u.account:
            raise ProbeError("%s: no account after register" % u.name)
        return u

    def linked_device(self, of, name, bundle=None):
        """Another device of `of`'s account (M021): a fresh install that takes `of`'s link bundle (Me →
        Security → Copy link payload), then starts on it — the same account, a device of its own. `bundle`: one
        exported earlier (`of` may be gone by now)."""
        if bundle is None:
            bundle = of.cmd("link_export")["bundle"]
        u = User(self, "%s-%s" % (name, uuid.uuid4().hex[:6]), of.container)
        self.users.append(u)
        u.start()
        u.wait_ready()
        u.cmd("link_import", bundle=bundle, pin=PIN, timeout=60)
        u.restart()
        u.account = u.cmd("whoami")["account"]
        if u.account != of.account:
            raise ProbeError("%s: linked as %s, not %s" % (u.name, u.account, of.account))
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


def set_call_invite_policy(u, policy):
    """Me → Security → Who can call me (privacy P007). A new profile takes contacts_only: a call from someone
    the user has not added is dropped silently, so a scenario about strangers' calls says so here."""
    got = u.cmd("call_invite_policy", policy=policy).get("call_invite_policy")
    if got != policy:
        raise ProbeError("%s: who can call me is %r, not %r" % (u.name, got, policy))


def summaries_settle(u, thread, want, timeout=20):
    deadline = time.time() + timeout
    while True:
        got = u.call_summaries(thread)
        if len(got) >= want or time.time() > deadline:
            return got
        time.sleep(0.5)


def chats_with_settle(u, peer, want=1, timeout=20):
    """`u`'s chats with `peer`, once there are `want` (the call's record lands with the inbox that carries it)."""
    deadline = time.time() + timeout
    while True:
        got = [t for t in u.cmd("threads")["threads"] if t["peer"] == peer]
        if len(got) >= want or time.time() > deadline:
            return got
        time.sleep(0.5)


# --- scenarios ------------------------------------------------------------------------------------

def scenario_first_contact(lab):
    """Bob has never heard of Alice: her call is the first thing he gets from her. A stranger's call rings
    only where Who can call me lets everyone call (privacy P007: a new profile takes contacts only, which
    drops it silently) — Bob chose Everyone."""
    a = lab.user("alice")
    b = lab.user("bob")
    a.wait_mesh()
    b.wait_mesh()
    set_call_invite_policy(b, "everyone")
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
    b_threads = chats_with_settle(b, a.account)
    if len(b_threads) != 1:
        raise ProbeError("bob has %d chats with alice" % len(b_threads))
    got = summaries_settle(b, b_threads[0]["id"], 1)
    if len(got) != 1:
        raise ProbeError("bob's history: %s" % got)
    return {"ring_s": round(rang, 2), "answer_to_audio_s": took}


def scenario_bystander_is_no_relay(lab):
    """#379: Carol's phone is on Bob's Wi-Fi with a live link to him, a contact of his, and no relay.
    Alice, behind her own NAT, calls Bob: the call needs a circuit and goes through the lab's relay
    within the usual bound. Carol's phone is never asked to carry it — it would refuse, and every try
    spends the call's time."""
    a = lab.user("alice")
    b = lab.user("bob")
    c = lab.user("carol", beside=b)
    for u in (a, b, c):
        u.wait_mesh()
    carol_peer = c.cmd("whoami").get("peer_id", "")
    if not carol_peer:
        raise ProbeError("carol: no peer id")
    # Carol and Bob talk: each keeps the other as a contact (device included), linked over their Wi-Fi.
    c_thread = c.cmd("open_chat", account=b.account)["thread"]
    b.cmd("open_chat", account=c.account)
    connect(c, c_thread, b)
    c.cmd("leave")
    assert_idle(b, c)
    # Alice and Bob have added each other: Bob's phone rings for contacts only (privacy P007).
    a_thread = a.cmd("open_chat", account=b.account)["thread"]
    b.cmd("open_chat", account=a.account)
    _, timing = connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    logs = {u.name: u.log_text() for u in (a, b)}
    if not any("EnsureViaCircuit ok relay=" in text or "EnsureViaCircuit nested ok relay=" in text
               for text in logs.values()):
        raise ProbeError("no call went through a circuit: the scenario checks nothing here")
    for name, text in logs.items():
        if "relay=" + carol_peer in text:
            raise ProbeError("%s: tried carol's phone as a circuit relay" % name)
        if "circuit-relay service not ready" in text:
            raise ProbeError("%s: a peer that is no relay was asked for a circuit" % name)
    return timing


# Seconds after an idle app reconnects to the seeds that a call starts: either side of the 20 s mark
# where that link dies (a 15 s reservation nothing renews, then 5 s cold — #382).
SEED_CHURN_CALL_OFFSETS_S = (3, 12, 18.5, 19.5, 26)
# The idle seed link dies this long after it connects (and the call must outlive that mark).
SEED_CHURN_LINK_LIFE_S = 20


def seed_link_events(u):
    """The user's link to the lab seed, as its log tells it: "up" / "down" in order."""
    hop_peer = u.lab.hop.rsplit("/p2p/", 1)[-1]
    events = []
    for line in u.log_text().splitlines():
        if "link connected key=%s " % hop_peer in line:
            events.append("up")
        elif "link dropped key=%s " % hop_peer in line:
            events.append("down")
    return events


def assert_audio_still_flows(*users, window_s=2.0):
    """Audio is arriving now, not only earlier in the call: each user's received frames keep rising."""
    before = {u.name: u.state().get("rx_audio_frames", 0) for u in users}
    time.sleep(window_s)
    for u in users:
        now = u.state().get("rx_audio_frames", 0)
        if now <= before[u.name]:
            raise ProbeError("%s: no audio for %.0f s (rx frames %s -> %s)" % (u.name, window_s, before[u.name], now))


def scenario_call_during_seed_churn(lab):
    """#382: an idle app's link to the seeds comes and goes (it reconnects, reserves once, and the
    link dies 20 s later). Calls placed at any point of that cycle — just before the link dies too —
    connect within the usual bound and keep their audio past the moment the idle link would have died."""
    a, a_thread, b, b_thread = open_pair(lab)
    # Any first call parks on the seeds, after which every reconnect to them reserves again.
    connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b)
    timings = {}
    for offset in SEED_CHURN_CALL_OFFSETS_S:
        for u in (a, b):
            wait_for(lambda u=u: not seed_link_events(u) or seed_link_events(u)[-1] == "down", 60,
                     "%s: the idle seed link to drop" % u.name)
        ups = {u.name: seed_link_events(u).count("up") for u in (a, b)}
        for u in (a, b):
            u.cmd("warm_seeds")  # what reconnects an idle app to the seeds in the field
        for u in (a, b):
            wait_for(lambda u=u: seed_link_events(u).count("up") > ups[u.name], 20, "%s: seed reconnect" % u.name)
        reconnected = time.time()
        time.sleep(max(0.0, reconnected + offset - time.time()))
        _, took = connect(a, a_thread, b)
        # Past the mark where the idle link would have died: the call still carries audio both ways.
        time.sleep(max(0.0, reconnected + SEED_CHURN_LINK_LIFE_S + 5 - time.time()))
        assert_audio_still_flows(a, b)
        a.cmd("leave")
        assert_idle(a, b)
        timings["at_%ss" % offset] = took["answer_to_audio_s"]
    return timings


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


# The app's background wake window (BackgroundSyncScheduler.h kBackgroundWakeWindowMs).
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
    opening the app (the phone's own call screen). The call outlasts the push's
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
    return root_in(u.container, script, u.name)


def root_in(container, script, who):
    r = subprocess.run(["docker", "exec", "--user", "0", container, "sh", "-c", script],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise ProbeError("%s: %s: %s" % (who, script, r.stderr.strip()))
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


def net_drop_together(users, seconds):
    """Several users' links go down at the same moment (each behind its own NAT) and come back together."""
    gws = [(u, net(u, "ip route show default | awk '{print $3; exit}'")) for u in users]
    for u, _ in gws:
        net(u, "ip link set eth0 down")
    time.sleep(seconds)
    for u, gw in gws:
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


# A phone's link (--nat): delay and jitter (ms) and loss (%, `loss_corr` % burstiness) each way,
# upload / download in kbit/s, `limit` packets queued (netem's default 1000 holds seconds at a phone's rate).
# On the road a cell link is lossy and jittery and its upload thin. A weak one is a phone at the edge of
# coverage: slow and lossy in bursts, about a second of queue — not reordering (cells rarely reorder; wide
# netem jitter does) and not 6 % independent loss each way, which no call app survives.
PHONE_ROAD = {"delay": 60, "jitter": 40, "loss": 3, "up": 600, "down": 2000}
PHONE_WEAK = {"delay": 150, "jitter": 20, "loss": 2, "loss_corr": 25, "up": 256, "down": 1000, "limit": 200}
# One of PHONE_ROAD's troubles at a time, for the video measurements (video_*_{lossy,jittery,narrow}): loss alone
# on a wide link, reordering jitter alone, and its thin upload alone.
PHONE_LOSSY = {"delay": 60, "loss": 3, "up": 4000, "down": 8000}
PHONE_JITTERY = {"delay": 60, "jitter": 40, "loss": 0, "up": 4000, "down": 8000}
PHONE_NARROW = {"delay": 60, "loss": 0, "up": 600, "down": 2000}
# Loss in bursts, as a cell fade or a busy Wi-Fi gives (netem's Gilbert-Elliott model): `burst` (p, r) in %, p the
# chance a packet starts a burst and r that one ends it — bursts of 1/r packets (~10), p / (p + r) of them lost
# (~3 %, as PHONE_LOSSY's independent loss). A burst hides two or three audio packets in a row, which FEC
# (one packet back) cannot recover: what concealment is for.
PHONE_BURSTY = {"delay": 60, "burst": (0.3, 10), "up": 4000, "down": 8000}
# None of them: what a call puts on the wire when nothing holds it back.
PHONE_WIDE = {"delay": 20, "loss": 0, "up": 8000, "down": 8000}
# A long path and nothing else (a US <-> China call through a relay): 250 ms more round trip on each user's link.
PHONE_FAR = {"delay": 125, "loss": 0, "up": 4000, "down": 8000}
# The same long path, lossy as a long mobile one is.
PHONE_FAR_LOSSY = {"delay": 125, "loss": 3, "up": 4000, "down": 8000}


def gw_net(u, script):
    """Run `script` as root on the user's NAT gateway (user-x's is gw-x)."""
    if not u.container:
        raise ProbeError("%s: network changes need --nat (a container per user)" % u.name)
    return root_in(u.container.replace("user-", "gw-", 1), script, u.name + "'s gateway")


def gw_inner_dev(u):
    return gw_net(u, "ip -o -4 addr show | awk '/ 10\\.119\\./{print $2; exit}'")


def phone_link(u, profile):
    """The user's link behaves like a phone's: netem on its upload (its eth0) and on its download
    (its gateway's side of the NAT, which carries only this user)."""
    jitter = " %dms distribution normal" % profile["jitter"] if profile.get("jitter") else ""
    if profile.get("burst"):
        loss = "loss gemodel %s%% %s%%" % profile["burst"]
    else:
        loss = "loss %s%% %s%%" % (profile["loss"], profile.get("loss_corr", 0))
    netem = "netem limit %d delay %dms%s %s" % (profile.get("limit", 1000), profile["delay"], jitter, loss)
    net(u, "tc qdisc replace dev eth0 root %s rate %dkbit" % (netem, profile["up"]))
    gw_net(u, "tc qdisc replace dev %s root %s rate %dkbit" % (gw_inner_dev(u), netem, profile["down"]))


def clear_link(u):
    net(u, "tc qdisc del dev eth0 root 2>/dev/null; true")
    gw_net(u, "tc qdisc del dev %s root 2>/dev/null; true" % gw_inner_dev(u))


def handover(u, outage=1.5):
    """A phone changes cell (or Wi-Fi to cellular): its link is gone for `outage` s and comes back on
    a new address, so its NAT maps it anew."""
    gw = net(u, "ip route show default | awk '{print $3; exit}'")
    net(u, "ip link set eth0 down")
    time.sleep(outage)
    net(u, "ip link set eth0 up && ip route replace default via %s" % gw)
    change_address(u)


class Road:
    """Phones on the road (--nat), as a context: `users`' links behave like `profile` (one PHONE_* for all, or
    {user: PHONE_* or None} — None leaves that one's link clean), and with `handover_s=(lo, hi)` each one hands
    over every lo..hi s on its own random schedule. `park()` stops the handovers (the links stay as they are);
    leaving clears the links. The schedule's seed is printed — PP_APP_LAB_ROAD_SEED replays it."""

    def __init__(self, users, profile, handover_s=None):
        self.users, self.profile, self.handover_s = list(users), profile, handover_s
        self.seed = int(os.environ.get("PP_APP_LAB_ROAD_SEED") or random.randrange(1 << 30))
        self.handovers = {u.name: 0 for u in self.users}
        self._stop = threading.Event()
        self._threads = []
        self._errors = []

    def __enter__(self):
        print("road seed=%d (%s)" % (self.seed, ", ".join(u.name for u in self.users)), flush=True)
        for u in self.users:
            clear_link(u)
            profile = self.profile.get(u) if isinstance(self.profile, dict) else self.profile
            if profile:
                phone_link(u, profile)
        if self.handover_s:
            for i, u in enumerate(self.users):
                t = threading.Thread(target=self._drive, args=(u, random.Random(self.seed + i)), daemon=True)
                t.start()
                self._threads.append(t)
        return self

    def _drive(self, u, rng):
        lo, hi = self.handover_s
        while not self._stop.wait(rng.uniform(lo, hi)):
            try:
                handover(u)
                self.handovers[u.name] += 1
            except ProbeError as e:
                self._errors.append(str(e))
                return

    def park(self):
        self._stop.set()
        for t in self._threads:
            t.join()
        if self._errors:
            raise ProbeError("road: " + "; ".join(self._errors))

    def __exit__(self, *exc):
        try:
            self.park()
        finally:
            for u in self.users:
                clear_link(u)
        return False


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


# --- calling again after a quiet spell (#577) -----------------------------------------------------
# A VPN (Clash TUN) or a carrier NAT forgets a UDP flow after a quiet minute or so. PP_APP_LAB_NAT_UDP_TIMEOUT
# sets the gateways' conntrack timeouts (s) for the scenario; PP_APP_LAB_IDLE_S how long the pair stays quiet.

CONNTRACK_UDP = ("nf_conntrack_udp_timeout", "nf_conntrack_udp_timeout_stream")


def gateway_of(container):
    return container.replace("-user-", "-gw-")


def set_nat_udp_timeout(containers, seconds):
    """Every gateway's UDP conntrack timeouts (a flow with no packet for that long is forgotten; unreplied,
    replied); returns what they were, per gateway. A gateway's /proc/sys is read-only, so a privileged
    container in its network namespace writes them."""
    before = {}
    for gw in sorted({gateway_of(c) for c in containers}):
        values = seconds if isinstance(seconds, list) else [seconds, seconds]
        script = "cd /proc/sys/net/netfilter && cat %s && " % " ".join(CONNTRACK_UDP) + " && ".join(
            "echo %d > %s" % (v, k) for k, v in zip(CONNTRACK_UDP, values))
        out = subprocess.run(["docker", "run", "--rm", "--privileged", "--network", "container:" + gw,
                              "--entrypoint", "sh", "pp-hard-gw:local", "-c", script], check=True,
                             capture_output=True, text=True).stdout.split()
        before[gw] = [int(v) for v in out]
    return before


def log_mark(u):
    path = os.path.join(u.dir, "probe-%d.log" % u.runs)
    return path, os.path.getsize(path) if os.path.exists(path) else 0


def log_since(mark, pattern):
    path, offset = mark
    with open(path, "rb") as f:
        f.seek(offset)
        text = f.read().decode("utf-8", "replace")
    return [line for line in text.splitlines() if re.search(pattern, line)]


DROP = re.compile(r"\] link dropped key=(\S+) .*?path=(\S+) .*?reason=(\S+) was_connected=1(?: last_rx_age_ms=(\d+))?"
                  r"(?: keepalive_ms=(\d+))?")


def drops_since(mark):
    return ["%s %s %s rx_age=%s ka=%s" % (m.group(1)[:10], m.group(2), m.group(3), m.group(4), m.group(5) or 0)
            for m in (DROP.search(line) for line in log_since(mark, r"\] link dropped .*was_connected=1")) if m]


def reach_since(mark):
    return [line.split("call.reach ", 1)[1] for line in log_since(mark, r"call\.reach ")]


def scenario_call_again_after_quiet(lab):
    """#577: Alice and Bob talk, hang up, say nothing for a few minutes, and Alice calls again. Each sits
    behind a NAT that forgets quiet UDP flows (a VPN, a carrier). Says which links the quiet spell cost
    and what the second call's reach paid."""
    idle_s = float(os.environ.get("PP_APP_LAB_IDLE_S", "180"))
    nat_timeout = int(os.environ.get("PP_APP_LAB_NAT_UDP_TIMEOUT", "60"))
    reachable = os.environ.get("PP_APP_LAB_REACHABLE", "1") == "1"
    # PP_APP_LAB_SEEDS: "both" (each knows both seeds), "split" (Alice knows the first, Bob the second: her call
    # goes through a relay she is not parked on, as the phone's in #577), "one" (the first only).
    seeds = os.environ.get("PP_APP_LAB_SEEDS", "both")
    a_seeds, b_seeds = {"both": ([lab.hop, lab.hop2], [lab.hop, lab.hop2]), "split": ([lab.hop], [lab.hop2]),
                        "one": (None, None)}[seeds]
    before = set_nat_udp_timeout(lab.containers, nat_timeout) if lab.containers else {}
    try:
        a = lab.user("alice", reachable=reachable, seeds=a_seeds)
        b = lab.user("bob", reachable=reachable, seeds=b_seeds)
        a.wait_mesh()
        b.wait_mesh()
        set_call_invite_policy(b, "everyone")
        a_thread = a.cmd("open_chat", account=b.account)["thread"]
        b.cmd("open_chat", account=a.account)
        marks = [log_mark(a), log_mark(b)]
        started = time.time()
        _, first = connect(a, a_thread, b, timeout=60)
        first["connect_s"] = round(time.time() - started, 2)
        first["reach"] = reach_since(marks[0])
        a.cmd("leave")
        assert_idle(a, b)
        quiet = [log_mark(a), log_mark(b)]
        time.sleep(idle_s)
        out = {"nat_udp_timeout_s": nat_timeout, "idle_s": idle_s, "seeds": seeds, "reachable": reachable,
               "first": first,
               "quiet_drops": {"alice": drops_since(quiet[0]), "bob": drops_since(quiet[1])}}
        again = [log_mark(a), log_mark(b)]
        started = time.time()
        _, second = connect(a, a_thread, b, timeout=60)
        second["connect_s"] = round(time.time() - started, 2)
        second["reach"] = reach_since(again[0])
        out["second"] = second
        a.cmd("leave")
        assert_idle(a, b)
        print("  %s" % json.dumps(out))
        return out
    finally:
        for gw, values in before.items():
            set_nat_udp_timeout([gw.replace("-gw-", "-user-")], values)


# --- a full seed (p2p-mesh N033)------------------------------------------------------------------
# The lab's first hop is sized small (docker-compose.app-lab.yml: PP_APP_LAB_HOP_RESERVATIONS,
# PP_APP_LAB_HOP_GROUP_CALLS), so pp-node-load fills it with strangers' parks or calls. The users know
# both seeds, as every phone knows every org seed; the nodes' own metrics say where each one went.

METRIC_LINE = re.compile(r'^([a-z_]+)(\{[^}]*\})?\s+([-0-9.eE+]+)$')
LEVELS = {1: "open", 2: "busy", 3: "full", 4: "draining"}


def node_metric(status_url, name):
    """One series from a node's /metrics (`name` with its labels, e.g. pp_service_load{service="park"})."""
    import urllib.request
    with urllib.request.urlopen(status_url + "/metrics", timeout=5) as r:
        for line in r.read().decode().splitlines():
            m = METRIC_LINE.match(line)
            if m and m.group(1) + (m.group(2) or "") == name:
                return float(m.group(3))
    return 0.0


def node_level(status_url, service):
    return LEVELS.get(int(node_metric(status_url, 'pp_service_load{service="%s"}' % service)), "unknown")


class Filler:
    """pp-node-load fills the lab's first hop (park: reservations; calls: hosted group calls) until the node
    itself says `service` is full; stopping it frees what it took."""

    def __init__(self, lab, mode, service, count):
        self.lab, self.mode, self.service, self.count = lab, mode, service, count
        self.proc = None

    def __enter__(self):
        if not self.lab.hop2:
            raise ProbeError("this lab has no second seed (hop2)")
        tool = os.path.join(os.path.dirname(self.lab.probe), "pp-node-load")
        cmd = [tool, "--hop", self.lab.hop, "--mode", self.mode, "--duration", "3600", "--ramp", "20", "--tag",
               "fill-" + uuid.uuid4().hex[:6]]
        if self.mode == "park":
            cmd += ["--clients", str(self.count), "--renew", "10"]
        else:  # two-member audio-only calls: they hold a session each and carry little
            cmd += ["--calls", str(self.count), "--members", "2", "--video-kbps", "0", "--audio-kbps", "8"]
        self.log = open(os.path.join(self.lab.root, "filler-%s.log" % self.mode), "w")
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        wait_for(lambda: node_level(self.lab.hop_status, self.service) == "full", 60,
                 "the first hop's %s to fill (%s)" % (self.service, self.log.name))
        time.sleep(2)  # /metrics gauges follow once a second: read what it holds once they caught up
        return self

    def __exit__(self, *exc):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()
        # Leave the hop as found: the filler's parks and calls end only when their links time out, and the next
        # scenario must not meet a full hop (group_call_three did, after these ran).
        wait_for(lambda: node_level(self.lab.hop_status, self.service) == "open", 180,
                 "the first hop's %s to be open again after the filler" % self.service)
        return False


def scenario_full_seed_parks_elsewhere(lab):
    """The first seed is full of other people's parked phones: Alice and Bob, who stay reachable and know both
    seeds, park on the second at once, and Bob's call to Alice rings and connects. When the first seed has room again they park
    there too (two relays each), without anyone restarting."""
    baseline = node_metric(lab.hop2_status, "pp_circuit_relay_reservations")
    with Filler(lab, "park", "park", int(os.environ.get("PP_APP_LAB_HOP_RESERVATIONS", "24"))):
        refused = 'pp_circuit_relay_requests_total{op="reserve",result="refused"}'
        refused_before = node_metric(lab.hop_status, refused)
        started = time.time()
        # Both stay reachable (Me → Network), so both park: what a phone does.
        a = lab.user("alice", reachable=True, seeds=[lab.hop, lab.hop2])
        b = lab.user("bob", reachable=True, seeds=[lab.hop, lab.hop2])
        a.wait_mesh()
        b.wait_mesh()
        wait_for(lambda: node_metric(lab.hop2_status, "pp_circuit_relay_reservations") >= baseline + 2, 60,
                 "both parked on the second seed")
        parked_s = round(time.time() - started, 2)
        # The filler's own renewals may dip under full and let one of its clients back in: the first seed's
        # refusals say it turned these two away.
        if node_metric(lab.hop_status, refused) < refused_before + 2:
            raise ProbeError("the full seed did not refuse them")
        a.cmd("open_chat", account=b.account)
        b_thread = b.cmd("open_chat", account=a.account)["thread"]
        _, call = connect(b, b_thread, a, timeout=60)
        b.cmd("leave")
        assert_idle(a, b)
    # The filler's parks end with their links; then the first seed holds exactly these two.
    freed = time.time()
    wait_for(lambda: node_metric(lab.hop_status, "pp_circuit_relay_reservations") == 2 or None, 300,
             "both parked on the first seed again once it had room (and the filler's parks gone)")
    return {"parked_elsewhere_s": parked_s, "call": call, "back_on_first_s": round(time.time() - freed, 2)}


def scenario_full_hop_known_before_the_call(lab):
    """Phones stay reachable, so they are parked on both seeds before anyone calls: the park replies say the
    first seed's group calls are full (p2p-mesh N033 "loads"), and Alice's group call is planned on the second
    from the start — nobody even asks the full one."""
    with Filler(lab, "calls", "media_relay", int(os.environ.get("PP_APP_LAB_HOP_GROUP_CALLS", "8"))):
        (a, b, c), threads = open_group(lab, seeds=[lab.hop, lab.hop2], reachable=True)
        # Parked on the second seed too: the users have heard both seeds' levels.
        wait_for(lambda: node_metric(lab.hop2_status, "pp_circuit_relay_reservations") >= 3, 60, "all parked")
        asked = 'pp_media_relay_quotes_total{result="refused_busy"}'
        asked_before = node_metric(lab.hop_status, asked)
        started = time.time()
        group_call(a, threads[a.name], [b, c])
        for u in (a, b, c):
            assert_hears(u, 2, timeout=60)
        took = round(time.time() - started, 2)
        if node_metric(lab.hop2_status, "pp_media_relay_participants") < 3:
            raise ProbeError("the call is not on the second seed")
        refused = node_metric(lab.hop_status, asked) - asked_before
        leave_all([a, b, c])
    if refused:
        raise ProbeError("the full seed was asked for the call %d times: the plan did not know it was full" % refused)
    return {"all_hear_all_s": took}


def scenario_full_hop_hosts_the_call_elsewhere(lab):
    """The first seed hosts as many group calls as it takes: Alice's group call, whose members know both seeds,
    is hosted on the second, and everyone hears everyone."""
    with Filler(lab, "calls", "media_relay", int(os.environ.get("PP_APP_LAB_HOP_GROUP_CALLS", "8"))):
        filled = node_metric(lab.hop_status, "pp_media_relay_participants")
        (a, b, c), threads = open_group(lab, seeds=[lab.hop, lab.hop2])
        started = time.time()
        group_call(a, threads[a.name], [b, c])
        for u in (a, b, c):
            assert_hears(u, 2, timeout=60)
        took = round(time.time() - started, 2)
        if node_metric(lab.hop2_status, "pp_media_relay_participants") < 3:
            raise ProbeError("the call is not on the second seed")
        if node_metric(lab.hop_status, "pp_media_relay_participants") > filled:
            raise ProbeError("someone of the call joined the full hop")
        leave_all([a, b, c])
    return {"all_hear_all_s": took}


# --- group calls ----------------------------------------------------------------------------------
# docs/architecture/CALLS.md (V050): the first answer is a 1:1 call; the third person moves the call
# onto the planned hop (media_relay). Hostless: whoever placed it may leave and the rest go on.


def add_contacts(users, links):
    """Who has added whom: "bob>dave" — Bob added Dave, Dave never added Bob; "bob=carol" — both
    added each other. Adding is the app's Add contact by a directory search (`open_chat`)."""
    by_name = {u.name.rsplit("-", 1)[0]: u for u in users}  # the lab suffixes each name
    for link in links:
        sep = "=" if "=" in link else ">"
        adder, _, added = link.partition(sep)
        if adder not in by_name or added not in by_name:
            raise ValueError("contact link %r: want 'a>b' or 'a=b' between users of the scenario" % link)
        for x, y in ([(adder, added), (added, adder)] if sep == "=" else [(adder, added)]):
            by_name[x].cmd("open_chat", account=by_name[y].account)


def open_group(lab, n=3, title="lab group", contacts=(), seeds=None, reachable=False):
    """n people; the first creates a group with the others, who accept its invites. The owner and
    each member have added each other (an invite comes from a contact); members have added nobody
    else unless `contacts` says so (see `add_contacts`) — linked before the group, as friends are."""
    names = ["alice", "bob", "carol", "dave", "erin"][:n]
    users = [lab.user(name, seeds=seeds, reachable=reachable) for name in names]
    for u in users:
        u.wait_mesh()
    owner, members = users[0], users[1:]
    for m in members:
        owner.cmd("open_chat", account=m.account)
        m.cmd("open_chat", account=owner.account)
    add_contacts(users, contacts)
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


def scenario_group_owner_hands_over(lab):
    """Alice owns a group with Bob and Carol and closes it: she must hand it over (to a reachable member)
    or drop it; members just leave. She hands it to Bob: Bob owns it, Alice is off the roster and her
    chat is gone; Carol (a member) would still just leave."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    plan = a.cmd("close_plan", thread=threads[a.name])
    if plan["kind"] != "leave_as_owner" or sorted(plan["successors"]) != sorted([b.account, c.account]):
        raise ProbeError("alice (owner): close plan %s" % plan)
    if b.cmd("close_plan", thread=threads[b.name])["kind"] != "leave":
        raise ProbeError("bob (member): close plan is not leave")
    # The chat header's sheet offers the owner's items (rename for everyone) to Alice only.
    if not a.cmd("group_sheet", group_id=plan_group(a, threads[a.name]))["is_owner"]:
        raise ProbeError("alice: her group's sheet does not see her as the owner")
    if b.cmd("group_sheet", group_id=plan_group(b, threads[b.name]))["is_owner"]:
        raise ProbeError("bob: his group's sheet sees him as the owner before the hand-over")
    a.cmd("close_chat", thread=threads[a.name], successor=b.account)
    for u in (b, c):
        wait_for(lambda: (lambda r: r.get("owner") == b.account and a.account not in r["members"])(
            u.cmd("group_roster", group_id=plan_group(u, threads[u.name]))), 40, "%s: bob owns, alice left" % u.name)
    if any(t["id"] == threads[a.name] for t in a.cmd("threads")["threads"]):
        raise ProbeError("alice: the group chat is still there after handing it over")
    if b.cmd("close_plan", thread=threads[b.name])["kind"] != "leave_as_owner":
        raise ProbeError("bob: not the owner after the hand-over")
    if not b.cmd("group_sheet", group_id=plan_group(b, threads[b.name]))["is_owner"]:
        raise ProbeError("bob: his group's sheet does not see him as the owner after the hand-over")
    if c.cmd("close_plan", thread=threads[c.name])["kind"] != "leave":
        raise ProbeError("carol: no longer a plain member")


def plan_group(u, thread):
    """The group id of `u`'s group chat `thread`."""
    return next(t for t in u.cmd("threads")["threads"] if t["id"] == thread)["group_id"]


def wait_for(fn, timeout, what):
    deadline = time.time() + timeout
    while True:
        got = fn()
        if got:
            return got
        if time.time() > deadline:
            raise ProbeError("timed out waiting for %s" % what)
        time.sleep(0.5)


def chat_row(u, thread, reply=None):
    """The chats-list row of `thread` as `u` sees it (ChatPresenter), and the list's own fields."""
    got = reply or u.cmd("threads")
    for row in got["threads"]:
        if row["id"] == thread:
            return row, got
    raise ProbeError("%s: no thread %s" % (u.name, thread))


def scenario_chat_list_settings(lab):
    """Bob's chats list as he sees it: Alice's message shows unread; he reads it, marks it unread by
    hand (a dot) and opening it clears that; he pins, mutes and archives the chat, opens the archived
    list and brings it back."""
    a, a_thread, b, b_thread = open_pair(lab)
    b.cmd("home")  # the chat is not open: its new messages count
    a.cmd("send", thread=a_thread, text="hello from the list")
    wait_for(lambda: chat_row(b, b_thread)[0].get("unread", 0) > 0, 20, "bob: alice's message unread")
    row, _ = chat_row(b, b_thread, b.cmd("mark_read", thread=b_thread))
    if row.get("unread") or row.get("unread_dot"):
        raise ProbeError("bob: still unread after Mark as read: %s" % row)
    row, _ = chat_row(b, b_thread, b.cmd("mark_unread", thread=b_thread))
    if not row.get("unread_dot") or row.get("unread"):
        raise ProbeError("bob: Mark as unread shows no dot (or a count): %s" % row)
    b.cmd("show_chat", thread=b_thread)
    row, _ = chat_row(b, b_thread)
    if row.get("unread_dot") or not row.get("active"):
        raise ProbeError("bob: opening the chat kept the dot / did not open it: %s" % row)
    row, _ = chat_row(b, b_thread, b.cmd("pin", thread=b_thread))
    if not row.get("pinned") or row.get("position") != 0:
        raise ProbeError("bob: a pinned chat is not first: %s" % row)
    row, _ = chat_row(b, b_thread, b.cmd("mute", thread=b_thread))
    if not row.get("muted"):
        raise ProbeError("bob: mute did not take: %s" % row)
    row, got = chat_row(b, b_thread, b.cmd("archive", thread=b_thread))
    if row.get("listed") or got.get("archived_count") != 1:
        raise ProbeError("bob: an archived chat is still in the list: %s / %s" % (row, got.get("archived_count")))
    row, got = chat_row(b, b_thread, b.cmd("show_archived"))
    if not row.get("listed") or not got.get("showing_archived"):
        raise ProbeError("bob: the archived list does not show the chat: %s" % row)
    row, got = chat_row(b, b_thread, b.cmd("unarchive", thread=b_thread))
    if not row.get("listed") or got.get("archived_count") != 0 or got.get("showing_archived"):
        raise ProbeError("bob: unarchived, the list did not come back: %s / %s" % (row, got))
    b.cmd("unpin", thread=b_thread)
    b.cmd("unmute", thread=b_thread)


def transcript_row(u, thread, message):
    for row in u.cmd("transcript", thread=thread)["rows"]:
        if row["id"] == message:
            return row
    return None


def reactions_of(u, thread, message):
    row = transcript_row(u, thread, message)
    return {r["emoji"]: r["count"] for r in (row or {}).get("reactions", [])}


def scenario_chat_message_actions(lab):
    """A message's buttons, seen from the other side: Bob reacts to Alice's message and takes it back;
    Alice deletes one message for everyone (Bob's copy goes) and another for herself (Bob keeps it)."""
    a, a_thread, b, b_thread = open_pair(lab)
    a.cmd("show_chat", thread=a_thread)
    b.cmd("show_chat", thread=b_thread)
    first = a.cmd("send", thread=a_thread, text="react to me")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, first), 20, "bob: alice's message")
    b.cmd("react", message=first, emoji="👍")
    wait_for(lambda: reactions_of(a, a_thread, first).get("👍") == 1, 20, "alice: bob's reaction")
    b.cmd("react", message=first, emoji="👍")  # again: takes it back
    wait_for(lambda: "👍" not in reactions_of(a, a_thread, first), 20, "alice: bob's reaction gone")
    a.cmd("delete_for_everyone", message=first)
    wait_for(lambda: (transcript_row(b, b_thread, first) or {"type": "deleted"})["type"] == "deleted", 20,
             "bob: the message deleted for everyone")
    second = a.cmd("send", thread=a_thread, text="only mine goes")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, second), 20, "bob: alice's second message")
    a.cmd("delete_for_me", message=second)
    if transcript_row(a, a_thread, second) and transcript_row(a, a_thread, second)["type"] != "deleted":
        raise ProbeError("alice: a message deleted for her still shows")
    time.sleep(2)
    if not transcript_row(b, b_thread, second) or transcript_row(b, b_thread, second)["type"] == "deleted":
        raise ProbeError("bob: alice's delete-for-me took his copy")


def thread_with(u, account):
    """`u`'s direct thread with `account`, or None."""
    for t in u.cmd("threads")["threads"]:
        if t["kind"] == "direct" and t["peer"] == account:
            return t["id"]
    return None


def scenario_first_message_brings_keys(lab):
    """Bob's directory never answers. Alice finds him through hers and writes: her first message carries
    her key card, so Bob checks it and answers with no lookup, and adds her to his contacts from the chat
    (what he holds for her — no lookup either; his phone rings for contacts only, privacy P007); then she
    calls him and he answers."""
    a = lab.user("alice")
    b = lab.user("bob", directory=False)
    a.wait_mesh()
    b.wait_mesh()
    a_thread = a.cmd("open_chat", account=b.account)["thread"]
    first = a.cmd("send", thread=a_thread, text="hi bob, it's alice")["message_id"]
    b_thread = wait_for(lambda: thread_with(b, a.account), 30, "bob: alice's chat")
    wait_for(lambda: transcript_row(b, b_thread, first), 20, "bob: alice's first message")
    reply = b.cmd("send", thread=b_thread, text="hi alice")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, reply), 20, "alice: bob's answer")
    b.cmd("add_to_contacts", thread=b_thread)
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    for u in (a, b):
        u.cmd("leave", check=False)
    assert_idle(a, b)


def scenario_friend_shares_a_contact(lab):
    """Alice has no directory. Carol, who knows Bob, shares his contact card in her chat with Alice;
    Alice adds Bob from it (his keys checked against his account, his route from Carol's card) and they
    talk, with no lookup on Alice's side."""
    c = lab.user("carol")
    b = lab.user("bob")
    a = lab.user("alice", directory=False)
    for u in (a, b, c):
        u.wait_mesh()
    c.cmd("open_chat", account=b.account)  # Carol knows Bob (her contacts)
    c_thread = c.cmd("open_chat", account=a.account)["thread"]
    hello = c.cmd("send", thread=c_thread, text="meet bob")["message_id"]
    a_thread = wait_for(lambda: thread_with(a, c.account), 30, "alice: carol's chat")
    wait_for(lambda: transcript_row(a, a_thread, hello), 20, "alice: carol's message")
    c.cmd("share_contact", thread=c_thread, account=b.account)

    def card_row():
        for row in a.cmd("transcript", thread=a_thread)["rows"]:
            if row["type"] == "contact_card":
                return row
        return None
    card = wait_for(card_row, 20, "alice: bob's contact card")
    a.cmd("show_chat", thread=a_thread)
    a.cmd("add_from_card", message=card["id"])
    ab_thread = a.cmd("chat_with", account=b.account)["thread"]
    first = a.cmd("send", thread=ab_thread, text="hi bob, carol sent me your card")["message_id"]
    ba_thread = wait_for(lambda: thread_with(b, a.account), 30, "bob: alice's chat")
    wait_for(lambda: transcript_row(b, ba_thread, first), 20, "bob: alice's message")
    reply = b.cmd("send", thread=ba_thread, text="hi alice")["message_id"]
    wait_for(lambda: transcript_row(a, ab_thread, reply), 20, "alice: bob's answer")


def scenario_devices_hand_over_their_cards(lab):
    """A call names each side's device; each asks that device for its account card over Amp, and the card
    — not the call's word — ties the device to the account. With the cards kept, a message to the account
    goes over the live link to its device."""
    a, a_thread, b, b_thread = open_pair(lab)
    a_peer = a.cmd("whoami")["peer_id"]
    b_peer = b.cmd("whoami")["peer_id"]
    call_id = a.cmd("call", thread=a_thread)["call_id"]
    b.wait_surface("ringing", 40, call_id=call_id)
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 40, call_id=call_id)
    # The call screen reads In call while its media still connects; behind NATs the call's link (a circuit
    # carrier) comes up after that. Its media flowing both ways is the live link the message must take.
    assert_media_flows(a, b)
    wait_for(lambda: a_peer in b.cmd("kept_card", account=a.account)["devices"], 40,
             "bob: alice's device, from her card")
    wait_for(lambda: b_peer in a.cmd("kept_card", account=b.account)["devices"], 40,
             "alice: bob's device, from his card")
    sent = a.cmd("send", thread=a_thread, text="over the call's link")["message_id"]
    row = wait_for(lambda: transcript_row(b, b_thread, sent), 20, "bob: alice's message")
    if row.get("transport") != "direct":
        raise ProbeError("bob: alice's message came by %s, not over the link" % row.get("transport"))
    for u in (a, b):
        u.cmd("leave", check=False)
    assert_idle(a, b)


def scenario_strangers_meet_by_invite(lab):
    """Neither has a directory. Bob's computer stays reachable without the relay; he sends Alice his invite
    line. She pastes it: her device asks his for his card over the mesh, checks it against his account, and
    writes to him; he answers. No lookup anywhere."""
    a = lab.user("alice", directory=False)
    b = lab.user("bob", directory=False, reachable=True)
    for u in (a, b):
        u.wait_mesh()
    invite = "Add me on PP: " + b.cmd("locator")["locator"]
    added = a.cmd("add_from_locator", text=invite, timeout=60)
    if not added["keys_known"]:
        raise ProbeError("alice: bob's card not fetched from his device (%s)" % added)
    a_thread = a.cmd("chat_with", account=b.account)["thread"]
    first = a.cmd("send", thread=a_thread, text="hi bob, from your invite")["message_id"]
    b_thread = wait_for(lambda: thread_with(b, a.account), 30, "bob: alice's chat")
    wait_for(lambda: transcript_row(b, b_thread, first), 20, "bob: alice's message")
    reply = b.cmd("send", thread=b_thread, text="hi alice")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, reply), 30, "alice: bob's answer")


def scenario_group_members_key_each_other(lab):
    """Bob and Carol have no directory and have never met. Alice (who has one) writes to each, and each
    adds her from her invite line (group invites come from contacts only); she makes a group of the three:
    her roster notice carries each member's card, so Bob and Carol write to the group — encrypted to and
    checked against each other — with no lookup."""
    a = lab.user("alice", reachable=True)
    b = lab.user("bob", directory=False)
    c = lab.user("carol", directory=False)
    for u in (a, b, c):
        u.wait_mesh()
    for m in (b, c):
        a_thread = a.cmd("open_chat", account=m.account)["thread"]
        hello = a.cmd("send", thread=a_thread, text="hi %s" % m.name)["message_id"]
        m_thread = wait_for(lambda: thread_with(m, a.account), 30, "%s: alice's chat" % m.name)
        wait_for(lambda: transcript_row(m, m_thread, hello), 20, "%s: alice's message" % m.name)
        m.cmd("add_from_locator", text=a.cmd("locator")["locator"], timeout=60)
    created = a.cmd("create_group", title="no lookups", accounts=[b.account, c.account])
    group_id = created["group_id"]
    threads = {a.account: created["thread"]}
    for m in (b, c):
        invite = wait_for(lambda: next((i for i in m.cmd("group_invites")["invites"] if i["group_id"] == group_id),
                                       None), 40, "%s: the group invite" % m.name)
        threads[m.account] = m.cmd("accept_group_invite", nonce=invite["nonce"], group_id=group_id)["thread"]
    everyone = sorted(u.account for u in (a, b, c))
    for u in (a, b, c):
        wait_for(lambda: sorted(u.cmd("group_roster", group_id=group_id)["members"]) == everyone or None, 40,
                 "%s: the whole roster" % u.name)
    wait_for(lambda: b.cmd("kept_card", account=c.account).get("account_id") or None, 30,
             "bob: carol's card, from the roster")
    # Alice keeps her copies of the roster notices without the cards (they are kept as cards).
    for m in (b, c):
        for row in a.cmd("transcript", thread=thread_with(a, m.account))["rows"]:
            if row["type"] == "system" and row["payload_bytes"] > 8000:
                raise ProbeError("alice: %d bytes kept in her chat with %s (%s)" % (row["payload_bytes"], m.name,
                                                                                    row["text"]))
    wait_for(lambda: c.cmd("kept_card", account=b.account).get("account_id") or None, 30,
             "carol: bob's card, from the roster")
    def said(u, text):
        return next((r for r in u.cmd("transcript", thread=threads[u.account])["rows"] if r.get("text") == text), None)
    b.cmd("show_chat", thread=threads[b.account])
    b.cmd("compose_send", text="hi carol, from the group")
    wait_for(lambda: said(c, "hi carol, from the group"), 30, "carol: bob's group message")
    wait_for(lambda: said(a, "hi carol, from the group"), 30, "alice: bob's group message")
    c.cmd("show_chat", thread=threads[c.account])
    c.cmd("compose_send", text="hi bob")
    wait_for(lambda: said(b, "hi bob"), 30, "bob: carol's group message")


def scenario_group_after_member_removed(lab):
    """The owner removes a member, which moves the group's epoch: everyone left keeps reading everyone —
    each copy is keyed by the pair's own session, not by the group's epoch (G011). Before, the owner's
    second message and every other member's failed: "Missing key_init_b64"."""
    users, threads = open_group(lab, n=4)
    a, b, c, d = users

    def said(u, text):
        return any(r.get("text") == text for r in u.cmd("transcript", thread=threads[u.name])["rows"])

    def send(u, text):
        u.cmd("show_chat", thread=threads[u.name])
        u.cmd("compose_send", text=text)

    send(a, "before the removal")
    for u in (b, c, d):
        wait_for(lambda u=u: said(u, "before the removal") or None, 30, "%s: alice's message" % u.name)
    a.cmd("remove_member", thread=threads[a.name], account=d.account)
    for u in (b, c):
        wait_for(lambda u=u: d.account not in u.cmd("group_roster", group_id=lab_group_id(a, threads))["members"]
                 or None, 30, "%s: dave removed" % u.name)
    for sender, readers in ((a, (b, c)), (b, (a, c)), (c, (a, b))):
        for i in (1, 2):
            text = "%s %d after the removal" % (sender.name, i)
            send(sender, text)
            for r in readers:
                wait_for(lambda r=r, text=text: said(r, text) or None, 30, "%s: %r" % (r.name, text))
    if said(d, "alice 1 after the removal"):
        raise ProbeError("dave read the group after his removal")


def lab_group_id(owner, threads):
    """The group id of the owner's group thread."""
    for t in owner.cmd("threads")["threads"]:
        if t.get("id") == threads[owner.name] and t.get("group_id"):
            return t["group_id"]
    raise ProbeError("%s: no group id for its group thread" % owner.name)


def scenario_restored_device_is_heard(lab):
    """Bob loses his phone and restores his account from his recovery phrase (multi-device M023): a new device
    with no chats, no sessions and no numbering, while Alice still holds their old chat and session. Every
    message he sends after the restore reaches her — none taken for one she already has (#378) — and her
    answer reaches him."""
    a, a_thread, b, b_thread = open_pair(lab)
    for i in range(3):
        sent = b.cmd("send", thread=b_thread, text="before %d" % i)["message_id"]
        wait_for(lambda: transcript_row(a, a_thread, sent), 20, "alice: bob's message %d" % i)
    phrase = b.cmd("recovery_phrase", pin=PIN)["phrase"]
    b.cmd("locker_publish", timeout=60)
    b.restore_from_phrase(phrase)
    b.wait_mesh()
    b_thread = b.cmd("open_chat", account=a.account)["thread"]
    after = [b.cmd("send", thread=b_thread, text="after %d" % i)["message_id"] for i in range(3)]
    for i, sent in enumerate(after):
        wait_for(lambda: transcript_row(a, a_thread, sent), 30, "alice: bob's message %d after the restore" % i)
    reply = a.cmd("send", thread=a_thread, text="welcome back")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, reply), 30, "bob: alice's answer after the restore")


def scenario_restored_device_hears_first(lab):
    """Bob's phone is gone; he restores his account on a new one from his recovery phrase and has not written
    yet when Alice does. The restored phone is a new device Alice never heard of: it tells its contacts its
    card, so her message goes to it, opening a session that brings her card (the restore kept none) — not to
    the lost phone."""
    a, a_thread, b, b_thread = open_pair(lab)
    hello = a.cmd("send", thread=a_thread, text="before")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, hello), 20, "bob: alice's message before the restore")
    phrase = b.cmd("recovery_phrase", pin=PIN)["phrase"]
    b.cmd("locker_publish", timeout=60)
    b.restore_from_phrase(phrase)
    b.wait_mesh()
    wait_for(lambda: "device list sent to 1 of 1" in b.log_text(), 30, "bob: his new device told to alice")
    sent = a.cmd("send", thread=a_thread, text="are you there?")["message_id"]

    def heard():
        for t in b.cmd("threads")["threads"]:
            if transcript_row(b, t["id"], sent):
                return True
        return None
    wait_for(heard, 30, "bob: alice's message on his restored phone")


def scenario_recovery_phrase_needs_the_pin(lab):
    """Me → Security → Show recovery phrase (multi-device M023): the PIN must open the vault again — a wrong one
    shows nothing — and the 24 words are the same each time; a device linked to the account shows the same
    phrase (the seed travels with the account keys)."""
    a = lab.user("alice")
    wrong = a.cmd("recovery_phrase", pin=PIN + "0", check=False)
    if wrong.get("ok"):
        raise ProbeError("alice: a wrong PIN showed the recovery phrase")
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    if len(phrase.split()) != 24:
        raise ProbeError("alice: recovery phrase %r is not 24 words" % phrase)
    if a.cmd("recovery_phrase", pin=PIN)["phrase"] != phrase:
        raise ProbeError("alice: the recovery phrase changed")
    laptop = lab.linked_device(a, "alice-laptop")
    if laptop.cmd("recovery_phrase", pin=PIN)["phrase"] != phrase:
        raise ProbeError("alice's laptop: another recovery phrase than her phone's")


def scenario_account_locker_round_trip(lab):
    """Alice's account state sealed into her locker at the relay (multi-device M023 s3): anyone holding only her
    recovery phrase — here Bob's app, as a restoring device would — finds the locker, checks it is hers and opens
    her contacts, her group and her card floors; a phrase that never published finds nothing; a sync with nothing
    new writes nothing, and an edit stores a newer version."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    group_id = lab_group_id(a, threads)
    first = a.cmd("locker_publish", timeout=60)["version"]
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    found = b.cmd("locker_fetch", phrase=phrase, timeout=60)
    if not found.get("found"):
        raise ProbeError("bob: no locker for alice's phrase")
    if sorted(found["contacts"]) != sorted([b.account, c.account]):
        raise ProbeError("alice's locker: contacts %s (want bob and carol)" % found["contacts"])
    if group_id not in found["groups"]:
        raise ProbeError("alice's locker: groups %s lack %s" % (found["groups"], group_id))
    if found["card_floors"] < 2:
        raise ProbeError("alice's locker: %d card floors (want her contacts')" % found["card_floors"])
    # The BIP-0039 test phrase (seed 7f…): a valid phrase no account in the lab uses.
    unused = ("legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful "
              "legal winner thank year wave sausage worth title")
    if b.cmd("locker_fetch", phrase=unused, timeout=60).get("found"):
        raise ProbeError("bob: a locker for a phrase that never published")
    # Bob's own app publishes his locker by itself (registered, with a seed), and finding Alice's did not stop it.
    wait_for(lambda: b.cmd("locker_fetch", phrase=b.cmd("recovery_phrase", pin=PIN)["phrase"],
                           timeout=60).get("found") or None, 30, "bob: his own locker, published by itself")
    unchanged = a.cmd("locker_publish", timeout=60)["version"]
    if unchanged != first:
        raise ProbeError("alice: nothing changed, yet locker version %d after %d" % (unchanged, first))
    a.cmd("rename_contact", account=b.account, remark="Bob from the hike")
    second = a.cmd("locker_publish", timeout=60)["version"]
    if second <= first:
        raise ProbeError("alice: locker version %d after %d" % (second, first))


def scenario_restore_from_phrase(lab):
    """Alice loses every device and reinstalls from her recovery phrase alone (multi-device M023): the same account
    on a new device; her group comes back from her locker, checks its members and is active; Bob's next group
    message reaches her new device and hers reaches him; the restored app republishes the locker by itself."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    group_id = lab_group_id(a, threads)
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    a.cmd("locker_publish", timeout=60)
    a.restore_from_phrase(phrase)
    a.wait_mesh()
    roster = checked_roster(a, group_id, "active")
    if sorted(roster["members"]) != sorted(u.account for u in (a, b, c)):
        raise ProbeError("alice: restored roster %s" % roster["members"])
    a_thread = group_thread_of(a, group_id)
    if not a_thread:
        raise ProbeError("alice: no group chat after the restore")
    b.cmd("show_chat", thread=threads[b.name])
    b.cmd("compose_send", text="welcome back")
    wait_for(lambda: said_in(a, a_thread, "welcome back"), 40, "alice: bob's message on her restored device")
    a.cmd("show_chat", thread=a_thread)
    a.cmd("compose_send", text="back from my phrase")
    wait_for(lambda: said_in(b, threads[b.name], "back from my phrase"), 40, "bob: alice's message")
    wait_for(lambda: a.log_text().count("Account locker published") >= 2, 30, "alice: the locker republished")


def scenario_restore_from_a_saved_copy(lab):
    """Alice keeps a saved copy of her account (an exported locker file, M023 §6 amendment): she names Bob, saves
    a copy, and her phone dies before her locker caught up. Restored from her phrase and the file, the newer copy —
    the file — wins: Bob keeps his name; and they still talk."""
    a, a_thread, b, b_thread = open_pair(lab)
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    a.cmd("locker_publish", timeout=60)
    a.cmd("rename_contact", account=b.account, remark="Bob (in the saved copy)")
    copy_path = os.path.join(a.dir, "exported-copy.json")
    saved = a.cmd("locker_export", path=copy_path)
    a.crash()  # before the locker's own sync (5 s after the rename)
    with open(copy_path) as f:
        copy_text = f.read()
    a.restore_from_phrase(phrase, saved_copy=copy_text)
    if "locker version %d," % saved["version"] not in a.log_text():
        raise ProbeError("alice: the restore did not use the saved copy (version %d)" % saved["version"])
    row = next((c for c in a.cmd("contacts")["contacts"] if c["account"] == b.account), None)
    if row is None or row["remark"] != "Bob (in the saved copy)":
        raise ProbeError("alice: Bob after the restore %s" % row)
    a.wait_mesh()
    a_thread = a.cmd("chat_with", account=b.account)["thread"]
    a.cmd("send", thread=a_thread, text="back from my saved copy")
    wait_for(lambda: said_in(b, b_thread, "back from my saved copy"), 40, "bob: alice's message")


def scenario_linked_devices_share_contact_edits(lab):
    """Alice's phone and her newly linked laptop share one address book through her locker (multi-device M023
    §7): the laptop's first sync brings the phone's contacts (the link carries none); a remark set on the laptop
    reaches the phone by itself (the laptop's write nudges it); Bob removed on the laptop is removed on the phone too
    (the removal carries, not undone by the phone's older copy)."""
    a, a_thread, b, b_thread = open_pair(lab)
    a.cmd("locker_publish", timeout=60)
    laptop = lab.linked_device(a, "alice-laptop")

    def remark_on(u, account):
        row = next((c for c in u.cmd("contacts")["contacts"] if c["account"] == account), None)
        return None if row is None else row["remark"]

    laptop.cmd("locker_publish", timeout=60)
    if remark_on(laptop, b.account) is None:
        raise ProbeError("alice's laptop: no Bob after its first sync (%s)" % laptop.cmd("contacts")["contacts"])
    laptop.cmd("rename_contact", account=b.account, remark="Bob (named on the laptop)")
    laptop.cmd("locker_publish", timeout=60)
    # No sync asked on the phone: the laptop's write nudges it (a notice through Alice's own mailbox).
    wait_for(lambda: remark_on(a, b.account) == "Bob (named on the laptop)", 40,
             "alice's phone: the laptop's remark for Bob, by itself")
    laptop.cmd("remove_contact", account=b.account)
    laptop.cmd("locker_publish", timeout=60)
    a.cmd("locker_publish", timeout=60)
    if remark_on(a, b.account) is not None:
        raise ProbeError("alice's phone: Bob still a contact after the laptop removed him")
    a.cmd("locker_publish", timeout=60)
    laptop.cmd("locker_publish", timeout=60)
    if remark_on(laptop, b.account) is not None:
        raise ProbeError("alice's laptop: Bob came back from the phone's copy")


def scenario_linked_devices_both_send(lab):
    """Alice's phone and her newly linked laptop are two devices of one account (M021). The laptop's first
    message brings Bob its device list; from then on Bob's answers reach both devices, and both keep writing
    — each in its own device pair, so neither's message is taken for the other's."""
    a, a_thread, b, b_thread = open_pair(lab)
    first = a.cmd("send", thread=a_thread, text="hi from the phone")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, first), 30, "bob: the phone's first message")
    reply = b.cmd("send", thread=b_thread, text="hi alice")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, reply), 30, "phone: bob's answer")
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    l_thread = laptop.cmd("open_chat", account=b.account)["thread"]
    from_laptop = laptop.cmd("send", thread=l_thread, text="hi from the laptop")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, from_laptop), 30, "bob: the laptop's first message")
    both = b.cmd("send", thread=b_thread, text="hello both")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, both), 30, "phone: bob's answer to both")
    wait_for(lambda: transcript_row(laptop, l_thread, both), 30, "laptop: bob's answer to both")
    sent = []
    for i in range(3):
        sent.append(("phone %d" % i, a.cmd("send", thread=a_thread, text="phone %d" % i)["message_id"]))
        sent.append(("laptop %d" % i, laptop.cmd("send", thread=l_thread, text="laptop %d" % i)["message_id"]))
    for what, m in sent:
        wait_for(lambda: transcript_row(b, b_thread, m), 30, "bob: %s" % what)


def assert_signed_out(u, thread):
    """`u` is signed out of its account (M021 d4): its list says so, and it can no longer send."""
    wait_for(lambda: u.cmd("devices").get("signed_out") or None, 40, "%s: signed out" % u.name)
    sent = u.cmd("send", thread=thread, text="from a signed-out device", check=False)
    if sent.get("ok") or "signed out" not in str(sent.get("error")):
        raise ProbeError("%s: a signed-out device sent (%s)" % (u.name, sent))


def scenario_main_device_signs_a_device_out(lab):
    """Alice's phone is her main device (M021 d4); her laptop, linked to it, joins at the end of the list and
    cannot give out a transfer code. The phone hands the main role to the laptop, which then takes the phone off
    the account: the phone is signed out and can no longer send; Bob learns the shorter list, and he and the
    laptop keep talking."""
    a, a_thread, b, b_thread = open_pair(lab)
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    phone_id = a.cmd("whoami")["peer_id"]
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: a.cmd("devices")["devices"] == [phone_id, laptop_id] or None, 40,
             "phone: the laptop at the end of the list")
    if laptop.cmd("devices")["main"] != phone_id:
        raise ProbeError("laptop: main device %s, not the phone" % laptop.cmd("devices"))
    if laptop.cmd("link_export", check=False).get("ok"):
        raise ProbeError("laptop: gave out a transfer code though it is not the main device")
    a.cmd("make_main", peer_id=laptop_id)
    wait_for(lambda: laptop.cmd("devices")["main"] == laptop_id or None, 40, "laptop: the main device")
    laptop.cmd("remove_device", peer_id=phone_id)
    assert_signed_out(a, a_thread)
    wait_for(lambda: phone_id not in b.cmd("kept_card", account=a.account)["devices"] or None, 40,
             "bob: Alice's list without the phone")
    l_thread = laptop.cmd("open_chat", account=b.account)["thread"]
    m = laptop.cmd("send", thread=l_thread, text="the laptop is the main device now")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, m), 30, "bob: the laptop's message")
    r = b.cmd("send", thread=b_thread, text="noted")["message_id"]
    wait_for(lambda: transcript_row(laptop, l_thread, r), 30, "laptop: bob's answer")


def scenario_restore_signs_out_the_other_devices(lab):
    """Alice's laptop is linked to her phone; she restores her account from its recovery phrase on the phone (a new
    device, M023): it starts a fresh list holding only itself, as the main device (M021 d4); the laptop hears of it
    through the shared mailbox and is signed out. Bob and the restored phone talk."""
    a, a_thread, b, b_thread = open_pair(lab)
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: laptop_id in a.cmd("devices")["devices"] or None, 40, "phone: the laptop on the list")
    l_thread = laptop.cmd("open_chat", account=b.account)["thread"]
    a.cmd("locker_publish", timeout=60)
    a.restore_from_phrase(phrase)
    a.wait_mesh()
    restored_id = a.cmd("whoami")["peer_id"]
    wait_for(lambda: a.cmd("devices")["devices"] == [restored_id] or None, 40, "restored phone: a fresh list")
    assert_signed_out(laptop, l_thread)
    a_thread = a.cmd("chat_with", account=b.account)["thread"]
    m = a.cmd("send", thread=a_thread, text="back, and the only device")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, m), 40, "bob: the restored phone's message")
    r = b.cmd("send", thread=b_thread, text="welcome back")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, r), 40, "restored phone: bob's answer")


def scenario_linked_device_introduces_itself(lab):
    """A newly linked device tells its contacts it exists (M021 d3): Alice links a laptop; before the laptop writes
    to anyone, Bob holds Alice's list with it, and his next message reaches the laptop as well as the phone."""
    a, a_thread, b, b_thread = open_pair(lab)
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: laptop_id in b.cmd("kept_card", account=a.account)["devices"] or None, 40,
             "bob: Alice's list with the laptop")
    m = b.cmd("send", thread=b_thread, text="hello, new laptop")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, m), 30, "phone: bob's message")
    l_thread = wait_for(lambda: thread_with(laptop, b.account), 30, "laptop: a chat with bob")
    wait_for(lambda: transcript_row(laptop, l_thread, m), 30, "laptop: bob's message")


def scenario_linked_devices_see_what_each_sent(lab):
    """What one of Alice's devices sends shows on her others (M021 d3), as hers: the phone writes to Bob and the
    laptop shows it in its chat with Bob; the laptop writes back and the phone shows that. Bob reads each once.
    Her reaction on the phone shows on the laptop, and her delete for everyone on the laptop deletes it on the
    phone too."""
    a, a_thread, b, b_thread = open_pair(lab)
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: laptop_id in a.cmd("devices")["devices"] or None, 40, "phone: the laptop on the list")
    from_phone = a.cmd("send", thread=a_thread, text="sent on the phone")["message_id"]
    l_thread = wait_for(lambda: thread_with(laptop, b.account), 40, "laptop: a chat with bob")
    row = wait_for(lambda: transcript_row(laptop, l_thread, from_phone), 40, "laptop: the phone's message")
    if row["from"] != "local:self" or row["text"] != "sent on the phone":
        raise ProbeError("laptop: the phone's message as %s" % row)
    from_laptop = laptop.cmd("send", thread=l_thread, text="sent on the laptop")["message_id"]
    row = wait_for(lambda: transcript_row(a, a_thread, from_laptop), 40, "phone: the laptop's message")
    if row["from"] != "local:self":
        raise ProbeError("phone: the laptop's message as %s" % row)
    for m in (from_phone, from_laptop):
        wait_for(lambda m=m: transcript_row(b, b_thread, m), 30, "bob: alice's message")
    ids = [r["id"] for r in b.cmd("transcript", thread=b_thread)["rows"]]
    if ids.count(from_phone) != 1 or ids.count(from_laptop) != 1:
        raise ProbeError("bob: alice's messages not once each (%s)" % ids)
    from_bob = b.cmd("send", thread=b_thread, text="react to this")["message_id"]
    wait_for(lambda: transcript_row(a, a_thread, from_bob), 40, "phone: bob's message")
    wait_for(lambda: transcript_row(laptop, l_thread, from_bob), 40, "laptop: bob's message")
    a.cmd("show_chat", thread=a_thread)
    a.cmd("react", message=from_bob, emoji="👍")
    wait_for(lambda: reactions_of(laptop, l_thread, from_bob).get("👍") == 1 or None, 40,
             "laptop: the phone's reaction")
    laptop.cmd("show_chat", thread=l_thread)
    laptop.cmd("delete_for_everyone", message=from_phone)
    wait_for(lambda: (transcript_row(a, a_thread, from_phone) or {"type": "deleted"})["type"] == "deleted", 40,
             "phone: its message deleted for everyone from the laptop")
    wait_for(lambda: (transcript_row(b, b_thread, from_phone) or {"type": "deleted"})["type"] == "deleted", 40,
             "bob: the message deleted")


def next_message_notice(u, thread, seen, timeout=40):
    """The next background message notice for `thread` after the first `seen` of them: {mentions_us, alerts}."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        with u.lock:
            notices = [m for _, m in u.history
                       if m.get("event") == "notice" and m.get("kind") == "message" and m.get("thread") == thread]
        if len(notices) > seen:
            return notices[seen]
        time.sleep(0.2)
    raise ProbeError("%s: no notice for message %d in %s" % (u.name, seen + 1, thread))


def scenario_muted_group_alerts_on_a_mention(lab):
    """Mentions in a muted group (group-chat G015): Carol mutes the group and puts the app away. Bob's plain
    message is silent; Bob mentioning her alerts and marks the chat "You were mentioned" until she opens it;
    Bob's `@everyone` does not reach her (only the owner's does); once she mutes its mentions too, even Alice's
    mention is silent — noticed, with the group open while she is away, but not alerting."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    c_thread = threads[c.name]
    c.cmd("mute", thread=c_thread)
    c.cmd("show_chat", thread=c.cmd("open_chat", account=a.account)["thread"])  # the group is not the open chat
    c.cmd("minimize")  # out of sight, still connected (a phone in the background stops its network)
    steps = [
        (b, "plain news", [], False, (False, False)),
        (b, "@%s look" % c.name, [c.account], False, (True, True)),
        (b, "@everyone hey", [], True, (False, False)),
    ]
    seen = 0
    for sender, text, mentions, everyone, (mentions_us, alerts) in steps:
        sender.cmd("send_mentions", thread=threads[sender.name], text=text, mentions=mentions, everyone=everyone)
        notice = next_message_notice(c, c_thread, seen)
        seen += 1
        if notice.get("mentions_us") != mentions_us or notice.get("alerts") != alerts:
            raise ProbeError("carol: %r from %s gave %s" % (text, sender.name, notice))
        row = next(t for t in c.cmd("threads")["threads"] if t["id"] == c_thread)
        if row.get("mentioned") != (seen >= 2):
            raise ProbeError("carol: the group's mention marker after %r: %s" % (text, row))
    c.cmd("show_chat", thread=c_thread)  # read: the marker goes
    if next(t for t in c.cmd("threads")["threads"] if t["id"] == c_thread).get("mentioned"):
        raise ProbeError("carol: still marked mentioned after opening the group")
    c.cmd("minimize")  # the group stays open: away, it still notifies
    c.cmd("mute_mentions", thread=c_thread)
    a.cmd("send_mentions", thread=threads[a.name], text="@%s now" % c.name, mentions=[c.account])
    notice = next_message_notice(c, c_thread, seen)
    if not notice.get("mentions_us") or notice.get("alerts"):
        raise ProbeError("carol: a mention with mentions muted gave %s" % notice)


def scenario_group_owner_appoints_an_admin(lab):
    """Roles and presets (group-chat G017, o1): Alice, the owner, makes Bob an admin. Every member's roster says so,
    from the roster Alice signed, and the group's chat says who made whom an admin. As an admin Bob's `@everyone`
    reaches Carol, muted and away, as the owner's does. Alice sets the group managed: everyone holds the preset.
    Alice takes Bob's role back: his `@everyone` no longer reaches anyone."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    group_id = plan_group(a, threads[a.name])

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    a.cmd("set_group_role", thread=threads[a.name], account=b.account, role="admin")
    for u in users:
        wait_for(lambda: roster(u)["roles"].get(b.account) == "admin" or None, 40, "%s: bob is an admin" % u.name)
    wait_for(lambda: any(r["text"] == "Member made admin" and r["from"] == a.account
                         for r in c.cmd("transcript", thread=threads[c.name])["rows"]) or None, 20,
             "carol: the line saying alice made bob an admin")

    c_thread = threads[c.name]
    c.cmd("mute", thread=c_thread)
    c.cmd("show_chat", thread=c.cmd("open_chat", account=a.account)["thread"])  # the group is not the open chat
    c.cmd("minimize")
    b.cmd("send_mentions", thread=threads[b.name], text="@everyone meeting at five", mentions=[], everyone=True)
    notice = next_message_notice(c, c_thread, 0)
    if not notice.get("mentions_us") or not notice.get("alerts"):
        raise ProbeError("carol: an admin's @everyone gave %s" % notice)

    a.cmd("set_group_preset", thread=threads[a.name], preset="managed")
    for u in users:
        wait_for(lambda: roster(u).get("preset") == "managed" or None, 40, "%s: the group is managed" % u.name)

    a.cmd("set_group_role", thread=threads[a.name], account=b.account, role="member")
    wait_for(lambda: roster(c)["roles"].get(b.account) == "member" or None, 40, "carol: bob is a member again")
    b.cmd("send_mentions", thread=threads[b.name], text="@everyone once more", mentions=[], everyone=True)
    notice = next_message_notice(c, c_thread, 1)
    if notice.get("mentions_us") or notice.get("alerts"):
        raise ProbeError("carol: a former admin's @everyone gave %s" % notice)


def scenario_admin_removes_while_owner_is_away(lab):
    """Changes on top of the owner's statement (group-chat G018, o2): Alice, the owner, makes Bob an admin and goes
    offline. Bob removes Dave — his own signed change, to Carol (and Alice's inbox) first, then to Dave: Carol's
    roster loses Dave, and Dave's chat goes read-only, saying Bob removed him. Carol leaves (her signed leave, no
    member_left). Alice comes back, finds both changes in her inbox and folds them into her next statement: Alice
    and Bob hold the same roster and statement, and no change is left unfolded."""
    users, threads = open_group(lab, 4)
    a, b, c, d = users
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    a.cmd("set_group_role", thread=threads[a.name], account=b.account, role="admin")
    for u in users:
        wait_for(lambda u=u: roster(u)["roles"].get(b.account) == "admin" or None, 40, "%s: bob is an admin" % u.name)
    a.quit()  # the owner is away from here on

    b.cmd("remove_member", thread=threads[b.name], account=d.account)
    wait_for(lambda: d.account not in roster(c)["members"] or None, 60, "carol: dave removed by bob")
    if roster(c)["held_changes"] < 1:
        raise ProbeError("carol: bob's removal is not held as a change (%s)" % roster(c))
    wait_for(lambda: roster(d).get("sync_state") == "removed" or None, 60, "dave: his group read-only")
    wait_for(lambda: any(r.get("text") == "Member removed" and r.get("from") == b.account
                         for r in d.cmd("transcript", thread=threads[d.name])["rows"]) or None, 30,
             "dave: the line saying bob removed him")
    sent = d.cmd("send_mentions", thread=threads[d.name], text="still here?", mentions=[], check=False)
    if sent.get("ok") or "chat.group.removed" not in str(sent.get("error")):
        raise ProbeError("dave: a send to a group he was removed from gave %s" % sent)

    c.cmd("leave_group", thread=threads[c.name])
    wait_for(lambda: c.account not in roster(b)["members"] or None, 60, "bob: carol left")
    if roster(b)["held_changes"] < 2:
        raise ProbeError("bob: the removal and the leave are not both held (%s)" % roster(b))

    a.start()
    a.wait_ready()
    a.wait_mesh()
    both = sorted([a.account, b.account])

    def settled(u):
        r = roster(u)
        return r if sorted(r["members"]) == both and r["held_changes"] == 0 else None

    ra = wait_for(lambda: settled(a), 120, "alice: the changes folded into her statement")
    rb = wait_for(lambda: settled(b), 60, "bob: the folded statement, nothing left unfolded")
    if (ra["statement_epoch"], ra["changes_hash"]) != (rb["statement_epoch"], rb["changes_hash"]):
        raise ProbeError("alice and bob hold different rosters: %s / %s" % (ra, rb))
    if ra["roles"].get(b.account) != "admin":
        raise ProbeError("alice: bob is no longer an admin after the fold (%s)" % ra["roles"])


def invite_and_join(inviter, inviter_thread, group_id, newcomer, timeout=60):
    """`inviter` invites `newcomer` to the group by account (invite_to_group, G019) and `newcomer` taps Accept on
    that invite — a new one, not one the group sent before: the invite card, and `newcomer`'s group thread."""
    seen = {i["nonce"] for i in newcomer.cmd("group_invites")["invites"]}
    inviter.cmd("invite_to_group", thread=inviter_thread, account=newcomer.account, timeout=60)
    invite = wait_for(lambda: next((i for i in newcomer.cmd("group_invites")["invites"]
                                    if i["group_id"] == group_id and i["nonce"] not in seen), None), timeout,
                      "%s: %s's group invite" % (newcomer.name, inviter.name))
    if invite["from"] != inviter.account:
        raise ProbeError("%s: the invite is from %s, not %s" % (newcomer.name, invite["from"], inviter.name))
    accepted = newcomer.cmd("accept_group_invite", nonce=invite["nonce"], group_id=group_id, timeout=60)
    thread = accepted.get("thread") or wait_for(lambda: group_thread_of(newcomer, group_id), timeout,
                                                "%s: the group's chat after accepting" % newcomer.name)
    return invite, thread


def wait_added(users, group_id, newcomer, adder, timeout=90):
    """Each of `users` lists `newcomer` in the group, brought in by `adder` (the roster's added_by, G019)."""
    def listed(u):
        r = u.cmd("group_roster", group_id=group_id)
        return r if newcomer.account in r["members"] and r["added_by"].get(newcomer.account) == adder.account else None
    for u in users:
        wait_for(lambda u=u: listed(u), timeout, "%s: %s on the roster, added by %s" % (u.name, newcomer.name,
                                                                                         adder.name))


def refused_invite(u, thread, newcomer, why):
    """`u`'s invite_to_group for `newcomer` is refused, and says `why` (its message key)."""
    got = u.cmd("invite_to_group", thread=thread, account=newcomer.account, check=False, timeout=60)
    if got.get("ok") or why not in str(got.get("error")):
        raise ProbeError("%s: inviting %s gave %s (wanted %s)" % (u.name, newcomer.name, got, why))


def scenario_member_invites_while_owner_is_away(lab):
    """Members invite (group-chat G019, o4): the group is open, so any member may invite. Alice, the owner, goes
    offline; Bob invites Dave, his friend and nobody else's. The invite shows how many are in the group, not who.
    Dave accepts: his accept goes back to Bob, whose device signs the add, sends it to the members and hands Dave the
    group — so Dave joins with the owner away, and everyone lists him as added by Bob. Bob, Carol and Dave talk in
    the group. Alice comes back, finds the add in her inbox and folds it into her next statement: Dave stays, added
    by Bob, and no change is left unfolded."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    d = lab.user("dave")
    d.wait_mesh()
    add_contacts([b, d], ["bob=dave"])
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    before = roster(a)["statement_epoch"]
    a.quit()  # the owner is away from here on

    invite, threads[d.name] = invite_and_join(b, threads[b.name], group_id, d)
    if invite.get("member_count") != 3:
        raise ProbeError("dave: the invite says %s are in the group (wanted 3)" % invite.get("member_count"))
    wait_added([d, b, c], group_id, d, b)
    everyone = sorted([a.account, b.account, c.account, d.account])
    for u in (b, c, d):
        wait_for(lambda u=u: sorted(roster(u)["members"]) == everyone or None, 60, "%s: all four" % u.name)

    for sender, readers in ((d, (b, c)), (b, (c, d)), (c, (b, d))):
        text = "%s in the group with dave" % sender.name.rsplit("-", 1)[0]
        sender.cmd("send_mentions", thread=threads[sender.name], text=text, mentions=[])
        for r in readers:
            wait_for(lambda r=r, text=text: said_in(r, threads[r.name], text), 60, "%s: %r" % (r.name, text))

    a.start()
    a.wait_ready()
    a.wait_mesh()

    def folded(u):
        r = roster(u)
        return r if (sorted(r["members"]) == everyone and r["added_by"].get(d.account) == b.account and
                     r["held_changes"] == 0 and r["statement_epoch"] > before) else None

    ra = wait_for(lambda: folded(a), 120, "alice: bob's add folded into her statement")
    rb = wait_for(lambda: folded(b), 60, "bob: the folded statement, nothing left unfolded")
    if (ra["statement_epoch"], ra["changes_hash"]) != (rb["statement_epoch"], rb["changes_hash"]):
        raise ProbeError("alice and bob hold different rosters: %s / %s" % (ra, rb))


def scenario_managed_group_only_admins_invite(lab):
    """Who invites in a managed group (group-chat G019, o4): Alice sets the group managed and makes Bob an admin.
    Carol, a plain member, is refused when she invites Dave (her friend, as he is Bob's); Bob, an admin, invites him:
    Dave accepts, and everyone lists him as added by Bob."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    d = lab.user("dave")
    d.wait_mesh()
    add_contacts([b, c, d], ["bob=dave", "carol=dave"])
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    a.cmd("set_group_preset", thread=threads[a.name], preset="managed")
    a.cmd("set_group_role", thread=threads[a.name], account=b.account, role="admin")
    for u in users:
        wait_for(lambda u=u: (lambda r: r.get("preset") == "managed" and r["roles"].get(b.account) == "admin")(
            roster(u)) or None, 40, "%s: managed, bob an admin" % u.name)

    refused_invite(c, threads[c.name], d, "chat.group.err.invite_not_allowed")
    invite_and_join(b, threads[b.name], group_id, d)
    wait_added([a, b, c, d], group_id, d, b)
    if any(i["from"] == c.account for i in d.cmd("group_invites")["invites"]):
        raise ProbeError("dave: holds an invite from carol, whose invite was refused")


def scenario_admin_readmits_a_removed_member(lab):
    """Bringing back someone removed (group-chat G020 5, o4): Alice makes Bob an admin; Bob removes Dave. Carol, a
    plain member, may not invite Dave back (he is on the group's removed list); Bob, an admin, may: Dave accepts, the
    add re-admits him, and he is back on everyone's roster and heard in the group."""
    users, threads = open_group(lab, 4, contacts=("bob=dave", "carol=dave"))
    a, b, c, d = users
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    a.cmd("set_group_role", thread=threads[a.name], account=b.account, role="admin")
    for u in users:
        wait_for(lambda u=u: roster(u)["roles"].get(b.account) == "admin" or None, 40, "%s: bob is an admin" % u.name)
    b.cmd("remove_member", thread=threads[b.name], account=d.account)
    for u in (a, c):
        wait_for(lambda u=u: d.account not in roster(u)["members"] or None, 60, "%s: dave removed by bob" % u.name)
    wait_for(lambda: roster(d).get("sync_state") == "removed" or None, 60, "dave: his group read-only")

    refused_invite(c, threads[c.name], d, "chat.group.err.invite_removed")
    _, threads[d.name] = invite_and_join(b, threads[b.name], group_id, d)
    wait_added([a, b, c, d], group_id, d, b)
    wait_for(lambda: roster(d).get("sync_state") == "active" or None, 60, "dave: his group active again")
    d.cmd("send_mentions", thread=threads[d.name], text="back again", mentions=[])
    for u in (a, b, c):
        wait_for(lambda u=u: said_in(u, threads[u.name], "back again"), 60, "%s: dave heard again" % u.name)


def scenario_remove_everyone_they_added(lab):
    """Undoing someone's invites (group-chat G020 6, o4): in an open group Bob invites two friends of his, who join
    as added by Bob. Alice makes Carol an admin; Carol removes everyone Bob added in one go: both are gone from every
    roster, and Bob, a first member, stays."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    extras = [lab.user(name) for name in ("erin", "frank")]
    for x in extras:
        x.wait_mesh()
    add_contacts([b] + extras, ["bob=erin", "bob=frank"])
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    for x in extras:
        invite_and_join(b, threads[b.name], group_id, x)
        wait_added([a, b, c], group_id, x, b)

    a.cmd("set_group_role", thread=threads[a.name], account=c.account, role="admin")
    for u in users:
        wait_for(lambda u=u: roster(u)["roles"].get(c.account) == "admin" or None, 40,
                 "%s: carol is an admin" % u.name)
    removed = c.cmd("remove_everyone_added_by", thread=threads[c.name], account=b.account, timeout=60)
    if removed.get("removed") != 2:
        raise ProbeError("carol: removed %s of the two bob added" % removed.get("removed"))
    left = sorted([a.account, b.account, c.account])
    for u in users:
        wait_for(lambda u=u: sorted(roster(u)["members"]) == left or None, 60,
                 "%s: bob's two gone, bob still in" % u.name)
    for x in extras:
        wait_for(lambda x=x: roster(x).get("sync_state") == "removed" or None, 60,
                 "%s: the group read-only" % x.name)


def joined_by_link_line(u, thread, newcomer, maker):
    """`u`'s group chat has the line saying `newcomer` joined by `maker`'s link (G021: member_joined.by_link)."""
    for m in u.cmd("messages", thread=thread)["messages"]:
        try:
            detail = json.loads(json.loads(m.get("payload") or "{}").get("detail") or "{}")
        except ValueError:
            continue
        if (detail.get("member_identity") == newcomer.account and detail.get("added_by") == maker.account and
                detail.get("by_link")):
            return m
    return None


def join_by_link(u, link, timeout=60):
    """`u` opens `link`: the answer its maker's device gave (preview / refused / no_answer)."""
    return u.cmd("join_by_link", link=link, timeout=timeout)


def scenario_join_by_link(lab):
    """Invite links (group-chat G021, o5): in an open group Carol, a plain member, makes a link. Erin, who has no
    contacts at all, opens it: Carol's device answers with the group's name and how many are in it (never who).
    Erin joins: Carol's device signs the add with the link's id and hands Erin the group — the full roster, every
    member checked by Erin herself. Everyone lists Erin as added by Carol, and the others' chats say she joined by
    Carol's link. The roster counts the link's use; Erin talks in the group."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    e = lab.user("erin")
    e.wait_mesh()
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    made = c.cmd("make_invite_link", thread=threads[c.name], timeout=60)
    link, link_id = made["link"], made["link_id"]
    if not link.startswith("pp://join/") or group_id in link:
        raise ProbeError("carol: the link %r is not a pp://join/ link that names nothing of the group" % link)
    for u in users:
        wait_for(lambda u=u: any(l["link_id"] == link_id and l["maker"] == c.account for l in roster(u)["links"])
                 or None, 60, "%s: carol's link in the roster" % u.name)

    answer = join_by_link(e, link)
    preview = answer.get("preview")
    if not preview:
        raise ProbeError("erin: no preview for carol's link (%s)" % answer)
    if preview.get("member_count") != 3 or preview.get("title") != "lab group":
        raise ProbeError("erin: the preview says %s" % preview)
    joined = e.cmd("confirm_join", request_id=answer["request_id"], timeout=60)
    if joined.get("group_id") != group_id:
        raise ProbeError("erin: joined %s, not the group" % joined)
    threads[e.name] = joined["thread"]

    everyone = sorted([a.account, b.account, c.account, e.account])
    wait_for(lambda: sorted(roster(e)["members"]) == everyone or None, 90, "erin: the full roster")
    wait_added([a, b, c, e], group_id, e, c)
    for u in (a, b):
        wait_for(lambda u=u: joined_by_link_line(u, threads[u.name], e, c), 60,
                 "%s: \"erin joined by carol's link\"" % u.name)
    wait_for(lambda: next((l for l in roster(c)["links"] if l["link_id"] == link_id and l["uses"] == 1), None), 60,
             "carol: her link used once")

    text = "erin here, by carol's link"
    e.cmd("send_mentions", thread=threads[e.name], text=text, mentions=[])
    for r in (a, b, c):
        wait_for(lambda r=r: said_in(r, threads[r.name], text), 60, "%s: %r" % (r.name, text))


def refused_link(u, link, why, timeout=60):
    answer = join_by_link(u, link, timeout)
    if answer.get("refused") != why:
        raise ProbeError("%s: opening the link gave %s (wanted refused %s)" % (u.name, answer, why))


def with_wrong_secret(link):
    """`link` (pp://join/…) with one byte of its secret changed: the same link id and maker, a secret nobody made."""
    prefix = "pp://join/"
    raw = bytearray(base64.urlsafe_b64decode(link[len(prefix):] + "=" * (-len(link[len(prefix):]) % 4)))
    raw[16] ^= 0xFF  # the secret follows the 16-byte link id
    return prefix + base64.urlsafe_b64encode(bytes(raw)).decode().rstrip("=")


def scenario_refused_links(lab):
    """Links that no longer let anyone in (group-chat G021, o5), each refused with its reason by the maker's device:
    Bob makes a link and Alice, the owner, revokes it (revoked); Carol makes one that lasts a few seconds (expired —
    the joiner's device refuses it as soon as it is past its expiry, as the maker's would); Dave makes one and
    leaves the group (revoked: a link dies with its maker). Erin, a stranger, tries each; she joins nothing. A link
    with a wrong secret gets no reply at all (no answer): a stranger cannot learn from Bob's device whether a link
    exists."""
    users, threads = open_group(lab, 4)
    a, b, c, d = users
    e = lab.user("erin")
    e.wait_mesh()
    group_id = lab_group_id(a, threads)

    def roster(u):
        return u.cmd("group_roster", group_id=group_id)

    def live(u, link_id):
        return any(l["link_id"] == link_id for l in roster(u)["links"])

    revoked = b.cmd("make_invite_link", thread=threads[b.name], timeout=60)
    wait_for(lambda: live(a, revoked["link_id"]) or None, 60, "alice: bob's link in the roster")
    a.cmd("revoke_invite_link", thread=threads[a.name], link_id=revoked["link_id"], timeout=60)
    wait_for(lambda: not live(b, revoked["link_id"]) or None, 60, "bob: his link revoked by alice")
    refused_link(e, revoked["link"], "revoked")

    short = c.cmd("make_invite_link", thread=threads[c.name], expires_in_s=5, timeout=60)
    wait_for(lambda: live(c, short["link_id"]) or None, 30, "carol: her short link in the roster")
    time.sleep(6)
    refused_link(e, short["link"], "expired")

    gone = d.cmd("make_invite_link", thread=threads[d.name], timeout=60)
    wait_for(lambda: live(a, gone["link_id"]) or None, 60, "alice: dave's link in the roster")
    d.cmd("leave_group", thread=threads[d.name])
    wait_for(lambda: d.account not in roster(a)["members"] or None, 60, "alice: dave left")
    if live(a, gone["link_id"]):
        raise ProbeError("alice: dave's link is still live after he left")
    refused_link(e, gone["link"], "revoked")

    guessed = b.cmd("make_invite_link", thread=threads[b.name], timeout=60)
    wait_for(lambda: live(b, guessed["link_id"]) or None, 30, "bob: his second link in the roster")
    answer = join_by_link(e, with_wrong_secret(guessed["link"]))
    if not answer.get("no_answer"):
        raise ProbeError("erin: a link with a wrong secret was answered (%s): it must get no reply" % answer)

    if group_thread_of(e, group_id):
        raise ProbeError("erin: holds the group's chat after three refusals")


def scenario_call_grows_into_a_group(lab):
    """Make a group during a call (group-chat G022, o3): Alice calls Bob, then adds Carol, whom Bob does not know.
    With three in a call that started 1:1, everyone's call screen offers "Make a group…". Bob makes one: the
    group starts with him alone and one line ("Group made during a call"), and Alice and Carol each get the
    ordinary invite, shown as a banner in the call — Carol's too, though Bob is no contact of hers (the invite
    names the call both are in). Alice joins from the banner; Carol says Not now: she stays in the call and
    out of the group, and the invite card stays in her chat with Bob. The call is the same call throughout:
    it does not become the group's (a group's call would drop Carol)."""
    a, a_thread, b, _ = open_pair(lab)
    c = lab.user("carol")
    c.wait_mesh()
    a.cmd("open_chat", account=c.account)  # Alice and Carol are contacts; Bob and Carol are not
    c.cmd("open_chat", account=a.account)
    call_id, _ = connect(a, a_thread, b)
    if a.state().get("show_make_group"):
        raise ProbeError("alice: Make a group offered with two in the call")
    a.cmd("invite_to_call", account=c.account)
    c.wait_surface("ringing", 40, call_id=call_id)
    c.cmd("accept_voice")
    c.wait_surface("in_call", 40, call_id=call_id)
    assert_everyone_in_call([a, b, c], call_id)
    for u in (a, b, c):
        wait_state(u, lambda s: s.get("show_make_group"), 20, "%s: Make a group… on the call screen" % u.name)

    made = b.cmd("make_group_from_call")
    group_id = made["group_id"]
    if sorted(made["invited"]) != sorted([a.account, c.account]):
        raise ProbeError("bob: invited %s (wanted everyone else in the call)" % made["invited"])
    if not made["title"]:
        raise ProbeError("bob: the group has no default name")
    rows = b.cmd("transcript", thread=made["thread"])["rows"]
    if [r["text"] for r in rows if r["type"] == "system"] != ["Group made during a call"]:
        raise ProbeError("bob: the new group's chat is %s (wanted its one line)" % rows)
    if sorted(b.cmd("group_roster", group_id=group_id)["members"]) != [b.account]:
        raise ProbeError("bob: someone was added without Join")

    banners = {}
    for u in (a, c):
        banners[u.name] = wait_for(lambda: (lambda g: g if g["shown"] else None)(u.cmd("call_group_invite")), 40,
                                   "%s: the group banner in the call" % u.name)
        if banners[u.name]["from"] != b.account or banners[u.name]["title"] != made["title"]:
            raise ProbeError("%s: the banner says %s" % (u.name, banners[u.name]))

    joined = a.cmd("call_group_invite_join")
    if not joined.get("thread"):
        raise ProbeError("alice: no group chat after Join")
    wait_for(lambda: sorted(b.cmd("group_roster", group_id=group_id)["members"]) == sorted([a.account, b.account])
             or None, 40, "bob: alice in the group")
    if not any(r["text"] == "Group made during a call" for r in a.cmd("transcript", thread=joined["thread"])["rows"]):
        raise ProbeError("alice: her group chat does not say it was made during a call")
    if a.cmd("call_group_invite")["shown"]:
        raise ProbeError("alice: the banner stays after Join")

    c.cmd("call_group_invite_not_now")
    if c.cmd("call_group_invite")["shown"]:
        raise ProbeError("carol: the banner stays after Not now")
    if not any(i["group_id"] == group_id for i in c.cmd("group_invites")["invites"]):
        raise ProbeError("carol: the invite card left her chat with bob")
    if any(t.get("group_id") == group_id for t in c.cmd("threads")["threads"]):
        raise ProbeError("carol: in the group after Not now")

    # The live call is left alone: the same call, everyone still in it, and not the group's.
    time.sleep(3)
    assert_everyone_in_call([a, b, c], call_id)
    for u in (a, b):
        group_thread = next(t["id"] for t in u.cmd("threads")["threads"] if t.get("group_id") == group_id)
        if u.cmd("joinable", thread=group_thread)["call_id"]:
            raise ProbeError("%s: the new group has a call (the live one became the group's)" % u.name)
        if u.state().get("call_id") != call_id:
            raise ProbeError("%s: no longer in the call it was in" % u.name)
    leave_all([a, b, c])


def saw_notice(u, kind, **fields):
    """`u` was told once (a `notice` event of `kind` with these fields), at any time so far."""
    with u.lock:
        return any(m.get("event") == "notice" and m.get("kind") == kind and
                   all(m.get(k) == v for k, v in fields.items()) for _, m in u.history)


def member_row(u, account):
    """`account`'s row on `u`'s call screen (`members`), or None."""
    return next((m for m in u.state().get("members", []) if m["identity"] == account), None)


def scenario_group_call_guest(lab):
    """Guests in a group's call (calls V062): Alice owns a group with Bob and Carol; the three are in its call. Bob
    brings Erin, his friend and no member: her ring says joining shows her to the three people in the call, and once
    in, everyone sees her as Bob's guest ("Erin joined as Bob's guest", "Guest" on her tile). Carol, a plain member,
    cannot remove her; Alice, the owner, can: Erin's device leaves, the others take it as her leave and the call key
    moves on without her. Erin cannot come back by herself (she is in no group chat, so has no Join); Bob, a member
    in the call, brings her back."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    e = lab.user("erin")
    e.wait_mesh()
    b.cmd("open_chat", account=e.account)  # Bob and Erin are friends; nobody else knows her
    e.cmd("open_chat", account=b.account)
    group_id = plan_group(a, threads[a.name])
    call_id = group_call(a, threads[a.name], [b, c])
    assert_everyone_in_call([a, b, c], call_id)

    b.cmd("invite_to_call", account=e.account)
    e.wait_surface("ringing", 40, call_id=call_id)
    ring = e.state()
    if not ring.get("ring_guest") or ring.get("ring_people") != 3:
        raise ProbeError("erin: the ring says %s (wanted a guest's ring showing her to 3 people)" %
                         {k: ring.get(k) for k in ("ring_guest", "ring_people")})
    e.cmd("accept_voice")
    e.wait_surface("in_call", 40, call_id=call_id)
    assert_everyone_in_call([a, b, c, e], call_id)

    for u in (a, b, c, e):
        wait_state(u, lambda s: any(m["identity"] == e.account and m["guest"] and m["brought_by"] == b.account
                                    for m in s.get("members", [])), 20, "%s: erin shown as bob's guest" % u.name)
        if any(m["guest"] for m in u.state().get("members", []) if m["identity"] != e.account):
            raise ProbeError("%s: a member shown as a guest" % u.name)
    for u in (a, c):
        wait_for(lambda: saw_notice(u, "guest_joined", identity=e.account, brought_by=b.account), 10,
                 "%s: told erin joined as bob's guest" % u.name)
    if not member_row(a, e.account)["can_remove"] or not member_row(a, e.account)["can_add_to_group"]:
        raise ProbeError("alice (owner): no Remove from call… / Add to the group on erin: %s" % member_row(a, e.account))
    if member_row(c, e.account)["can_remove"]:
        raise ProbeError("carol (a member): offered Remove from call… on erin")
    if e.state().get("show_invite"):
        raise ProbeError("erin: a guest offered Add people")

    refused = c.cmd("call_remove", account=e.account, check=False)
    if refused.get("ok"):
        raise ProbeError("carol: a plain member removed a guest")
    time.sleep(2)
    if e.state().get("call_id") != call_id:
        raise ProbeError("erin: out of the call after a member's refused remove")

    epochs = {u.name: u.state().get("media_epoch", 0) for u in (a, b, c)}
    a.cmd("call_remove", account=e.account)
    e.wait_surface("none", 30)
    for u in (a, b, c):
        wait_state(u, lambda s: e.account not in s.get("participants", []), 20, "%s: erin out of the call" % u.name)
        wait_state(u, lambda s: s.get("media_epoch", 0) > epochs[u.name], 20,
                   "%s: the call key moved on without erin" % u.name)
    if len({u.state().get("media_epoch") for u in (a, b, c)}) != 1:
        raise ProbeError("the members hold different key epochs: %s" % {u.name: u.state().get("media_epoch")
                                                                         for u in (a, b, c)})
    assert_everyone_in_call([a, b, c], call_id)

    # A guest stays a guest: no group chat, so no Join; she does not ring back in by herself.
    if any(t.get("group_id") == group_id for t in e.cmd("threads")["threads"]):
        raise ProbeError("erin: holds the group's chat")
    time.sleep(3)
    if e.state().get("kind") != "none":
        raise ProbeError("erin: back in the call by herself (%s)" % e.state().get("kind"))

    b.cmd("invite_to_call", account=e.account)  # a member in the call brings her back
    e.wait_surface("ringing", 40, call_id=call_id)
    e.cmd("accept_voice")
    e.wait_surface("in_call", 40, call_id=call_id)
    assert_everyone_in_call([a, b, c, e], call_id)
    leave_all([a, b, c, e])


def scenario_a_device_asks_for_a_copy_it_missed(lab):
    """A copy missing for a device is asked for (M021 §9): Alice exports a transfer code and her phone goes off
    before her new laptop starts, so nobody tells Bob of the laptop. Bob writes to Alice — a copy for the phone
    only. The laptop sees it in her mailbox, asks Bob's device for its own (its card going with the ask), and
    Bob's device seals it one: the message shows on the laptop."""
    a, a_thread, b, b_thread = open_pair(lab)
    hello = a.cmd("send", thread=a_thread, text="hi bob")["message_id"]  # her card (the phone alone) reaches Bob
    wait_for(lambda: transcript_row(b, b_thread, hello), 30, "bob: alice's hello")
    if len(b.cmd("kept_card", account=a.account)["devices"]) != 1:
        raise ProbeError("bob: alice's card does not list her phone alone")
    bundle = a.cmd("link_export")["bundle"]
    a.quit()
    laptop = lab.linked_device(a, "alice-laptop", bundle=bundle)
    laptop.wait_mesh()
    m = b.cmd("send", thread=b_thread, text="for every one of alice's devices")["message_id"]
    l_thread = wait_for(lambda: thread_with(laptop, b.account), 90, "laptop: a chat with bob")
    row = wait_for(lambda: transcript_row(laptop, l_thread, m), 90, "laptop: bob's message, asked for")
    if row["text"] != "for every one of alice's devices":
        raise ProbeError("laptop: bob's message as %s" % row)
    if "copy asked for message_id=%s" % m not in laptop.log_text():
        raise ProbeError("laptop: the message came without an ask — nothing was missing")


def scenario_a_call_rings_every_device_and_stops_once_answered(lab):
    """A call rings every device of the account (M021 d5): Bob calls Alice; her phone and her laptop both ring.
    The phone answers: the laptop stops ringing at once (no missed call there), and Bob and the phone talk."""
    a, a_thread, b, b_thread = open_pair(lab)
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: laptop_id in a.cmd("devices")["devices"] or None, 40, "phone: the laptop on the list")
    call_id = b.cmd("call", thread=b_thread)["call_id"]
    a.wait_surface("ringing", 40, call_id=call_id)
    laptop.wait_surface("ringing", 40, call_id=call_id)
    a.cmd("accept_voice")
    laptop.wait_surface("none", 15)
    b.wait_surface("in_call", 40, call_id=call_id)
    a.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(b, a, timeout=40)
    if "ringing stopped call_id=%s" % call_id not in laptop.log_text():
        raise ProbeError("laptop: stopped ringing, but not because the phone answered")
    b.cmd("leave")
    assert_idle(a, b, laptop, timeout=40)


def scenario_a_call_rings_on_every_device_after_each_called_out(lab):
    """#555 B186 (M024): Alice's phone calls Bob, then her laptop does. On the account pair's one session the
    laptop's opener replaced the phone's on Bob's side, so Bob's call back opened on the laptop only and the phone
    never rang. The control lane goes per device now: Bob calls Alice, both her devices ring, and the phone — the
    one B186 left silent — answers with media both ways (its copy brought the media key too)."""
    a, a_thread, b, b_thread = open_pair(lab)
    laptop = lab.linked_device(a, "alice-laptop")
    laptop.wait_mesh()
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: laptop_id in a.cmd("devices")["devices"] or None, 40, "phone: the laptop on the list")
    l_thread = laptop.cmd("open_chat", account=b.account)["thread"]
    for caller, thread in ((a, a_thread), (laptop, l_thread)):
        connect(caller, thread, b)
        b.cmd("leave")
        assert_idle(a, laptop, b, timeout=40)
    call_id = b.cmd("call", thread=b_thread)["call_id"]
    a.wait_surface("ringing", 40, call_id=call_id)
    laptop.wait_surface("ringing", 40, call_id=call_id)
    a.cmd("accept_voice")
    laptop.wait_surface("none", 15)
    b.wait_surface("in_call", 40, call_id=call_id)
    a.wait_surface("in_call", 40, call_id=call_id)
    assert_media_flows(b, a, timeout=40)
    for u in (a, laptop):
        if "control lane: couldn't decrypt" in u.log_text():
            raise ProbeError("%s: a lane copy did not open" % u.name)
    b.cmd("leave")
    assert_idle(a, b, laptop, timeout=40)


def scenario_a_restored_device_hears_the_answer_to_its_call(lab):
    """#555 (iPhone call 1): Alice calls Bob, who answers — his card reaches her account. Her phone is gone; she
    restores on a new one (a fresh list holding only it, no cards kept) and calls Bob. Bob answers with the account
    copy (one device each, the M024 bridge) under the session her invite opened; the restored phone had dropped
    such an answer when it held no card of Bob's, and stayed waiting. Now Bob's card rides it (told per device), and
    a lane record of his that comes before any card waits for one: the call connects, and no record of Bob's is
    dropped for want of his card. (Here the shared mailbox's backlog may bring his card first: its records verify
    before their old account copies fail to open.)"""
    a, a_thread, b, b_thread = open_pair(lab)
    connect(a, a_thread, b)
    a.cmd("leave")
    assert_idle(a, b, timeout=40)
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    a.cmd("locker_publish", timeout=60)
    logged_before = len(a.log_text())
    a.restore_from_phrase(phrase)
    a.wait_mesh()
    a_thread = a.cmd("chat_with", account=b.account)["thread"]
    connect(a, a_thread, b)
    dropped = [line for line in a.log_text()[logged_before:].splitlines()
               if "signature check failed" in line and "sender=%s " % b.account in line
               and "held_for_card=1" not in line]
    if dropped:
        raise ProbeError("restored phone: dropped bob's lane record(s) it could not check: %s" % dropped[0])
    a.cmd("leave")
    assert_idle(a, b, timeout=40)


def scenario_a_newer_device_list_follows_the_messages(lab):
    """Every envelope says how new its sender's device list is (M021 d6): Carol shares a group with Bob but is
    not his contact, so what his main device announces never reaches her. Bob links a laptop: his next group
    message brings Carol his newer list (with the laptop: his first copy to her opens their pair, carrying his
    card). He takes the laptop off: his next message only says his list is newer, Carol asks his device for it
    through the relay (his privacy gate refuses her a direct link), and she stops writing to the laptop."""
    users, threads = open_group(lab, 3)
    a, b, c = users
    # Bob and Carol talk first: Bob learns Carol's devices from her message, and his answer opens their device
    # pair — so his later copies to her carry no card, only how new his list is.
    c.cmd("show_chat", thread=threads[c.name])
    c.cmd("compose_send", text="hello from carol")
    wait_for(lambda: said_in(b, threads[b.name], "hello from carol"), 40, "bob: carol's first message")
    b.cmd("show_chat", thread=threads[b.name])
    b.cmd("compose_send", text="hello group")
    wait_for(lambda: said_in(c, threads[c.name], "hello group"), 40, "carol: bob's first message")
    laptop = lab.linked_device(b, "bob-laptop")
    laptop.wait_mesh()
    laptop_id = laptop.cmd("whoami")["peer_id"]
    wait_for(lambda: laptop_id in b.cmd("devices")["devices"] or None, 40, "bob: the laptop on his list")
    if laptop_id in c.cmd("kept_card", account=b.account)["devices"]:
        raise ProbeError("carol: knew bob's laptop before any message said so")
    b.cmd("show_chat", thread=threads[b.name])
    b.cmd("compose_send", text="now on two devices")
    wait_for(lambda: laptop_id in c.cmd("kept_card", account=b.account)["devices"] or None, 60,
             "carol: bob's list with the laptop, from his message")
    b.cmd("remove_device", peer_id=laptop_id)
    b.cmd("compose_send", text="back to one")
    wait_for(lambda: laptop_id not in c.cmd("kept_card", account=b.account)["devices"] or None, 60,
             "carol: bob's list without the laptop, from his message")
    if "newer card asked of %s" % b.account not in c.log_text():
        raise ProbeError("carol: bob's shorter list did not come from what his message said")


def scenario_chat_quote_reply(lab):
    """Bob replies to Alice's message quoting it, from the composer (validated, routed like the GUI's
    Send); Alice sees his reply with her message quoted under it. A draft over the limit is refused."""
    a, a_thread, b, b_thread = open_pair(lab)
    a.cmd("show_chat", thread=a_thread)
    b.cmd("show_chat", thread=b_thread)
    original = a.cmd("send", thread=a_thread, text="are we still on for friday?")["message_id"]
    wait_for(lambda: transcript_row(b, b_thread, original), 20, "bob: alice's message")
    preview = b.cmd("quote", message=original)["preview"]
    if preview != "are we still on for friday?":
        raise ProbeError("bob: quote preview %r" % preview)
    b.cmd("compose_send", text="yes, 7pm")

    def quoted_reply():
        for row in a.cmd("transcript", thread=a_thread)["rows"]:
            text = row.get("text", "")
            if "yes, 7pm" in text and "are we still on for friday?" in text:
                return row
        return None
    wait_for(quoted_reply, 20, "alice: bob's quoted reply")
    try:
        b.cmd("compose_send", text="x" * 70000)
        raise ProbeError("bob: an over-long draft was sent")
    except ProbeError as e:
        if "over-long" in str(e):
            raise


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


def stream_id(account):
    """The media_relay stream an account publishes on in a call (PublisherStreamIdForIdentity)."""
    h = 2166136261
    for c in account.encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h or 1


def assert_everyone_in_call(users, call_id, timeout=40):
    """Each of `users` is in `call_id`, sees all of them in it and hears every other one (that one's
    stream keeps receiving). Watches everyone at once and stops early — once all hear and see each
    other, or once someone is out of the call — then names who misses whom."""
    want = {u.name: {o.name: stream_id(o.account) for o in users if o is not u} for u in users}
    heard = {u.name: set() for u in users}
    last = {u.name: live_streams(u) for u in users}
    deadline = time.time() + timeout
    while True:
        time.sleep(1.0)
        states = {u.name: u.state() for u in users}
        for u in users:
            now = {row["stream_id"]: row["rx_frames"] for row in states[u.name].get("streams", [])}
            heard[u.name] |= {n for n, sid in want[u.name].items() if now.get(sid, 0) > last[u.name].get(sid, 0)}
            last[u.name] = now
        out = [u for u in users if states[u.name].get("call_id") != call_id]
        done = all(len(heard[n]) == len(w) for n, w in want.items()) and all(
            len(st.get("participants", [])) == len(users) for st in states.values())
        if done or out or time.time() > deadline:
            break
    misses = ["%s: not in the call (%s)" % (u.name, states[u.name]) for u in out]
    for u in users:
        missed = [n for n in want[u.name] if n not in heard[u.name]]
        if missed and u not in out:
            misses.append("%s hears %s, not %s" % (u.name, ", ".join(sorted(heard[u.name])) or "nobody",
                                                   ", ".join(missed)))
        seen = states[u.name].get("participants", [])
        if len(seen) != len(users) and u not in out:
            misses.append("%s sees %d of %d in the call (not %s)" % (
                u.name, len(seen), len(users), ", ".join(o.name for o in users if o.account not in seen) or "?"))
    if misses:
        raise ProbeError("; ".join(misses))


def leave_all(users):
    for u in users:
        u.cmd("leave", check=False)
    assert_idle(*users)


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
        if u.state().get("path") != "hop":
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


# Group calls across who has added whom: members' contact links are a dimension of their own
# (TEST_STRATEGY.md, "Group calls and contacts"). The other group_* scenarios have no links.
def scenario_group_call_one_way_contact(lab):
    """Four in a call where one member pair is linked, one way (dogfood: a phone heard two of three,
    the others saw three of four): Bob added Dave, Dave never added Bob, Carol has neither. Alice
    (everyone's contact) calls: everyone hears and sees everyone."""
    users, threads = open_group(lab, 4, contacts=("bob>dave",))
    a, b, c, d = users
    call_id = group_call(a, threads[a.name], [b, c, d])
    assert_everyone_in_call(users, call_id)
    leave_all(users)


def scenario_group_call_placed_by_stranger(lab):
    """Dave, whom no other member has added (Bob and Carol added each other), calls the group of
    four: his ring and his call reach members who do not know him. A member's call rings only where Who
    can call me lets it (privacy P007: a new profile takes contacts only, which drops it silently) — Bob
    and Carol chose Everyone; Alice, the owner, added Dave."""
    users, threads = open_group(lab, 4, contacts=("bob=carol",))
    a, b, c, d = users
    for u in (b, c):
        set_call_invite_policy(u, "everyone")
    call_id = group_call(d, threads[d.name], [a, b, c])
    assert_everyone_in_call(users, call_id)
    leave_all(users)


def scenario_group_one_way_joins_late(lab):
    """The other way round, joining late: Dave added Bob, Bob never added Dave. Bob calls the group
    of four; Dave declines, then joins from the group's call button. Carol has not added Bob: his ring
    reaches her because she lets everyone call (privacy P007: contacts only would drop it)."""
    users, threads = open_group(lab, 4, contacts=("dave>bob",))
    a, b, c, d = users
    set_call_invite_policy(c, "everyone")
    call_id = b.cmd("call", thread=threads[b.name])["call_id"]
    for u in (a, c, d):
        u.wait_surface("ringing", 40, call_id=call_id)
    d.cmd("decline")
    for u in (a, c):
        u.cmd("accept_voice")
    assert_everyone_in_call([a, b, c], call_id)
    d.wait_surface("none", 20)
    join(d, threads[d.name], call_id)
    assert_everyone_in_call(users, call_id)
    leave_all(users)


def scenario_group_one_way_network_drops(lab):
    """Three in a call where Bob added Carol and Carol never added Bob; Carol's network drops for 6 s
    and comes back (a phone on the road reattaches to the hop): everyone hears and sees everyone
    again, in the same call."""
    users, threads = open_group(lab, 3, contacts=("bob>carol",))
    a, b, c = users
    call_id = group_call(a, threads[a.name], [b, c])
    assert_everyone_in_call(users, call_id)
    net_drop(c, 6, during=not_connected_while_down(c))
    still_in_call(users, call_id, 60)
    assert_everyone_in_call(users, call_id)
    leave_all(users)



def scenario_group_two_join_through_drops(lab):
    """Two people connect through retries at once (dogfood: the one who heard two of three and the
    one they could not hear both retried to get connected): Bob added Carol, Carol never added Bob.
    Alice calls; Bob and Carol answer and both their links drop twice while they connect. Once the
    network holds, everyone hears and sees everyone."""
    users, threads = open_group(lab, 3, contacts=("bob>carol",))
    a, b, c = users
    call_id = a.cmd("call", thread=threads[a.name])["call_id"]
    for u in (b, c):
        u.wait_surface("ringing", 40, call_id=call_id)
    for u in (b, c):
        u.cmd("accept_voice")
    net_drop_together((b, c), 6)
    time.sleep(3)
    net_drop_together((b, c), 6)
    for u in users:
        u.wait_surface("in_call", 90, call_id=call_id)
    assert_everyone_in_call(users, call_id, timeout=60)
    leave_all(users)



def scenario_group_fourth_answers_late(lab):
    """Real people answer at different times: Bob and Dave answer at once (the third join moves the
    call onto the hop), Carol 10 s later. Her accept reaches Alice after the hop was announced: she
    must still be told the hop, and everyone hears and sees everyone."""
    users, threads = open_group(lab, 4)
    a, b, c, d = users
    call_id = a.cmd("call", thread=threads[a.name])["call_id"]
    for u in (b, c, d):
        u.wait_surface("ringing", 40, call_id=call_id)
    for u in (b, d):
        u.cmd("accept_voice")
    assert_everyone_in_call([a, b, d], call_id)
    time.sleep(10)
    c.cmd("accept_voice")
    c.wait_surface("in_call", 40, call_id=call_id)
    assert_everyone_in_call(users, call_id, timeout=60)
    leave_all(users)


def group_thread_of(u, group_id):
    """`u`'s chat for the group, or None."""
    for t in u.cmd("threads")["threads"]:
        if t.get("group_id") == group_id:
            return t["id"]
    return None


def said_in(u, thread, text):
    return next((r for r in u.cmd("transcript", thread=thread)["rows"] if r.get("text") == text), None)


def checked_roster(u, group_id, state, timeout=60):
    """`u`'s roster of the group once its member check ended in `state` (group-chat G013)."""
    def settled():
        roster = u.cmd("group_roster", group_id=group_id)
        return roster if roster.get("sync_state") == state else None
    return wait_for(settled, timeout, "%s: group check ends %s" % (u.name, state))


def remove_and_wait(owner, owner_thread, group_id, member, watchers):
    owner.cmd("remove_member", thread=owner_thread, account=member.account)
    for u in watchers:
        wait_for(lambda u=u: member.account not in u.cmd("group_roster", group_id=group_id)["members"] or None, 40,
                 "%s: %s out" % (u.name, member.name))


def scenario_group_restore_checks_members(lab):
    """Carol's locker knows Alice (owner), Bob, Carol and Dave; then her phone is lost. Alice removes Dave;
    Carol restores from her recovery phrase: her group comes back checking its members, takes the owner's
    newer roster (no Dave — her stale one would have kept writing to him), and her next message reaches
    Alice and Bob."""
    users, threads = open_group(lab, 4)
    a, b, c, d = users
    group_id = lab_group_id(a, threads)
    phrase = c.cmd("recovery_phrase", pin=PIN)["phrase"]
    c.cmd("locker_publish", timeout=60)
    c.crash()  # the phone is lost: her locker stays as it was
    remove_and_wait(a, threads[a.name], group_id, d, (b,))
    c.restore_from_phrase(phrase)
    c.wait_mesh()
    roster = checked_roster(c, group_id, "active")
    if d.account in roster["members"] or sorted(roster["members"]) != sorted(u.account for u in (a, b, c)):
        raise ProbeError("carol: restored roster %s (Dave was removed after her locker)" % roster["members"])
    thread = group_thread_of(c, group_id)
    if not thread:
        raise ProbeError("carol: no group chat after the restore")
    c.cmd("show_chat", thread=thread)
    c.cmd("compose_send", text="back from a reinstall")
    for u in (a, b):
        wait_for(lambda u=u: said_in(u, threads[u.name], "back from a reinstall"), 40, "%s: Carol's message" % u.name)


def scenario_group_removed_member_restores(lab):
    """Dave's locker still has him in the group: his phone was lost before Alice removed him. When he restores
    from his recovery phrase, his group's member check ends removed: read-only, nothing sent. His locker then
    keeps the group as left (M023 s4), so restoring once more does not bring it back."""
    users, threads = open_group(lab, 3)
    a, b, d = users
    group_id = lab_group_id(a, threads)
    phrase = d.cmd("recovery_phrase", pin=PIN)["phrase"]
    d.cmd("locker_publish", timeout=60)
    d.crash()  # the phone is lost: his locker stays as it was
    remove_and_wait(a, threads[a.name], group_id, d, (b,))
    d.restore_from_phrase(phrase)
    d.wait_mesh()
    checked_roster(d, group_id, "removed")
    d.cmd("locker_publish", timeout=60)
    d.restore_from_phrase(phrase)
    d.wait_mesh()
    roster = d.cmd("group_roster", group_id=group_id)
    if roster.get("sync_state") or roster.get("members"):
        raise ProbeError("dave: a group he was removed from came back from his locker (%s)" % roster)


def scenario_group_restored_owner_takes_newer_roster(lab):
    """The owner checks too: Alice removes Dave and her phone dies before her locker caught up (it publishes a
    few seconds after a change), so her locker still has Dave. Restored from her recovery phrase, she asks her
    members; their newer statement — her own signature — wins over her locker, so Dave stays out, and her next
    message reaches Bob and Carol."""
    users, threads = open_group(lab, 4)
    a, b, c, d = users
    group_id = lab_group_id(a, threads)
    phrase = a.cmd("recovery_phrase", pin=PIN)["phrase"]
    time.sleep(6)  # a publish batched from the group's setup has run: the next one is the removal's own
    a.cmd("locker_publish", timeout=60)
    mark = len(a.log_text())
    remove_and_wait(a, threads[a.name], group_id, d, (b, c))
    a.crash()  # once Bob and Carol hold the removal, and before her locker publishes it
    if "Account locker published" in a.log_text()[mark:]:
        raise ProbeError("alice: her locker caught the removal before her phone died; nothing stale to restore")
    a.restore_from_phrase(phrase)
    a.wait_mesh()
    roster = checked_roster(a, group_id, "active")
    if d.account in roster["members"]:
        raise ProbeError("alice: her restored roster put Dave back (%s)" % roster["members"])
    thread = group_thread_of(a, group_id)
    a.cmd("show_chat", thread=thread)
    a.cmd("compose_send", text="the owner is back")
    for u in (b, c):
        wait_for(lambda u=u: said_in(u, threads[u.name], "the owner is back"), 40, "%s: Alice's message" % u.name)


# Phone networks (--nat, a NAT per user). Dogfood: four in a call, a phone on the highway heard two
# of three, the others saw three of four; that phone and the one it could not hear both retried to
# get connected, and had not added each other both ways.
# --- video: how frames travel (SyntheticVideo: real frame sizes, no camera or GPU in the lab) -----------

WATCH_VIDEO_S = 45


def wait_sees_video(u, senders, timeout=60):
    """`u` receives video from `senders` others (that many streams have frames)."""
    deadline = time.time() + timeout
    while True:
        rows = u.state().get("video_arrivals", [])
        if len([r for r in rows if r["frames"] > 0]) >= senders:
            return
        if time.time() > deadline:
            raise ProbeError("%s: sees video from %d of %d (%s)" % (u.name, len(rows), senders, rows))
        time.sleep(1.0)


def video_snapshot(users):
    """Now: each receiver's view of each sender's video {(receiver, sender): row} (VideoArrivalStats counters),
    each one's video frames sent {"tx", name: n}, audio frames received {"audio", name: n} and what it heard
    {"heard", name: row} (the engine's audio counters over every stream it plays, `streams` of them)."""
    snap = {}
    for u in users:
        state = u.state()
        name = u.name.split("-")[0]
        snap[("tx", name)] = state.get("tx_video_frames", 0)
        snap[("audio", name)] = state.get("rx_audio_frames", 0)
        snap[("heard", name)] = dict(state.get("audio_heard", {}), streams=len(users) - 1)
        for row in state.get("video_arrivals", []):
            sender = next((o.name.split("-")[0] for o in users if stream_id(o.account) == row["stream_id"]), "peer")
            snap[(name, sender)] = row
    return snap


def link_counters(users):
    """Each user's shaped links now: {(name, "up"|"down"): (bytes sent, packets dropped, packets sent)} (tc's root
    qdisc)."""
    def read(text):
        m = re.search(r"Sent (\d+) bytes (\d+) pkt \(dropped (\d+)", text)
        return (int(m.group(1)), int(m.group(3)), int(m.group(2))) if m else (0, 0, 0)
    def snmp(text):
        """UDP datagrams and TCP segments this user's network namespace sent (/proc/net/snmp)."""
        rows = {}
        lines = text.splitlines()
        for head, values in zip(lines[::2], lines[1::2]):
            name, _, fields = head.partition(":")
            rows[name] = dict(zip(fields.split(), values.partition(":")[2].split()))
        return int(rows.get("Udp", {}).get("OutDatagrams", 0)), int(rows.get("Tcp", {}).get("OutSegs", 0))
    out = {}
    for u in users:
        name = u.name.split("-")[0]
        out[(name, "up")] = read(net(u, "tc -s qdisc show dev eth0"))
        out[(name, "down")] = read(gw_net(u, "tc -s qdisc show dev %s" % gw_inner_dev(u)))
        out[(name, "proto")] = snmp(net(u, "cat /proc/net/snmp"))
    return out


def link_report(before, after, seconds):
    """What went over each user's links in the window: kbit/s, packets/s and their mean size, and packets the shaper
    dropped (its queue full)."""
    report = {}
    for key, value in sorted(after.items()):
        if key[1] == "proto":
            udp, tcp = value
            report["%s_sent" % key[0]] = "udp %d pkt/s tcp %d seg/s" % (
                (udp - before[key][0]) / seconds, (tcp - before[key][1]) / seconds)
            continue
        sent, dropped, packets = value
        was_sent, was_dropped, was_packets = before[key]
        n = max(packets - was_packets, 1)
        report["%s_%s" % key] = "%d kbit/s %d pkt/s %d B/pkt dropped=%d" % (
            (sent - was_sent) * 8 / 1000 / seconds, n / seconds, (sent - was_sent) / n, dropped - was_dropped)
    return report


def video_report(before, after, seconds):
    """How each one's frames reached each other one between two snapshots (before decoding): a 20 fps sender
    spaces them ~50 ms; a reliable lane stalls on a loss, then lets frames go together (bunched). The counters are
    the window's; the gap percentiles and the max are the whole call's. heard_<name> is what that one heard of all
    the others: audio concealed (lost or late: PLC and FEC; % of the audio it played), speech cut to catch up,
    packets that came too late, slots with nothing to play (underruns), and the jitter target at the end."""
    report = {}
    for kind in ("tx", "audio"):
        rates = {key[1]: round((n - before.get(key, 0)) / seconds, 1) for key, n in after.items() if key[0] == kind}
        report["video_sent_fps" if kind == "tx" else "audio_rx_fps"] = rates
    for key, row in sorted((k, v) for k, v in after.items() if k[0] == "heard"):
        was = before.get(key, {})
        delta = {k: row.get(k, 0) - was.get(k, 0) for k in row if k not in ("streams", "jitter_target_ms")}
        played_ms = max(seconds * 1000 * row["streams"], 1)
        report["heard_%s" % key[1]] = (
            "concealed=%dms (%.1f%%) speech_cuts=%d late=%d underruns=%d plc=%d fec=%d silence_skips=%d "
            "jitter_target=%dms" % (
                delta.get("concealed_ms", 0), 100.0 * delta.get("concealed_ms", 0) / played_ms,
                delta.get("speech_drops", 0), delta.get("late_drops", 0), delta.get("underruns", 0),
                delta.get("plc_frames", 0), delta.get("fec_frames", 0), delta.get("silence_drops", 0),
                row.get("jitter_target_ms", 0)))
    for key, row in sorted((k, v) for k, v in after.items() if k[0] not in ("tx", "audio", "heard")):
        was = before.get(key, {})
        frames = row["frames"] - was.get("frames", 0)
        bunched = row["bunched"] - was.get("bunched", 0)
        report["%s<-%s" % key] = "fps=%.1f stalls=%d bunched=%d%% lost=%d gap_max=%d (call p50=%d p95=%d)" % (
            frames / seconds, row["stalls"] - was.get("stalls", 0), round(100 * bunched / max(frames, 1)),
            row["lost"] - was.get("lost", 0), row["gap_max_ms"], row["gap_p50_ms"], row["gap_p95_ms"])
    return report


def video_call(users, thread, road=None, video=True):
    """users[0] calls with video on `thread`; the others answer with video; once everyone sees every other one,
    the call runs WATCH_VIDEO_S (with `road`, a PHONE_* profile, on everyone's links from then: set up on clean
    links, as a call that is up when the road gets rough). Returns how the frames arrived then, and leaves."""
    caller, answerers = users[0], users[1:]
    call_id = caller.cmd("call", thread=thread, video=video)["call_id"]
    for u in answerers:
        u.wait_surface("ringing", 60, call_id=call_id)
    for u in answerers:
        u.cmd("accept" if video else "accept_voice")
    for u in users:
        u.wait_surface("in_call", 90, call_id=call_id)
    for u in users:
        if video:
            wait_sees_video(u, len(users) - 1)
        else:
            assert_hears(u, len(users) - 1, timeout=60)
    extra = {}
    if road:
        with Road(users, road) as r:
            before, links_before, started = video_snapshot(users), link_counters(users), time.time()
            time.sleep(WATCH_VIDEO_S)
            after, links_after, seconds = video_snapshot(users), link_counters(users), time.time() - started
        extra["road_seed"] = r.seed
        extra["links"] = link_report(links_before, links_after, seconds)
    else:
        before, started = video_snapshot(users), time.time()
        time.sleep(WATCH_VIDEO_S)
        after, seconds = video_snapshot(users), time.time() - started
    report = video_report(before, after, seconds)
    report.update(extra)
    leave_all(users)
    return report


def scenario_group_video_three(lab):
    """Alice calls the group with video; Bob and Carol answer with video (over the hop). Everyone sees both others;
    reports how each one's frames arrived (a clean network: the baseline)."""
    lab.synthetic_video = True
    users, threads = open_group(lab, 3)
    return video_call(users, threads[users[0].name])


def scenario_group_video_on_the_road(lab):
    """The video dogfood: a group video call of three over the hop, then everyone's link turns into a phone's on
    the road (PHONE_ROAD: lossy, jittery; no handovers). Reports how each one's frames arrived on it."""
    lab.synthetic_video = True
    users, threads = open_group(lab, 3)
    return video_call(users, threads[users[0].name], road=PHONE_ROAD)


def scenario_video_on_the_road(lab):
    """The same for a 1:1 video call (its own direct media lane, not the hop): the comparison."""
    lab.synthetic_video = True
    a, a_thread, b, _ = open_pair(lab)
    return video_call([a, b], a_thread, road=PHONE_ROAD)


def voice_measurement(group, profile):
    """A voice call (no camera) on `profile`, set up as video_measurement's: what audio alone puts on the wire —
    each packet a 20 ms Opus frame, so the mean packet size less the frame is the stack's per-packet overhead."""
    def run(lab):
        if group:
            users, threads = open_group(lab, 3)
            return video_call(users, threads[users[0].name], road=profile, video=False)
        a, a_thread, b, _ = open_pair(lab)
        return video_call([a, b], a_thread, road=profile, video=False)
    return run


def video_measurement(group, profile):
    """A scenario: a group video call of three (or a 1:1 one) set up on clean links, then everyone's link turns
    into `profile`; reports how frames arrived on it. Opt-in (run with --only): measurements, not checks."""
    def run(lab):
        lab.synthetic_video = True
        if group:
            users, threads = open_group(lab, 3)
            return video_call(users, threads[users[0].name], road=profile)
        a, a_thread, b, _ = open_pair(lab)
        return video_call([a, b], a_thread, road=profile)
    return run


VIDEO_MEASUREMENTS = [("video_%s_%s" % (kind, name), video_measurement(kind == "group", profile))
                      for kind in ("group", "pair")
                      for name, profile in (("lossy", PHONE_LOSSY), ("bursty", PHONE_BURSTY),
                                            ("jittery", PHONE_JITTERY), ("narrow", PHONE_NARROW),
                                            ("wide", PHONE_WIDE))] + [
    ("voice_%s_wide" % kind, voice_measurement(kind == "group", PHONE_WIDE)) for kind in ("group", "pair")]


def call_link(u):
    """The call's link now (Amp ConnectionStats of the association its media rides), or None."""
    return u.state().get("link")


# The call-path matrix (--nat; run with --only call_path_<path>_<timing>[_two_hops]). A 1:1 voice call between
# Alice and Bob on a long path; each row is one path x one timing, on the lab hop or on two seeds. Every row checks
# the same things: the call connects, both hear each other, and each call link learns its round trip and resends a
# bounded share of its Reliable packets.
CALL_PATHS = {
    "far": (PHONE_FAR, PHONE_FAR),  # both far, nothing else: the round trip alone
    "far_lossy": (PHONE_FAR_LOSSY, PHONE_FAR_LOSSY),  # both far and lossy, as long mobile paths are
    "one_far": (None, PHONE_FAR_LOSSY),  # Alice near the relay, Bob far on a lossy path (#502's call)
}
# mid: set up on clean links, then the path turns long (a live call adapting). start: long from the start, the call
# 20 s later (setting one up). idle: long from the start, the call after 3 min idle. A phone holds no reservation
# while idle (it parks for a call), so this is a call set up on links and NAT mappings that have aged.
CALL_TIMINGS = {"mid": 0, "start": 20, "idle": 180}
LAB_HOP_IP = "198.51.100.10"


def place_voice_call(a, a_thread, b):
    """Alice calls Bob (voice); he answers; both are in the call and hear each other."""
    call_id = a.cmd("call", thread=a_thread, video=False)["call_id"]
    b.wait_surface("ringing", 60, call_id=call_id)
    b.cmd("accept_voice")
    for u in (a, b):
        u.wait_surface("in_call", 90, call_id=call_id)
    for u in (a, b):
        assert_hears(u, 1, timeout=60)
    return call_id


def check_call_links(profiles, settle=20, window=30):
    """After `settle` s on the path, over `window` s: everyone still hears the other, each call link has learned
    its round trip (at least 80 % of what its profile adds) and resent at most a quarter of its Reliable packets
    (two fifths on a lossy profile). Returns a line per user."""
    users = list(profiles)
    time.sleep(settle)
    before = {u: call_link(u) for u in users}
    time.sleep(window)
    after = {u: call_link(u) for u in users}
    for u in users:
        assert_hears(u, 1, timeout=30)
    report = {}
    for u in users:
        profile, was, now = profiles[u], before[u], after[u]
        if not now:
            raise ProbeError("%s: no call link counters" % u.name)
        sent = now["reliable_sent"] - was["reliable_sent"] if was and was["link_id"] == now["link_id"] else 0
        resent = now["retransmits"] - was["retransmits"] if sent else 0
        report[u.name.split("-")[0]] = "rtt=%d ms sent=%d resent=%d" % (now["srtt_ms"], sent, resent)
        want_rtt = int(0.8 * 2 * profile["delay"]) if profile else 0
        if now["srtt_ms"] < want_rtt:
            raise ProbeError("%s: the call link's round trip is %d ms on a path %d ms longer (not learned)"
                             % (u.name, now["srtt_ms"], 2 * profile["delay"]))
        bound = 0.4 if profile and profile.get("loss") else 0.25
        if sent and resent > sent * bound:
            raise ProbeError("%s: %d of %d Reliable packets resent (at most %d %%)" % (u.name, resent, sent, bound * 100))
    return report


class OnlyOwnRelay:
    """Two seeds (the _two_hops rows), as a context: the user in `container` cannot reach the lab hop (its
    gateway drops what goes there), only hop2 — a phone near its own relay that cannot get through to the other
    one, so it parks only on its own and the caller has to find it there. Entered before the user starts."""

    def __init__(self, container):
        self.u = argparse.Namespace(name=container, container=container)

    def __enter__(self):
        gw_net(self.u, "iptables -I FORWARD -d %s -j DROP" % LAB_HOP_IP)
        return self

    def __exit__(self, *exc):
        gw_net(self.u, "iptables -D FORWARD -d %s -j DROP; true" % LAB_HOP_IP)
        return False


def call_path_measurement(path, timing, two_hops):
    def run(lab):
        if two_hops and not lab.hop2:
            raise ProbeError("this lab has no second seed (hop2)")
        with contextlib.ExitStack() as stack:
            if two_hops:
                stack.enter_context(OnlyOwnRelay(lab.containers[1]))  # Bob's: users take the containers in turn
            # Two seeds: both know both (as every phone knows every prod relay); Bob's own comes first and is the
            # only one he can reach.
            a = lab.user("alice", seeds=[lab.hop, lab.hop2] if two_hops else None)
            b = lab.user("bob", seeds=[lab.hop2, lab.hop] if two_hops else None)
            a.wait_mesh()
            b.wait_mesh()
            a_thread = a.cmd("open_chat", account=b.account)["thread"]
            b.cmd("open_chat", account=a.account)
            a_profile, b_profile = CALL_PATHS[path]
            profiles = {a: a_profile, b: b_profile}
            if timing == "mid":
                place_voice_call(a, a_thread, b)
                stack.enter_context(Road([a, b], profiles))
            else:
                stack.enter_context(Road([a, b], profiles))
                time.sleep(CALL_TIMINGS[timing])
                place_voice_call(a, a_thread, b)
            report = check_call_links(profiles)
        leave_all([a, b])
        return report
    return run


CALL_PATH_MATRIX = [("call_path_%s_%s%s" % (path, timing, "_two_hops" if two_hops else ""),
                     call_path_measurement(path, timing, two_hops))
                    for two_hops in (False, True) for path in CALL_PATHS for timing in CALL_TIMINGS
                    if not two_hops or path == "one_far"]


def scenario_group_call_on_the_road(lab):
    """The dogfood call: Carol (a phone on the highway) and Bob (also on the road) hand over every
    10-25 s on lossy, jittery links; Bob added Carol, Carol never added Bob. Alice (owner) calls;
    everyone answers while the phones hand over, and the call goes on for 90 s. Then the handovers
    stop (the links stay lossy): everyone hears and sees everyone."""
    users, threads = open_group(lab, 4, contacts=("bob>carol",))
    a, b, c, d = users
    with Road((b, c), PHONE_ROAD, handover_s=(10, 25)) as road:
        call_id = a.cmd("call", thread=threads[a.name])["call_id"]
        for u in (b, c, d):
            u.wait_surface("ringing", 60, call_id=call_id)
        for u in (b, c, d):
            u.cmd("accept_voice")
        for u in users:
            u.wait_surface("in_call", 120, call_id=call_id)
        time.sleep(90)
        road.park()
        for u in users:
            u.wait_surface("in_call", 60, call_id=call_id)
        assert_everyone_in_call(users, call_id, timeout=60)
        handovers = dict(road.handovers)
    leave_all(users)
    return {"road_seed": road.seed, "handovers": handovers}


def scenario_group_call_weak_upload(lab):
    """Carol's phone is at the edge of coverage for the whole call of four (PHONE_WEAK: 256 kbit/s up,
    150 ms, 2 % loss in bursts): she joins and subscribes on it, and her accept reaches the others late.
    Everyone hears and sees everyone."""
    users, threads = open_group(lab, 4, contacts=("bob>carol",))
    a, b, c, d = users
    with Road((c,), PHONE_WEAK):
        call_id = a.cmd("call", thread=threads[a.name])["call_id"]
        for u in (b, c, d):
            u.wait_surface("ringing", 60, call_id=call_id)
        for u in (b, c, d):
            u.cmd("accept_voice")
        for u in users:
            u.wait_surface("in_call", 90, call_id=call_id)
        time.sleep(20)
        assert_everyone_in_call(users, call_id, timeout=60)
    leave_all(users)


SCENARIOS = [
    ("first_contact", scenario_first_contact),
    ("chat_list_settings", scenario_chat_list_settings),
    ("chat_message_actions", scenario_chat_message_actions),
    ("chat_quote_reply", scenario_chat_quote_reply),
    ("first_message_brings_keys", scenario_first_message_brings_keys),
    ("friend_shares_a_contact", scenario_friend_shares_a_contact),
    ("devices_hand_over_their_cards", scenario_devices_hand_over_their_cards),
    ("strangers_meet_by_invite", scenario_strangers_meet_by_invite),
    ("group_members_key_each_other", scenario_group_members_key_each_other),
    ("group_after_member_removed", scenario_group_after_member_removed),
    ("restored_device_is_heard", scenario_restored_device_is_heard),
    ("restored_device_hears_first", scenario_restored_device_hears_first),
    ("recovery_phrase_needs_the_pin", scenario_recovery_phrase_needs_the_pin),
    ("account_locker_round_trip", scenario_account_locker_round_trip),
    ("restore_from_phrase", scenario_restore_from_phrase),
    ("restore_from_a_saved_copy", scenario_restore_from_a_saved_copy),
    ("main_device_signs_a_device_out", scenario_main_device_signs_a_device_out),
    ("linked_device_introduces_itself", scenario_linked_device_introduces_itself),
    ("linked_devices_see_what_each_sent", scenario_linked_devices_see_what_each_sent),
    ("a_device_asks_for_a_copy_it_missed", scenario_a_device_asks_for_a_copy_it_missed),
    ("a_call_rings_every_device_and_stops_once_answered", scenario_a_call_rings_every_device_and_stops_once_answered),
    ("a_call_rings_on_every_device_after_each_called_out", scenario_a_call_rings_on_every_device_after_each_called_out),
    ("a_restored_device_hears_the_answer_to_its_call", scenario_a_restored_device_hears_the_answer_to_its_call),
    ("a_newer_device_list_follows_the_messages", scenario_a_newer_device_list_follows_the_messages),
    ("muted_group_alerts_on_a_mention", scenario_muted_group_alerts_on_a_mention),
    ("group_owner_appoints_an_admin", scenario_group_owner_appoints_an_admin),
    ("call_grows_into_a_group", scenario_call_grows_into_a_group),
    ("group_call_guest", scenario_group_call_guest),
    ("admin_removes_while_owner_is_away", scenario_admin_removes_while_owner_is_away),
    ("member_invites_while_owner_is_away", scenario_member_invites_while_owner_is_away),
    ("managed_group_only_admins_invite", scenario_managed_group_only_admins_invite),
    ("admin_readmits_a_removed_member", scenario_admin_readmits_a_removed_member),
    ("remove_everyone_they_added", scenario_remove_everyone_they_added),
    ("join_by_link", scenario_join_by_link),
    ("refused_links", scenario_refused_links),
    ("restore_signs_out_the_other_devices", scenario_restore_signs_out_the_other_devices),
    ("linked_devices_share_contact_edits", scenario_linked_devices_share_contact_edits),
    ("linked_devices_both_send", scenario_linked_devices_both_send),
    ("bystander_is_no_relay", scenario_bystander_is_no_relay),
    ("call_during_seed_churn", scenario_call_during_seed_churn),
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
    ("call_again_after_quiet", scenario_call_again_after_quiet),
    ("full_seed_parks_elsewhere", scenario_full_seed_parks_elsewhere),
    ("full_hop_hosts_the_call_elsewhere", scenario_full_hop_hosts_the_call_elsewhere),
    ("full_hop_known_before_the_call", scenario_full_hop_known_before_the_call),
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
    ("group_owner_hands_over", scenario_group_owner_hands_over),
    ("group_calling_at_once", scenario_group_calling_at_once),
    ("group_call_one_way_contact", scenario_group_call_one_way_contact),
    ("group_fourth_answers_late", scenario_group_fourth_answers_late),
    ("group_restore_checks_members", scenario_group_restore_checks_members),
    ("group_removed_member_restores", scenario_group_removed_member_restores),
    ("group_restored_owner_takes_newer_roster", scenario_group_restored_owner_takes_newer_roster),
    ("group_call_placed_by_stranger", scenario_group_call_placed_by_stranger),
    ("group_one_way_joins_late", scenario_group_one_way_joins_late),
    ("group_one_way_network_drops", scenario_group_one_way_network_drops),
    ("group_two_join_through_drops", scenario_group_two_join_through_drops),
    ("group_call_on_the_road", scenario_group_call_on_the_road),
    ("group_call_weak_upload", scenario_group_call_weak_upload),
    ("group_video_three", scenario_group_video_three),
    ("group_video_on_the_road", scenario_group_video_on_the_road),
    ("video_on_the_road", scenario_video_on_the_road),
    *VIDEO_MEASUREMENTS,
    *CALL_PATH_MATRIX,
]

# Scenarios run only when named (--only).
# The call-path matrix runs only when named, but for its two rows that guard #502's fixes on every --nat run.
CALL_PATH_GUARDS = {"call_path_far_mid", "call_path_far_start"}
OPT_IN = ({name for name, _ in VIDEO_MEASUREMENTS + CALL_PATH_MATRIX} - CALL_PATH_GUARDS) | {"call_again_after_quiet"}

# Scenarios that take the hop away: behind NATs there is then no path between users at all, so with
# --nat they run only when named.
NEEDS_DIRECT_PATH = {"seed_down", "seed_dies_in_use", "group_hop_down_mid_call"}
# Scenarios that change a user's network: they need a container per user (--nat).
NEEDS_NAT = {"network_drops_mid_call", "address_changes_mid_call", "group_member_network_drops",
             "group_one_way_network_drops", "group_two_join_through_drops", "group_call_on_the_road",
             "group_call_weak_upload", "group_video_on_the_road", "video_on_the_road",
             "bystander_is_no_relay"} | {
    name for name, _ in VIDEO_MEASUREMENTS + CALL_PATH_MATRIX}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--relay", default="http://127.0.0.1:18719/api/relay", help="lab relay base url")
    ap.add_argument("--hop", help="mesh seed multiaddr (default: the lab hop, from its /status)")
    ap.add_argument("--hop-status", default="http://127.0.0.1:18718")
    ap.add_argument("--hop2", help="the second seed's multiaddr (default: the lab's hop2, from its /status)")
    ap.add_argument("--hop2-status", default="http://127.0.0.1:18728")
    ap.add_argument("--hop2-ip", default="198.51.100.30", help="the second seed's address on the lab network")
    ap.add_argument("--hop-ip", default="198.51.100.10", help="the lab hop's address on the lab network")
    ap.add_argument("--relay-container", default="pp-app-lab-www-1",
                    help="lab relay gateway container, restarted when registrations run out")
    ap.add_argument("--hop-container", default="pp-app-lab-hop-1", help="the lab seed (seed_down stops it)")
    ap.add_argument("--nat", action="store_true",
                    help="each user in its own container behind its own NAT (docker-compose.app-lab-nat.yml)")
    ap.add_argument("--nat-containers",
                    default="pp-app-lab-user-a-1,pp-app-lab-user-b-1,pp-app-lab-user-c-1,pp-app-lab-user-d-1")
    ap.add_argument("--nat-relay", default="http://198.51.100.20:9097/api/relay",
                    help="the relay as users behind the NATs reach it")
    ap.add_argument("--nat-share", default=os.environ.get("PP_APP_LAB_NAT_SHARE", "/tmp/pp-app-lab-nat"),
                    help="profile dirs, mounted at the same path in the user containers")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--keep", action="store_true", help="keep profile dirs and logs of passing runs")
    ap.add_argument("--retry-failed", action="store_true",
                    help="run each failed scenario once more: passing then is a flake, failing again a failure")
    ap.add_argument("--summary", help="write every scenario's outcome here (JSON), for the nightly lab")
    args = ap.parse_args()
    if not args.hop:
        args.hop = seed_multiaddr(args.hop_status, args.hop_ip)
    if not args.hop2:
        try:
            args.hop2 = seed_multiaddr(args.hop2_status, args.hop2_ip)
        except OSError:
            args.hop2 = None  # a lab without the second seed: the two-hop rows fail, the rest run

    skipped = OPT_IN | (NEEDS_DIRECT_PATH if args.nat else NEEDS_NAT)
    selected = [(n, f) for n, f in SCENARIOS if n in args.only or (not args.only and n not in skipped)]
    if not selected:
        sys.exit("no scenario matches %s" % args.only)
    results = []
    for round_no in range(args.repeat):
        for name, fn in selected:
            results.append(run_scenario(args, name, fn))
    if args.retry_failed:
        for result in [r for r in results if r["result"] == "fail"]:
            print("RETRY %s" % result["name"])
            again = run_scenario(args, result["name"], dict(SCENARIOS)[result["name"]])
            if again["result"] == "pass":
                result.update(result="flaky", retry=again)
            else:
                result["retry"] = again
    if args.summary:
        with open(args.summary, "w") as f:
            json.dump({"nat": bool(args.nat), "results": results}, f, indent=2)
    sys.exit(1 if any(r["result"] == "fail" for r in results) else 0)


def seed_multiaddr(status_url, ip):
    import urllib.request
    with urllib.request.urlopen(status_url + "/status", timeout=5) as r:
        peer = json.load(r)["peer_id"]
    return "/ip4/%s/udp/18517/adp/1.0.0/p2p/%s" % (ip, peer)


def run_scenario(args, name, fn):
    """One scenario on a fresh lab: PASS / FAIL on stdout, and its outcome for the summary."""
    lab = Lab(args)
    started = time.time()
    ok = False
    outcome = {"name": name, "result": "fail", "seconds": 0.0, "detail": "", "extra": {}}
    try:
        extra = fn(lab) or {}
        ok = True
        outcome.update(result="pass", extra=extra)
        print("PASS %-30s %5.1fs %s" % (name, time.time() - started, json.dumps(extra) if extra else ""))
    except Exception as e:  # noqa: BLE001 — report every way a scenario can go wrong
        outcome["detail"] = str(e).splitlines()[0][:300] if str(e) else type(e).__name__
        print("FAIL %-30s %5.1fs %s" % (name, time.time() - started, e))
        if not isinstance(e, ProbeError):
            traceback.print_exc()
        for u in lab.users:
            tail = [m for _, m in u.history[-12:]]
            print("  %s surface=%s" % (u.name, u.surface))
            for m in tail:
                print("    %s" % json.dumps(m))
        print("  profiles and logs kept in %s" % lab.root)
        outcome["kept"] = lab.root
    finally:
        lab.close(ok)
        outcome["seconds"] = round(time.time() - started, 1)
    sys.stdout.flush()
    return outcome


if __name__ == "__main__":
    main()
