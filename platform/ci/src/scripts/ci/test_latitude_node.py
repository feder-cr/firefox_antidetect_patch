#!/usr/bin/env python3
"""latitude_node.py against a fake Latitude.sh API, a fake GitHub API and a fake SSH.

Each group runs on its own: an exception is that group's failure, not the file's.

  turning on: one machine for both legs, by name of the run, one runner per leg online;
    any failure is an error with that node off and both legs' runners removed, never
    another machine or another run's runner;
  turning off: by name, seen gone; with --if-legs-done only once GitHub says both legs
    have finished;
  the watch: both legs at once on the one node; GitHub's blips, the list's false alarms, a
    machine and a job as two witnesses in either order; the node kept while any leg needs
    it; a lost machine is lost to both legs, then one re-launch by value, the node off first
    and cancel last, never two, never of a run someone cancelled, never after a red leg,
    never of other code;
  the sweeper: only our names, and the machine freed behind a watch that died;
  secrets: nothing reaches a message;
  the runners on the node: one per leg, each its own user, home copy, guard and port,
    from an empty environment, killing any other run's job at its first step (Linux),
    which the watch turns into a failed run;
  exposure: a node's address in no message, its deployment id in no output, and no
    configuration on a connection not held to the host key the bootstrap made.

    python3 scripts/ci/test_latitude_node.py
"""
from __future__ import annotations

import argparse
import contextlib
import http.client
import io
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import urllib.error
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import latitude_node as t  # noqa: E402

KEY, PROJECT = "SECRET-KEY-123", "prj_secret456"
L, W = "linux-x86_64", "win-x86_64"
# The run's one node, which both legs share: one row, one address for both legs' Machine.
ROW_X = {"BaseID": "dpl_x", "PodPhase": "ci5a1x-deployment-x: Running", "PortMappings": {"22": 30001},
         "NodePublicIP": "1.1.1.1", "AuthPassword": "PASSWORD-789"}
LINUX, WIN = "build-" + L, "build-" + W
HEAD = "c0ffee"
HOST_KEY = "ssh-ed25519 AAAAHOSTKEY"
BOOTSTRAPPED = "== the user\nnode_bootstrap: host key: %s\nnode_bootstrap: ready\n" % HOST_KEY
START_L = "bash /usr/local/sbin/node_runner.sh 5 l"
START_W = "bash /usr/local/sbin/node_runner.sh 5 w"
PUT_RUNNER = "cat > /usr/local/sbin/node_runner.sh"


def listed(marks=t.NODE_MARK, status=t.RUNNING, run=5, attempt=1):
    return [{"id": "dpl_%s" % m, "name": "ci%sa%s%s" % (run, attempt, m), "status": status} for m in marks]


class FakeLatitude:
    """The API as measured on 2026-11-04: JSON:API, the server's hostname as its name."""

    def __init__(self, create_status=201, create_lost=False, rows=(ROW_X,), delete_takes=True, log=None, refuse_sites=()):
        self.create_status, self.create_lost = create_status, create_lost
        self.rows, self.delete_takes = list(rows), delete_takes
        self.nodes, self.created, self.deleted = [], [], []
        self.log = log if log is not None else []
        self.list_status = 200
        self.outage = None  # a function: true while everything answers 503
        self.list_body = None  # what the list of nodes answers instead, as raw bytes
        self.refuse_sites = list(refuse_sites)
        self.keys, self.keys_deleted = [], []
        # (region, USD an hour, the sites that have it)
        self.regions = [("Mexico", 1.48, ["MEX2"]), ("Netherlands", 1.09, ["AMS"]), ("United States", 1.09, ["DAL", "NYC"])]

    def __call__(self, method, url, headers, body):
        assert {k.lower(): v for k, v in headers.items()}["authorization"] == "Bearer " + KEY
        path = url.split("api.latitude.sh", 1)[1]
        if self.outage and self.outage():
            return 503, b"busy"
        if path.startswith("/plans"):
            assert t.PLAN in path
            regions = [{"name": name, "pricing": {"USD": {"hour": price}}, "locations": {"in_stock": sites}} for name, price, sites in self.regions]
            return 200, json.dumps({"data": [{"attributes": {"regions": regions}}]}).encode()
        if path.endswith("/ssh_keys") and method == "POST":
            sent = json.loads(body)["data"]["attributes"]
            self.keys.append({"id": "key_" + sent["name"], "attributes": sent})
            return 201, json.dumps({"data": {"id": "key_" + sent["name"]}}).encode()
        if path.endswith("/ssh_keys"):
            return 200, json.dumps({"data": self.keys}).encode()
        if "/ssh_keys/" in path:
            key_id = path.rsplit("/", 1)[1]
            self.keys_deleted.append(key_id)
            self.keys = [k for k in self.keys if k["id"] != key_id]
            return 204, b""
        if path.startswith("/servers?") and self.list_body is not None:
            return 200, self.list_body
        if path.startswith("/servers?"):
            data = [{"id": n["id"], "attributes": {"hostname": n["name"], "status": n.get("status"), "created_at": n.get("created_at")}} for n in self.nodes]
            return self.list_status, json.dumps({"data": data}).encode()
        if path == "/servers" and method == "POST":
            sent = json.loads(body)["data"]["attributes"]
            self.created.append(sent)
            node_id = "dpl_" + sent["hostname"][-1]
            if self.create_lost:
                self.nodes.append({"id": node_id, "name": sent["hostname"], "status": "unknown"})
                raise OSError("connection reset")
            if sent["site"] in self.refuse_sites:
                return 422, b'{"errors":[{"detail":"not available"}]}'
            if self.create_status != 201:
                return self.create_status, b'{"errors":[{"detail":"refused"}]}'
            self.nodes.append({"id": node_id, "name": sent["hostname"], "status": t.RUNNING})
            return 201, json.dumps({"data": {"id": node_id}}).encode()
        if path.startswith("/servers/") and method == "GET":
            node_id = path.rsplit("/", 1)[1]
            for row in self.rows:
                if row["BaseID"] == node_id:
                    if self.list_status != 200:
                        return self.list_status, b"busy"
                    host = row["NodePublicIP"] if row["PodPhase"].endswith(": Running") else None
                    return 200, json.dumps({"data": {"id": node_id, "attributes": {"primary_ipv4": host}}}).encode()
            return 404, b"{}"
        if method == "DELETE":
            node_id = path.rsplit("/", 1)[1]
            self.deleted.append(node_id)
            self.log.append("delete " + node_id)
            if self.delete_takes:
                self.nodes = [n for n in self.nodes if n["id"] != node_id]
            return 204, b""
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


def provisioned(api, gh, ssh):
    """provision() of the run's node: (True or None, error text)."""
    clock = Clock()
    latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")
        try:
            quiet(t.provision, latitude, github, 5, 1, lambda host, port: ssh, lambda: key, clock, clock.sleep)
            return True, ""
        except t.NodeError as exc:
            return None, str(exc)


def job(name, status="completed", conclusion="success", steps=(), created="2026-11-03T10:00:00Z", completed=None, runner=None):
    return {"name": name, "status": status, "conclusion": conclusion, "labels": ["latitude-5-1-" + name[len("build-"):]],
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


def watched(answers, nodes=t.NODE_MARK, status=t.RUNNING, answering=True, relaunch_of="", limit=6 * 3600, past_runs=(), gh=None,
            rows=(ROW_X,), api=None, sha=HEAD, probe=None):
    """watch() to its verdict, on a clock that only moves when it sleeps. answering: is the
    node's sshd there: a bool, or a function of the seconds since the start. One node: both
    legs ask the same machine, and both lose it together.
    (verdict, FakeGitHub, FakeLatitude, the log of deletes and dispatches, all it said, clock)"""
    clock, log = Clock(), []
    api, gh = api or FakeLatitude(rows=rows), gh or FakeGitHub()
    api.log, gh.log = log, log
    api.nodes = listed(nodes, status)
    gh.jobs_answers = answers
    gh.past_runs = list(past_runs) or gh.past_runs
    latitude, github = t.Latitude(PROJECT, KEY, api, clock.sleep), t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)

    def answers_at(host, port):
        assert (host, port) == ("1.1.1.1", 22), host
        return answering(clock.now) if callable(answering) else answering
    result, said = quiet(t.watch, latitude, github, 5, 1, "main", INPUTS, relaunch_of, probe or answers_at,
                         lambda: NOW + clock.now, clock.sleep, 60, limit, sha=sha)
    return result, gh, api, log, said, clock


def group_turning_on():
    out = []
    out.append(("plan: one, named in one place, and no argument names another",
                (HERE / "latitude_node.py").read_text(encoding="utf-8").count('"%s"' % t.PLAN) == 1 and "--plan" not in (HERE / "latitude_node.py").read_text(encoding="utf-8")))
    out.append(("names: the node's name says its run and its attempt, and that it serves every leg",
                t.node_name(5, 1) == "ci5a1x" and t.node_names(5, 1) == ["ci5a1x"] and t.NAME.match("ci5a1x").groups() == ("5", "1", t.NODE_MARK)
                and t.NODE_MARK not in {leg[0] for leg in t.node_legs()}))
    out.append(("names: each leg's runner says its run, its attempt and its leg, and is no node's name",
                [t.runner_name(5, 1, leg) for leg in t.node_legs()] == ["ci5a1l", "ci5a1w"] and t.node_name(5, 1) not in ("ci5a1l", "ci5a1w")))
    api, gh, ssh = FakeLatitude(), FakeGitHub(), FakeSsh()
    got, error = provisioned(api, gh, ssh)
    sent = api.created[0] if api.created else {}
    out.append(("on: one node for both legs, in the cheapest location with stock, the United States among equals, under the run's name, billed by the hour",
                got and [c["site"] for c in api.created] == ["DAL"] and sent.get("hostname") == t.node_name(5, 1)
                and sent.get("plan") == t.PLAN and sent.get("operating_system") == t.OPERATING_SYSTEM and sent.get("billing") == "hourly"
                and sent.get("project") == PROJECT and [n["name"] for n in api.nodes] == ["ci5a1x"]))
    out.append(("on: with this run's key, which is dropped once the node has answered",
                sent.get("ssh_keys") == ["key_ci5a1x"] and api.keys == [] and api.keys_deleted == ["key_ci5a1x"]))
    # Dallas fetched from GitHub at 1.5-2 MB/s on 04/11 while San Jose, at the same price,
    # ran at full speed: LATITUDE_EXCLUDE_SITES leaves sites out, and nothing else changes.
    from unittest import mock
    api_x = FakeLatitude()
    api_x.regions = [("United States", 1.09, ["DAL", "SJC2"]), ("Netherlands", 1.09, ["AMS"])]
    clock = Clock()
    plain = t.Latitude(PROJECT, KEY, api_x, clock.sleep).sites()
    left_out = t.Latitude(PROJECT, KEY, api_x, clock.sleep, exclude=[" dal "]).sites()
    with mock.patch.dict(os.environ, {"LATITUDE_PROJECT": PROJECT, "LATITUDE_API_KEY": KEY, "LATITUDE_EXCLUDE_SITES": "DAL,LON2"}):
        from_env, said = quiet(t.from_env)
    with mock.patch.dict(os.environ, {"LATITUDE_PROJECT": PROJECT, "LATITUDE_API_KEY": KEY, "LATITUDE_EXCLUDE_SITES": ""}):
        none_out, _ = quiet(t.from_env)
    out.append(("sites: LATITUDE_EXCLUDE_SITES leaves sites out, the cheapest of the rest first, and says so; empty, nothing changes",
                [s for _, s in plain] == ["DAL", "SJC2", "AMS"] and [s for _, s in left_out] == ["SJC2", "AMS"]
                and from_env.exclude == {"DAL", "LON2"} and "DAL, LON2" in said and none_out.exclude == set()))
    out.append(("on: one runner per leg, each its own name and its own label, online",
                [j["name"] for j in gh.jits] == ["ci5a1l", "ci5a1w"] and [j["labels"] for j in gh.jits] == [[t.leg_label(5, 1, L)], [t.leg_label(5, 1, W)]]
                and all(j["work_folder"] == "../work" for j in gh.jits)))
    commands = [c for c, _ in ssh.calls]
    stdin = dict(ssh.calls)
    out.append(("on: prepared once, before either runner starts; each started with the run and its leg named, its own configuration by stdin, never on a command line",
                commands.count("bash -s") == 1 and commands.index("bash -s") < commands.index(START_L) < commands.index(START_W)
                and b"node_bootstrap" in stdin["bash -s"] and b"\r" not in stdin["bash -s"]
                and stdin[START_L] == b"JIT1\n" and stdin[START_W] == b"JIT2\n" and not any("JIT" in c for c in commands)))
    out.append(("on: a node that went well stays on", api.deleted == []))
    out.append(("prices: the ceiling is above the plan's dearest location, as read", t.MAX_PRICE_USD_HOUR >= 1.48))
    got, error = provisioned(FakeLatitude(), FakeGitHub(), FakeSsh({"true": (255, "", "root@<node>: Permission denied (publickey).")}))
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
    except t.NodeError as exc:
        said = str(exc)
    out.append(("path: refused, the node's connection stops there, and its address stays out of the error", "size the path" in said and "9.9.9.9" not in said))
    seen = []
    made = t.node_ssh(Path("work"), seen.append)("9.9.9.9", 22)
    out.append(("path: every connection of provision goes through it", seen == ["9.9.9.9"] and isinstance(made, t.Ssh)))
    source = (HERE / "latitude_node.py").read_text(encoding="utf-8")
    body = source[source.index("def run_provision"):source.index("def run_watch")]
    out.append(("path: provision takes its connections from there", "node_ssh(work)" in body and "Ssh(" not in body))
    out.append(("up: a node that never answers says why, with ssh's own words",
                not got and "Permission denied (publickey)" in error and "ssh exited 255" in error))
    return out


