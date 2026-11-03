#!/usr/bin/env python3
"""theta_node.py against a fake Theta API, a fake GitHub API and a fake SSH.

Each group runs on its own: an exception is that group's failure, not the file's.

  turning on: each leg on its own machine, by name of the run, its runner online; any
    failure is an error with that node off, never another machine or another runner;
    a machine taken, by someone else or by a run still alive, fails clearly, per machine;
  turning off: by name, seen gone, each whatever happens to the other; with --if-legs-done
    only a leg's node whose job GitHub says has finished;
  the watch: both legs at once; GitHub's blips, the list's false alarms, a machine and a job
    as two witnesses in either order; a lost machine leaves the other leg to finish, then one
    re-launch by value, nodes off first and cancel last, never two, never of a run someone
    cancelled, never after a red leg, never of other code;
  the sweeper: only our names, and each machine freed behind a watch that died;
  secrets: nothing reaches a message;
  the runner on the node: from an empty environment, killing any other run's job at its
    first step (Linux), which the watch turns into a failed run;
  exposure: a node's address in no message, its deployment id in no output, and no
    configuration on a connection not held to the host key the bootstrap made.

    python3 scripts/ci/test_theta_node.py
"""
from __future__ import annotations

import argparse
import contextlib
import http.client
import io
import json
import os
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import urllib.error
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import theta_node as t  # noqa: E402

KEY, PROJECT = "SECRET-KEY-123", "prj_secret456"
L, W = "linux-x86_64", "win-x86_64"
ROW_L = {"BaseID": "dpl_l", "PodPhase": "ci5a1l-deployment-x: Running", "PortMappings": {"22": 30001},
         "NodePublicIP": "1.1.1.1", "AuthPassword": "PASSWORD-789"}
ROW_W = {"BaseID": "dpl_w", "PodPhase": "ci5a1w-deployment-x: Running", "PortMappings": {"22": 30002},
         "NodePublicIP": "2.2.2.2", "AuthPassword": "PASSWORD-789"}
LINUX, WIN = "build-" + L, "build-" + W
HEAD = "c0ffee"
HOST_KEY = "ssh-ed25519 AAAAHOSTKEY"
BOOTSTRAPPED = "== the user\nnode_bootstrap: host key: %s\nnode_bootstrap: ready\n" % HOST_KEY
START_RUNNER = "bash /usr/local/sbin/node_runner.sh 5"
PUT_RUNNER = "cat > /usr/local/sbin/node_runner.sh"


def listed(marks="lw", status="running", run=5, attempt=1):
    return [{"id": "dpl_%s" % m, "name": "ci%sa%s%s" % (run, attempt, m), "status": status} for m in marks]


class FakeTheta:
    def __init__(self, balance=10.5, create_status=201, create_lost=False, rows=(ROW_L, ROW_W), delete_takes=True, log=None):
        self.balance, self.create_status, self.create_lost = balance, create_status, create_lost
        self.rows, self.delete_takes = list(rows), delete_takes
        self.nodes, self.created, self.deleted = [], [], []
        self.log = log if log is not None else []
        self.list_status = 200
        self.outage = None  # a function: true while everything answers 503
        self.list_body = None  # what the list of nodes answers instead, as raw bytes
        self.resources = list(t.MACHINES.values())  # who gpu-resources lists, or "down"

    def __call__(self, method, url, headers, body):
        assert headers["x-api-key"] == KEY
        path = url.split("controller.thetaedgecloud.com", 1)[1]
        if self.outage and self.outage():
            return 503, b"busy"
        if path.endswith("/gpu-resources"):
            if self.resources == "down":
                return 503, b"busy"
            return 200, json.dumps({"body": {"resources": [{"resource_id": r} for r in self.resources]}}).encode()
        if path.endswith("/billing/balance"):
            return 200, json.dumps({"body": {"balance_usd": self.balance}}).encode()
        if path.startswith("/deployments/list"):
            return self.list_status, json.dumps({"body": self.rows}).encode()
        if path.endswith("/gpu-deployments") and method == "GET" and self.list_body is not None:
            return 200, self.list_body
        if path.endswith("/gpu-deployments") and method == "GET":
            return self.list_status, json.dumps({"body": {"deployments": self.nodes}}).encode()
        if path.endswith("/gpu-deployments") and method == "POST":
            sent = json.loads(body)
            self.created.append(sent)
            node_id = "dpl_" + sent["name"][-1]
            if self.create_lost:
                self.nodes.append({"id": node_id, "name": sent["name"], "status": "unknown"})
                raise OSError("connection reset")
            if self.create_status != 201:
                return self.create_status, b'{"message":"GPU resource not found"}'
            self.nodes.append({"id": node_id, "name": sent["name"], "status": "running", "machine_type": sent["resource_id"]})
            return 201, json.dumps({"body": {"deployment": {"id": node_id}}}).encode()
        if method == "DELETE":
            node_id = path.rsplit("/", 1)[1]
            self.deleted.append(node_id)
            self.log.append("delete " + node_id)
            if self.delete_takes:
                self.nodes = [n for n in self.nodes if n["id"] != node_id]
            return 200, b"{}"
        raise AssertionError("unexpected %s %s" % (method, path))


class FakeGitHub:
    def __init__(self, jit_status=201, online=True, log=None):
        self.jit_status, self.online = jit_status, online
        self.jits, self.dispatches, self.cancels, self.removed = [], [], [], []
        self.jobs_answers, self.run_status, self.runners, self.past_runs = [], {}, [], []
        self.log = log if log is not None else []
        self.head, self.run_conclusion, self.on_head, self.on_dispatch = HEAD, None, None, None
        self.busy_ids = ()

    def __call__(self, method, url, headers, body):
        path = url.split("api.github.com", 1)[1]
        if "/git/ref/heads/" in path:
            if self.on_head:
                self.on_head()
            if self.head == "down":
                return 503, b"busy"
            if self.head is None:
                return 404, b'{"message":"Not Found"}'
            return 200, json.dumps({"object": {"sha": self.head}}).encode()
        if path.endswith("generate-jitconfig"):
            sent = json.loads(body)
            self.jits.append(sent)
            self.log.append("register")
            if self.jit_status != 201:
                return self.jit_status, b'{"message":"Bad credentials"}'
            labels = [{"name": l} for l in sent["labels"]]
            self.runners.append({"id": 100 + len(self.runners), "name": sent["name"], "busy": False,
                                 "status": "online" if self.online else "offline", "labels": labels})
            return 201, json.dumps({"encoded_jit_config": "JIT%d" % len(self.jits), "runner": {"labels": labels}}).encode()
        if "/actions/runners?" in path:
            return 200, json.dumps({"runners": self.runners + [
                {"id": 8, "name": "other", "busy": False, "labels": [{"name": "self-hosted"}]}]}).encode()
        if method == "DELETE" and "/actions/runners/" in path:
            removed = path.rsplit("/", 1)[1]
            if removed in self.busy_ids:
                return 422, b'{"message":"Runner is currently running a job and cannot be deleted."}'
            self.removed.append(removed)
            self.runners = [r for r in self.runners if str(r["id"]) != removed]
            return 204, b""
        if "/jobs" in path:
            answer = self.jobs_answers.pop(0) if len(self.jobs_answers) > 1 else self.jobs_answers[0]
            if answer == "down":
                return 503, b"busy"
            if answer == "garbage":
                return 200, b"<html>"
            return 200, json.dumps({"jobs": answer}).encode()
        if path.endswith("/dispatches"):
            sent = json.loads(body)
            if self.on_dispatch:
                answer = self.on_dispatch(sent)
                if answer:
                    return answer
            self.dispatches.append(sent)
            self.log.append("dispatch")
            self.past_runs.append({"id": 77, "html_url": "https://github.com/o/r/actions/runs/77",
                                   "display_title": t.relaunch_title(sent["inputs"].get("relaunch_of"))})
            return 204, b""
        if path.endswith("/cancel"):
            self.cancels.append(path)
            self.log.append("cancel")
            return 202, b""
        if "/workflows/release.yml/runs?" in path:
            return 200, json.dumps({"workflow_runs": self.past_runs}).encode()
        if "/actions/runs/" in path:
            run = path.rsplit("/", 1)[1]
            status = self.run_status.get(run)
            if status == 500:
                return 500, b"oops"
            if status is None:
                return 404, b"{}"
            return 200, json.dumps({"status": status, "conclusion": self.run_conclusion}).encode()
        raise AssertionError("unexpected %s %s" % (method, path))


class FakeSsh:
    def __init__(self, answers=None):
        self.calls, self.answers = [], {"bash -s": (0, BOOTSTRAPPED, ""), **(answers or {})}

    def pin(self, host_key):
        self.calls.append(("pin", host_key))
        return self

    def __call__(self, command, stdin=None, timeout=60):
        self.calls.append((command, stdin))
        return self.answers.get(command, (0, "ready", ""))


class Runaway(BaseException):
    """A loop that never ends, stopped by the clock it sleeps on. Not an Exception: the
    watch counts those as blind polls."""


class Clock:
    # Far past anything that ends: without it a loop that never ends is a hung test, not a failed one.
    LIMIT_S = 3 * 24 * 3600

    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds
        if self.now > self.LIMIT_S:
            raise Runaway("still looping after %d simulated seconds" % self.now)


def quiet(fn, *args, **kwargs):
    """fn's result, and what it said to stderr."""
    sink = io.StringIO()
    with contextlib.redirect_stderr(sink):
        return fn(*args, **kwargs), sink.getvalue()


def provisioned(api, gh, ssh, leg=L):
    """provision() of one leg: (True or None, error text)."""
    clock = Clock()
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")
        try:
            quiet(t.provision, theta, github, 5, 1, leg, lambda host, port: ssh, lambda: key, clock, clock.sleep)
            return True, ""
        except t.ThetaError as exc:
            return None, str(exc)


def job(name, status="completed", conclusion="success", steps=(), created="2026-11-03T10:00:00Z", completed=None, runner=None):
    return {"name": name, "status": status, "conclusion": conclusion, "labels": ["theta-5-1-" + name[len("build-"):]],
            "created_at": created, "completed_at": completed, "runner_name": runner, "steps": job_steps(steps)}


def job_steps(conclusions) -> list:
    """As GitHub lists them: a step with no conclusion yet is the one in progress, the rest pending."""
    steps, running = [], False
    for conclusion in conclusions:
        status = "completed" if conclusion else ("pending" if running else "in_progress")
        running = running or conclusion is None
        steps.append({"name": "s", "status": status, "conclusion": conclusion})
    return steps


NOW = t.parse_time("2026-11-03T12:00:00Z")
RUN_L = job(LINUX, status="in_progress", conclusion=None, steps=("success",), runner="ci5a1l")
RUN_W = job(WIN, status="in_progress", conclusion=None, steps=("success",), runner="ci5a1w")
QUEUED_L = job(LINUX, status="queued", conclusion=None, created="2026-11-03T11:59:00Z")
QUEUED_W = job(WIN, status="queued", conclusion=None, created="2026-11-03T11:59:00Z")
GREEN_L = job(LINUX, completed="2026-11-03T11:30:00Z", steps=("success",), runner="ci5a1l")
GREEN_W = job(WIN, completed="2026-11-03T11:50:00Z", steps=("success",), runner="ci5a1w")
RED_L = job(LINUX, conclusion="failure", steps=("success", "failure", "skipped"), completed="2026-11-03T11:59:00Z", runner="ci5a1l")
RED_W = job(WIN, conclusion="failure", steps=("success", "failure", "skipped"), completed="2026-11-03T11:59:00Z", runner="ci5a1w")
# A dead runner, as measured: no cancel, the job fails about 9.5 minutes later with its steps left as they were.
DEAD_L = job(LINUX, conclusion="failure", steps=("success", None, None), completed="2026-11-03T11:59:00Z", runner="ci5a1l")
DEAD_W = job(WIN, conclusion="failure", steps=("success", None, None), completed="2026-11-03T11:59:00Z", runner="ci5a1w")
# The same with a cancel: cancelled at the cancel + 5:00, the same steps. A person's, whatever the machine.
CANCELLED_W = job(WIN, conclusion="cancelled", steps=("success", None, None), completed="2026-11-03T11:59:00Z", runner="ci5a1w")
QUEUE_CANCELLED_W = job(WIN, conclusion="cancelled")
INPUTS = {"kind": "candidate", "compile_cache": "gha", "core_ref": "main", "wrapper_ref": "v1", "relaunch_of": "5"}
LOSE_L = {"l": False, "w": True}
LOSE_W = {"l": True, "w": False}


def watched(answers, nodes="lw", status="running", answering=True, relaunch_of="", limit=6 * 3600, past_runs=(), gh=None,
            rows=(ROW_L, ROW_W), api=None, sha=HEAD, probe=None):
    """watch() to its verdict, on a clock that only moves when it sleeps. answering: is sshd
    there: a bool, a function of the seconds since the start, or either by leg's mark.
    (verdict, FakeGitHub, FakeTheta, the log of deletes and dispatches, all it said, clock)"""
    clock, log = Clock(), []
    api, gh = api or FakeTheta(rows=rows), gh or FakeGitHub()
    api.log, gh.log = log, log
    api.nodes = listed(nodes, status)
    gh.jobs_answers = answers
    gh.past_runs = list(past_runs) or gh.past_runs
    theta, github = t.Theta(PROJECT, KEY, api, clock.sleep), t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)

    def answers_at(host, port):
        said = answering[{"1.1.1.1": "l", "2.2.2.2": "w"}[host]] if isinstance(answering, dict) else answering
        return said(clock.now) if callable(said) else said
    result, said = quiet(t.watch, theta, github, 5, 1, "main", INPUTS, relaunch_of, probe or answers_at,
                         lambda: NOW + clock.now, clock.sleep, 60, limit, sha=sha)
    return result, gh, api, log, said, clock


