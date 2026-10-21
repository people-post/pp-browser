#!/usr/bin/env python3
"""App lab: scripted users on the real app stack (pp-app-probe) over the real relay.

Each user is one pp-app-probe process (its own profile dir) talking to the lab relay
(packaging/pp-node/docker-compose.app-lab.yml) with the local hop as its only mesh seed. Scenarios
press the same buttons people press, the way they press them when a call will not get through:
both calling at once, calling again and again, an app restart while ringing. Every scenario ends
by checking nothing is left stuck and the call history says what happened.

  scripts/test/pp_app_lab.py --probe build/src/app/node/pp-app-probe [--only first_contact]

(`pp_local_test.sh run --suite app` brings the lab up and runs this.)
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

    def __init__(self, lab, name):
        self.lab = lab
        self.name = name
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
        self.proc = subprocess.Popen(
            [self.lab.probe, "--config", self.config, "--pin", PIN, "--log-file", log],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1)
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
            self.proc.kill()
            raise ProbeError("%s: quit hung (killed)" % self.name)

    def crash(self):
        """The app dies with no goodbye (force-quit, OOM kill)."""
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
        self.root = tempfile.mkdtemp(prefix="pp-app-lab-")
        self.keep = args.keep
        self.relay_container = args.relay_container
        self.hop_container = args.hop_container
        self.hop_status = args.hop_status
        self.users = []

    def config_for(self, data_dir):
        return {
            "config_version": 1,
            "data_dir": data_dir,
            "relay": {"base_url": self.relay},
            "directory": {"base_url": self.relay},
            "registration": {"base_url": self.relay},
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
        u = User(self, "%s-%s" % (name, uuid.uuid4().hex[:6]))
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
        if ok and not self.keep:
            shutil.rmtree(self.root, ignore_errors=True)
        self.users = []


# --- outcome checks -------------------------------------------------------------------------------

def assert_idle(*users, timeout=30):
    """Nobody is left ringing or in a call."""
    for u in users:
        u.wait_surface("none", timeout)
        s = u.state()
        if s.get("phase") not in ("Idle", None, ""):
            raise ProbeError("%s: phase %s after the call" % (u.name, s.get("phase")))


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


def scenario_seed_down(lab):
    """The relay seed goes away (crashed, unreachable) after both are online: the call still
    connects directly, and the answer does not sit out the whole seed-park wait."""
    a, a_thread, b, b_thread = open_pair(lab)
    subprocess.run(["docker", "stop", "-t", "1", lab.hop_container], check=True, stdout=subprocess.DEVNULL)
    try:
        _, took = connect(a, a_thread, b, timeout=40)
        a.cmd("leave")
        assert_idle(a, b)
    finally:
        subprocess.run(["docker", "start", lab.hop_container], check=True, stdout=subprocess.DEVNULL)
        lab.wait_hop()
    return took


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
    ("seed_down", scenario_seed_down),
]

# Run only when named (--only): seed_down documents a known gap — with the seed silently gone, the
# answer still waits out the whole seed park (call_accept waits on it, ≤ 12 s).
OPT_IN = {"seed_down"}


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
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--keep", action="store_true", help="keep profile dirs and logs of passing runs")
    args = ap.parse_args()
    if not args.hop:
        import urllib.request
        with urllib.request.urlopen(args.hop_status + "/status", timeout=5) as r:
            peer = json.load(r)["peer_id"]
        args.hop = "/ip4/%s/udp/18517/adp/1.0.0/p2p/%s" % (args.hop_ip, peer)

    selected = [(n, f) for n, f in SCENARIOS if n in args.only or (not args.only and n not in OPT_IN)]
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