def group_no_fallback():
    out = []
    api, gh = FakeLatitude(create_status=403), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: a refusal (the spending limit, say) is final: an error, nothing on", got is None and "403" in error and api.nodes == [] and gh.jits == [] and len(api.created) == 1))
    api, gh = FakeLatitude(refuse_sites=["DAL"]), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("on: a location that no longer has it, between the list and the order: the next cheapest",
                got and [c["site"] for c in api.created] == ["DAL", "NYC"] and len(api.nodes) == 1))
    api, gh = FakeLatitude(refuse_sites=["DAL", "NYC", "AMS"]), FakeGitHub()
    api.regions[0] = ("Mexico", t.MAX_PRICE_USD_HOUR + 0.5, ["MEX2"])
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: no location at or under the ceiling takes it: the dearest is not ordered, and the error says so",
                got is None and [c["site"] for c in api.created] == ["DAL", "NYC", "AMS"] and "MEX2" in error and "ceiling" in error and gh.jits == []))
    api = FakeLatitude()
    api.regions = []
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: nowhere has the plan: an error naming it, nothing created, no key registered", got is None and t.PLAN in error and api.created == [] and api.keys == []))
    api = FakeLatitude(create_lost=True)
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: the answer to the create is lost: the node it may have made is deleted", got is None and api.deleted == ["dpl_x"]))
    api = FakeLatitude(rows=())
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: a node never Running within the limit is deleted", got is None and api.deleted == ["dpl_x"] and api.nodes == []))
    api = FakeLatitude()
    got, error = provisioned(api, FakeGitHub(), FakeSsh({"true": (255, "", "refused")}))
    out.append(("fail: no SSH within the limit: deleted", got is None and api.deleted == ["dpl_x"]))
    api, gh = FakeLatitude(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({"bash -s": (1, "", "node_bootstrap: ERROR: missing: git")}))
    out.append(("fail: a bootstrap that refuses: deleted, and nothing registered", got is None and "missing: git" in error and api.deleted == ["dpl_x"] and gh.jits == []))
    api, gh = FakeLatitude(), FakeGitHub(jit_status=401)
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: GitHub refuses the registration: deleted", got is None and api.deleted == ["dpl_x"]))
    api, gh = FakeLatitude(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({START_L: (1, "", "no")}))
    out.append(("fail: the first leg's runner does not start: deleted, its registered runner removed, and the other leg's never started",
                got is None and api.deleted == ["dpl_x"] and gh.removed == ["100"] and [j["name"] for j in gh.jits] == ["ci5a1l"]))
    api, gh = FakeLatitude(), FakeGitHub()
    gh.runners = [{"id": 90, "name": "ci6a1l", "busy": False, "status": "online", "labels": [{"name": t.leg_label(6, 1, L)}]}]
    ssh = FakeSsh({START_W: (1, "", "no")})
    got, error = provisioned(api, gh, ssh)
    out.append(("fail: the second leg's runner does not start: the one node deleted, both legs' runners removed, another run's never",
                got is None and api.deleted == ["dpl_x"] and api.nodes == [] and len(gh.removed) == 2 and "90" not in gh.removed
                and [r["name"] for r in gh.runners] == ["ci6a1l"]
                and START_L in [c for c, _ in ssh.calls]))
    api, gh = FakeLatitude(), FakeGitHub(online=False)
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: a runner that never comes online fails the run within the limit, it does not wait 24 hours in the queue",
                got is None and "online" in error and api.deleted == ["dpl_x"] and t.RUNNER_ONLINE_LIMIT_S <= 600))

    class OnlyLinuxOnline(FakeGitHub):
        def __call__(self, method, url, headers, body):
            answer = super().__call__(method, url, headers, body)
            for runner in self.runners:
                runner["status"] = "online" if runner["name"] == "ci5a1l" else "offline"
            return answer
    api, gh = FakeLatitude(), OnlyLinuxOnline()
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: one leg's runner online and the other's never: the node deleted, both runners removed",
                got is None and "online" in error and api.deleted == ["dpl_x"] and sorted(gh.removed) == ["100", "101"]))
    # The command line: exit 1, and nothing a job could read.
    for var in ("LATITUDE_API_KEY", "LATITUDE_PROJECT"):
        os.environ.pop(var, None)
    sink = io.StringIO()
    with contextlib.redirect_stdout(sink):
        code, said = quiet(t.main, ["provision", "--run", "5", "--attempt", "1"])
    out.append(("fail: the command exits 1 with an error for the run's page, naming the legs left with nowhere to run",
                code == 1 and sink.getvalue() == "" and "::error::" in said and L in said and W in said))
    try:
        quiet(t.main, ["provision", "--run", "5", "--attempt", "1", "--leg", "linux-arm64"])
        refused = False
    except SystemExit as exc:
        refused = exc.code != 0
    out.append(("no fallback: the command takes no leg, so nothing can ask it for another (no linux-arm64 from firefox-56 on), and the node's legs are the two x86_64 ones",
                refused and t.node_legs() == [L, W]))
    source = (HERE / "latitude_node.py").read_text(encoding="utf-8")
    out.append(("no fallback: nothing in the script names a builder or sends a leg to GitHub's runners",
                "builder" not in source.replace("test_release_builder", "") and "provision_outputs" not in source and "GitHub's runners, nothing" not in source))
    return out


def group_two_runs():
    """No fixed machine: two runs do not get in each other's way, and what no run needs goes."""
    out = []

    def taken(nodes, run_status):
        api, gh = FakeLatitude(), FakeGitHub()
        api.nodes = list(nodes)
        gh.run_status = run_status
        got, error = provisioned(api, gh, FakeSsh())
        return got, error, api, gh
    live = {"id": "d9", "name": "ci9a1x", "status": t.RUNNING}
    got, error, api, gh = taken([live], {"9": "in_progress"})
    out.append(("two runs: a live run's node is that run's: this run's goes on, and it is not touched", got and api.deleted == []))
    got, error, api, gh = taken([live], {"9": "completed"})
    out.append(("two runs: a node whose run has finished goes", got and api.deleted == ["d9"]))
    earlier = [{"id": "d0%s" % m, "name": "ci5a0%s" % m, "status": t.RUNNING} for m in (t.NODE_MARK, "l", "w")]
    got, error, api, gh = taken(earlier, {"5": "in_progress"})
    out.append(("two runs: an earlier attempt of this same run goes, whatever its nodes were called (one per leg before 2026-11-04)",
                got and sorted(api.deleted) == ["d0l", "d0w", "d0x"]))
    got, error, api, gh = taken([{"id": "d_old", "name": "ci5a1x", "status": t.RUNNING}], {"5": "in_progress"})
    out.append(("same run: a node of this very attempt left by a provision that failed goes before the new one is made",
                got and api.deleted == ["d_old"] and len(api.created) == 1 and [n["id"] for n in api.nodes] == ["dpl_x"]))
    by_hand = {"id": "dh", "name": "myserver", "status": t.RUNNING}
    got, error, api, gh = taken([by_hand], {})
    out.append(("by hand: a server of this project with another name is never touched", got and api.deleted == []))
    other = {"id": "dh", "name": "cihandbuild1", "status": t.RUNNING}
    got, error, api, gh = taken([other], {})
    out.append(("by hand: even with a name that starts with ci", got and api.deleted == []))
    return out


def run_off_with(answers, if_legs_done=True, nodes=t.NODE_MARK, api=None, runners=()):
    """The off command: (exit code, FakeLatitude, FakeGitHub, stdout, stderr)."""
    api, gh, clock = api or FakeLatitude(), FakeGitHub(), Clock()
    api.nodes = listed(nodes)
    gh.jobs_answers = answers
    gh.runners = list(runners)
    saved = t.from_env, t.github_from_env, t.turn_off
    real_turn_off = t.turn_off
    t.from_env = lambda: t.Latitude(PROJECT, KEY, api, clock.sleep)
    t.github_from_env = lambda: t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    t.turn_off = lambda latitude, names: real_turn_off(latitude, names, clock, clock.sleep)
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
    api = FakeLatitude()
    api.nodes = listed() + [{"id": "dpl_2", "name": "ci5a2x", "status": t.RUNNING}, {"id": "dpl_6", "name": "ci6a1x", "status": t.RUNNING},
                            {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "status": t.RUNNING}]
    latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
    deleted = t.turn_off(latitude, t.node_names(5, 1), clock, clock.sleep)
    out.append(("off: this attempt's one node and nothing else, by name", deleted == ["dpl_x"] and sorted(n["id"] for n in api.nodes) == ["dpl_2", "dpl_6", "dpl_lab"]))
    out.append(("off: nothing on is not an error", t.turn_off(t.Latitude(PROJECT, KEY, FakeLatitude(), clock.sleep), t.node_names(5, 1), clock, clock.sleep) == []))

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
    out.append(("off: a leg not listed yet is not a finished leg: the node stays, and the warning names that leg alone",
                code == 0 and api.deleted == [] and W in said and L not in said))
    code, api, _, _, said = run_off_with([[GREEN_L, RUN_W]])
    out.append(("off: one leg finished, the other running on the same node: the node stays for it, and the warning names only that one",
                code == 0 and api.deleted == [] and W in said and L not in said))
    code, api, _, _, said = run_off_with([[RUN_L, GREEN_W]])
    out.append(("off: the same the other way round", code == 0 and api.deleted == [] and L in said and W not in said))
    code, api, _, printed, _ = run_off_with([[GREEN_L, GREEN_W]])
    out.append(("off: both legs have finished: the node deleted", code == 0 and api.deleted == ["dpl_x"] and "ci5a1x" in printed))
    code, api, _, _, _ = run_off_with([[RUN_L, RUN_W]], if_legs_done=False)
    out.append(("off without the option (a node job did not finish): the node deleted, asking nothing", code == 0 and api.deleted == ["dpl_x"]))
    runners = [{"id": 90, "name": "ci5a1l", "busy": False, "labels": [{"name": t.leg_label(5, 1, L)}]},
               {"id": 91, "name": "ci5a1w", "busy": False, "labels": [{"name": t.leg_label(5, 1, W)}]},
               {"id": 92, "name": "ci6a1l", "busy": False, "labels": [{"name": t.leg_label(6, 1, L)}]}]
    code, api, gh, printed, _ = run_off_with([[GREEN_L, RUN_W]], runners=runners)
    out.append(("off: only the runner of the leg that went, and never another run's", gh.removed == ["90"] and "ci5a1l" in printed))
    return out