def group_turning_on():
    out = []
    out.append(("machines: each leg on its own fixed PC, the two the design names, and no third",
                t.MACHINES == {L: "community_18a9f767a3e940349d2a634e173598f2", W: "community_921db380bf634e279a706d1d1311c129"}
                and sorted(t.MACHINES) == sorted(t.node_legs()) and len(set(t.MACHINES.values())) == 2))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("machines: each named in one place, and no argument names one", all(source.count(m) == 1 for m in t.MACHINES.values()) and "--machine" not in source))
    out.append(("names: a node's name says its run, its attempt and its leg, so its machine",
                {t.node_name(5, 1, leg) for leg in t.node_legs()} == {"ci5a1l", "ci5a1w"} and t.leg_of("w") == W and t.leg_of("l") == L
                and t.NAME.match("ci5a1l").groups() == ("5", "1", "l")))
    for leg in (L, W):
        api, gh, ssh = FakeTheta(), FakeGitHub(), FakeSsh()
        got, error = provisioned(api, gh, ssh, leg)
        sent = api.created[0] if api.created else {}
        out.append(("on, %s: its own machine, under the run's name, with this run's key and a price ceiling" % leg,
                    got and [c["resource_id"] for c in api.created] == [t.MACHINES[leg]] and sent.get("name") == t.node_name(5, 1, leg)
                    and sent.get("ssh_public_key") == "ssh-ed25519 AAAA ci-node" and sent.get("ssh_port") == 22
                    and sent.get("container_image") == t.IMAGE and sent.get("max_price_per_hour_usd") == t.MAX_PRICE_USD_HOUR))
        out.append(("on, %s: one runner, its own label, online" % leg,
                    [j["name"] for j in gh.jits] == [t.node_name(5, 1, leg)] and gh.jits[0]["labels"] == [t.leg_label(5, 1, leg)]
                    and gh.jits[0]["work_folder"] == "../work"))
        commands = [c for c, _ in ssh.calls]
        stdin = dict(ssh.calls)
        out.append(("on, %s: prepared before the runner starts, the run named, and the configuration goes by stdin, never on a command line" % leg,
                    commands.index("bash -s") < commands.index(START_RUNNER)
                    and b"node_bootstrap" in stdin["bash -s"] and b"\r" not in stdin["bash -s"]
                    and stdin[START_RUNNER] == b"JIT1\n" and not any("JIT" in c for c in commands)))
        out.append(("on, %s: a node that went well stays on" % leg, api.deleted == []))
    out.append(("prices: the ceiling is above both machines' price", t.MAX_PRICE_USD_HOUR >= 0.182))
    got, error = provisioned(FakeTheta(), FakeGitHub(), FakeSsh({"true": (255, "", "root@<node>: Permission denied (publickey).")}))
    calls = []

    def route(argv, **kw):
        calls.append(argv)
        return SimpleNamespace(returncode=0, stdout="9.9.9.9 via 10.1.0.1 dev eth0 src 10.1.0.11 uid 1001", stderr="")
    t.clamp_path("9.9.9.9", route)
    out.append(("path: the node's address alone goes through small packets, by the same gateway",
                calls[-1] == ["sudo", "-n", "ip", "route", "replace", "9.9.9.9/32", "via", "10.1.0.1", "dev", "eth0", "mtu", "lock", "1200"]))

    def refused(argv, **kw):
        return SimpleNamespace(returncode=1 if "sudo" in argv else 0, stdout="9.9.9.9 dev eth0 src 10.1.0.11", stderr="9.9.9.9 refused")
    try:
        t.clamp_path("9.9.9.9", refused)
        said = ""
    except t.ThetaError as exc:
        said = str(exc)
    out.append(("path: refused, the node's connection stops there, and its address stays out of the error", "size the path" in said and "9.9.9.9" not in said))
    seen = []
    made = t.node_ssh(Path("work"), seen.append)("9.9.9.9", 22)
    out.append(("path: every connection of provision goes through it", seen == ["9.9.9.9"] and isinstance(made, t.Ssh)))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    body = source[source.index("def run_provision"):source.index("def run_watch")]
    out.append(("path: provision takes its connections from there", "node_ssh(work)" in body and "Ssh(" not in body))
    out.append(("up: a node that never answers says why, with ssh's own words",
                not got and "Permission denied (publickey)" in error and "ssh exited 255" in error))
    return out


def group_no_fallback():
    out = []
    api, gh = FakeTheta(create_status=409), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: a refusal (the price, say) is final: an error, nothing on", got is None and "409" in error and api.nodes == [] and gh.jits == [] and len(api.created) == 1))
    for leg in (L, W):
        api, gh = FakeTheta(create_status=404), FakeGitHub()
        api.resources = [m for m in t.MACHINES.values() if m != t.MACHINES[leg]]
        got, error = provisioned(api, gh, FakeSsh(), leg)
        out.append(("fail, %s: its machine is taken and the other is free: asked again for about 6 minutes, then a message naming the leg and its machine, never the other machine" % leg,
                    got is None and "taken" in error and leg in error and t.MACHINES[leg] in error and api.nodes == [] and gh.jits == []
                    and {c["resource_id"] for c in api.created} == {t.MACHINES[leg]} and len(api.created) == 1 + t.FREE_LIMIT_S // 30 and 300 <= t.FREE_LIMIT_S <= 420))

    class FreedLater(FakeTheta):
        def __call__(self, method, url, headers, body):
            if method == "POST" and len(self.created) < 8:
                self.created.append(json.loads(body))
                return 404, b'{"message":"GPU resource not found"}'
            return super().__call__(method, url, headers, body)
    api = FreedLater()
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("on: a machine we just freed (3m40 measured), still 404, is waited for", got and len(api.created) == 9 and 8 * 30 >= 220))
    api = FakeTheta(balance=2.5)
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: the balance would not cover both nodes at their age limit: an error, nothing created",
                got is None and "balance" in error and api.created == [] and t.MIN_BALANCE_USD >= t.GUARD_HOURS * (0.156 + 0.182)))
    api = FakeTheta(create_lost=True)
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: the answer to the create is lost: the node it may have made is deleted", got is None and api.deleted == ["dpl_l"]))
    api = FakeTheta(rows=())
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: a node never Running within the limit is deleted", got is None and api.deleted == ["dpl_l"] and api.nodes == []))
    api = FakeTheta()
    got, error = provisioned(api, FakeGitHub(), FakeSsh({"true": (255, "", "refused")}))
    out.append(("fail: no SSH within the limit: deleted", got is None and api.deleted == ["dpl_l"]))
    api, gh = FakeTheta(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({"bash -s": (1, "", "node_bootstrap: ERROR: missing: git")}))
    out.append(("fail: a bootstrap that refuses: deleted, and nothing registered", got is None and "missing: git" in error and api.deleted == ["dpl_l"] and gh.jits == []))
    api, gh = FakeTheta(), FakeGitHub(jit_status=401)
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: GitHub refuses the registration: deleted", got is None and api.deleted == ["dpl_l"]))
    api, gh = FakeTheta(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({START_RUNNER: (1, "", "no")}))
    out.append(("fail: the runner does not start: deleted, and the registered one removed",
                got is None and api.deleted == ["dpl_l"] and gh.removed == ["100"]))
    api, gh = FakeTheta(), FakeGitHub(online=False)
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: a runner that never comes online fails the run within the limit, it does not wait 24 hours in the queue",
                got is None and "online" in error and api.deleted == ["dpl_l"] and t.RUNNER_ONLINE_LIMIT_S <= 600))
    api, gh = FakeTheta(), FakeGitHub()
    api.nodes = listed("w")
    gh.runners = [{"id": 90, "name": "ci5a1w", "busy": False, "status": "online", "labels": [{"name": t.leg_label(5, 1, W)}]}]
    got, error = provisioned(api, gh, FakeSsh({START_RUNNER: (1, "", "no")}), L)
    out.append(("fail: a leg that fails leaves the other's node and runner alone: stop-node takes that one",
                got is None and api.deleted == ["dpl_l"] and [n["id"] for n in api.nodes] == ["dpl_w"] and "90" not in gh.removed))
    # The command line: exit 1, and nothing a job could read.
    for var in ("THETA_API_KEY", "THETA_PROJECT"):
        os.environ.pop(var, None)
    sink = io.StringIO()
    with contextlib.redirect_stdout(sink):
        code, said = quiet(t.main, ["provision", "--run", "5", "--attempt", "1", "--leg", W])
    out.append(("fail: the command exits 1 with an error for the run's page, naming the leg", code == 1 and sink.getvalue() == "" and "::error::" in said and W in said))
    try:
        quiet(t.main, ["provision", "--run", "5", "--attempt", "1", "--leg", "linux-arm64"])
        refused = False
    except SystemExit as exc:
        refused = exc.code != 0
    out.append(("no fallback: the arm64 leg has no node, and the command will not make one", refused))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("no fallback: nothing in the script names a builder or sends a leg to GitHub's runners",
                "builder" not in source.replace("test_release_builder", "") and "provision_outputs" not in source and "GitHub's runners, nothing" not in source))
    return out


def group_two_runs():
    """One machine, one run, per machine."""
    out = []

    def taken(nodes, run_status, leg=L):
        api, gh = FakeTheta(), FakeGitHub()
        api.nodes = list(nodes)
        gh.run_status = run_status
        got, error = provisioned(api, gh, FakeSsh(), leg)
        return got, error, api, gh
    live = {"id": "d9", "name": "ci9a1l", "status": "running"}
    got, error, api, gh = taken([live], {"9": "in_progress"})
    out.append(("two runs: a live run's node on this machine refuses this leg at once, naming it, with nothing created",
                got is None and "run 9" in error and "ci9a1l" in error and t.MACHINES[L] in error and api.created == [] and api.deleted == []))
    got, error, api, gh = taken([live], {"9": "in_progress"}, W)
    out.append(("two runs: on the other machine it does not refuse this leg, and is not touched", got and api.deleted == []))
    got, error, api, gh = taken([live], {"9": "completed"})
    out.append(("two runs: a node on this machine whose run has finished goes, and the machine is waited for", got and api.deleted == ["d9"]))
    got, error, api, gh = taken([live], {"9": "completed"}, W)
    out.append(("two runs: on the other machine it is the other provision's", got and api.deleted == []))
    got, error, api, gh = taken(listed("lw", attempt=0), {"5": "in_progress"})
    out.append(("two runs: an earlier attempt of this same run goes, both its nodes", got and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    got, error, api, gh = taken(listed("w"), {"5": "in_progress"})
    out.append(("same run: the other leg's node of this attempt is left alone", got and api.deleted == []))
    for leg, mark in ((L, "l"), (W, "w")):
        by_hand = {"id": "dh", "name": "gpunodegn8rdcmn6agw1", "status": "running", "machine_type": t.MACHINES[leg]}
        got, error, api, gh = taken([by_hand], {}, leg)
        out.append(("taken, %s: a node of this project made by hand on its machine fails it at once, by name, untouched" % leg,
                    got is None and "gpunodegn8rdcmn6agw1" in error and t.MACHINES[leg] in error and api.deleted == [] and api.created == []))
        other_leg = W if leg == L else L
        got, error, api, gh = taken([by_hand], {}, other_leg)
        out.append(("taken, %s: on the other machine it is other work" % leg, got and api.deleted == []))
    other = {"id": "dh", "name": "cihandbuild1", "status": "running", "machine_type": "community_other"}
    got, error, api, gh = taken([other], {})
    out.append(("taken: a node on a third machine is other work, even with a name that starts with ci", got and api.deleted == []))
    return out


def run_off_with(answers, if_legs_done=True, nodes="lw", api=None, runners=()):
    """The off command: (exit code, FakeTheta, FakeGitHub, stdout, stderr)."""
    api, gh, clock = api or FakeTheta(), FakeGitHub(), Clock()
    api.nodes = listed(nodes)
    gh.jobs_answers = answers
    gh.runners = list(runners)
    saved = t.from_env, t.github_from_env, t.turn_off
    real_turn_off = t.turn_off
    t.from_env = lambda: t.Theta(PROJECT, KEY, api, clock.sleep)
    t.github_from_env = lambda: t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    t.turn_off = lambda theta, names: real_turn_off(theta, names, clock, clock.sleep)
    before = os.environ.get("RUNNER_ADMIN_TOKEN")
    os.environ["RUNNER_ADMIN_TOKEN"] = "ADMIN"
    printed = io.StringIO()
    try:
        with contextlib.redirect_stdout(printed):
            code, said = quiet(t.run_off, argparse.Namespace(run="5", attempt="1", if_legs_done=if_legs_done))
    finally:
        t.from_env, t.github_from_env, t.turn_off = saved
        os.environ.pop("RUNNER_ADMIN_TOKEN") if before is None else os.environ.__setitem__("RUNNER_ADMIN_TOKEN", before)
    return code, api, gh, printed.getvalue(), said


def group_off():
    out = []
    clock = Clock()
    api = FakeTheta()
    api.nodes = listed("lw") + [{"id": "dpl_2", "name": "ci5a2l", "status": "running"}, {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "status": "running"}]
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    deleted = t.turn_off(theta, t.node_names(5, 1), clock, clock.sleep)
    out.append(("off: both of this attempt's nodes and nothing else, by name", sorted(deleted) == ["dpl_l", "dpl_w"] and sorted(n["id"] for n in api.nodes) == ["dpl_2", "dpl_lab"]))
    out.append(("off: nothing on is not an error", t.turn_off(t.Theta(PROJECT, KEY, FakeTheta(), clock.sleep), t.node_names(5, 1), clock, clock.sleep) == []))

    class Refused(FakeGitHub):
        def __call__(self, method, url, headers, body):
            return 401, b'{"message":"Bad credentials"}'
    got, _ = quiet(t.remove_runners, t.GitHub("o/r", "T", "BAD", Refused(), clock.sleep), [t.leg_label(5, 1, L)])
    out.append(("off: runners GitHub will not list are a warning, never a failed delete", got == []))

    code, api, _, _, _ = run_off_with([[RUN_L, RUN_W]])
    out.append(("off after a watch that crashed: legs still running, the healthy nodes stay", code == 0 and api.deleted == []))
    code, api, _, _, _ = run_off_with(["down"])
    out.append(("off after a watch that crashed: GitHub does not answer, which is not a yes", code == 0 and api.deleted == []))
    code, api, _, _, _ = run_off_with(["garbage"])
    out.append(("off after a watch that crashed: nor is an answer that cannot be read", code == 0 and api.deleted == []))
    code, api, _, _, said = run_off_with([[GREEN_L]])
    out.append(("off: a leg not listed yet is not a finished leg, and each is asked for itself", code == 0 and api.deleted == ["dpl_l"] and W in said))
    code, api, _, _, said = run_off_with([[GREEN_L, RUN_W]])
    out.append(("off: one leg finished, the other running: its node goes, the other's stays", code == 0 and api.deleted == ["dpl_l"] and W in said and L not in said))
    code, api, _, _, _ = run_off_with([[GREEN_L, GREEN_W]])
    out.append(("off: both legs have finished: both deleted", code == 0 and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    code, api, _, _, _ = run_off_with([[RUN_L, RUN_W]], if_legs_done=False)
    out.append(("off without the option (a node job did not finish): both deleted, asking nothing", code == 0 and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    runners = [{"id": 90, "name": "ci5a1l", "busy": False, "labels": [{"name": t.leg_label(5, 1, L)}]},
               {"id": 91, "name": "ci5a1w", "busy": False, "labels": [{"name": t.leg_label(5, 1, W)}]},
               {"id": 92, "name": "ci6a1l", "busy": False, "labels": [{"name": t.leg_label(6, 1, L)}]}]
    code, api, gh, printed, _ = run_off_with([[GREEN_L, RUN_W]], runners=runners)
    out.append(("off: only the runner of the leg that went, and never another run's", gh.removed == ["90"] and "ci5a1l" in printed))
    return out


def group_stop_node():
    """Both nodes off, each whatever happens to the other, for as long as the step can try."""
    out = []
    clock = Clock()
    gh = FakeGitHub()
    labels = [t.leg_label(5, 1, W), t.leg_label(5, 1, L)]
    gh.runners = [{"id": 50, "name": "dead", "busy": False, "labels": [{"name": labels[0]}]},
                  {"id": 51, "name": "unused", "busy": False, "labels": [{"name": labels[1]}]}]
    gh.busy_ids = ("50",)
    removed = t.GitHub("o/r", "T", "A", gh, clock.sleep).remove_runners(labels)
    out.append(("stop: a runner GitHub says is still on a job (422) fails nothing, and the next one still goes", removed == ["unused"] and gh.removed == ["51"]))

    class SecondDelete(FakeTheta):
        def __call__(self, method, url, headers, body):
            if method == "DELETE":
                self.deleted.append("x")
                self.nodes = []
                return 404, b'{"message":"GPU deployment not found"}'
            return super().__call__(method, url, headers, body)
    api = SecondDelete()
    api.nodes = listed("l")
    out.append(("stop: a delete that answers 404 is a node gone", t.turn_off(t.Theta(PROJECT, KEY, api, clock.sleep), ["ci5a1l"], clock, clock.sleep) == ["dpl_l"]))

    def off():
        clock = Clock()
        api = FakeTheta()
        api.nodes = listed("lw")
        return clock, api

    clock, api = off()
    api.outage = lambda: clock.now < 300
    deleted, _ = quiet(t.turn_off, t.Theta(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
    out.append(("stop: Theta fails for 5 minutes, list and delete: both still go", sorted(deleted) == ["dpl_l", "dpl_w"] and api.nodes == []))

    class DeleteFails(FakeTheta):
        failing = "dpl_l"
        until = 300

        def __call__(self, method, url, headers, body):
            if method == "DELETE" and url.endswith(self.failing) and clock.now < self.until:
                return 502, b"bad"
            return super().__call__(method, url, headers, body)
    clock = Clock()
    api = DeleteFails()
    api.nodes = listed("lw")
    deleted, _ = quiet(t.turn_off, t.Theta(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
    out.append(("stop: one node's delete fails for 5 minutes: the other goes at once, and that one in the end",
                deleted[0] == "dpl_w" and sorted(deleted) == ["dpl_l", "dpl_w"] and api.nodes == []))
    for failing, stays, gone in (("dpl_l", "ci5a1l", "ci5a1w"), ("dpl_w", "ci5a1w", "ci5a1l")):
        clock = Clock()
        api = DeleteFails()
        api.failing, api.until = failing, 10 ** 9
        api.nodes = listed("lw")
        try:
            quiet(t.turn_off, t.Theta(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
            message = ""
        except t.ThetaError as exc:
            message = str(exc)
        out.append(("stop: %s can never be deleted: the other is gone all the same, and the error names only the one that may be on" % stays,
                    stays in message and gone not in message and [n["name"] for n in api.nodes] == [stays]))
    clock, api = off()
    api.outage = lambda: True
    try:
        quiet(t.turn_off, t.Theta(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
        message = ""
    except t.ThetaError as exc:
        message = str(exc)
    out.append(("stop: Theta never answers: an error naming both, once the time is up and not before",
                "ci5a1l" in message and "ci5a1w" in message and t.OFF_ANSWER_LIMIT_S <= clock.now <= t.OFF_ANSWER_LIMIT_S + 60))

    class ForAWhile(FakeTheta):
        polls = 0
        last = 40

        def __call__(self, method, url, headers, body):
            self.polls += 1
            return (503, b"busy") if self.polls <= self.last else super().__call__(method, url, headers, body)
    code, api, _, _, said = run_off_with([[GREEN_L, GREEN_W]], api=ForAWhile())
    out.append(("stop: a passed run, and Theta back in time: green, nothing to report", code == 0 and "::error::" not in said and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    down = FakeTheta()
    down.outage = lambda: True
    code, _, _, _, said = run_off_with([[GREEN_L, GREEN_W]], api=down)
    out.append(("stop: a passed run, and a node may be on: the job is red, and says it may be billing and that the build has its own results",
                code == 1 and "::error::" in said and "billing" in said and "build" in said))
    one = DeleteFails()
    clock = Clock()
    one.failing, one.until = "dpl_l", 10 ** 9
    code, api, _, _, said = run_off_with([[GREEN_L, GREEN_W]], api=one)
    out.append(("stop: one node refused: red for that one, the other gone", code == 1 and [n["name"] for n in api.nodes] == ["ci5a1l"] and "ci5a1l" in said and "ci5a1w" not in said))
    return out


def group_bootstrap():
    out = []
    bootstrap = (HERE / "node_bootstrap.sh").read_text(encoding="utf-8")
    snapshot = bootstrap.index("snapshot.ubuntu.com")
    out.append(("bootstrap: CA certificates before the snapshot, whose redirect to HTTPS needs them (a fresh node found no package)",
                0 < bootstrap.index("--no-install-recommends ca-certificates") < snapshot))
    out.append(("bootstrap: a failed index stops it, by name, both times",
                bootstrap.count("apt-get update -qq -o APT::Update::Error-Mode=any") == 2 and "apt-get update -qq\n" not in bootstrap))
    out.append(("bootstrap: every download by version and sha256, nothing else fetched, the runner's version pinned",
                all(v in bootstrap for v in ("RUNNER_VERSION=", "RUNNER_SHA256=", "NODE_SHA256=", "RUSTUP_SHA256=", "GLSLANG_SHA256="))
                and bootstrap.count("  fetch ") == 0 or bootstrap.count("fetch \"https://") == 4))
    as_runner = next(l for l in bootstrap.splitlines() if l.startswith("as_runner()"))
    out.append(("bootstrap: its checks see the PATH the job will have, where glslang 14 comes before Ubuntu's 11.8",
                "/usr/local/bin:" in as_runner and as_runner.index("/usr/local/bin") < as_runner.index("/usr/bin")
                and "/usr/local/bin:/usr/sbin:/usr/bin" in bootstrap.split("> $R/.path")[0].rsplit("echo ", 1)[1]))
    out.append(("bootstrap: the job's user has no sudo, and the script stops if it finds one",
                "NOPASSWD" not in bootstrap and "rm -f /etc/sudoers.d/runner" in bootstrap
                and '! as_runner sudo -n true 2>/dev/null || missing+=' in bootstrap))
    out.append(("names: run ids of today fit Theta's 20 characters", t.node_name(41647921995, 12, W) == "ci41647921995a12w" and len(t.node_name(41647921995, 12, W)) <= 20))
    out.append(("labels: one per leg, with the run and the attempt", t.leg_label(41647921995, 2, W) == "theta-41647921995-2-win-x86_64"))
    out.append(("time: Theta's 5 decimals and GitHub's none", t.parse_time("2026-11-03T14:35:38.39446Z") == t.parse_time("2026-11-03T14:35:38Z") and t.parse_time("nonsense") is None))
    return out


def group_github_blip():
    """Finding 1: a blip of GitHub's API deleted a healthy node mid-build."""
    out = []
    result, gh, api, _, _, _ = watched(["down"] * 8 + [[RUN_L, RUN_W]], limit=600)
    out.append(("blip: polls of errors, then legs still running: the watch goes on and the nodes stay", result == "continue" and api.deleted == [] and gh.dispatches == []))
    result, gh, api, _, _, _ = watched(["garbage"] * 8 + [[RUN_L, RUN_W]], limit=600)
    out.append(("blip: an answer that is not JSON is the same", result == "continue" and api.deleted == []))
    try:
        watched(["down"])
        blind = None
    except t.WatchBlind as exc:
        blind = str(exc)
    out.append(("blind: GitHub silent for the limit stops the watch with its own error", blind and str(t.BLIND_POLLS) in blind))
    code, printed, _, gh, api = watch_step("watch-1", ["down"])
    out.append(("blind: the command says no verdict, exits 3, and neither deletes a node nor ends the run",
                code == 3 and "verdict=" not in printed and api.deleted == [] and gh.cancels == []))
    return out


def group_false_lost():
    """Finding 2: Theta's list alone said lost, after 5 minutes."""
    out = []
    then_done = [[RUN_L, RUN_W]] * 40 + [[GREEN_L, GREEN_W]]
    result, gh, api, _, _, _ = watched(list(then_done), nodes="w")
    # With no row and no address kept, the list is all there is: the other witness is the address it saw.
    out.append(("list: a node gone from the list from the first look, with nothing else to ask, and its leg running: lost at the limit", result == "lost" and len(gh.dispatches) == 1))

    class ListLosesIt(FakeTheta):
        polls = 0

        def __call__(self, method, url, headers, body):
            if url.endswith("/gpu-deployments") and method == "GET":
                self.polls += 1
                if self.polls > 2:
                    self.nodes = [n for n in self.nodes if n["id"] != "dpl_l"]
            return super().__call__(method, url, headers, body)
    result, gh, api, _, _, _ = watched(list(then_done), api=ListLosesIt())
    out.append(("list: gone for 40 minutes with sshd answering and a leg running: not lost", result == "done" and gh.dispatches == []))
    result, gh, api, _, _, _ = watched(list(then_done), status="pending")
    out.append(("list: another status with sshd answering: not lost", result == "done" and gh.dispatches == []))
    result, gh, api, _, _, _ = watched(list(then_done), rows=(dict(ROW_L, PodPhase="x: Pending"), ROW_W))
    out.append(("list: a pod not Running from the first look: no address, the leg left to the list", result == "lost"))
    silent_5 = {"l": lambda s: s >= 5 * 60, "w": True}
    result, gh, _, _, _, _ = watched(list(then_done), answering=silent_5)
    out.append(("both: down for the old 5 minutes, then sshd back: not lost", result == "done" and gh.dispatches == []))
    out.append(("the limit is at least 15 minutes", t.DOWN_LIMIT_S >= 15 * 60))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=1)
    almost = t.leg_state(RUN_L, machine, NOW + t.DOWN_LIMIT_S - 1)
    enough = t.leg_state(RUN_L, machine, NOW + t.DOWN_LIMIT_S)
    out.append(("both: not a second before the limit, and lost at it", almost is None and enough == "lost"))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=None, answers=False, steps_done=1)
    out.append(("evidence: a leg running, sshd silent and Theta's API silent too: that may be Theta, the leg is left alone",
                t.leg_state(RUN_L, machine, NOW + 3600) is None))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=1)
    machine.see(NOW + 600, listed=False, answers=False, steps_done=2)
    out.append(("evidence: a leg that finished a step says the machine lives, and the wait starts again",
                machine.down_since is None and t.leg_state(RUN_L, machine, NOW + t.DOWN_LIMIT_S) is None))
    out.append(("status: the string the API really answers (measured: running), read as it is", t.RUNNING == "running"))
    live = FakeTheta()
    live.nodes = listed("lw")
    clock = Clock()
    machines = {L: t.Machine(), W: t.Machine()}
    seen = []

    def banner(host, port):
        seen.append(host)
        return True
    quiet(t.look_at_machines, t.Theta(PROJECT, KEY, live, clock.sleep), 5, 1, machines, {}, banner, NOW)
    out.append(("status: each node the real API calls running is listed, each with its own address, and each asked by itself",
                machines[L].listed is True and machines[L].address == ("1.1.1.1", 30001) and machines[W].address == ("2.2.2.2", 30002)
                and machines[L].down_since is None and seen == ["1.1.1.1", "2.2.2.2"]))
    live.list_status = 500
    machines = {L: t.Machine(("h", 1))}
    quiet(t.look_at_machines, t.Theta(PROJECT, KEY, live, clock.sleep), 5, 1, machines, {}, lambda h, p: True, NOW)
    out.append(("status: the API down says nothing about the machine", machines[L].listed is None and machines[L].down_since is None))
    return out


def group_lost_either_order():
    """Finding 7 and GitHub's timing: two witnesses, whichever comes first."""
    out = []
    out.append(("silent: a failed job with no failed step, also one with no steps", t.died_silently(DEAD_L) and t.died_silently(job(LINUX, conclusion="failure"))))
    out.append(("silent: not a compile error, not green, not skipped",
                not t.died_silently(RED_L) and not t.died_silently(GREEN_L) and not t.died_silently(job(LINUX, conclusion="skipped"))))
    result, gh, api, _, _, clock = watched([[RUN_L, RUN_W]], nodes="", answering=False)
    out.append(("first the machines: gone from the list, GitHub still says in progress: lost at the limit",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, _, _, clock = watched([[DEAD_L, DEAD_W]], answering=False)
    out.append(("first GitHub: the jobs failed at once, the list still says running, sshd silent: lost at the limit, and only at it",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, _, _, clock = watched([[RUN_L, RUN_W]], answering=False)
    out.append(("silence: Theta says running, sshd silent, GitHub says in progress: lost at the limit, it does not wait for GitHub",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, _, said, _ = watched([[DEAD_L, GREEN_W]])
    out.append(("a leg with no failed step on a machine that answers: not a lost machine, no re-launch, it says so, and its node is left for stop-node",
                result == "done" and gh.dispatches == [] and "no re-launch" in said and api.deleted == ["dpl_w"]))
    result, gh, _, _, _, _ = watched([[RED_L, GREEN_W]], answering=False, nodes="")
    out.append(("a compile error is a defect, whatever the machine does: no re-launch", result == "done" and gh.dispatches == []))
    result, gh, _, _, _, _ = watched([[job(LINUX, conclusion="cancelled", steps=("success", "cancelled")), job(WIN, conclusion="cancelled")]])
    out.append(("a run cancelled by hand does not come back", result == "done" and gh.dispatches == []))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=0)
    out.append(("no job listed yet is not lost, whatever the machine does", t.leg_state(None, machine, NOW + 3600) is None))
    return out


def group_two_legs():
    """The two legs at once: what happens to one does not take the other with it."""
    out = []
    runs = [[RUN_L, RUN_W]] * 3
    result, gh, api, log, _, clock = watched(runs + [[GREEN_L, RUN_W]] * 3 + [[GREEN_L, GREEN_W]])
    out.append(("both healthy: done, with no re-launch, each node deleted as its own leg ended, not when the other did",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and log == ["delete dpl_l", "delete dpl_w"]))
    result, gh, api, log, _, _ = watched(runs + [[RED_L, RUN_W]] * 3 + [[RED_L, GREEN_W]])
    out.append(("a red leg, no loss: the other goes on, no re-launch, this job does not end the run", result == "done" and gh.dispatches == [] and gh.cancels == [] and log == ["delete dpl_l", "delete dpl_w"]))

    long_enough = int(t.DOWN_LIMIT_S / 60) + 30
    survivor = [[RUN_L, RUN_W]] * long_enough + [[DEAD_L, GREEN_W]]
    result, gh, api, log, said, clock = watched(list(survivor), answering=LOSE_L)
    out.append(("loss of one: the healthy leg is not touched until it ends, the dead node goes at the limit, then one re-launch, this run's cancel last",
                result == "lost" and log == ["delete dpl_l", "delete dpl_w", "dispatch", "cancel"] and clock.now >= long_enough * 60
                and len(gh.dispatches) == 1 and len(gh.cancels) == 1 and L in said))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, GREEN_W]], answering=LOSE_L)
    out.append(("loss of one: the dead leg's job still says in progress when the other ends: it does not wait for GitHub",
                result == "lost" and clock.now >= long_enough * 60 and len(gh.dispatches) == 1))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * long_enough + [[GREEN_L, RUN_W]], answering=LOSE_W)
    out.append(("loss of one, the other way round: the same", result == "lost" and log == ["delete dpl_w", "delete dpl_l", "dispatch", "cancel"]
                and len(gh.dispatches) == 1 and "the machine of %s was lost" % W in said and "the machine of %s" % L not in said))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, GREEN_W]], answering=LOSE_L)
    out.append(("loss of one: the annotation names the leg whose machine was lost, and not the other", "::warning::the machine of %s was lost" % L in said and "the machine of %s" % W not in said))
    result, gh, api, log, said, _ = watched([[GREEN_L, RUN_W]], answering=LOSE_W)
    out.append(("a leg already green and the other lost: re-launch of the whole run, the green one comes back through the cache",
                result == "lost" and gh.dispatches[0]["inputs"]["relaunch_of"] == "5" and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, RED_W]], answering=LOSE_L)
    out.append(("loss of one, and the surviving leg ended red: no re-launch, it would fail again, the run fails and says why",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "no re-launch" in said and W in said and clock.now >= long_enough * 60))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, GREEN_W]], answering=LOSE_L, relaunch_of="4")
    out.append(("second loss, the other leg still healthy: it finishes, then the run fails, no third try",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "second loss" in said and clock.now >= long_enough * 60))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, CANCELLED_W]], answering=LOSE_L)
    out.append(("loss of one, then a person cancels: no re-launch", result == "done" and gh.dispatches == [] and gh.cancels == []))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]], answering=False)
    out.append(("both lost: no one to wait for, one re-launch", result == "lost" and sorted(api.deleted) == ["dpl_l", "dpl_w"] and len(gh.dispatches) == 1 and L in said and W in said))
    result, gh, api, log, said, clock = watched([[QUEUED_L, RUN_W]] * (int(t.STUCK_S / 60) + 30) + [[QUEUED_L, GREEN_W]])
    out.append(("a runner not coming for one leg: its node goes, the healthy leg finishes, then the run fails, no re-launch",
                result == "failed" and log == ["delete dpl_l", "delete dpl_w", "cancel"] and gh.dispatches == [] and "not coming" in said and clock.now >= t.STUCK_S + 30 * 60))
    result, gh, api, log, said, _ = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, GREEN_W]], answering=LOSE_L,
                                            api=_refuses_delete_of("dpl_l", first=3))
    out.append(("a dead node's delete refused for a while: asked again at the next poll, and the other leg's watch goes on",
                result == "lost" and "dpl_l" in api.deleted and len(gh.dispatches) == 1))
    # 4 tries of one delete: the survivor's, refused on the very poll that concludes.
    result, gh, api, log, said, _ = watched([[RUN_L, RUN_W]] * long_enough + [[RUN_L, GREEN_W]], answering=LOSE_L, api=_refuses_delete_of("dpl_w", first=4))
    out.append(("a survivor's node still on when the run concludes: both machines free before the dispatch all the same",
                result == "lost" and log[-3:] == ["delete dpl_w", "dispatch", "cancel"] and api.nodes == []))
    result, gh, api, log, said, _ = watched([[QUEUED_L, RUN_W]] * (int(t.STUCK_S / 60) + 30) + [[QUEUED_L, GREEN_W]], api=_refuses_delete_of("dpl_w", first=4))
    out.append(("the same when the run fails instead: no node left on", result == "failed" and api.nodes == []))
    return out