def group_stop_node():
    """The run's node off, for as long as the step can try."""
    out = []
    clock = Clock()
    gh = FakeGitHub()
    labels = [t.leg_label(5, 1, W), t.leg_label(5, 1, L)]
    gh.runners = [{"id": 50, "name": "dead", "busy": False, "labels": [{"name": labels[0]}]},
                  {"id": 51, "name": "unused", "busy": False, "labels": [{"name": labels[1]}]}]
    gh.busy_ids = ("50",)
    removed = t.GitHub("o/r", "T", "A", gh, clock.sleep).remove_runners(labels)
    out.append(("stop: a runner GitHub says is still on a job (422) fails nothing, and the next one still goes", removed == ["unused"] and gh.removed == ["51"]))

    class SecondDelete(FakeLatitude):
        def __call__(self, method, url, headers, body):
            if method == "DELETE":
                self.deleted.append("x")
                self.nodes = []
                return 404, b'{"message":"GPU deployment not found"}'
            return super().__call__(method, url, headers, body)
    api = SecondDelete()
    api.nodes = listed()
    out.append(("stop: a delete that answers 404 is a node gone", t.turn_off(t.Latitude(PROJECT, KEY, api, clock.sleep), ["ci5a1x"], clock, clock.sleep) == ["dpl_x"]))

    def off():
        clock = Clock()
        api = FakeLatitude()
        api.nodes = listed()
        return clock, api

    clock, api = off()
    api.outage = lambda: clock.now < 300
    deleted, _ = quiet(t.turn_off, t.Latitude(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
    out.append(("stop: the API fails for 5 minutes, list and delete: the node still goes", deleted == ["dpl_x"] and api.nodes == []))

    class DeleteFails(FakeLatitude):
        failing = "dpl_x"
        until = 300

        def __call__(self, method, url, headers, body):
            if method == "DELETE" and url.endswith(self.failing) and clock.now < self.until:
                return 502, b"bad"
            return super().__call__(method, url, headers, body)
    clock = Clock()
    api = DeleteFails()
    api.nodes = listed()
    deleted, _ = quiet(t.turn_off, t.Latitude(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
    out.append(("stop: the node's delete fails for 5 minutes: asked again, and gone in the end",
                deleted == ["dpl_x"] and api.nodes == [] and clock.now >= 300))
    # turn_off takes names, each whatever happens to the other: what a node of the old
    # one-per-leg design, or a provision's leftover, would need. Nothing passes two today.
    clock = Clock()
    api = DeleteFails()
    api.failing, api.until = "dpl_l", 10 ** 9
    api.nodes = listed("lw")
    try:
        quiet(t.turn_off, t.Latitude(PROJECT, KEY, api, clock.sleep), ["ci5a1l", "ci5a1w"], clock, clock.sleep)
        message = ""
    except t.NodeError as exc:
        message = str(exc)
    out.append(("stop: of two names, one that can never be deleted: the other is gone all the same, and the error names only the one that may be on",
                "ci5a1l" in message and "ci5a1w" not in message and [n["name"] for n in api.nodes] == ["ci5a1l"]))
    clock = Clock()
    api = DeleteFails()
    api.until = 10 ** 9
    api.nodes = listed()
    try:
        quiet(t.turn_off, t.Latitude(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
        message = ""
    except t.NodeError as exc:
        message = str(exc)
    out.append(("stop: the node can never be deleted: an error naming it, and it is still listed",
                "ci5a1x" in message and [n["name"] for n in api.nodes] == ["ci5a1x"]))
    clock, api = off()
    api.outage = lambda: True
    try:
        quiet(t.turn_off, t.Latitude(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
        message = ""
    except t.NodeError as exc:
        message = str(exc)
    out.append(("stop: the API never answers: an error naming the node, once the time is up and not before",
                "ci5a1x" in message and t.OFF_ANSWER_LIMIT_S <= clock.now <= t.OFF_ANSWER_LIMIT_S + 60))

    class ForAWhile(FakeLatitude):
        polls = 0
        last = 40

        def __call__(self, method, url, headers, body):
            self.polls += 1
            return (503, b"busy") if self.polls <= self.last else super().__call__(method, url, headers, body)
    code, api, _, _, said = run_off_with([[GREEN_L, GREEN_W]], api=ForAWhile())
    out.append(("stop: a passed run, and the API back in time: green, nothing to report", code == 0 and "::error::" not in said and api.deleted == ["dpl_x"]))
    down = FakeLatitude()
    down.outage = lambda: True
    code, _, _, _, said = run_off_with([[GREEN_L, GREEN_W]], api=down)
    out.append(("stop: a passed run, and a node may be on: the job is red, and says it may be billing and that the build has its own results",
                code == 1 and "::error::" in said and "billing" in said and "build" in said))
    one = DeleteFails()
    clock = Clock()
    one.failing, one.until = "dpl_x", 10 ** 9
    code, api, _, _, said = run_off_with([[GREEN_L, GREEN_W]], api=one)
    out.append(("stop: the node refused: red, naming it", code == 1 and [n["name"] for n in api.nodes] == ["ci5a1x"] and "ci5a1x" in said and "billing" in said))
    return out


def group_prefetch():
    """The node fetches the Mozilla tag right after node_bootstrap.sh, before its runners
    exist: on 04/11 the first leg spent 110-180 s fetching it from GitHub after its job
    had started. Only a head start: whoever waits on it falls back to GitHub."""
    out = []
    upstream = {"mozilla_repository": "https://github.com/mozilla-firefox/firefox", "mozilla_tag": "FIREFOX_159_0_RELEASE"}
    api, gh, ssh = FakeLatitude(), FakeGitHub(), FakeSsh()
    clock = Clock()
    latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")
        quiet(t.provision, latitude, github, 5, 1, lambda host, port: ssh, lambda: key, clock, clock.sleep, upstream=upstream)
    sent = [(c, s) for c, s in ssh.calls if c == "bash -s" and s and b"mozilla.git" in s]
    order = [c for c, _ in ssh.calls if isinstance(c, str)]
    out.append(("prefetch: the node fetches the series' tag after node_bootstrap.sh and before node_runner.sh, once",
                len(sent) == 1 and b"FIREFOX_159_0_RELEASE" in sent[0][1] and b"mozilla-firefox/firefox" in sent[0][1]
                and order.index("cat > /usr/local/sbin/node_runner.sh") > max(i for i, c in enumerate(order) if c == "bash -s")))
    api, gh, ssh = FakeLatitude(), FakeGitHub(), FakeSsh()
    got, error = provisioned(api, gh, ssh)
    out.append(("prefetch: without the series' UPSTREAM.json nothing is fetched, and the node comes up as before",
                got and not any(s and b"mozilla.git" in s for c, s in ssh.calls if c == "bash -s")))
    workflow_text = (HERE.parent.parent / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    out.append(("prefetch: the node job passes the series' UPSTREAM.json", "provision" in workflow_text and "--upstream .series/UPSTREAM.json" in workflow_text))
    if not sys.platform.startswith("linux"):
        print("skip  prefetch: bash and git, Linux only")
        return out
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = tmp / "firefox"
        subprocess.run(["git", "init", "-q", str(src)], check=True)
        (src / "a.txt").write_text("a\n", encoding="utf-8")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        for args in (["add", "a.txt"], ["commit", "-qm", "x"], ["tag", "TAG"]):
            subprocess.run(["git", "-C", str(src), *args], check=True, env=env)
        shared = tmp / "shared"
        for repository, tag in (("file://" + str(src), "TAG"), ("file://" + str(tmp / "gone"), "TAG")):
            done = subprocess.run(["bash", "-s"], input=t.prefetch_script(repository, tag, str(shared)), capture_output=True, text=True, timeout=30)
            for _ in range(100):
                if any((shared / "mozilla.git" / ("%s-TAG" % s)).exists() for s in ("fetched", "failed")):
                    break
                subprocess.run(["sleep", "0.1"])
            state = sorted(p.name for p in (shared / "mozilla.git").glob("*-TAG"))
            if "gone" not in repository:
                tagged = subprocess.run(["git", "-C", str(shared / "mozilla.git"), "rev-parse", "TAG"], capture_output=True, text=True).returncode == 0
                out.append(("prefetch: for real, it returns at once and the fetch marks itself whole (%s)" % state,
                            done.returncode == 0 and state == ["fetched-TAG"] and tagged
                            and oct((shared).stat().st_mode & 0o7777) == "0o1777"))
            else:
                out.append(("prefetch: a fetch that fails marks itself failed, and a new one starts clean (%s)" % state,
                            done.returncode == 0 and state == ["failed-TAG"]))
    return out


def group_bootstrap():
    out = []
    bootstrap = (HERE / "node_bootstrap.sh").read_text(encoding="utf-8")
    download = bootstrap.index('fetch "$NODE_DEPS_URL"')
    out.append(("bootstrap: curl and CA certificates from the image's sources only when missing, before the archive's download (a fresh node had none)",
                0 < bootstrap.index("if ! command -v curl") < bootstrap.index("ca-certificates curl") < download))
    out.append(("bootstrap: apt's one source is the archive's, on disk; nothing names a mirror (the snapshot was down most of 2026-11-04)",
                'echo "deb [trusted=yes] file:$DEPS/debs ./" > /etc/apt/sources.list' in bootstrap
                and "/etc/apt/sources.list.d/*.sources" in bootstrap and "ubuntu.com" not in bootstrap
                and download < bootstrap.index("> /etc/apt/sources.list") < bootstrap.index('"${PACKAGES[@]}"')))
    out.append(("bootstrap: a failed index stops it, by name",
                bootstrap.count("apt-get update -qq -o APT::Update::Error-Mode=any") == 2 and "apt-get update -qq\n" not in bootstrap))
    out.append(("bootstrap: every download by version and sha256, nothing else fetched, the runner's version pinned",
                all(v in bootstrap for v in ("RUNNER_VERSION=", "RUNNER_SHA256=", "NODE_SHA256=", "RUSTUP_SHA256=", "NODE_DEPS_SHA256="))
                and bootstrap.count("fetch \"https://") == 3 and bootstrap.count('fetch "$NODE_DEPS_URL" "$NODE_DEPS_SHA256"') == 1
                and "cmake -S" not in bootstrap and "KhronosGroup" not in bootstrap))
    as_runner = next(l for l in bootstrap.splitlines() if l.startswith("as_runner()"))
    out.append(("bootstrap: its checks see the PATH the job will have, where glslang 14 comes before Ubuntu's 11.8",
                "/usr/local/bin:" in as_runner and as_runner.index("/usr/local/bin") < as_runner.index("/usr/bin")
                and "/usr/local/bin:/usr/sbin:/usr/bin" in bootstrap.split("> $R/.path")[0].rsplit("echo ", 1)[1]))
    out.append(("bootstrap: the job's user has no sudo, and the script stops if it finds one",
                "NOPASSWD" not in bootstrap and "rm -f /etc/sudoers.d/runner" in bootstrap
                and '! as_runner sudo -n true 2>/dev/null || missing+=' in bootstrap))
    out.append(("names: run ids of today fit in a hostname", t.node_name(41647921995, 12) == "ci41647921995a12x" and len(t.node_name(41647921995, 12)) <= 20
                and t.runner_name(41647921995, 12, W) == "ci41647921995a12w"))
    out.append(("labels: one per leg, with the run and the attempt", t.leg_label(41647921995, 2, W) == "latitude-41647921995-2-win-x86_64"))
    out.append(("time: Latitude's +00:00 and GitHub's Z are one time", t.parse_time("2026-11-03T14:35:38+00:00") == t.parse_time("2026-11-03T14:35:38Z") is not None))
    runner = (HERE / "node_runner.sh").read_text(encoding="utf-8")
    out.append(("runner: as many open files as on GitHub's runners, not a login's 1024, from before it starts",
                "ulimit -n 65536" in runner and runner.index("ulimit -n 65536") < runner.index("./run.sh")))
    out.append(("bootstrap: the system it is written for is the one the node asks for",
                "= noble ]" in bootstrap and "24_04" in t.OPERATING_SYSTEM and "jammy" not in bootstrap and "/etc/apt/sources.list.d/*.sources" in bootstrap))
    out.append(("time: decimals and none", t.parse_time("2026-11-03T14:35:38.39446Z") == t.parse_time("2026-11-03T14:35:38Z") and t.parse_time("nonsense") is None))
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
    """Finding 2: the API's list alone said lost, after 5 minutes."""
    out = []
    then_done = [[RUN_L, RUN_W]] * 40 + [[GREEN_L, GREEN_W]]
    result, gh, api, _, _, _ = watched(list(then_done), nodes="")
    # With no row and no address kept, the list is all there is: the other witness is the address it saw.
    out.append(("list: a node gone from the list from the first look, with nothing else to ask, and its leg running: lost at the limit", result == "lost" and len(gh.dispatches) == 1))

    class ListLosesIt(FakeLatitude):
        polls = 0

        def __call__(self, method, url, headers, body):
            if "/servers?" in url and method == "GET":
                self.polls += 1
                if self.polls > 2:
                    self.nodes = [n for n in self.nodes if n["id"] != "dpl_x"]
            return super().__call__(method, url, headers, body)
    result, gh, api, _, _, _ = watched(list(then_done), api=ListLosesIt())
    out.append(("list: gone for 40 minutes with sshd answering and a leg running: not lost", result == "done" and gh.dispatches == []))
    result, gh, api, _, _, _ = watched(list(then_done), status="pending")
    out.append(("list: another status with sshd answering: not lost", result == "done" and gh.dispatches == []))
    result, gh, api, _, _, _ = watched(list(then_done), rows=(dict(ROW_X, PodPhase="x: Pending"),))
    out.append(("list: a pod not Running from the first look: no address, the leg left to the list", result == "lost"))
    result, gh, _, _, _, _ = watched(list(then_done), answering=lambda s: s >= 5 * 60)
    out.append(("both: down for the old 5 minutes, then sshd back: not lost", result == "done" and gh.dispatches == []))
    out.append(("the limit is at least 15 minutes", t.DOWN_LIMIT_S >= 15 * 60))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=1)
    almost = t.leg_state(RUN_L, machine, NOW + t.DOWN_LIMIT_S - 1)
    enough = t.leg_state(RUN_L, machine, NOW + t.DOWN_LIMIT_S)
    out.append(("both: not a second before the limit, and lost at it", almost is None and enough == "lost"))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=None, answers=False, steps_done=1)
    out.append(("evidence: a leg running, sshd silent and the API silent too: that may be the API, the leg is left alone",
                t.leg_state(RUN_L, machine, NOW + 3600) is None))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=1)
    machine.see(NOW + 600, listed=False, answers=False, steps_done=2)
    out.append(("evidence: a leg that finished a step says the machine lives, and the wait starts again",
                machine.down_since is None and t.leg_state(RUN_L, machine, NOW + t.DOWN_LIMIT_S) is None))
    out.append(("status: the string the API really answers (measured: deploying, then on), read as it is", t.RUNNING == "on"))
    live = FakeLatitude()
    live.nodes = listed()
    clock = Clock()
    machines = {L: t.Machine(), W: t.Machine()}
    seen = []

    def banner(host, port):
        seen.append(host)
        return True
    quiet(t.look_at_machines, t.Latitude(PROJECT, KEY, live, clock.sleep), 5, 1, machines, {}, banner, NOW)
    out.append(("status: the one node the real API calls on is listed, and both legs see it, at its one address, each asking it",
                machines[L].listed is True and machines[W].listed is True and machines[L].address == machines[W].address == ("1.1.1.1", 22)
                and machines[L].down_since is None and machines[W].down_since is None and seen == ["1.1.1.1", "1.1.1.1"]))
    other = FakeLatitude(rows=(ROW_X, dict(ROW_X, BaseID="dpl_y", NodePublicIP="2.2.2.2")))
    other.nodes = [{"id": "dpl_y", "name": "ci6a1x", "status": t.RUNNING}, {"id": "dpl_z", "name": "ci5a2x", "status": t.RUNNING}]
    machines, seen = {L: t.Machine(), W: t.Machine()}, []
    quiet(t.look_at_machines, t.Latitude(PROJECT, KEY, other, clock.sleep), 5, 1, machines, {}, banner, NOW)
    out.append(("status: another run's node, or another attempt's, is not this run's machine",
                machines[L].listed is False and machines[W].listed is False and machines[L].address is None and seen == []))
    live.list_status = 500
    machines = {L: t.Machine(("h", 1))}
    quiet(t.look_at_machines, t.Latitude(PROJECT, KEY, live, clock.sleep), 5, 1, machines, {}, lambda h, p: True, NOW)
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
    out.append(("silence: the API says running, sshd silent, GitHub says in progress: lost at the limit, it does not wait for GitHub",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, _, said, _ = watched([[DEAD_L, GREEN_W]])
    out.append(("a leg with no failed step on a machine that answers: not a lost machine, no re-launch, it says so, and the node is left for stop-node",
                result == "done" and gh.dispatches == [] and "no re-launch" in said and api.deleted == []))
    result, gh, api, _, said, _ = watched([[GREEN_L, DEAD_W]])
    out.append(("the same the other way round: the node kept, though the other leg needs it no more",
                result == "done" and gh.dispatches == [] and W in said and api.deleted == []))
    result, gh, _, _, _, _ = watched([[RED_L, GREEN_W]], answering=False, nodes="")
    out.append(("a compile error is a defect, whatever the machine does: no re-launch", result == "done" and gh.dispatches == []))
    result, gh, _, _, _, _ = watched([[job(LINUX, conclusion="cancelled", steps=("success", "cancelled")), job(WIN, conclusion="cancelled")]])
    out.append(("a run cancelled by hand does not come back", result == "done" and gh.dispatches == []))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=0)
    out.append(("no job listed yet is not lost, whatever the machine does", t.leg_state(None, machine, NOW + 3600) is None))
    return out


def group_two_legs():
    """The two legs at once, on one node: a leg that ends does not take the node from the
    other, and a machine lost is lost to both."""
    out = []
    runs = [[RUN_L, RUN_W]] * 3
    result, gh, api, log, _, clock = watched(runs + [[GREEN_L, RUN_W]] * 3 + [[GREEN_L, GREEN_W]])
    first_only, _, early, _, _, _ = watched(runs + [[GREEN_L, RUN_W]], limit=10 * 60)
    out.append(("both healthy: done, with no re-launch, the node deleted once both legs ended, not when the first did",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and log == ["delete dpl_x"]
                and first_only == "continue" and early.deleted == []))
    result, gh, api, log, _, _ = watched(runs + [[RED_L, RUN_W]] * 3 + [[RED_L, GREEN_W]])
    out.append(("a red leg, no loss: the other goes on, on the node, no re-launch, this job does not end the run",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and log == ["delete dpl_x"]))

    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]], answering=False)
    out.append(("loss of the machine mid-build: both legs lose it together, the node goes at the limit, then one re-launch, this run's cancel last",
                result == "lost" and log == ["delete dpl_x", "dispatch", "cancel"] and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300
                and len(gh.dispatches) == 1 and len(gh.cancels) == 1 and "the machine of %s and %s was lost" % (L, W) in said))
    result, gh, api, log, said, clock = watched([[DEAD_L, RUN_W]], answering=False)
    out.append(("loss: GitHub has failed one leg's job and still says the other's is in progress: both lost at the limit, it does not wait for GitHub",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, log, said, clock = watched([[GREEN_L, RUN_W]], answering=False)
    out.append(("a leg already green, then the machine lost: re-launch of the whole run, the green one comes back through the cache; the annotation names only the lost leg",
                result == "lost" and gh.dispatches[0]["inputs"]["relaunch_of"] == "5" and api.deleted == ["dpl_x"]
                and "::warning::the machine of %s was lost" % W in said and "the machine of %s" % L not in said))
    result, gh, api, log, said, clock = watched([[RUN_L, GREEN_W]], answering=False)
    out.append(("the same the other way round", result == "lost" and len(gh.dispatches) == 1 and api.deleted == ["dpl_x"]
                and "::warning::the machine of %s was lost" % L in said and "the machine of %s" % W not in said))
    result, gh, api, log, said, clock = watched([[RED_L, RUN_W]], answering=False)
    out.append(("a leg ended red, then the machine lost: no re-launch, it would fail again, the run fails and says why",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "no re-launch" in said and "%s ended red" % L in said
                and api.deleted == ["dpl_x"] and clock.now >= t.DOWN_LIMIT_S))
    result, gh, api, log, said, clock = watched([[GREEN_L, RUN_W]], answering=False, relaunch_of="4")
    out.append(("second loss, a leg already green: the run fails, no third try",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "second loss" in said))
    result, gh, api, log, said, clock = watched([[RUN_L, RUN_W]] * 5 + [[RUN_L, CANCELLED_W]], answering=False)
    out.append(("the machine silent, then a person cancels: no re-launch, the node off",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and api.deleted == ["dpl_x"]))
    result, gh, api, log, said, clock = watched([[QUEUED_L, QUEUED_W]], answering=False)
    out.append(("the machine lost before either leg started: lost at the limit, one re-launch, both named",
                result == "lost" and api.deleted == ["dpl_x"] and len(gh.dispatches) == 1 and L in said and W in said
                and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, log, said, clock = watched([[QUEUED_L, RUN_W]] * (int(t.STUCK_S / 60) + 30) + [[QUEUED_L, GREEN_W]])
    out.append(("a runner not coming for one leg: the node stays for the healthy leg, which finishes, then the node goes and the run fails, no re-launch",
                result == "failed" and log == ["delete dpl_x", "cancel"] and gh.dispatches == [] and "not coming" in said and clock.now >= t.STUCK_S + 30 * 60))
    result, gh, api, log, said, _ = watched([[RUN_L, RUN_W]], answering=False, api=_refuses_delete_of("dpl_x", first=3))
    out.append(("the node's delete refused for a while: asked again until it is gone, and only then the dispatch",
                result == "lost" and api.deleted == ["dpl_x"] and log == ["delete dpl_x", "dispatch", "cancel"] and api.nodes == [] and len(gh.dispatches) == 1))
    # 1 refusal: the watch's own delete, on the very poll that concludes.
    result, gh, api, log, said, _ = watched([[RUN_L, GREEN_W]], answering=False, api=_refuses_delete_of("dpl_x", first=1))
    out.append(("the node still on when the run concludes: free before the dispatch all the same",
                result == "lost" and log == ["delete dpl_x", "dispatch", "cancel"] and api.nodes == []))
    result, gh, api, log, said, _ = watched([[QUEUED_L, RUN_W]] * (int(t.STUCK_S / 60) + 30) + [[QUEUED_L, GREEN_W]], api=_refuses_delete_of("dpl_x", first=4))
    out.append(("the same when the run fails instead: no node left on", result == "failed" and api.nodes == []))
    return out


def _refuses_delete_of(node_id, first):
    class Refuses(FakeLatitude):
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
    result, gh, api, log, _, _ = watched([[DEAD_L, GREEN_W]], answering=False)
    out.append(("re-launch: the node gone before the dispatch, seen gone, whichever leg was lost",
                result == "lost" and log == ["delete dpl_x", "dispatch", "cancel"] and api.nodes == []))
    out.append(("re-launch: the refs are explicit, a pushed tag's empty ones no longer pick a default",
                t.relaunch_inputs(5, "release", "firefox-55", "gha", "engine/firefox-55", "v1", "true", "*") ==
                {"kind": "release", "compile_cache": "gha", "core_ref": "engine/firefox-55", "wrapper_ref": "v1", "relaunch_of": "5",
                 "release_tag": "firefox-55", "gtests": "true", "gtest_filter": "*"}))
    for bad in (("", "v1"), ("main", "")):
        try:
            t.relaunch_inputs(5, "candidate", "", "gha", *bad)
            refused = False
        except t.NodeError:
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
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and api.deleted == ["dpl_x"] and "not coming" in said
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
    result, gh, api, log, _, clock = watched([[GREEN_L, RUN_W]] * 3 + [[GREEN_L, GREEN_W]])
    out.append(("a next watch job: a leg green before it began: ended at once, not read as lost, and the node goes when the other ends",
                result == "done" and gh.dispatches == [] and clock.now < t.DOWN_LIMIT_S and log == ["delete dpl_x"]))
    result, gh, api, log, _, clock = watched([[RED_L, RUN_W]] * 3 + [[RED_L, GREEN_W]])
    out.append(("a next watch job: a compile error before it began: the same", result == "done" and gh.dispatches == [] and log == ["delete dpl_x"]))
    result, gh, api, log, _, clock = watched([[GREEN_L, GREEN_W]], nodes="")
    out.append(("a next watch job: both legs ended and the node already gone: done at once, nothing read as lost",
                result == "done" and gh.dispatches == [] and clock.now == 0))
    result, gh, api, log, said, clock = watched([[DEAD_L, RUN_W]] * 40 + [[DEAD_L, GREEN_W]], nodes="")
    out.append(("a next watch job: the node gone (deleted, or lost between two jobs) with a leg dead and the other still said to run: seen again from what GitHub and the API say, both lost at the limit, one re-launch",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, log, said, clock = watched([[DEAD_L, RUN_W]] * 3 + [[DEAD_L, GREEN_W]])
    out.append(("a next watch job: a dead runner's leg the last one left with its node, the machine answering: no loss, the node kept for stop-node",
                result == "done" and gh.dispatches == [] and api.deleted == []))
    return out


def group_sweep():
    out = []

    def swept(nodes, run_status=None, github=True, jobs=None, **kwargs):
        clock = Clock()
        api, gh = FakeLatitude(), FakeGitHub()
        api.nodes = nodes
        gh.run_status = run_status or {}
        gh.jobs_answers = [jobs if jobs is not None else [RUN_L, RUN_W]]
        latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
        hub = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep) if github else None
        return quiet(t.sweep, latitude, hub, NOW, **kwargs)[0], api
    young = "2026-11-03T11:00:00Z"
    old = "2026-11-03T02:00:00Z"
    by_hand = {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "created_at": "2020-01-01T00:00:00Z"}
    other = {"id": "dpl_c", "name": "cinema", "created_at": "2020-01-01T00:00:00Z"}
    hand = {"id": "dpl_h", "name": "cihandbuild1", "created_at": "2020-01-01T00:00:00Z"}
    node = {"id": "d1", "name": "ci5a1x", "created_at": young}
    node_6 = {"id": "d6", "name": "ci6a1x", "created_at": young}
    gone, api = swept([by_hand, other, hand, dict(node, created_at=old), dict(node_6, created_at=old)], {"5": "in_progress", "6": "in_progress"})
    out.append(("sweep: past the limit ours go even if their run says it is alive; by hand or another name never, however old",
                gone == ["ci5a1x", "ci6a1x"] and api.deleted == ["d1", "d6"]))
    out.append(("sweep: the limit lets one leg of 350 minutes and the node job finish, and no more than that and an hour",
                t.GUARD_HOURS * 60 >= 350 + 45 + t.STUCK_S / 60 and t.GUARD_HOURS * 60 <= 350 + 45 + t.STUCK_S / 60 + 70))
    out.append(("sweep: a forgotten node costs less than the account's first limit of $100, even two runs' at once", t.GUARD_HOURS * 2 * t.MAX_PRICE_USD_HOUR < 100))
    gone, api = swept([dict(node, created_at="2026-11-03T02:00:00+00:00")], {"5": "in_progress"})
    out.append(("sweep: the API's own way of writing a time counts", gone == ["ci5a1x"]))
    clock = Clock()
    api = FakeLatitude()
    api.nodes = [dict(node)]
    api.keys = [{"id": "k%d" % i, "attributes": {"name": n}} for i, n in enumerate(("ci4a1x", "ci5a1x", "laptop"))]
    gh = FakeGitHub()
    gh.run_status, gh.jobs_answers = {"5": "in_progress"}, [[RUN_L, RUN_W]]
    quiet(t.sweep, t.Latitude(PROJECT, KEY, api, clock.sleep), t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep), NOW)
    out.append(("sweep: a key whose node is gone goes; the key of a node still on, and a key by hand, never", api.keys_deleted == ["k0"]))
    gone, api = swept([node], {"5": "in_progress"})
    out.append(("sweep: a young node whose run still needs it stays", gone == [] and api.deleted == []))
    gone, api = swept([node], {"5": "completed"})
    out.append(("sweep: a young node whose run has finished goes", gone == ["ci5a1x"]))
    gone, api = swept([node], {"5": "in_progress"}, jobs=[GREEN_L, RUN_W])
    out.append(("sweep: behind a dead watch: one leg over, the other still running on it: the node stays", gone == [] and api.deleted == []))
    gone, api = swept([node], {"5": "in_progress"}, jobs=[RUN_L, GREEN_W])
    out.append(("sweep: the same the other way round", gone == [] and api.deleted == []))
    gone, api = swept([node], {"5": "in_progress"}, jobs=[GREEN_L, GREEN_W])
    out.append(("sweep: both legs over, the run still going on with its gates: the node goes", gone == ["ci5a1x"] and api.deleted == ["d1"]))
    gone, api = swept([node], {"5": "in_progress"}, jobs=[])
    out.append(("sweep: no legs listed yet is not legs finished", gone == [] and api.deleted == []))
    gone, api = swept([node], {"5": 500})
    out.append(("sweep: GitHub's error is not 'the run has finished'", gone == [] and api.deleted == []))
    gone, api = swept([node, dict(node_6, created_at=old)], github=False)
    out.append(("sweep: from a machine with no GitHub, the age alone", gone == ["ci6a1x"] and api.deleted == ["d6"]))
    gone, api = swept([dict(node_6, created_at=old)], dry_run=True)
    out.append(("sweep: a dry run deletes nothing", gone == ["ci6a1x"] and api.deleted == []))
    force_cancelled = [job(LINUX, conclusion="cancelled", steps=("success", None, None), runner="ci5a1l"), job(WIN, conclusion="cancelled"),
                       job("watch-1", conclusion="cancelled")]
    gone, api = swept([node], {"5": "completed"}, jobs=force_cancelled)
    out.append(("sweep: a force-cancel leaves no stop-node: the run over, its node goes at the next sweep, however young", gone == ["ci5a1x"]))
    gone, api = swept([node], {"5": "queued"}, jobs=force_cancelled)
    out.append(("sweep: and before the run reads completed, its legs having ended are enough", gone == ["ci5a1x"]))
    old_design = [{"id": "d1l", "name": "ci5a1l", "created_at": young}, {"id": "d1w", "name": "ci5a1w", "created_at": young}]
    gone, api = swept(list(old_design), {"5": "in_progress"}, jobs=[GREEN_L, RUN_W])
    out.append(("sweep: nodes of the one-per-leg design behind a dead watch: one leg over is not enough for either", gone == []))
    gone, api = swept(list(old_design), {"5": "in_progress"}, jobs=[GREEN_L, GREEN_W])
    out.append(("sweep: and both legs over: both go", gone == ["ci5a1l", "ci5a1w"]))
    return out


def group_secrets():
    out = []

    class Leaky(FakeLatitude):
        def __call__(self, method, url, headers, body):
            if url.endswith("/ssh_keys"):
                return 403, ("key %s project %s refused" % (KEY, PROJECT)).encode()
            return super().__call__(method, url, headers, body)
    clock = Clock()
    try:
        t.Latitude(PROJECT, KEY, Leaky(), clock.sleep).ssh_keys()
        message = ""
    except t.NodeError as exc:
        message = str(exc)
    out.append(("secrets: an error names neither the key nor the project, in the URL or the answer",
                message and KEY not in message and PROJECT not in message))
    address = t.Latitude(PROJECT, KEY, FakeLatitude(), clock.sleep).address("dpl_x")
    out.append(("address: host and port of the server, nothing else", address == ("1.1.1.1", 22)))
    pending = dict(ROW_X, PodPhase="x: Pending")
    out.append(("address: a server with no address yet has none", t.Latitude(PROJECT, KEY, FakeLatitude(rows=(pending,)), clock.sleep).address("dpl_x") is None))
    out.append(("address: a server the API no longer knows has none", t.Latitude(PROJECT, KEY, FakeLatitude(rows=()), clock.sleep).address("dpl_x") is None))
    flaky = []

    def transport(method, url, headers, body):
        flaky.append(url)
        return (503, b"busy") if len(flaky) < 3 else (200, b'{"data":[]}')
    out.append(("reads: a busy API is asked again", t.Latitude(PROJECT, KEY, transport, clock.sleep).nodes() == [] and len(flaky) == 3))
    source = (HERE / "latitude_node.py").read_text(encoding="utf-8")
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
    """node_runner.sh, as it runs on the node, with the system's commands faked:
    systemd-run and unshare run what follows their "--" (unshare undoes the mounts when
    it ends, as the end of a namespace would), mount --bind points its target at its source,
    runuser goes straight to the command, and the users are a file. The two legs run one
    after the other: without a real namespace, two at once would share the mount points."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        template, home, toolcache, tmp_dir = tmp / "template", tmp / "home", tmp / "toolcache", tmp / "tmpdir"
        root, cgroup, bin_dir, guard_dir = tmp / "legs", tmp / "cgroup", tmp / "bin", tmp / "root_owned"
        for d in (template / "actions-runner", home, toolcache, tmp_dir, cgroup, bin_dir, guard_dir, root / "l" / "home"):
            d.mkdir(parents=True)
        (toolcache / "node").write_text("v20")
        (root / "l" / "home" / "stale").write_text("an earlier runner's")
        meminfo = tmp / "meminfo"
        meminfo.write_text("MemTotal:       100663296 kB\nMemFree:        1 kB\n")
        trace, kills, record, users, mounts = tmp / "trace", tmp / "kills", tmp / "record", tmp / "users", tmp / "mounts"
        run = template / "actions-runner" / "run.sh"
        run.write_text("#!/bin/bash\n"
                       'echo "start $ACTIONS_RUNNER_INPUT_JITCONFIG work=$(ls -d "$HOME/work" | wc -l) stale=$(ls "$HOME/stale" 2>/dev/null | wc -l) dir=$(pwd -P)" >> "%s"\n'
                       'env > "%s/env-$ACTIONS_RUNNER_INPUT_JITCONFIG"\n'
                       'sleep 0.3\necho "end $ACTIONS_RUNNER_INPUT_JITCONFIG" >> "%s"\nexit 3\n' % (trace, tmp, trace),
                       newline="\n")
        run.chmod(0o755)
        before_dashes = 'a=(); while [ "$1" != "--" ]; do a+=("$1"); shift; done; shift\n'
        fakes = {"systemd-run": "#!/bin/bash\n" + before_dashes + 'echo "systemd-run ${a[*]}" >> "%s"\nexec "$@"\n' % record,
                 "unshare": "#!/bin/bash\n" + before_dashes + 'echo "unshare ${a[*]}" >> "%s"\n"$@"; code=$?\n'
                            'if [ -f "%s" ]; then tac "%s" | while IFS= read -r dst; do rm -f "$dst"; mv "$dst.under" "$dst"; done; rm -f "%s"; fi\n'
                            'exit $code\n' % (record, mounts, mounts, mounts),
                 "mount": '#!/bin/bash\necho "mount $*" >> "%s"\n'
                          'if [ "$1" = "--bind" ]; then mv "$3" "$3.under" && ln -s "$2" "$3" && echo "$3" >> "%s"; fi\n' % (record, mounts),
                 "runuser": '#!/bin/bash\nu=""; while [ "$1" != "--" ]; do [ "$1" = "-u" ] && u="$2"; shift; done; shift\n'
                            'echo "runuser $u" >> "%s"\nexec "$@"\n' % record,
                 "id": '#!/bin/bash\n[ "$1" = "-u" ] && grep -qx "$2" "%s" 2>/dev/null\n' % users,
                 "useradd": '#!/bin/bash\necho "useradd $*" >> "%s"\necho "${@: -1}" >> "%s"\n' % (record, users),
                 "pkill": '#!/bin/bash\necho "$*" >> "%s"\nexit "${FAKE_PKILL_EXIT:-0}"\n' % kills,
                 "install": '#!/bin/bash\nfor a; do d="$a"; done; mkdir -p "$d"\n',
                 "chown": '#!/bin/bash\necho "$*" >> "%s/chowns"\n' % tmp}
        for name, text in fakes.items():
            (bin_dir / name).write_text(text, newline="\n")
            (bin_dir / name).chmod(0o755)
        log = tmp / "log"
        env = dict(os.environ, PATH="%s%s%s" % (bin_dir, os.pathsep, os.environ["PATH"]), NODE_RUNNER_TEMPLATE=str(template),
                   NODE_RUNNER_HOME=str(home), NODE_RUNNER_TOOLCACHE=str(toolcache), NODE_RUNNER_TMP=str(tmp_dir),
                   NODE_RUNNER_CGROUP=str(cgroup), NODE_RUNNER_ROOT=str(root), NODE_RUNNER_MEMINFO=str(meminfo),
                   NODE_RUNNER_LOG=str(log), NODE_RUNNER_GUARD=str(guard_dir / "job_guard"),
                   NODE_RUNNER_SHARED=str(tmp / "shared"), **leaks)
        script = str(HERE / "node_runner.sh")

        def start(config, *args):
            return subprocess.run(["bash", script, *args], input=config, env=env, capture_output=True, text=True, timeout=30)

        def ended(leg):
            leg_log = Path("%s-%s.log" % (log, leg))
            for _ in range(100):
                if leg_log.exists() and leg_log.read_text().count("exited with") >= 1:
                    return leg_log.read_text()
                subprocess.run(["sleep", "0.1"])
            return leg_log.read_text() if leg_log.exists() else ""
        refused = [start("", "5", "l"), start("CFG\n"), start("CFG\n", "5;rm", "l"), start("CFG\n", "5"),
                   start("CFG\n", "5", "lw"), start("CFG\n", "5", "L"), start("CFG\n", "5", "1"), start("CFG\n", "5", "l;rm")]
        started = {}
        for leg in ("l", "w"):
            started[leg] = start("CFG-%s\n" % leg, "5", leg)
            started[leg + "_at_once"] = not trace.exists() or "end CFG-%s" % leg not in trace.read_text()
            started[leg + "_log"] = ended(leg)

        def hooked(leg, run_id, pkill_exit="0"):
            kills.write_text("")
            done = subprocess.run(["bash", "-e", "%s-%s.sh" % (guard_dir / "job_guard", leg)], capture_output=True, text=True,
                                  env={"GITHUB_RUN_ID": run_id, "PATH": env["PATH"], "FAKE_PKILL_EXIT": pkill_exit})
            return done, kills.read_text().splitlines()
        hooks = {leg: {"ok": hooked(leg, "5"), "other": hooked(leg, "6"), "no_pkill": hooked(leg, "6", "2")[0].returncode,
                       "mode": Path("%s-%s.sh" % (guard_dir / "job_guard", leg)).stat().st_mode & 0o111}
                 for leg in ("l", "w")}
        return {"started": started, "refused": refused, "trace": trace.read_text().splitlines(),
                "env": {leg: (tmp / ("env-CFG-%s" % leg)).read_text() for leg in ("l", "w")},
                "record": record.read_text().splitlines(), "chowns": (tmp / "chowns").read_text(), "hooks": hooks,
                "guard": str(guard_dir / "job_guard"), "home": str(home), "toolcache": str(toolcache), "tmp_dir": str(tmp_dir),
                "cgroup": str(cgroup), "root": str(root), "dirs": {leg: str((root / leg / "home" / "actions-runner").resolve()) for leg in ("l", "w")},
                "copied_toolcache": all((root / leg / "toolcache" / "node").exists() for leg in ("l", "w")),
                "guard_in_homes": any(root.rglob("job_guard*")) or any(template.rglob("job_guard*")),
                "mounts_left": home.is_symlink() or toolcache.is_symlink() or tmp_dir.is_symlink(),
                "shared": str(tmp / "shared"), "shared_made": (tmp / "shared").is_dir()}


def group_node_deps():
    """The node's packages come from one archive of ours, by name and sha256: Ubuntu's
    snapshot service, where they came from at every start, was down for most of
    2026-11-04 (503, 502) and three candidates could not prepare their node."""
    out = []
    bootstrap = (HERE / "node_bootstrap.sh").read_text(encoding="utf-8").replace("\r\n", "\n")
    build = (HERE / "node_deps_build.sh").read_text(encoding="utf-8").replace("\r\n", "\n")
    name = re.search(r"^NODE_DEPS=(node-deps-\d{8}-[0-9a-f]{12})$", bootstrap, re.M)
    sha = re.search(r"^NODE_DEPS_SHA256=([0-9a-f]{64})$", bootstrap, re.M)
    out.append(("node deps: the archive is named and pinned by sha256, and its URL is this repository's release asset, through the API",
                bool(name and sha) and re.search(r"^NODE_DEPS_ASSET=[0-9]+$", bootstrap, re.M) is not None
                and "https://api.github.com/repos/feder-cr/firefox_antidetect_patch/releases/assets/$NODE_DEPS_ASSET" in bootstrap))
    fetch = bootstrap.split("fetch() {", 1)[1].split(chr(10) + "}", 1)[0]
    out.append(("node deps: every GitHub release file comes through the API by asset id first, asked for its bytes, and the direct address second, in rounds (2026-11-08: releases/download answered 504 for hours, the API now and then, two candidates lost their node)",
                re.search(r"^RUNNER_ASSET=[0-9]+$", bootstrap, re.M) is not None
                and 'fetch "https://api.github.com/repos/actions/runner/releases/assets/$RUNNER_ASSET"' in bootstrap
                and 'fetch "$NODE_DEPS_URL" "$NODE_DEPS_SHA256" /tmp/node-deps.tar "$NODE_DEPS_WEB_URL"' in bootstrap
                and bootstrap.count("releases/download") == 2
                and '-H "Accept: application/octet-stream"' in fetch and 'for url in "$1" ${4:+"$4"}' in fetch
                and "for round in 1 2 3" in fetch and fetch.index("sha256sum -c") < fetch.index("return 0")))
    out.append(("node deps: the builder names it as the bootstrap reads it, a plain tar a fresh node can open",
                'NAME="node-deps-$(date -u +%Y%m%d)-$HASH"' in build and '-cf "$OUT/$NAME.tar"' in build and "zstd" not in build))
    out.append(("node deps: glslang comes built in the archive, at the version and from the source the bootstrap pins",
                'cp -a "$DEPS/glslang-root/usr/local/." /usr/local/' in bootstrap
                and "GLSLANG_VERSION=\"$(value GLSLANG_VERSION)\"" in build and "GLSLANG_SHA256=\"$(value GLSLANG_SHA256)\"" in build
                and "does not match its sha256" in build))
    if not sys.platform.startswith("linux"):
        print("skip  node deps: bash, Linux only")
        return out
    # The builder reads the bootstrap's list as bash reads it: the same packages, the
    # comments out, nothing more and nothing less.
    read = next(line for line in build.splitlines() if line.startswith("mapfile -t PACKAGES"))
    script = "\n".join([
        'BOOT="%s"' % (HERE / "node_bootstrap.sh"),
        'source <(sed -n "/^PACKAGES=(/,/^)/p" "$BOOT")',
        'WANT=("${PACKAGES[@]}"); unset PACKAGES',
        read,
        '[ "${WANT[*]}" = "${PACKAGES[*]}" ] && echo "same ${#PACKAGES[@]}"'])
    done = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30)
    said = done.stdout.split()
    out.append(("node deps: the builder's list is the bootstrap's (%s)" % (done.stdout.strip() or done.stderr.strip()[-200:]),
                len(said) == 2 and said[0] == "same" and int(said[1]) > 30))
    # The bootstrap's own fetch, against an archive that is not the pinned one: it stops, by name.
    begin = bootstrap.index("fetch() {")
    fetch = bootstrap[begin:bootstrap.index("\n}\n", begin) + 3]
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / "node-deps.tar"
        bad.write_bytes(b"not the archive")
        script = "\n".join([
            "set -euo pipefail",
            "FETCH_PAUSE=0",
            'die() { echo "node_bootstrap: ERROR: $*" >&2; exit 1; }',
            fetch,
            'fetch "file://%s" "%s" "%s/got.tar"' % (bad, "0" * 64, tmp)])
        done = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30)
    out.append(("node deps: an archive that is not the pinned one stops the bootstrap, by name",
                done.returncode != 0 and "does not match its sha256" in done.stderr
                and done.stderr.count("(round ") == 3))
    # The first address down, the second serving the file: it arrives, after one failure.
    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / "node-deps.tar"
        good.write_bytes(b"the archive")
        digest = hashlib.sha256(b"the archive").hexdigest()
        script = "\n".join([
            "set -euo pipefail",
            "FETCH_PAUSE=0",
            'die() { echo "node_bootstrap: ERROR: $*" >&2; exit 1; }',
            fetch,
            'fetch "file://%s/missing.tar" "%s" "%s/got.tar" "file://%s"' % (tmp, digest, tmp, good),
            'cat "%s/got.tar"' % tmp])
        done = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30)
    out.append(("node deps: the first address down, the second serving the file: the bootstrap goes on with it",
                done.returncode == 0 and done.stdout == "the archive" and done.stderr.count("(round 1)") == 1))
    # The runner's call as bash reads it: four words, the direct address last (a literal
    # backslash-n once made the fourth word "n" and the direct address a fifth, never tried).
    begin = bootstrap.index('  fetch "https://api.github.com/repos/actions/runner/')
    lines = []
    for line in bootstrap[begin:].splitlines():
        lines.append(line)
        if not line.endswith(chr(92)):
            break
    script = chr(10).join(["RUNNER_ASSET=1", "RUNNER_SHA256=s", "RUNNER_VERSION=9",
                           'fetch() { echo "$#"; printf %s "$4"; }'] + lines)
    done = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=30)
    words = done.stdout.splitlines()
    out.append(("node deps: the runner's call gives fetch four words, the direct address the fourth (%s)" % words,
                words[:1] == ["4"] and done.stdout.endswith("https://github.com/actions/runner/releases/download/v9/actions-runner-linux-x64-9.tar.gz")))
    return out


def group_runner_isolated():
    """node_runner.sh for real: systemd-run, unshare and the mounts, as root. Faked commands
    cannot see a mount the kernel refuses: candidate 41810835741 had both runners offline
    because the cgroup2 remount failed ("already mounted") and the runner never started."""
    out = []
    if not sys.platform.startswith("linux") or os.geteuid() != 0 or not shutil_which("systemd-run"):
        print("skip  runner, isolated: needs Linux, root and systemd")
        return out
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # The leg's user has to cross it, as it crosses /srv and /home on the node.
        tmp.chmod(0o755)
        for d in ("template/actions-runner", "jobhome", "tc", "tmpview"):
            (tmp / d).mkdir(parents=True)
        seen = tmp / "seen"
        seen.touch()
        seen.chmod(0o666)
        run = tmp / "template" / "actions-runner" / "run.sh"
        run.write_text("#!/bin/bash\n"
                       'echo "$(id -un) $HOME $(pwd) $SCCACHE_SERVER_PORT $ACTIONS_RUNNER_INPUT_JITCONFIG '
                       '$(cat /proc/self/cgroup) $(cat /sys/fs/cgroup/memory.max)" > "%s"\n' % seen, newline="\n")
        run.chmod(0o755)
        env = dict(os.environ, NODE_RUNNER_TEMPLATE=str(tmp / "template"), NODE_RUNNER_HOME=str(tmp / "jobhome"),
                   NODE_RUNNER_TOOLCACHE=str(tmp / "tc"), NODE_RUNNER_TMP=str(tmp / "tmpview"), NODE_RUNNER_ROOT=str(tmp / "legs"),
                   NODE_RUNNER_GUARD=str(tmp / "guard"), NODE_RUNNER_LOG=str(tmp / "log"))
        try:
            subprocess.run(["bash", str(HERE / "node_runner.sh"), "7", "q"], input="CFGQ\n", env=env, capture_output=True, text=True, timeout=60)
            for _ in range(200):
                if seen.exists() and seen.read_text().strip():
                    break
                subprocess.run(["sleep", "0.1"])
            got = seen.read_text().split() if seen.exists() else []
            log = (tmp / "log-q.log").read_text() if (tmp / "log-q.log").exists() else ""
        finally:
            subprocess.run(["userdel", "runner-q"], capture_output=True)
        out.append(("runner, isolated: it starts, as its leg's user, in its leg's home at the job's path, with its own port, its own cgroup and a memory ceiling (%s)" % (got or log[-200:]),
                    len(got) >= 7 and got[0] == "runner-q" and got[1] == str(tmp / "jobhome") and got[2] == str(tmp / "jobhome" / "actions-runner")
                    and got[3] == str(4300 + 17) and got[4] == "CFGQ" and got[5] == "0::/" and got[6].isdigit()))
    return out


def group_runner():
    out = []
    if not sys.platform.startswith("linux"):
        print("skip  runner: node_runner.sh runs on Linux only")
        return out
    leaks = {"CARGO_LEAK": "root", "CONTAINER_SECRET": "x", "ACTIONS_RUNNER_HOOK_JOB_COMPLETED": "/nope", "SCCACHE_SERVER_PORT": "4226"}
    r = runner_case(leaks)
    s = r["started"]
    out.append(("runner: returns at once, for each leg", s["l"].returncode == 0 and s["w"].returncode == 0 and s["l_at_once"] and s["w_at_once"]))
    out.append(("runner: no configuration, no run id or one that is not a number, refused",
                all(c.returncode != 0 for c in r["refused"][:3]) and "first argument" in r["refused"][2].stderr))
    out.append(("runner: the leg's letter is required, one lowercase letter and nothing else",
                all(c.returncode != 0 and "second argument" in c.stderr for c in r["refused"][3:])))
    out.append(("runner: one runner per leg, its config once, in its own copy of the prepared home, its work directory there, nothing of an earlier runner's",
                r["trace"] == ["start CFG-l work=1 stale=0 dir=%s" % r["dirs"]["l"], "end CFG-l",
                               "start CFG-w work=1 stale=0 dir=%s" % r["dirs"]["w"], "end CFG-w"]))
    out.append(("runner: how each ended is logged, in its own log", s["l_log"].count("exited with 3") == 1 and s["w_log"].count("exited with 3") == 1
                and "leg l" in s["l_log"] and "leg w" in s["w_log"]))
    allowed = {"HOME", "PATH", "LANG", "SCCACHE_SERVER_PORT", "ACTIONS_RUNNER_HOOK_JOB_STARTED", "ACTIONS_RUNNER_INPUT_JITCONFIG", "NODE_SHARED",
               "PWD", "OLDPWD", "SHLVL", "_"}
    envs = {leg: dict(l.split("=", 1) for l in text.splitlines() if "=" in l) for leg, text in r["env"].items()}
    out.append(("runner: an empty environment: nothing of root's reaches the job's start, neither CARGO_* nor anything else",
                all(not set(e) & (set(leaks) - {"SCCACHE_SERVER_PORT"}) and set(e) <= allowed for e in envs.values())))
    out.append(("runner: HOME, PATH and LANG are the job's, HOME at the same path for both legs, as sccache's key wants",
                all(e.get("LANG") == "C.UTF-8" and e.get("PATH", "").startswith("/home/runner/.cargo/bin:/opt/node/bin:") and e.get("HOME") == r["home"]
                    for e in envs.values())))
    ports = [envs[leg].get("SCCACHE_SERVER_PORT") for leg in ("l", "w")]
    out.append(("runner: each leg its own sccache port, never sccache's default nor root's", all(ports) and len(set(ports)) == 2 and "4226" not in ports))
    # Both legs fetched the same Mozilla tag and applied the same series, each for itself (04/11):
    # one directory both reach, writable by both users, where materialize does it once.
    runner_text = (HERE / "node_runner.sh").read_text(encoding="utf-8")
    out.append(("runner: both legs get the same shared directory for the source tree, made writable by both users",
                r["shared_made"] and all(envs[leg].get("NODE_SHARED") == r["shared"] for leg in ("l", "w"))
                and 'install -d -m 1777 "$SHARED"' in runner_text))
    record = r["record"]
    useradds = [l for l in record if l.startswith("useradd ")]
    uids = [l.split("--uid ", 1)[1].split()[0] for l in useradds if "--uid " in l]
    out.append(("runner: each leg its own user, made once, with its own uid, and the runner runs as it",
                [l.rsplit(" ", 1)[1] for l in useradds] == ["runner-l", "runner-w"] and len(set(uids)) == 2
                and [l for l in record if l.startswith("runuser ")] == ["runuser runner-l", "runuser runner-w"]))
    units = [l for l in record if l.startswith("systemd-run ")]
    # The whole machine less 4 GB, not half of it: the Windows gtest link alone needs more
    # than a half (44.8 GB when the memory watch killed it at 45.7, 2026-11-07).
    ceiling = "MemoryMax=%dM" % (100663296 // 1024 - 4096)
    out.append(("runner: each leg its own cgroup, its own unit, the machine less 4 GB as its ceiling, no swap",
                len(units) == 2 and all(ceiling in u and "MemorySwapMax=0" in u and "--scope" in u for u in units)
                and "--unit=leg-l-5" in units[0] and "--unit=leg-w-5" in units[1]))
    out.append(("runner: each leg its own mounts: its copies of home, tool cache and /tmp at the same paths, and its cgroup seen from inside",
                [l for l in record if l.startswith("unshare ")] == ["unshare --mount --cgroup --propagation private"] * 2
                and all("mount --bind %s/%s/%s %s" % (r["root"], leg, src, dst) in record
                        for leg in ("l", "w") for src, dst in (("home", r["home"]), ("toolcache", r["toolcache"]), ("tmp", r["tmp_dir"])))
                and record.count("mount -t cgroup2 cgroup2 %s" % r["cgroup"]) == 2 and r["copied_toolcache"] and not r["mounts_left"]))
    out.append(("runner: each leg's copies are its own user's", all("-R runner-%s:runner-%s %s/%s/home %s/%s/toolcache" % (leg, leg, r["root"], leg, r["root"], leg)
                                                                    in r["chowns"] for leg in ("l", "w"))))
    hooks = r["hooks"]
    out.append(("runner: the job of another run fails at its leg's own hook; this run's goes on",
                all(h["ok"][0].returncode == 0 and h["other"][0].returncode != 0 and "serves only run 5" in h["other"][0].stderr and h["mode"] != 0
                    for h in hooks.values())
                and all("ACTIONS_RUNNER_HOOK_JOB_STARTED" in e for e in envs.values())))
    out.append(("runner: the hook kills its own leg's user's every process, the Worker with them, never the other leg's; this run's job kills nothing",
                hooks["l"]["other"][1] == ["-KILL -u runner-l"] and hooks["w"]["other"][1] == ["-KILL -u runner-w"]
                and hooks["l"]["ok"][1] == [] and hooks["w"]["ok"][1] == []))
    out.append(("runner: and the hook still fails the job if the kill itself fails", all(h["no_pkill"] != 0 for h in hooks.values())))
    text = (HERE / "node_runner.sh").read_text(encoding="utf-8")
    out.append(("runner: each leg's hook is its own, outside the job's home, in root's directory by default, and made root's: a job can move anything in its home (measured)",
                not r["guard_in_homes"] and all("root:root %s-%s.sh" % (r["guard"], leg) in r["chowns"] for leg in ("l", "w"))
                and all(e.get("ACTIONS_RUNNER_HOOK_JOB_STARTED") == "%s-%s.sh" % (r["guard"], leg) for leg, e in envs.items())
                and 'GUARD="${NODE_RUNNER_GUARD:-/usr/local/sbin/job_guard}-$LEG.sh"' in text))
    out.append(("runner: no --preserve-environment, and no flag that the runner's `run` does not take", "--preserve-environment" not in text and "--disableupdate" not in text))
    out.append(("runner: one runner, one job: nothing cleans up after a job or waits for one before it; the only removal is the leg's old copy, before its fresh one",
                "pkill -KILL -u runner 2>" not in text and "PIDFILE" not in text and text.count("rm -rf") == 1
                and text.index('rm -rf "$LEG_ROOT"') < text.index('cp -a "$TEMPLATE"')))
    return out


def group_cancelled_by_hand():
    """A person's cancel never comes back, whatever the machine does. Both shapes as measured."""
    out = []
    dead_running = job(WIN, status="in_progress", conclusion=None, steps=("success", None, None), runner="ci5a1w")

    result, gh, api, _, _, clock = watched([[GREEN_L, DEAD_W]], answering=False)
    out.append(("cancel: no cancel, the runner dead: failure and steps left as they were: the machine is lost, at the limit",
                result == "lost" and len(gh.dispatches) == 1 and clock.now >= t.DOWN_LIMIT_S))
    result, gh, api, _, _, clock = watched([[GREEN_L, CANCELLED_W]], answering=False)
    out.append(("cancel: with a cancel, the same steps, cancelled: no re-launch, and the node off with no 15 minutes' wait",
                result == "done" and gh.dispatches == [] and gh.cancels == [] and clock.now == 0 and api.deleted == ["dpl_x"]))
    result, gh, _, _, said, _ = watched([[GREEN_L, CANCELLED_W]])
    out.append(("cancel: on a machine that answers: done, and it is no leg that died with nothing to show",
                result == "done" and "no failed step" not in said and not t.died_silently(CANCELLED_W, NOW)))
    hub = FakeGitHub()
    hub.jobs_answers = [[GREEN_L, CANCELLED_W]]
    out.append(("cancel: and stop-node finds the legs finished, so it deletes what is left", t.finished_legs(t.GitHub("o/r", "T", "A", hub, lambda s: None), 5, 1) == [L, W]))
    result, gh, api, _, _, clock = watched([[DEAD_L, QUEUE_CANCELLED_W]], answering=False)
    out.append(("cancel: a machine lost before it and a leg cancelled in the queue: the same, at once, the node off",
                result == "done" and gh.dispatches == [] and clock.now == 0 and api.deleted == ["dpl_x"]))
    hosted = job("gtest-linux-x86_64", conclusion="cancelled", steps=("cancelled",), runner="GitHub Actions 7")
    result, gh, _, _, _, clock = watched([[GREEN_L, DEAD_W, hosted]], answering=False)
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

    class CancelledDuringDelete(FakeLatitude):
        def __call__(self, method, url, headers, body):
            if method == "DELETE":
                gh.jobs_answers = [[RUN_L, QUEUE_CANCELLED_W]]
            return super().__call__(method, url, headers, body)
    gh = FakeGitHub()
    result, gh, api, _, _, _ = watched([[RUN_L, RUN_W]], answering=False, gh=gh, api=CancelledDuringDelete())
    out.append(("cancel: during the node's delete: no re-launch", result == "done" and gh.dispatches == [] and api.deleted == ["dpl_x"]))
    gh = FakeGitHub()
    result, gh, api, _, said, _ = watched([[RUN_L, RUN_W]], answering=False, gh=gh, api=CancelledDuringDelete(), relaunch_of="4")
    out.append(("cancel: during the delete, of a re-launched run: no failure called a second loss", result == "done" and gh.cancels == [] and "second loss" not in said))
    gh = FakeGitHub()
    gh.on_head = lambda: setattr(gh, "jobs_answers", [[RUN_L, QUEUE_CANCELLED_W]])
    result, gh, _, _, _, _ = watched([[RUN_L, RUN_W]], answering=False, gh=gh)
    out.append(("cancel: right before the dispatch: no re-launch", result == "done" and gh.dispatches == []))

    class GoesDownAtTheDelete(FakeLatitude):
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
    result, gh, _, _, _, clock = watched([[QUEUED_L, RUN_W]] * 35 + [[RUN_L, RUN_W]] * 3 + [[GREEN_L, GREEN_W]], answering=lambda s: not 600 <= s < 1100)
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
    got = t.GitHub("o/r", "T", "A", transport, lambda s: None).branch_head("latitude/a b")
    out.append(("same code: a branch with a slash and a space is read as itself", got == "s" and seen[0].endswith("/git/ref/heads/latitude/a%20b")))
    return out


# What GitHub sets in every job, and what a step sets for each name it declares.
RUNNER_ENV = {"GITHUB_RUN_ID": "5", "GITHUB_RUN_ATTEMPT": "1", "GITHUB_REF_NAME": "main", "GITHUB_SHA": HEAD,
              "GITHUB_REPOSITORY": "o/r", "GITHUB_OUTPUT": os.devnull}
STEP_VALUES = {"LATITUDE_API_KEY": KEY, "LATITUDE_PROJECT": PROJECT, "RUNNER_ADMIN_TOKEN": "ADMIN", "GH_TOKEN": "TOKEN", "KIND": "release",
               "TAG": "firefox-99", "COMPILE_CACHE": "gha", "CORE_REF": "core-x", "WRAPPER_REF": "wrap-y", "GTESTS": "true",
               "GTEST_FILTER_INPUT": "X*", "RELAUNCH_OF": "", "ONLY_IF": "--if-legs-done", "LATITUDE_EXCLUDE_SITES": ""}


def workflow():
    import yaml
    return yaml.safe_load((HERE.parent.parent / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))


def run_step(step, answers, nodes=t.NODE_MARK, answering=False, wrap_provision=None, **overrides):
    """A step of release.yml, as written: its command line through main(), with nothing in
    the environment but the names the step declares and what GitHub sets, on the real
    from_env and github_from_env. Only HTTP, the clock and sshd's banner are faked.
    (exit code, stdout, stderr, FakeGitHub, FakeLatitude)"""
    env = dict(RUNNER_ENV, PATH=os.environ["PATH"], **{name: overrides.get(name, STEP_VALUES[name]) for name in step["env"]})
    clock, api, gh = Clock(), FakeLatitude(), FakeGitHub()
    api.nodes = listed(nodes)
    gh.jobs_answers = answers

    class Answer(io.BytesIO):
        status = 200

    def urlopen(req, timeout=60):
        headers = {k.lower(): v for k, v in req.header_items()}
        if "api.latitude.sh" in req.full_url:
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
    real_watch, real_latitude, real_github, real_turn_off = t.watch, t.from_env, t.github_from_env, t.turn_off
    t.watch = lambda *args, **kwargs: real_watch(*args, probe=lambda h, p: answering, clock=lambda: NOW + clock.now, sleep=clock.sleep, **kwargs)
    t.from_env = lambda: fast(real_latitude())
    t.github_from_env = lambda: fast(real_github())
    t.turn_off = lambda latitude, names, *rest: real_turn_off(latitude, names, clock, clock.sleep) if not rest else real_turn_off(latitude, names, *rest)
    t.urllib.request.urlopen = urlopen
    if wrap_provision:
        t.provision = wrap_provision
    os.environ.clear()
    os.environ.update(env)
    printed = io.StringIO()
    try:
        command = os.path.expandvars(step["run"].replace("\\\n", " ").split("|")[0])
        words = shlex.split(command)
        argv = words[next(i for i, w in enumerate(words) if w.endswith("latitude_node.py")) + 1:]
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
        out.append(("watch step, %s: from its own names to a re-launch of every input by value, on this commit, the node off" % name,
                    code == 0 and "verdict=lost" in printed and api.deleted == ["dpl_x"] and len(gh.cancels) == 1
                    and gh.dispatches == [{"ref": "main", "inputs": {"kind": "release", "compile_cache": "gha", "core_ref": "core-x", "wrapper_ref": "wrap-y",
                                                                     "relaunch_of": "5", "release_tag": "firefox-99", "gtests": "true", "gtest_filter": "X*"}}]))
    code, printed, said, gh, api = watch_step("watch-1", list(lost), RELAUNCH_OF="4")
    out.append(("watch step: RELAUNCH_OF reaches it: a second loss fails the job, no dispatch", code == 1 and "verdict=failed" in printed and gh.dispatches == []))
    code, printed, said, gh, api = watch_step("watch-1", [[QUEUED_L, RUN_W]] * 30 + [[QUEUED_L, GREEN_W]], answering=True)
    out.append(("watch step: RUNNER_ADMIN_TOKEN reaches the list of runners, GH_TOKEN the rest: no poll failed on the wrong one",
                code == 1 and "verdict=failed" in printed and "this poll saw nothing" not in said))
    code, printed, said, gh, api = watch_step("watch-1", [[GREEN_L, GREEN_W]])
    out.append(("watch step: legs that finished: verdict done, exit 0, the node off", code == 0 and "verdict=done" in printed and api.deleted == ["dpl_x"]))

    node_step = doc["jobs"]["node"]["steps"][-1]
    seen = []

    def fake_provision(latitude, github, run, attempt, make_ssh, make_key, *rest, upstream=None):
        seen.append((run, attempt, github.repo))
    # From an empty directory: where the job runs, .series/UPSTREAM.json exists, and on a
    # developer's machine it may not; the case is the one where it cannot be read.
    here = os.getcwd()
    with tempfile.TemporaryDirectory() as empty:
        os.chdir(empty)
        try:
            code, printed, said, _, _ = run_step(node_step, [[]], wrap_provision=fake_provision)
        finally:
            os.chdir(here)
    out.append(("node step: reaches provision once, for the run's one node, from its own names, and prints nothing a log would keep",
                code == 0 and printed == "" and seen == [("5", "1", "o/r")]))
    out.append(("node step: an UPSTREAM.json it cannot read costs the head start only, never the run",
                code == 0 and "no tag fetched ahead" in said))
    out.append(("node step: no leg in its names: the node is the run's, for every leg", "LEG" not in node_step["env"] and "--leg" not in node_step["run"]))
    stop_step = next(s for s in doc["jobs"]["stop-node"]["steps"] if " off " in s.get("run", ""))
    code, printed, said, gh, api = run_step(stop_step, [[GREEN_L, RUN_W]])
    out.append(("stop step: with the option from its own names, a leg still running: the node stays for it", code == 0 and api.deleted == [] and W in said))
    code, printed, said, gh, api = run_step(stop_step, [[GREEN_L, GREEN_W]])
    out.append(("stop step: with the option, both legs finished: the node goes", code == 0 and api.deleted == ["dpl_x"] and "ci5a1x" in printed))
    code, printed, said, gh, api = run_step(stop_step, [[RUN_L, RUN_W]], ONLY_IF="")
    out.append(("stop step: without it, the node goes", code == 0 and api.deleted == ["dpl_x"]))
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

        class LatitudeBreaks(FakeLatitude):
            left = 4 * 3

            def __call__(self, method, url, headers, body):
                if self.left:
                    self.left -= 1
                    raise make()
                return super().__call__(method, url, headers, body)
        result, gh, api, _, _, _ = watched(list(through), gh=Breaks(), api=LatitudeBreaks())
        out.append(("shape: %s from either service, poll after poll: the watch goes on, to its verdict, the node off" % name,
                    result == "done" and api.deleted == ["dpl_x"] and gh.dispatches == []))
    for name, make in (("IncompleteRead", lambda: http.client.IncompleteRead(b"x")), ("BadStatusLine", lambda: http.client.BadStatusLine("x"))):
        tries = []

        def breaks_twice(method, url, headers, body):
            tries.append(url)
            if len(tries) <= 2:
                raise make()
            return 200, b'{"data":[]}'
        clock = Clock()
        out.append(("shape: %s is asked again like any read, inside the client" % name, t.Latitude(PROJECT, KEY, breaks_twice, clock.sleep).nodes() == [] and len(tries) == 3))

        def always_breaks(method, url, headers, body):
            raise make()
        try:
            t.Latitude(PROJECT, KEY, always_breaks, clock.sleep).nodes()
            kind = None
        except t.NoAnswer as exc:
            kind = str(exc)
        out.append(("shape: and for good it is no answer, which names the failure and not the project", kind and name in kind and PROJECT not in kind))
    for body in (b'{"data":null}', b"null", b"", b'{}', b"<html>"):
        api = FakeLatitude()
        api.list_body = body
        result, gh, api, _, _, _ = watched(list(through), api=api)
        out.append(("shape: the API's list answers %r: not a crash, not a lost machine, the watch reaches its verdict" % body,
                    result == "done" and gh.dispatches == []))
        clock = Clock()
        try:
            t.Latitude(PROJECT, KEY, api, clock.sleep).nodes()
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
    api = FakeLatitude(delete_takes=False)
    api.nodes = listed()
    deleted, said = quiet(t.turn_off, t.Latitude(PROJECT, KEY, api, clock.sleep), t.node_names(5, 1), clock, clock.sleep)
    out.append(("linger: a delete accepted and a node still listed: a warning, past the limit, not an error",
                deleted == ["dpl_x"] and "::warning::" in said and clock.now >= t.GONE_LIMIT_S))
    result, gh, _, log, _, _ = watched([[RUN_L, RUN_W]], answering=False, api=FakeLatitude(delete_takes=False))
    out.append(("linger: it does not stop the re-launch", result == "lost" and log[-2:] == ["dispatch", "cancel"]))
    api, gh = FakeLatitude(), FakeGitHub()
    api.nodes = [{"id": "d9", "name": "ci9a1x", "status": t.RUNNING}]
    gh.run_status = {"9": "in_progress"}
    gh.past_runs = [{"id": 5, "display_title": t.relaunch_title(9)}]
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("linger: the re-launch finds its old run's node still listed, that run still ending: it goes",
                got and api.deleted == ["d9"]))
    api = FakeLatitude()
    api.list_body = b'{"data":[]}'
    clock = Clock()
    latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
    out.append(("empty: an empty list: nothing on", t.turn_off(latitude, t.node_names(5, 1), clock, clock.sleep) == [] and latitude.nodes() == []))
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("empty: nothing to clear either", got))

    failed_late = job(WIN, conclusion="failure", steps=("success", "failure"), completed="2026-11-03T12:10:00Z", runner="ci5a1w")
    answers = [[GREEN_L, RUN_W]] * 8 + [[GREEN_L, failed_late]]
    result, gh, api, _, _, clock = watched(list(answers), answering=lambda s: s < 120)
    out.append(("failed step: a leg dies, GitHub marks its step failed after the machine went silent, the API still lists it: lost at the limit",
                result == "lost" and len(gh.dispatches) == 1 and 120 + t.DOWN_LIMIT_S - 60 <= clock.now <= 120 + t.DOWN_LIMIT_S + 300))
    result, gh, _, _, _, _ = watched(list(answers))
    out.append(("failed step: the same job on a machine that answers: a defect, no re-launch", result == "done" and gh.dispatches == []))
    unconcluded = job(WIN, conclusion="failure", steps=("success", "failure", None), completed="2026-11-03T11:59:00Z", runner="ci5a1w")
    result, gh, _, _, _, _ = watched([[GREEN_L, unconcluded]], answering=False)
    out.append(("failed step: steps left unconcluded say the runner went away, whenever it ended: lost", result == "lost"))
    out.append(("failed step: a compile error before the machine's silence stays a compile error",
                not t.died_silently(RED_L, NOW) and t.died_silently(RED_L, NOW - 3600)))
    return out


def group_deployment_id():
    """A deployment id reaches no log: the repository is public."""
    out = []

    class Echoes(FakeLatitude):
        failing = ()

        def __call__(self, method, url, headers, body):
            if any(part in url and method == verb for verb, part in self.failing):
                return 500, b'{"message":"deployment dpl_x is busy"}'
            return super().__call__(method, url, headers, body)
    clock = Clock()
    api = Echoes()
    api.nodes = listed()
    api.failing = (("DELETE", "/servers/"),)
    latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
    try:
        _, said = quiet(t.turn_off, latitude, ["ci5a1x"], clock, clock.sleep)
        message = said
    except t.NodeError as exc:
        message = str(exc)
    out.append(("id: a failed delete, in its URL and its answer: the error names the node by its name, not its id",
                "ci5a1x" in message and "dpl_x" not in message and "/servers/" in message))
    for verb, part, call in (("GET", "/servers/dpl_9", lambda th: th.address("dpl_9")),
                             ("POST", "/servers", lambda th: th.create("ci5a1x", "k"))):
        api = Echoes()
        api.nodes = listed()
        api.failing = ((verb, part),)
        latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
        latitude.nodes()
        try:
            call(latitude)
            message = "no error"
        except t.NodeError as exc:
            message = str(exc)
        out.append(("id: a node seen in the list, and an answer of %s %s that repeats it" % (verb, part), "HTTP 500" in message and "dpl_x" not in message))
    api = FakeLatitude()
    latitude = t.Latitude(PROJECT, KEY, api, clock.sleep)
    created = latitude.create("ci5a1x", "k")
    out.append(("id: a node just created is known too", created in latitude.http.secrets))
    api = Echoes()
    api.failing = (("DELETE", "/servers/"),)
    try:
        t.Latitude(PROJECT, KEY, api, clock.sleep).delete("dpl_x")
        message = "no error"
    except t.NodeError as exc:
        message = str(exc)
    out.append(("id: a delete of a node never listed here, in its own URL", "HTTP 500" in message and "dpl_x" not in message))
    code, api, _, printed, said = run_off_with([[RUN_L, RUN_W]], if_legs_done=False)
    out.append(("id: stop-node's log says which node went by its name", api.deleted == ["dpl_x"] and "ci5a1x" in printed
                and "dpl_" not in printed + said))
    return out


def group_refused_job():
    """The hook kills a foreign job's Worker and its runner: this run's leg then waits with
    no runner, on a machine that answers. That has to end in a failed run."""
    out = []
    result, gh, api, _, said, clock = watched([[QUEUED_L, GREEN_W]])
    out.append(("refused job: a leg's runner killed: the run fails within the stuck limit, no re-launch, the node off, and nothing started again",
                result == "failed" and gh.dispatches == [] and gh.jits == [] and len(gh.cancels) == 1 and api.deleted == ["dpl_x"] and clock.now <= t.STUCK_S + 120))
    return out


class ScriptedRun:
    """subprocess.run for Ssh: what the real ssh would answer, by remote command."""

    def __init__(self, answers):
        self.answers, self.argvs = answers, []

    def __call__(self, argv, **kwargs):
        self.argvs.append(argv)
        code, out, err = self.answers.get(shlex.split(argv[-1])[-1], (0, b"", b""))
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
    latitude = t.Latitude(PROJECT, KEY, FakeLatitude(), clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", FakeGitHub(), clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")

        def attempt():
            try:
                t.provision(latitude, github, 5, 1, lambda host, port: t.Ssh(host, port, key), lambda: key, clock, clock.sleep)
                return ""
            except t.NodeError as exc:
                return str(exc)
        return with_run(ScriptedRun(answers), lambda: quiet(attempt))


def group_address():
    """The repository is public: a node's address reaches no log."""
    out = []
    refused = b"ssh: connect to host 1.1.1.1 port 22: Connection refused\r\n"
    ssh = t.Ssh("1.1.1.1", 22, "k")
    run = ScriptedRun({"x": (255, b"from 1.1.1.1 in 130001", refused)})
    code, stdout, stderr = with_run(run, lambda: ssh("x"))
    out.append(("address: what ssh says comes back without the host, and the rest of it intact",
                code == 255 and stderr == "ssh: connect to host <node> port 22: Connection refused\r\n" and stdout == "from <node> in 130001"))
    out.append(("root: through the image's user and sudo, with no prompt to hang on, the command quoted whole",
                "ubuntu@1.1.1.1" in run.argvs[0] and "root@1.1.1.1" not in run.argvs[0] and run.argvs[0][-1] == "sudo -n bash -c x"))
    run = ScriptedRun({"cat > /a b": (0, b"", b"")})
    with_run(run, lambda: ssh("cat > /a b"))
    out.append(("root: a command with spaces and a redirect stays one command", shlex.split(run.argvs[0][-1]) == ["sudo", "-n", "bash", "-c", "cat > /a b"]))
    # Port 22 was scrubbed like the old high ports: "22" inside a host key turned into <port>,
    # and the pin went to a key that does not exist.
    with_22 = b"node_bootstrap: host key: ssh-ed25519 AAAA22bb22\nnode 22.23.4\n"
    code, stdout, _ = with_run(ScriptedRun({"x": (0, with_22, b"")}), lambda: ssh("x"))
    out.append(("address: on port 22, a host key and a version with 22 in them come back whole",
                t.HOST_KEY_LINE.search(stdout).group(1) == "ssh-ed25519 AAAA22bb22" and "22.23.4" in stdout))
    error, said = provisioned_through_ssh({"bash -s": (255, b"", refused)})
    out.append(("address: a failed bootstrap's error, on its way to ::error::, does not carry it",
                "<node>" in error and "1.1.1.1" not in error + said and "Connection refused" not in said))
    error, said = provisioned_through_ssh({"bash -s": (0, BOOTSTRAPPED.encode(), b""), PUT_RUNNER: (255, b"", refused)})
    out.append(("address: nor the log of a node that came up", "node up after" in said and "1.1.1.1" not in error + said and "Connection refused" not in said))
    for code in (255, 1):
        for start in (START_L, START_W):
            error, said = provisioned_through_ssh({"bash -s": (0, BOOTSTRAPPED.encode(), b""), start: (code, b"", refused)})
            out.append(("address: ssh ending with %d at a runner's start (%s)" % (code, start[-1]), error and "1.1.1.1" not in error + said and "Connection refused" not in said))
    if shutil_which("ssh"):
        import socket
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory() as tmp:
            code, _, stderr = t.Ssh("127.0.0.1", port, Path(tmp) / "none")("true", timeout=30)
        out.append(("address: the real ssh's own words on a closed port", code != 0 and "127.0.0.1" not in stderr))
    return out


def group_host_key():
    """The bootstrap makes a host key of the node's own, and only the connection that runs
    it goes unverified. It carries no secret."""
    out = []
    api, gh, ssh = FakeLatitude(), FakeGitHub(), FakeSsh()
    got, _ = provisioned(api, gh, ssh)
    commands = [c for c, _ in ssh.calls]
    out.append(("host key: pinned after the bootstrap, before the runner's script and its configuration",
                got and ("pin", HOST_KEY) in ssh.calls
                and commands.index("bash -s") < commands.index("pin") < commands.index(PUT_RUNNER) < commands.index(START_L) < commands.index(START_W)))
    api, gh = FakeLatitude(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({"bash -s": (0, "node_bootstrap: ready\n", "")}))
    out.append(("host key: a bootstrap that names none fails the run: nothing registered, deleted",
                got is None and "host key" in error and gh.jits == [] and api.deleted == ["dpl_x"]))
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        first = t.Ssh("1.1.1.1", 22, key)
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


GROUPS = [group_prefetch, group_node_deps, group_runner_isolated, group_turning_on, group_no_fallback, group_two_runs, group_off, group_stop_node, group_bootstrap, group_github_blip, group_false_lost,
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