def _refuses_delete_of(node_id, first):
    class Refuses(FakeTheta):
        left = first

        def __call__(self, method, url, headers, body):
            if method == "DELETE" and url.endswith(node_id) and self.left:
                self.left -= 1
                return 502, b"bad"
            return super().__call__(method, url, headers, body)
    return Refuses()


def group_relaunch():
    """Decision 3 and findings 3, 13, 17: once, by value, in the right order."""
    out = []
    lost = [[RUN_L, RUN_W]]
    result, gh, api, log, said, _ = watched(list(lost), nodes="", answering=False)
    out.append(("re-launch: one, with the run's own inputs by value and its mark",
                result == "lost" and gh.dispatches == [{"ref": "main", "inputs": INPUTS}] and len(gh.cancels) == 1))
    out.append(("re-launch: the nodes first, seen gone, then the dispatch, this run's cancel last", log[-2:] == ["dispatch", "cancel"]))
    result, gh, api, log, _, _ = watched([[DEAD_L, GREEN_W]], answering=LOSE_L)
    out.append(("re-launch: both machines free before the dispatch: it needs both back",
                log == ["delete dpl_w", "delete dpl_l", "dispatch", "cancel"] and api.nodes == []))
    out.append(("re-launch: the refs are explicit, a pushed tag's empty ones no longer pick a default",
                t.relaunch_inputs(5, "release", "firefox-55", "gha", "engine/firefox-55", "v1", "true", "*") ==
                {"kind": "release", "compile_cache": "gha", "core_ref": "engine/firefox-55", "wrapper_ref": "v1", "relaunch_of": "5",
                 "release_tag": "firefox-55", "gtests": "true", "gtest_filter": "*"}))
    for bad in (("", "v1"), ("main", "")):
        try:
            t.relaunch_inputs(5, "candidate", "", "gha", *bad)
            refused = False
        except t.ThetaError:
            refused = True
        out.append(("re-launch: no ref, no re-launch (%r)" % (bad,), refused))
    result, gh, api, log, said, _ = watched(list(lost), nodes="", answering=False, relaunch_of="4")
    out.append(("second loss: no dispatch, the run fails and says it was the second, and a queued leg does not wait 24 hours",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "second loss" in said))
    titled = [{"display_title": t.relaunch_title(5)}]
    result, gh, api, log, said, _ = watched(list(lost), nodes="", answering=False, past_runs=titled)
    out.append(("double dispatch: a re-launch of this run already there: not another, and this run still ends",
                result == "lost" and gh.dispatches == [] and len(gh.cancels) == 1 and "already" in said))
    result, gh, api, log, said, _ = watched(list(lost), nodes="", answering=False, past_runs=[{"display_title": t.relaunch_title(6)}])
    out.append(("double dispatch: another run's re-launch is not this run's", len(gh.dispatches) == 1))
    return out


def group_stuck():
    """A queued job waits up to 24 hours: the watch and the sweeper end that, in that order."""
    out = []
    old_l = job(LINUX, status="queued", conclusion=None, created="2026-11-03T01:00:00Z")
    old_w = job(WIN, status="queued", conclusion=None, created="2026-11-03T01:00:00Z")
    out.append(("stuck: not within the limit", not t.stuck([QUEUED_L, QUEUED_W], NOW)))
    out.append(("stuck: not with a leg running, which a cancel would cut", not t.stuck([RUN_L, old_w], NOW)))
    out.append(("stuck: not on a label of someone else's", not t.stuck([dict(old_w, labels=["ubuntu-24.04"])], NOW)))
    out.append(("stuck: queued alone past the limit", t.stuck([GREEN_L, old_w], NOW)))
    result, gh, api, _, said, clock = watched([[old_l, old_w]])
    out.append(("a leg whose runner never comes, on a machine that answers: the run fails in bounded time, no re-launch, the nodes off, GitHub's old times not counted",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and sorted(api.deleted) == ["dpl_l", "dpl_w"] and "not coming" in said
                and t.STUCK_S <= clock.now <= t.STUCK_S + 120))
    result, gh, _, _, _, _ = watched([[old_l, old_w]], nodes="", answering=False)
    out.append(("the same with the machine gone: lost", result == "lost" and len(gh.dispatches) == 1))
    out.append(("the sweeper gives the watch time to speak first: well past its stuck limit and its lost limit",
                t.SWEEP_STUCK_S >= t.STUCK_S + t.DOWN_LIMIT_S and t.SWEEP_STUCK_S >= 45 * 60))
    gh = FakeGitHub()
    gh.jobs_answers = [[job(LINUX, status="queued", conclusion=None, created="2026-11-03T11:30:00Z"), QUEUED_W]]
    gh.past_runs = [{"id": 7, "run_attempt": 1}]
    clock = Clock()
    got, _ = quiet(t.cancel_stuck, t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep), NOW)
    out.append(("the sweeper: 30 minutes on the queue is still the watch's", got == [] and gh.cancels == []))
    return out


def group_watch_stages():
    """Finding 11: one job's share of the time, and the next goes on, needing nothing from it."""
    out = []
    result, gh, api, _, _, clock = watched([[RUN_L, RUN_W]], limit=t.STAGE_LIMIT_S)
    out.append(("stage: at its limit the watch says continue, and turns nothing off", result == "continue" and api.deleted == [] and clock.now >= t.STAGE_LIMIT_S))
    out.append(("stage: it stops by itself well before the 360 minutes GitHub gives a job", t.STAGE_LIMIT_S <= 340 * 60))
    result, gh, api, _, _, clock = watched([[]])
    out.append(("no legs listed at all for a while: nothing to watch", result == "done" and clock.now == (t.NO_LEGS_POLLS - 1) * 60))
    result, gh, api, log, _, clock = watched([[GREEN_L, RUN_W]] * 3 + [[GREEN_L, GREEN_W]], nodes="w")
    out.append(("a next watch job: a leg green before it began, its node gone: ended at once, not read as lost", result == "done" and gh.dispatches == [] and clock.now < t.DOWN_LIMIT_S))
    result, gh, api, log, _, clock = watched([[RED_L, RUN_W]] * 3 + [[RED_L, GREEN_W]], nodes="w")
    out.append(("a next watch job: a compile error before it began, its node gone: the same", result == "done" and gh.dispatches == []))
    result, gh, api, log, said, clock = watched([[DEAD_L, RUN_W]] * 40 + [[DEAD_L, GREEN_W]], nodes="w")
    out.append(("a next watch job: a loss the last one settled, its node deleted: seen again from what GitHub and Theta say, one re-launch when the other leg ends",
                result == "lost" and len(gh.dispatches) == 1 and clock.now >= 40 * 60))
    return out


def group_sweep():
    out = []

    def swept(nodes, run_status=None, github=True, jobs=None, **kwargs):
        clock = Clock()
        api, gh = FakeTheta(), FakeGitHub()
        api.nodes = nodes
        gh.run_status = run_status or {}
        gh.jobs_answers = [jobs if jobs is not None else [RUN_L, RUN_W]]
        theta = t.Theta(PROJECT, KEY, api, clock.sleep)
        hub = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep) if github else None
        return quiet(t.sweep, theta, hub, NOW, **kwargs)[0], api
    young = "2026-11-03T11:00:00Z"
    old = "2026-11-03T02:00:00Z"
    by_hand = {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "created_at": "2020-01-01T00:00:00Z"}
    other = {"id": "dpl_c", "name": "cinema", "created_at": "2020-01-01T00:00:00Z"}
    hand = {"id": "dpl_h", "name": "cihandbuild1", "created_at": "2020-01-01T00:00:00Z"}
    node_l = {"id": "d1", "name": "ci5a1l", "created_at": young}
    node_w = {"id": "d2", "name": "ci5a1w", "created_at": young}
    gone, api = swept([by_hand, other, hand, dict(node_l, created_at=old), dict(node_w, created_at=old)], {"5": "in_progress"})
    out.append(("sweep: past the limit both of ours go even if their run says it is alive; by hand or another name never, however old",
                gone == ["ci5a1l", "ci5a1w"] and api.deleted == ["d1", "d2"]))
    out.append(("sweep: the limit lets one leg of 350 minutes and the node job finish, and no more than that and an hour",
                t.GUARD_HOURS * 60 >= 350 + 45 + t.STUCK_S / 60 and t.GUARD_HOURS * 60 <= 350 + 45 + t.STUCK_S / 60 + 70))
    out.append(("sweep: the most a forgotten pair of nodes costs is what provision asks the balance to hold", t.GUARD_HOURS * (0.156 + 0.182) <= t.MIN_BALANCE_USD))
    gone, api = swept([node_l, node_w], {"5": "in_progress"})
    out.append(("sweep: young nodes whose run still needs them stay", gone == [] and api.deleted == []))
    gone, api = swept([node_l, node_w], {"5": "completed"})
    out.append(("sweep: young nodes whose run has finished go", gone == ["ci5a1l", "ci5a1w"]))
    gone, api = swept([node_l, node_w], {"5": "in_progress"}, jobs=[GREEN_L, RUN_W])
    out.append(("sweep: behind a dead watch: one leg over, the other running: only that leg's node goes", gone == ["ci5a1l"] and api.deleted == ["d1"]))
    gone, api = swept([node_l, node_w], {"5": "in_progress"}, jobs=[GREEN_L, GREEN_W])
    out.append(("sweep: both legs over, the run still going on with arm64: both go", gone == ["ci5a1l", "ci5a1w"]))
    gone, api = swept([node_l, node_w], {"5": "in_progress"}, jobs=[])
    out.append(("sweep: no legs listed yet is not legs finished", gone == [] and api.deleted == []))
    gone, api = swept([node_l], {"5": 500})
    out.append(("sweep: GitHub's error is not 'the run has finished'", gone == [] and api.deleted == []))
    gone, api = swept([node_l, {"id": "d6", "name": "ci6a1w", "created_at": old}], github=False)
    out.append(("sweep: from a machine with no GitHub, the age alone", gone == ["ci6a1w"] and api.deleted == ["d6"]))
    gone, api = swept([{"id": "d6", "name": "ci6a1w", "created_at": old}], dry_run=True)
    out.append(("sweep: a dry run deletes nothing", gone == ["ci6a1w"] and api.deleted == []))
    force_cancelled = [job(LINUX, conclusion="cancelled", steps=("success", None, None), runner="ci5a1l"), job(WIN, conclusion="cancelled"),
                       job("watch-1", conclusion="cancelled")]
    gone, api = swept([node_l, node_w], {"5": "completed"}, jobs=force_cancelled)
    out.append(("sweep: a force-cancel leaves no stop-node: the run over, its nodes go at the next sweep, however young", gone == ["ci5a1l", "ci5a1w"]))
    gone, api = swept([node_l, node_w], {"5": "queued"}, jobs=force_cancelled)
    out.append(("sweep: and before the run reads completed, their legs having ended are enough", gone == ["ci5a1l", "ci5a1w"]))
    return out


def group_secrets():
    out = []

    class Leaky(FakeTheta):
        def __call__(self, method, url, headers, body):
            if url.endswith("/billing/balance"):
                return 403, ("key %s project %s refused" % (KEY, PROJECT)).encode()
            return super().__call__(method, url, headers, body)
    clock = Clock()
    try:
        t.Theta(PROJECT, KEY, Leaky(), clock.sleep).balance_usd()
        message = ""
    except t.ThetaError as exc:
        message = str(exc)
    out.append(("secrets: an error names neither the key nor the project, in the URL or the answer",
                message and KEY not in message and PROJECT not in message))
    address = t.Theta(PROJECT, KEY, FakeTheta(), clock.sleep).address("dpl_l")
    out.append(("address: host and port of the Running container, nothing else of the row", address == ("1.1.1.1", 30001)))
    pending = dict(ROW_L, PodPhase="x: Pending")
    out.append(("address: a Pending container has none yet", t.Theta(PROJECT, KEY, FakeTheta(rows=(pending,)), clock.sleep).address("dpl_l") is None))
    flaky = []

    def transport(method, url, headers, body):
        flaky.append(url)
        return (503, b"busy") if len(flaky) < 3 else (200, b'{"body":{"balance_usd":3}}')
    out.append(("reads: a busy API is asked again", t.Theta(PROJECT, KEY, transport, clock.sleep).balance_usd() == 3.0 and len(flaky) == 3))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("no key travels: nothing is kept between jobs, so there is nothing to seal", "openssl" not in source and "seal" not in source))
    return out


def shutil_which(name):
    import shutil
    return shutil.which(name)


def group_probe():
    out = []
    import socket
    import threading
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]

    def banner():
        conn, _ = server.accept()
        conn.sendall(b"SSH-2.0-OpenSSH_8.9\r\n")
        conn.close()
    thread = threading.Thread(target=banner)
    thread.start()
    out.append(("probe: sshd's banner is an answer", t.ssh_answers("127.0.0.1", port, timeout=5) is True))
    thread.join()
    server.close()
    out.append(("probe: a closed port is not", t.ssh_answers("127.0.0.1", port, timeout=2) is False))
    return out


def runner_case(leaks) -> dict:
    """node_runner.sh, as it runs on the node, with the system's commands faked."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        home, bin_dir = tmp / "home", tmp / "bin"
        guard = tmp / "root_owned" / "job_guard.sh"
        guard.parent.mkdir()
        (home / "actions-runner").mkdir(parents=True)
        bin_dir.mkdir()
        trace, envs, kills = tmp / "trace", tmp / "envs", tmp / "kills"
        run = home / "actions-runner" / "run.sh"
        run.write_text("#!/bin/bash\n"
                       'echo "start $ACTIONS_RUNNER_INPUT_JITCONFIG work=$(ls -d "$HOME/work" | wc -l)" >> "%s"\n'
                       'env >> "%s"\n'
                       'sleep 0.3\necho "end $ACTIONS_RUNNER_INPUT_JITCONFIG" >> "%s"\nexit 3\n' % (trace, envs, trace),
                       newline="\n")
        run.chmod(0o755)
        fakes = {"runuser": '#!/bin/bash\nwhile [ "$1" != "--" ]; do shift; done; shift; exec "$@"\n',
                 "pkill": '#!/bin/bash\necho "$*" >> "%s"\nexit "${FAKE_PKILL_EXIT:-0}"\n' % kills,
                 "install": '#!/bin/bash\nfor a; do d="$a"; done; mkdir -p "$d"\n',
                 "chown": '#!/bin/bash\necho "$*" >> "%s/chowns"\n' % tmp}
        for name, text in fakes.items():
            (bin_dir / name).write_text(text, newline="\n")
            (bin_dir / name).chmod(0o755)
        log = tmp / "log"
        env = dict(os.environ, PATH="%s%s%s" % (bin_dir, os.pathsep, os.environ["PATH"]), NODE_RUNNER_HOME=str(home),
                   NODE_RUNNER_LOG=str(log), NODE_RUNNER_GUARD=str(guard), **leaks)
        script = str(HERE / "node_runner.sh")

        def start(config, run_id="5"):
            return subprocess.run(["bash", script, run_id], input=config, env=env, capture_output=True, text=True, timeout=30)
        started = start("CFG1\n")
        empty, no_run, bad_run = start(""), subprocess.run(["bash", script], input="CFG\n", env=env, capture_output=True, text=True, timeout=30), start("CFG\n", "5;rm")
        for _ in range(100):
            if log.exists() and log.read_text().count("exited with") >= 1:
                break
            subprocess.run(["sleep", "0.1"])

        def hooked(run_id, pkill_exit="0"):
            kills.write_text("")
            done = subprocess.run(["bash", "-e", str(guard)], capture_output=True, text=True,
                                  env={"GITHUB_RUN_ID": run_id, "PATH": env["PATH"], "FAKE_PKILL_EXIT": pkill_exit})
            return done, kills.read_text().splitlines()
        guard_ok, kills_ok = hooked("5")
        guard_other, kills_other = hooked("6")
        guard_no_pkill, _ = hooked("6", "2")
        return {"code": started.returncode, "refused": (empty.returncode, no_run.returncode, bad_run.returncode),
                "trace": trace.read_text().splitlines(), "log": log.read_text(), "env": envs.read_text(),
                "guard_ok": guard_ok.returncode, "guard_other": guard_other, "guard_mode": guard.stat().st_mode & 0o111,
                "kills_ok": kills_ok, "kills_other": kills_other, "guard_no_pkill": guard_no_pkill.returncode,
                "chowns": (tmp / "chowns").read_text(), "guard": str(guard), "home_has_guard": any(home.rglob("job_guard.sh"))}


def group_runner():
    out = []
    if not sys.platform.startswith("linux"):
        print("skip  runner: node_runner.sh runs on Linux only")
        return out
    leaks = {"CARGO_LEAK": "root", "CONTAINER_SECRET": "x", "ACTIONS_RUNNER_HOOK_JOB_COMPLETED": "/nope"}
    r = runner_case(leaks)
    out.append(("runner: returns at once; no configuration, no run id, or one that is not a number is refused",
                r["code"] == 0 and all(c != 0 for c in r["refused"])))
    out.append(("runner: one runner, its config once, its work directory there", r["trace"] == ["start CFG1 work=1", "end CFG1"]))
    out.append(("runner: how it ended is logged", r["log"].count("exited with 3") == 1))
    names = {l.split("=", 1)[0] for l in r["env"].splitlines() if "=" in l}
    out.append(("runner: an empty environment: nothing of root's reaches the job's start, neither CARGO_* nor anything else",
                not names & set(leaks)
                and names <= {"HOME", "PATH", "LANG", "ACTIONS_RUNNER_HOOK_JOB_STARTED", "ACTIONS_RUNNER_INPUT_JITCONFIG", "PWD", "OLDPWD", "SHLVL", "_"}))
    out.append(("runner: HOME and PATH are the job's", "LANG=C.UTF-8" in r["env"] and "PATH=/home/runner/.cargo/bin:/opt/node/bin:" in r["env"]))
    out.append(("runner: the job of another run fails at the runner's own hook; this run's goes on",
                r["guard_ok"] == 0 and r["guard_other"].returncode != 0 and "serves only run 5" in r["guard_other"].stderr
                and r["guard_mode"] != 0 and "ACTIONS_RUNNER_HOOK_JOB_STARTED" in r["env"]))
    out.append(("runner: the hook kills the user's every process, the Worker with them, so no always() step follows; this run's job kills nothing",
                r["kills_other"] == ["-KILL -u runner"] and r["kills_ok"] == []))
    out.append(("runner: and the hook still fails the job if the kill itself fails", r["guard_no_pkill"] != 0))
    text = (HERE / "node_runner.sh").read_text(encoding="utf-8")
    out.append(("runner: the hook is outside the job's home, in root's directory by default, and made root's: a job can move anything in its home (measured)",
                not r["home_has_guard"] and "root:root %s" % r["guard"] in r["chowns"]
                and "ACTIONS_RUNNER_HOOK_JOB_STARTED=%s" % r["guard"] in r["env"]
                and 'GUARD="${NODE_RUNNER_GUARD:-/usr/local/sbin/job_guard.sh}"' in text))
    out.append(("runner: no --preserve-environment, and no flag that the runner's `run` does not take", "--preserve-environment" not in text and "--disableupdate" not in text))
    out.append(("runner: one node, one job: nothing cleans up after a job or waits for one before it", "pkill -KILL -u runner 2>" not in text and "PIDFILE" not in text and "rm -rf" not in text))
    return out


def group_cancelled_by_hand():
    """A person's cancel never comes back, whatever the machine does. Both shapes as measured."""
    out = []
    dead_running = job(WIN, status="in_progress", conclusion=None, steps=("success", None, None), runner="ci5a1w")

    result, gh, api, _, _, clock = watched([[GREEN_L, DEAD_W]], answering=False)
    out.append(("cancel: no cancel, the runner dead: failure and steps left as they were: the machine is lost, at the limit",
                result == "lost" and len(gh.dispatches) == 1 and clock.now >= t.DOWN_LIMIT_S))
    result, gh, api, _, _, clock = watched([[GREEN_L, CANCELLED_W]], answering=False)
    out.append(("cancel: with a cancel, the same steps, cancelled: no re-launch, and both nodes off with no 15 minutes' wait",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and clock.now == 0 and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    result, gh, _, _, said, _ = watched([[GREEN_L, CANCELLED_W]])
    out.append(("cancel: on a machine that answers: done, and it is no leg that died with nothing to show",
                result == "done" and "no failed step" not in said and not t.died_silently(CANCELLED_W, NOW)))
    hub = FakeGitHub()
    hub.jobs_answers = [[GREEN_L, CANCELLED_W]]
    out.append(("cancel: and stop-node finds the legs finished, so it deletes what is left", t.finished_legs(t.GitHub("o/r", "T", "A", hub, lambda s: None), 5, 1) == [L, W]))
    result, gh, api, _, _, clock = watched([[DEAD_L, QUEUE_CANCELLED_W]], answering=False)
    out.append(("cancel: a machine lost before it and a leg cancelled in the queue: the same, at once, both nodes off",
                result == "done" and gh.dispatches == [] and clock.now == 0 and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    arm = job("build-linux-arm64", conclusion="cancelled", steps=("cancelled",), runner="GitHub Actions 7")
    result, gh, _, _, _, clock = watched([[GREEN_L, DEAD_W, arm]], answering=False)
    out.append(("cancel: only a job on GitHub's runners shows it: no re-launch, at once", result == "done" and gh.dispatches == [] and clock.now == 0))
    result, gh, api, _, _, _ = watched([[RUN_L, QUEUE_CANCELLED_W]], nodes="", answering=False)
    out.append(("cancel: a leg still running at the limit, the other cancelled: no re-launch, the nodes off",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and api.nodes == []))
    gate = job("gate / drive", conclusion="cancelled")
    result, gh, _, _, _, _ = watched([[GREEN_L, dead_running, gate]], answering=False)
    out.append(("cancel: the running leg's job not over yet, a job that needs the legs cancelled: no re-launch", result == "done" and gh.dispatches == []))
    result, gh, _, _, said, _ = watched([[RUN_L, QUEUE_CANCELLED_W]], nodes="", answering=False, relaunch_of="4")
    out.append(("cancel: of a re-launched run is no second loss", result == "done" and gh.cancels == [] and "second loss" not in said))
    watch_ran_out = job("watch-1", conclusion="cancelled")
    result, gh, _, _, _, _ = watched([[GREEN_L, DEAD_W, watch_ran_out]], answering=False)
    out.append(("cancel: a watch that hit its own limit is not one: the machine is lost", result == "lost" and len(gh.dispatches) == 1))
    for status in ("queued", "in_progress", 500, None):
        gh = FakeGitHub()
        gh.run_status = {"5": status}
        result, gh, _, _, _, _ = watched([[RUN_L, QUEUED_W]], nodes="", answering=False, gh=gh)
        out.append(("cancel: what the run's own row says is not read (%s): the jobs decide" % status, result == "lost" and len(gh.dispatches) == 1))

    class CancelledDuringDelete(FakeTheta):
        def __call__(self, method, url, headers, body):
            if method == "DELETE":
                gh.jobs_answers = [[RUN_L, QUEUE_CANCELLED_W]]
            return super().__call__(method, url, headers, body)
    gh = FakeGitHub()
    result, gh, api, _, _, _ = watched([[RUN_L, RUN_W]], answering=False, gh=gh, api=CancelledDuringDelete())
    out.append(("cancel: during the nodes' delete: no re-launch", result == "done" and gh.dispatches == [] and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    gh = FakeGitHub()
    result, gh, api, _, said, _ = watched([[RUN_L, RUN_W]], answering=False, gh=gh, api=CancelledDuringDelete(), relaunch_of="4")
    out.append(("cancel: during the delete, of a re-launched run: no failure called a second loss", result == "done" and gh.cancels == [] and "second loss" not in said))
    gh = FakeGitHub()
    gh.on_head = lambda: setattr(gh, "jobs_answers", [[RUN_L, QUEUE_CANCELLED_W]])
    result, gh, _, _, _, _ = watched([[RUN_L, RUN_W]], answering=False, gh=gh)
    out.append(("cancel: right before the dispatch: no re-launch", result == "done" and gh.dispatches == []))

    class GoesDownAtTheDelete(FakeTheta):
        def __call__(self, method, url, headers, body):
            if method == "DELETE":
                gh.jobs_answers = ["down"]
            return super().__call__(method, url, headers, body)
    gh = FakeGitHub()
    try:
        watched([[RUN_L, RUN_W]], answering=False, gh=gh, api=GoesDownAtTheDelete())
        blind = False
    except t.WatchBlind:
        blind = True
    out.append(("cancel: the jobs cannot be read: no re-launch on a guess, and the watch says so in the end", blind and gh.dispatches == []))
    return out


def group_runner_clock():
    """The clock of a runner that is not coming starts when the watch can see it, and counts only what is the runner's fault."""
    out = []
    result, gh, api, _, _, clock = watched([[QUEUED_L, QUEUED_W]] * 2 + ["down"] * 4 * 25 + [[QUEUED_L, QUEUED_W], [RUN_L, RUN_W], [GREEN_L, GREEN_W]])
    out.append(("clock: GitHub down with the runners there: they cannot take a job meanwhile, so the run does not fail for it",
                result == "done" and gh.cancels == [] and clock.now > t.STUCK_S))
    silent_for_a_while = {"l": lambda s: not 600 <= s < 1100, "w": True}
    result, gh, _, _, _, clock = watched([[QUEUED_L, RUN_W]] * 35 + [[RUN_L, RUN_W]] * 3 + [[GREEN_L, GREEN_W]], answering=silent_for_a_while)
    out.append(("clock: a machine silent for a while does not count against its runner: the wait starts again when it is back, and its runner still comes",
                result == "done" and gh.cancels == []))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=True, answers=True, steps_done=0)
    machine.expect_runner(True, NOW)
    machine.expect_runner(True, NOW + 500)
    out.append(("clock: from the first look that expected it", machine.runner_waited(NOW + 600) == 600
                and t.leg_state(QUEUED_L, machine, NOW + t.STUCK_S - 1) is None and t.leg_state(QUEUED_L, machine, NOW + t.STUCK_S) == "stuck"))
    machine.expect_runner(False, NOW + 700)
    out.append(("clock: and no more once the leg has a runner", machine.runner_waited(NOW + 800) == -1))
    gh = FakeGitHub()
    alive = {"name": "watch-2", "status": "in_progress", "conclusion": None}
    old_w = job(WIN, status="queued", conclusion=None, created="2026-11-03T08:00:00Z")
    gh.jobs_answers = [[GREEN_L, old_w, alive]]
    gh.past_runs = [{"id": 7, "run_attempt": 1}]
    clock = Clock()
    hub = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    got, _ = quiet(t.cancel_stuck, hub, NOW)
    out.append(("sweeper: a watch still there is left to speak, however old GitHub's times", got == [] and gh.cancels == []))
    gh.jobs_answers = [[GREEN_L, old_w, dict(alive, status="completed", conclusion="failure")]]
    got, _ = quiet(t.cancel_stuck, hub, NOW)
    out.append(("sweeper: no watch left: the old times are enough", set(got) == {7} and gh.cancels))
    return out


def group_same_code():
    """A dispatch names a branch: a re-launch of other code is refused."""
    out = []
    lost = [[RUN_L, RUN_W]]
    gh = FakeGitHub()
    gh.head = "newer"
    result, gh, api, log, said, _ = watched(list(lost), nodes="", answering=False, gh=gh)
    out.append(("same code: the branch has moved: no dispatch, the run fails and names both commits, the nodes off and a queued leg ended",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "newer" in said and HEAD in said))
    gh = FakeGitHub()
    gh.head = None
    result, gh, _, _, _, _ = watched(list(lost), nodes="", answering=False, gh=gh)
    out.append(("same code: the branch is gone: the same", result == "failed" and gh.dispatches == []))
    result, gh, _, _, _, _ = watched(list(lost), nodes="", answering=False, sha="")
    out.append(("same code: no idea what this run built: the same", result == "failed" and gh.dispatches == []))
    gh = FakeGitHub()
    gh.head = "down"
    try:
        watched(list(lost), nodes="", answering=False, gh=gh)
        blind = False
    except t.WatchBlind:
        blind = True
    out.append(("same code: the branch cannot be read: no dispatch on a guess", blind and gh.dispatches == []))
    result, gh, _, _, _, _ = watched(list(lost), nodes="", answering=False)
    out.append(("same code: the same commit: re-launched", result == "lost" and len(gh.dispatches) == 1))
    seen = []

    def transport(method, url, headers, body):
        seen.append(url)
        return 200, json.dumps({"object": {"sha": "s"}}).encode()
    got = t.GitHub("o/r", "T", "A", transport, lambda s: None).branch_head("theta/a b")
    out.append(("same code: a branch with a slash and a space is read as itself", got == "s" and seen[0].endswith("/git/ref/heads/theta/a%20b")))
    return out


# What GitHub sets in every job, and what a step sets for each name it declares.
RUNNER_ENV = {"GITHUB_RUN_ID": "5", "GITHUB_RUN_ATTEMPT": "1", "GITHUB_REF_NAME": "main", "GITHUB_SHA": HEAD,
              "GITHUB_REPOSITORY": "o/r", "GITHUB_OUTPUT": os.devnull}
STEP_VALUES = {"THETA_API_KEY": KEY, "THETA_PROJECT": PROJECT, "RUNNER_ADMIN_TOKEN": "ADMIN", "GH_TOKEN": "TOKEN", "KIND": "release",
               "TAG": "firefox-99", "COMPILE_CACHE": "gha", "CORE_REF": "core-x", "WRAPPER_REF": "wrap-y", "GTESTS": "true",
               "GTEST_FILTER_INPUT": "X*", "RELAUNCH_OF": "", "ONLY_IF": "--if-legs-done", "LEG": L}


def workflow():
    import yaml
    return yaml.safe_load((HERE.parent.parent / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))


def run_step(step, answers, nodes="lw", answering=False, wrap_provision=None, **overrides):
    """A step of release.yml, as written: its command line through main(), with nothing in
    the environment but the names the step declares and what GitHub sets, on the real
    from_env and github_from_env. Only HTTP, the clock and sshd's banner are faked.
    (exit code, stdout, stderr, FakeGitHub, FakeTheta)"""
    env = dict(RUNNER_ENV, PATH=os.environ["PATH"], **{name: overrides.get(name, STEP_VALUES[name]) for name in step["env"]})
    clock, api, gh = Clock(), FakeTheta(), FakeGitHub()
    api.nodes = listed(nodes)
    gh.jobs_answers = answers

    class Answer(io.BytesIO):
        status = 200

    def urlopen(req, timeout=60):
        headers = {k.lower(): v for k, v in req.header_items()}
        if "thetaedgecloud" in req.full_url:
            status, raw = api(req.get_method(), req.full_url, headers, req.data)
        else:
            token = "ADMIN" if "/actions/runners" in req.full_url else "TOKEN"
            assert headers["authorization"] == "Bearer %s" % token, req.full_url
            status, raw = gh(req.get_method(), req.full_url, headers, req.data)
        if status >= 400:
            raise urllib.error.HTTPError(req.full_url, status, "", {}, io.BytesIO(raw))
        return Answer(raw)

    def fast(service):
        for client in (getattr(service, name, None) for name in ("http", "admin")):
            if client:
                client.sleep = clock.sleep
        return service

    saved = t.watch, t.from_env, t.github_from_env, t.urllib.request.urlopen, t.turn_off, t.provision, dict(os.environ)
    real_watch, real_theta, real_github, real_turn_off = t.watch, t.from_env, t.github_from_env, t.turn_off
    t.watch = lambda *args, **kwargs: real_watch(*args, probe=lambda h, p: answering, clock=lambda: NOW + clock.now, sleep=clock.sleep, **kwargs)
    t.from_env = lambda: fast(real_theta())
    t.github_from_env = lambda: fast(real_github())
    t.turn_off = lambda theta, names, *rest: real_turn_off(theta, names, clock, clock.sleep) if not rest else real_turn_off(theta, names, *rest)
    t.urllib.request.urlopen = urlopen
    if wrap_provision:
        t.provision = wrap_provision
    os.environ.clear()
    os.environ.update(env)
    printed = io.StringIO()
    try:
        command = os.path.expandvars(step["run"].replace("\\\n", " ").split("|")[0])
        words = shlex.split(command)
        argv = words[next(i for i, w in enumerate(words) if w.endswith("theta_node.py")) + 1:]
        with contextlib.redirect_stdout(printed):
            code, said = quiet(t.main, argv)
    finally:
        t.watch, t.from_env, t.github_from_env, t.urllib.request.urlopen, t.turn_off, t.provision = saved[:6]
        os.environ.clear()
        os.environ.update(saved[6])
    return code, printed.getvalue(), said, gh, api


def watch_step(job_name, answers, **kwargs):
    step = next(s for s in workflow()["jobs"][job_name]["steps"] if s.get("id") == "watch")
    return run_step(step, answers, **kwargs)


def group_workflow_steps():
    """release.yml's names and the ones the command reads are one list, or a job dies at
    its start: every input and token reaches what needs it, by the step's own names."""
    out = []
    doc = workflow()
    lost = [[RUN_L, RUN_W]]
    for name in ("watch-1", "watch-2"):
        code, printed, said, gh, api = watch_step(name, list(lost))
        out.append(("watch step, %s: from its own names to a re-launch of every input by value, on this commit, both nodes off" % name,
                    code == 0 and "verdict=lost" in printed and sorted(api.deleted) == ["dpl_l", "dpl_w"] and len(gh.cancels) == 1
                    and gh.dispatches == [{"ref": "main", "inputs": {"kind": "release", "compile_cache": "gha", "core_ref": "core-x", "wrapper_ref": "wrap-y",
                                                                     "relaunch_of": "5", "release_tag": "firefox-99", "gtests": "true", "gtest_filter": "X*"}}]))
    code, printed, said, gh, api = watch_step("watch-1", list(lost), RELAUNCH_OF="4")
    out.append(("watch step: RELAUNCH_OF reaches it: a second loss fails the job, no dispatch", code == 1 and "verdict=failed" in printed and gh.dispatches == []))
    code, printed, said, gh, api = watch_step("watch-1", [[QUEUED_L, RUN_W]] * 30 + [[QUEUED_L, GREEN_W]], answering=True)
    out.append(("watch step: RUNNER_ADMIN_TOKEN reaches the list of runners, GH_TOKEN the rest: no poll failed on the wrong one",
                code == 1 and "verdict=failed" in printed and "this poll saw nothing" not in said))
    code, printed, said, gh, api = watch_step("watch-1", [[GREEN_L, GREEN_W]])
    out.append(("watch step: legs that finished: verdict done, exit 0, both nodes off", code == 0 and "verdict=done" in printed and sorted(api.deleted) == ["dpl_l", "dpl_w"]))

    node_step = doc["jobs"]["node"]["steps"][-1]
    seen = []

    def fake_provision(theta, github, run, attempt, leg, make_ssh, make_key, *rest):
        seen.append((run, attempt, leg, github.repo))
    for leg in (L, W):
        code, printed, said, _, _ = run_step(node_step, [[]], wrap_provision=fake_provision, LEG=leg)
        out.append(("node step, %s: reaches provision for that leg, from its own names, and prints nothing a log would keep" % leg,
                    code == 0 and printed == "" and seen[-1] == ("5", "1", leg, "o/r")))
    stop_step = next(s for s in doc["jobs"]["stop-node"]["steps"] if " off " in s.get("run", ""))
    code, printed, said, gh, api = run_step(stop_step, [[GREEN_L, RUN_W]])
    out.append(("stop step: with the option from its own names, only the finished leg's node goes", code == 0 and api.deleted == ["dpl_l"] and "ci5a1l" in printed))
    code, printed, said, gh, api = run_step(stop_step, [[RUN_L, RUN_W]], ONLY_IF="")
    out.append(("stop step: without it, both", code == 0 and sorted(api.deleted) == ["dpl_l", "dpl_w"]))
    return out


def group_any_shape():
    """A service's failure of any shape is a blind poll; a bug is not swallowed for ever."""
    out = []
    through = [[RUN_L, RUN_W], [RUN_L, GREEN_W], [GREEN_L, GREEN_W]]
    for name, make in (("IncompleteRead", lambda: http.client.IncompleteRead(b"x")),
                       ("BadStatusLine", lambda: http.client.BadStatusLine("x")),
                       ("RemoteDisconnected", lambda: http.client.RemoteDisconnected("x"))):
        class Breaks(FakeGitHub):
            left = 4 * 3

            def __call__(self, method, url, headers, body):
                if self.left:
                    self.left -= 1
                    raise make()
                return super().__call__(method, url, headers, body)

        class ThetaBreaks(FakeTheta):
            left = 4 * 3

            def __call__(self, method, url, headers, body):
                if self.left:
                    self.left -= 1
                    raise make()
                return super().__call__(method, url, headers, body)
        result, gh, api, _, _, _ = watched(list(through), gh=Breaks(), api=ThetaBreaks())
        out.append(("shape: %s from either service, poll after poll: the watch goes on, to its verdict, both nodes off" % name,
                    result == "done" and sorted(api.deleted) == ["dpl_l", "dpl_w"] and gh.dispatches == []))
    for name, make in (("IncompleteRead", lambda: http.client.IncompleteRead(b"x")), ("BadStatusLine", lambda: http.client.BadStatusLine("x"))):
        tries = []

        def breaks_twice(method, url, headers, body):
            tries.append(url)
            if len(tries) <= 2:
                raise make()
            return 200, b'{"body":{"deployments":[]}}'
        clock = Clock()
        out.append(("shape: %s is asked again like any read, inside the client" % name, t.Theta(PROJECT, KEY, breaks_twice, clock.sleep).nodes() == [] and len(tries) == 3))

        def always_breaks(method, url, headers, body):
            raise make()
        try:
            t.Theta(PROJECT, KEY, always_breaks, clock.sleep).nodes()
            kind = None
        except t.NoAnswer as exc:
            kind = str(exc)
        out.append(("shape: and for good it is no answer, which names the failure and not the project", kind and name in kind and PROJECT not in kind))
    for body in (b'{"body":null}', b"null", b"", b'{"body":{}}', b"<html>"):
        api = FakeTheta()
        api.list_body = body
        result, gh, api, _, _, _ = watched(list(through), api=api)
        out.append(("shape: Theta's list answers %r: not a crash, not a lost machine, the watch reaches its verdict" % body,
                    result == "done" and gh.dispatches == []))
        clock = Clock()
        try:
            t.Theta(PROJECT, KEY, api, clock.sleep).nodes()
            read_as_empty = True
        except t.NoAnswer:
            read_as_empty = False
        out.append(("shape: and it is no answer, never \"nothing on\" (%r)" % body, not read_as_empty))

    def bug(host, port):
        raise TypeError("a bug of ours")
    try:
        watched([[RUN_L, RUN_W]], probe=bug)
        stopped = ""
    except t.WatchBlind as exc:
        stopped = str(exc)
    out.append(("shape: a bug is carried for a bounded number of polls, then the watch stops and names it", "TypeError" in stopped and str(t.BLIND_POLLS) in stopped))
    flips = []

    def bug_once(host, port):
        flips.append(1)
        if len(flips) <= 3:
            raise KeyError("once")
        return True
    result, gh, _, _, said, _ = watched([[RUN_L, RUN_W]] * 3 + list(through), probe=bug_once)
    out.append(("shape: an error that passes uses up nothing, and it was said", result == "done" and "KeyError" in said))
    return out


def group_clear_verdicts():
    out = []
    lost = [[RUN_L, RUN_W]]
    result, gh, _, _, said, _ = watched(list(lost), nodes="", answering=False)
    out.append(("verdict: a first loss names the new run, by id and link", result == "lost" and "run 77" in said and "actions/runs/77" in said))
    earlier = [{"id": 88, "html_url": "https://x/88", "display_title": t.relaunch_title(5)}]
    result, gh, _, _, said, _ = watched(list(lost), nodes="", answering=False, past_runs=earlier)
    out.append(("verdict: and so does a re-launch found already there", "run 88" in said and gh.dispatches == []))

    class NotListed(FakeGitHub):
        def __call__(self, method, url, headers, body):
            answer = super().__call__(method, url, headers, body)
            self.past_runs = []
            return answer
    result, gh, _, _, said, clock = watched(list(lost), nodes="", answering=False, gh=NotListed())
    out.append(("verdict: a new run not listed yet goes by its name, and the wait for it is bounded",
                result == "lost" and t.relaunch_title(5) in said and len(gh.dispatches) == 1))

    def create_404(body, resources, leg=W):
        class Answers(FakeTheta):
            def __call__(self, method, url, headers, sent):
                if method == "POST":
                    self.created.append(json.loads(sent))
                    return 404, body
                return super().__call__(method, url, headers, sent)
        api = Answers()
        api.resources = resources
        return provisioned(api, FakeGitHub(), FakeSsh(), leg)[1], api
    error, api = create_404(b'{"message":"GPU resource not found"}', [t.MACHINES[L]])
    out.append(("verdict: a 404 with this leg's machine out of gpu-resources past the wait: not available, taken or off, whatever the 404 says, naming it",
                "not available" in error and "off" in error and t.MACHINES[W] in error and W in error and t.MACHINES[L] not in error
                and len(api.created) == 1 + t.FREE_LIMIT_S // 30))
    error, api = create_404(b'{"message":"Cannot find deployment image img_x"}', list(t.MACHINES.values()))
    out.append(("verdict: a 404 with the machine listed: free, and something else is missing, named, blaming neither owner nor PC",
                "Cannot find deployment image" in error and "is free" in error and t.TEMPLATE in error and "taken" not in error and "not available" not in error))
    error, api = create_404(b'{"message":"GPU resource not found"}', ["community_other"])
    out.append(("verdict: gpu-resources listing only others: not available", "not available" in error))
    error, api = create_404(b'{"message":"GPU resource not found"}', "down")
    out.append(("verdict: gpu-resources not answering: it does not guess", "does not say why" in error and "taken" not in error and "is free" not in error))
    result, _, _, _, said, _ = watched(list(lost), nodes="", answering=False, relaunch_of="4")
    out.append(("verdict: a second loss says it is a failure and not a person's cancel", result == "failed" and "::error::" in said and "not a cancel by a person" in said))
    result, _, _, _, said, _ = watched([[QUEUED_L, QUEUED_W]])
    out.append(("verdict: and so does giving up", result == "failed" and "::error::" in said and "not a cancel by a person" in said))
    out.append(("verdict: a runner gone without taking the leg: it says what uses one up, a job the node refused", "refuses another run's job" in said))
    gh = FakeGitHub()
    gh.runners = [{"id": 50, "name": "ci5a1l", "status": "online", "labels": []}, {"id": 51, "name": "ci5a1w", "status": "online", "labels": []}]
    result, _, _, _, said, _ = watched([[QUEUED_L, QUEUED_W]], gh=gh)
    out.append(("verdict: runners still there have refused nothing, and it does not say so", result == "failed" and "not coming" in said and "refuses" not in said))
    return out


def group_dispatch_once():
    """A dispatch is not idempotent: a lost answer is looked for before asking again."""
    out = []
    lost = [[RUN_L, RUN_W]]

    def lost_answer(happens, times=1):
        gh = FakeGitHub()
        tries = []

        def on_dispatch(sent):
            tries.append(sent)
            if len(tries) > times:
                return None
            if happens:
                gh.dispatches.append(sent)
                gh.past_runs.append({"id": 77, "html_url": "u", "display_title": t.relaunch_title(5)})
            return (502, b"bad gateway")
        gh.on_dispatch = on_dispatch
        result = watched(list(lost), nodes="", answering=False, gh=gh)
        return result[0], gh, tries
    result, gh, tries = lost_answer(happens=True)
    out.append(("dispatch: the answer lost and the run there: one request, one run", result == "lost" and len(tries) == 1 and len(gh.dispatches) == 1))
    result, gh, tries = lost_answer(happens=False)
    out.append(("dispatch: the answer lost and no run after the wait: asked again, one run", result == "lost" and len(tries) == 2 and len(gh.dispatches) == 1))

    class ListDown(FakeGitHub):
        def __call__(self, method, url, headers, body):
            if "/workflows/release.yml/runs?" in url and self.dispatch_tried:
                return 503, b"busy"
            if url.endswith("/dispatches"):
                self.dispatch_tried += 1
                return 502, b"bad gateway"
            return super().__call__(method, url, headers, body)
        dispatch_tried = 0
    clock, gh = Clock(), ListDown()
    hub = t.GitHub("o/r", "T", "A", gh, clock.sleep)
    try:
        quiet(t.dispatch_once, hub, 5, "main", INPUTS, clock, clock.sleep)
        raised = False
    except t.NoAnswer:
        raised = True
    out.append(("dispatch: the answer lost and the list silent too: never a second request on a guess", raised and gh.dispatch_tried == 1))

    class Refused(FakeGitHub):
        def __call__(self, method, url, headers, body):
            if url.endswith("/dispatches"):
                return 422, b'{"message":"no such input"}'
            return super().__call__(method, url, headers, body)
    result, gh, _, _, said, _ = watched(list(lost), nodes="", answering=False, gh=Refused())
    out.append(("dispatch: GitHub refuses it: the run fails at once, by name, and a queued leg does not wait",
                result == "failed" and len(gh.cancels) == 1 and "refuses" in said))
    return out


def group_lingering_and_silence():
    """A node still listed after its delete, an empty list as null, a failed step on a dead
    runner: none measured, each cheap to survive."""
    out = []
    clock = Clock()
    api = FakeTheta(delete_takes=False)
    api.nodes = listed("lw")
    deleted, said = quiet(t.turn_off, t.Theta(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
    out.append(("linger: a delete accepted and a node still listed: a warning, past the limit, not an error",
                sorted(deleted) == ["dpl_l", "dpl_w"] and "::warning::" in said and clock.now >= t.GONE_LIMIT_S))
    result, gh, _, log, _, _ = watched([[RUN_L, RUN_W]], answering=False, api=FakeTheta(delete_takes=False))
    out.append(("linger: it does not stop the re-launch", result == "lost" and log[-2:] == ["dispatch", "cancel"]))
    api, gh = FakeTheta(), FakeGitHub()
    api.nodes = [{"id": "d9", "name": "ci9a1l", "status": "running"}]
    gh.run_status = {"9": "in_progress"}
    gh.past_runs = [{"id": 5, "display_title": t.relaunch_title(9)}]
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("linger: the re-launch finds its old run's node still listed, that run still ending: not refused for it",
                got and api.deleted == ["d9"]))
    api = FakeTheta()
    api.list_body = b'{"body":{"deployments":null}}'
    clock = Clock()
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    out.append(("null: an empty list as null: nothing on", t.turn_off(theta, t.node_names(5, 1), clock, clock.sleep) == [] and theta.nodes() == []))
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("null: nothing to refuse either", got))

    failed_late = job(WIN, conclusion="failure", steps=("success", "failure"), completed="2026-11-03T12:10:00Z", runner="ci5a1w")
    silent_from_2 = {"l": True, "w": lambda s: s < 120}
    answers = [[GREEN_L, RUN_W]] * 8 + [[GREEN_L, failed_late]]
    result, gh, api, _, _, clock = watched(list(answers), answering=silent_from_2)
    out.append(("failed step: a leg dies, GitHub marks its step failed after the machine went silent, Theta still lists it: lost at the limit",
                result == "lost" and len(gh.dispatches) == 1 and 120 + t.DOWN_LIMIT_S - 60 <= clock.now <= 120 + t.DOWN_LIMIT_S + 300))
    result, gh, _, _, _, _ = watched(list(answers))
    out.append(("failed step: the same job on a machine that answers: a defect, no re-launch", result == "done" and gh.dispatches == []))
    unconcluded = job(WIN, conclusion="failure", steps=("success", "failure", None), completed="2026-11-03T11:59:00Z", runner="ci5a1w")
    result, gh, _, _, _, _ = watched([[GREEN_L, unconcluded]], answering=LOSE_W)
    out.append(("failed step: steps left unconcluded say the runner went away, whenever it ended: lost", result == "lost"))
    out.append(("failed step: a compile error before the machine's silence stays a compile error",
                not t.died_silently(RED_L, NOW) and t.died_silently(RED_L, NOW - 3600)))
    return out


def group_deployment_id():
    """A deployment id reaches no log: the repository is public."""
    out = []

    class Echoes(FakeTheta):
        failing = ()

        def __call__(self, method, url, headers, body):
            if any(part in url and method == verb for verb, part in self.failing):
                return 500, b'{"message":"deployment dpl_l is busy"}'
            return super().__call__(method, url, headers, body)
    clock = Clock()
    api = Echoes()
    api.nodes = listed("l")
    api.failing = (("DELETE", "gpu-deployments"),)
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    try:
        _, said = quiet(t.turn_off, theta, ["ci5a1l"], clock, clock.sleep)
        message = said
    except t.ThetaError as exc:
        message = str(exc)
    out.append(("id: a failed delete, in its URL and its answer: the error names the node by its name, not its id",
                "ci5a1l" in message and "dpl_l" not in message and "gpu-deployments" in message))
    for verb, part, call in (("GET", "/deployments/list", lambda th: th.address("dpl_9")),
                             ("POST", "gpu-deployments", lambda th: th.create("ci5a1l", L, "k")),
                             ("GET", "/billing/balance", lambda th: th.balance_usd())):
        api = Echoes()
        api.nodes = listed("l")
        api.failing = ((verb, part),)
        theta = t.Theta(PROJECT, KEY, api, clock.sleep)
        theta.nodes()
        try:
            call(theta)
            message = "no error"
        except t.ThetaError as exc:
            message = str(exc)
        out.append(("id: a node seen in the list, and an answer of %s %s that repeats it" % (verb, part), "HTTP 500" in message and "dpl_l" not in message))
    api = FakeTheta()
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    created = theta.create("ci5a1l", L, "k")
    out.append(("id: a node just created is known too", created in theta.http.secrets))
    api = Echoes()
    api.failing = (("DELETE", "gpu-deployments"),)
    try:
        t.Theta(PROJECT, KEY, api, clock.sleep).delete("dpl_l")
        message = "no error"
    except t.ThetaError as exc:
        message = str(exc)
    out.append(("id: a delete of a node never listed here, in its own URL", "HTTP 500" in message and "dpl_l" not in message))
    code, api, _, printed, said = run_off_with([[RUN_L, RUN_W]], if_legs_done=False)
    out.append(("id: stop-node's log says which nodes went by their names", sorted(api.deleted) == ["dpl_l", "dpl_w"] and "ci5a1l" in printed and "ci5a1w" in printed
                and "dpl_" not in printed + said))
    return out


def group_refused_job():
    """The hook kills a foreign job's Worker and its runner: this run's leg then waits with
    no runner, on a machine that answers. That has to end in a failed run."""
    out = []
    result, gh, api, _, said, clock = watched([[QUEUED_L, GREEN_W]])
    out.append(("refused job: a leg's runner killed: the run fails within the stuck limit, no re-launch, the node off, and nothing started again",
                result == "failed" and gh.dispatches == [] and gh.jits == [] and len(gh.cancels) == 1 and "dpl_l" in api.deleted and clock.now <= t.STUCK_S + 120))
    return out


class ScriptedRun:
    """subprocess.run for Ssh: what the real ssh would answer, by remote command."""

    def __init__(self, answers):
        self.answers, self.argvs = answers, []

    def __call__(self, argv, **kwargs):
        self.argvs.append(argv)
        code, out, err = self.answers.get(argv[-1], (0, b"", b""))
        return subprocess.CompletedProcess(argv, code, out, err)


def with_run(fake, fn):
    real = t.subprocess.run
    t.subprocess.run = fake
    try:
        return fn()
    finally:
        t.subprocess.run = real


def provisioned_through_ssh(answers):
    """provision() with the real Ssh class over a scripted ssh: (error text, all it said)."""
    clock = Clock()
    theta = t.Theta(PROJECT, KEY, FakeTheta(), clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", FakeGitHub(), clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")

        def attempt():
            try:
                t.provision(theta, github, 5, 1, L, lambda host, port: t.Ssh(host, port, key), lambda: key, clock, clock.sleep)
                return ""
            except t.ThetaError as exc:
                return str(exc)
        return with_run(ScriptedRun(answers), lambda: quiet(attempt))


def group_address():
    """The repository is public: a node's address reaches no log."""
    out = []
    refused = b"ssh: connect to host 1.1.1.1 port 30001: Connection refused\r\n"
    ssh = t.Ssh("1.1.1.1", 30001, "k")
    code, stdout, stderr = with_run(ScriptedRun({"x": (255, b"from 1.1.1.1:30001 in 130001", refused)}), lambda: ssh("x"))
    out.append(("address: what ssh says comes back without host or port, and the rest of it intact",
                code == 255 and stderr == "ssh: connect to host <node> port <port>: Connection refused\r\n"
                and stdout == "from <node>:<port> in 130001"))
    error, said = provisioned_through_ssh({"bash -s": (255, b"", refused)})
    out.append(("address: a failed bootstrap's error, on its way to ::error::, does not carry it",
                "<node>" in error and "1.1.1.1" not in error + said and "30001" not in error + said))
    error, said = provisioned_through_ssh({"bash -s": (0, BOOTSTRAPPED.encode(), b""), PUT_RUNNER: (255, b"", refused)})
    out.append(("address: nor the log of a node that came up", "node up after" in said and "1.1.1.1" not in error + said and "30001" not in error + said))
    for code in (255, 1):
        error, said = provisioned_through_ssh({"bash -s": (0, BOOTSTRAPPED.encode(), b""), START_RUNNER: (code, b"", refused)})
        out.append(("address: ssh ending with %d at the runner's start" % code, error and "1.1.1.1" not in error + said and "30001" not in error + said))
    api = FakeTheta(rows=(dict(ROW_L, PortMappings={"22": "1.1.1.1:30001"}), ROW_W))
    result, gh, _, _, said, _ = watched([[GREEN_L, GREEN_W]], api=api)
    out.append(("address: a port that cannot be read: no answer, and not repeated", result == "done" and "30001" not in said))
    if shutil_which("ssh"):
        import socket
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory() as tmp:
            code, _, stderr = t.Ssh("127.0.0.1", port, Path(tmp) / "none")("true", timeout=30)
        out.append(("address: the real ssh's own words on a closed port", code != 0 and "<node>" in stderr and "127.0.0.1" not in stderr and str(port) not in stderr))
    return out


def group_host_key():
    """The image's host key is the same on every node: the bootstrap makes one, and only
    the connection that runs it goes unverified. It carries no secret."""
    out = []
    api, gh, ssh = FakeTheta(), FakeGitHub(), FakeSsh()
    got, _ = provisioned(api, gh, ssh)
    commands = [c for c, _ in ssh.calls]
    out.append(("host key: pinned after the bootstrap, before the runner's script and its configuration",
                got and ("pin", HOST_KEY) in ssh.calls
                and commands.index("bash -s") < commands.index("pin") < commands.index(PUT_RUNNER) < commands.index(START_RUNNER)))
    api, gh = FakeTheta(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({"bash -s": (0, "node_bootstrap: ready\n", "")}))
    out.append(("host key: a bootstrap that names none fails the run: nothing registered, deleted",
                got is None and "host key" in error and gh.jits == [] and api.deleted == ["dpl_l"]))
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        first = t.Ssh("1.1.1.1", 30001, key)
        pinned = first.pin(HOST_KEY)
        known = Path(tmp, "node_key.known_hosts").read_text(encoding="utf-8")
    first_argv, pinned_argv = " ".join(first.argv), " ".join(pinned.argv)
    out.append(("host key: the pinned connection refuses any other key, under a name that survives a new address",
                "StrictHostKeyChecking=no" in first_argv and "StrictHostKeyChecking=yes" in pinned_argv
                and "HostKeyAlias=%s" % t.HOST_ALIAS in pinned_argv and "HostKeyAlgorithms=ssh-ed25519" in pinned_argv
                and "known_hosts" in pinned_argv and "StrictHostKeyChecking=no" not in pinned_argv and known == "%s %s\n" % (t.HOST_ALIAS, HOST_KEY)))
    if sys.platform.startswith("linux"):
        served = host_key_section("ssh-ed25519 AAAANEW")
        out.append(("bootstrap: a new host key, sshd told, and named once sshd serves it (keyscan's banner line on stdout, as OpenSSH 10 prints it)",
                    served.returncode == 0 and t.HOST_KEY_LINE.search(served.stdout).group(1) == "ssh-ed25519 AAAANEW"))
        served = host_key_section("ssh-ed25519 AAAAOLD")
        out.append(("bootstrap: sshd still serves the old key: it stops, and names none", served.returncode != 0 and t.HOST_KEY_LINE.search(served.stdout) is None and "does not serve" in served.stderr))
    return out


def host_key_section(serves):
    """The bootstrap's host key section under bash, its paths in a temporary directory and
    its commands faked: ssh-keyscan answers `serves`, ssh-keygen makes AAAANEW."""
    bootstrap = (HERE / "node_bootstrap.sh").read_text(encoding="utf-8")
    section = bootstrap[bootstrap.index('echo "== a host key'):bootstrap.index('echo "node_bootstrap: ready')]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        fake_bin, keys = tmp / "bin", tmp / "ssh"
        fake_bin.mkdir()
        keys.mkdir()
        (keys / "ssh_host_ed25519_key.pub").write_text("ssh-ed25519 AAAAOLD root@image\n")
        section = section.replace("/etc/ssh/", "%s/" % keys)
        assert "/etc/" not in section
        victim = subprocess.Popen(["sleep", "30"])
        fakes = {"ssh-keygen": '#!/bin/bash\necho "ssh-ed25519 AAAANEW root@node" > "%s/ssh_host_ed25519_key.pub"\n' % keys,
                 "pgrep": "#!/bin/bash\necho %d\n" % victim.pid,
                 "ssh-keyscan": '#!/bin/bash\necho "# 127.0.0.1:22 SSH-2.0-OpenSSH_10.2"\necho "127.0.0.1 %s"\n' % serves,
                 "sleep": "#!/bin/bash\nexit 0\n"}
        for name, text in fakes.items():
            (fake_bin / name).write_text(text, newline="\n")
            (fake_bin / name).chmod(0o755)
        script = 'set -euo pipefail\ndie() { echo "ERROR: $*" >&2; exit 1; }\n' + section
        try:
            return subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30,
                                  env={"PATH": "%s%s%s" % (fake_bin, os.pathsep, os.environ["PATH"])})
        finally:
            victim.kill()
            victim.wait()


GROUPS = [group_turning_on, group_no_fallback, group_two_runs, group_off, group_stop_node, group_bootstrap, group_github_blip, group_false_lost,
          group_lost_either_order, group_two_legs, group_relaunch, group_stuck, group_watch_stages, group_sweep, group_secrets,
          group_probe, group_runner, group_cancelled_by_hand, group_runner_clock, group_same_code,
          group_workflow_steps, group_any_shape, group_clear_verdicts, group_dispatch_once, group_lingering_and_silence,
          group_refused_job, group_address, group_host_key, group_deployment_id]


def cases() -> list:
    out = []
    for group in GROUPS:
        try:
            out += group()
        except (Exception, SystemExit, Runaway) as exc:
            out.append(("%s: stopped on %s: %s" % (group.__name__, type(exc).__name__, exc), False))
    return out


def main() -> int:
    got = cases()
    failures = 0
    for name, ok in got:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(got) - failures, len(got)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
