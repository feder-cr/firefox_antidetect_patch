#!/usr/bin/env python3
"""theta_node.py against a fake Theta API, a fake GitHub API and a fake SSH.

Each group runs on its own: an exception is that group's failure, not the file's.

  turning on: the one machine, by name of the run, the first leg's runner online; any
    failure is an error with nothing left on, never another machine or another runner;
    the machine taken, by someone else or by a run still alive, fails clearly;
  turning off: by name, seen gone; with --if-legs-done only when GitHub says so;
  the watch: GitHub's blips, the list's false alarms, the machine and the job as two
    witnesses in either order; one re-launch by value, off first and cancel last, never
    two; the next leg's runner registered only when it is due;
  the sweeper: only our names, and the machine freed behind a watch that died;
  secrets: nothing reaches a message, and the node's SSH key travels encrypted;
  the runners on the node: one after the other, from an empty environment, refusing any
    other run's job (Linux).

    python3 scripts/ci/test_theta_node.py
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import theta_node as t  # noqa: E402

KEY, PROJECT = "SECRET-KEY-123", "prj_secret456"
RUNNING = {"BaseID": "dpl_1", "PodPhase": "ci5a1-deployment-x: Running", "PortMappings": {"22": 30043},
           "NodePublicIP": "1.2.3.4", "AuthPassword": "PASSWORD-789"}
LINUX, WIN = "build-linux-x86_64", "build-win-x86_64"


class FakeTheta:
    def __init__(self, balance=10.5, create_status=201, create_lost=False, rows=(RUNNING,), delete_takes=True, log=None):
        self.balance, self.create_status, self.create_lost = balance, create_status, create_lost
        self.rows, self.delete_takes = list(rows), delete_takes
        self.nodes, self.created, self.deleted = [], [], []
        self.log = log if log is not None else []
        self.list_status = 200

    def __call__(self, method, url, headers, body):
        assert headers["x-api-key"] == KEY
        path = url.split("controller.thetaedgecloud.com", 1)[1]
        if path.endswith("/billing/balance"):
            return 200, json.dumps({"body": {"balance_usd": self.balance}}).encode()
        if path.startswith("/deployments/list"):
            return self.list_status, json.dumps({"body": self.rows}).encode()
        if path.endswith("/gpu-deployments") and method == "GET":
            return self.list_status, json.dumps({"body": {"deployments": self.nodes}}).encode()
        if path.endswith("/gpu-deployments") and method == "POST":
            sent = json.loads(body)
            self.created.append(sent)
            if self.create_lost:
                self.nodes.append({"id": "dpl_1", "name": sent["name"], "status": "unknown"})
                raise OSError("connection reset")
            if self.create_status != 201:
                return self.create_status, b'{"message":"machine not available"}'
            self.nodes.append({"id": "dpl_1", "name": sent["name"], "status": "running", "machine_type": sent["resource_id"]})
            return 201, json.dumps({"body": {"deployment": {"id": "dpl_1"}}}).encode()
        if method == "DELETE":
            node_id = path.rsplit("/", 1)[1]
            self.deleted.append(node_id)
            self.log.append("delete")
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

    def __call__(self, method, url, headers, body):
        path = url.split("api.github.com", 1)[1]
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
            self.dispatches.append(json.loads(body))
            self.log.append("dispatch")
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
            return 200, json.dumps({"status": status}).encode()
        raise AssertionError("unexpected %s %s" % (method, path))


class FakeSsh:
    def __init__(self, answers=None):
        self.calls, self.answers = [], answers or {}

    def __call__(self, command, stdin=None, timeout=60):
        self.calls.append((command, stdin))
        return self.answers.get(command, (0, "ready", ""))


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def quiet(fn, *args, **kwargs):
    """fn's result, and what it said to stderr."""
    sink = io.StringIO()
    with contextlib.redirect_stderr(sink):
        return fn(*args, **kwargs), sink.getvalue()


def provisioned(api, gh, ssh):
    """provision(): (outputs or None, error text, all it said)."""
    clock = Clock()
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")
        try:
            out, _ = quiet(t.provision, theta, github, 5, 1, lambda host, port: ssh, lambda: key, clock, clock.sleep)
            return out, ""
        except t.ThetaError as exc:
            return None, str(exc)


def job(name, status="completed", conclusion="success", steps=(), created="2026-11-03T10:00:00Z", completed=None):
    return {"name": name, "status": status, "conclusion": conclusion, "labels": ["theta-5-1-" + name[len("build-"):]],
            "created_at": created, "completed_at": completed, "steps": [{"name": "s", "conclusion": c} for c in steps]}


NOW = t.parse_time("2026-11-03T12:00:00Z")
RUNNING_LEG = job(LINUX, status="in_progress", conclusion=None, steps=("success",))
QUEUED_WIN = job(WIN, status="queued", conclusion=None, created="2026-11-03T11:59:00Z")
GREEN_LINUX = job(LINUX, completed="2026-11-03T11:30:00Z", steps=("success",))
GREEN_WIN = job(WIN, completed="2026-11-03T11:50:00Z", steps=("success",))
FAILED_STEP = job(LINUX, conclusion="failure", steps=("success", "failure", "skipped"), completed="2026-11-03T11:59:00Z")
SILENT = job(LINUX, conclusion="failure", steps=("success", "cancelled"), completed="2026-11-03T11:59:00Z")
INPUTS = {"kind": "candidate", "compile_cache": "gha", "core_ref": "main", "wrapper_ref": "v1", "relaunch_of": "5"}


def watched(answers, node=True, answering=True, relaunch_of="", limit=6 * 3600, past_runs=(), gh=None, ssh=None, rows=(RUNNING,)):
    """watch() to its verdict, on a clock that only moves when it sleeps. answering: is
    sshd there, a bool or a function of the seconds since the start."""
    clock, log = Clock(), []
    api, gh = FakeTheta(rows=rows, log=log), gh or FakeGitHub(log=log)
    gh.log = log
    if node:
        api.nodes = [{"id": "dpl_1", "name": "ci5a1", "status": node if isinstance(node, str) else "running"}]
    gh.jobs_answers = answers
    gh.past_runs = list(past_runs)
    theta, github = t.Theta(PROJECT, KEY, api, clock.sleep), t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
    ssh = ssh or FakeSsh()
    probe = (lambda h, p: answering(clock.now)) if callable(answering) else (lambda h, p: answering)
    result, said = quiet(t.watch, theta, github, 5, 1, "main", INPUTS, lambda: ssh, relaunch_of, probe, ("1.2.3.4", 30043),
                         lambda: NOW + clock.now, clock.sleep, 60, limit)
    return result, gh, api, ssh, log, said, clock


def group_turning_on():
    out = []
    api, gh, ssh = FakeTheta(), FakeGitHub(), FakeSsh()
    got, error = provisioned(api, gh, ssh)
    sent = api.created[0] if api.created else {}
    out.append(("on: the one machine, under the run's name, with this run's key and a price ceiling",
                sent.get("resource_id") == t.MACHINE and sent.get("name") == "ci5a1"
                and sent.get("ssh_public_key") == "ssh-ed25519 AAAA ci-node" and sent.get("ssh_port") == 22
                and sent.get("container_image") == t.IMAGE and sent.get("max_price_per_hour_usd") == t.MAX_PRICE_USD_HOUR))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("on: the machine is named in one place", source.count(t.MACHINE) == 1))
    out.append(("no other machine, ever: no argument names one, on the command line or in the code",
                "--machine" not in source and "machine=" not in source and "machine," not in source.split("def create", 1)[1].split("\n", 1)[0]))
    out.append(("on: the outputs name this run and attempt, and one runner is registered: the first leg's, online",
                got == {"runner": "theta-5-1", "attempt": "1", "node": "dpl_1"}
                and [j["name"] for j in gh.jits] == ["ci5a1-1"]
                and gh.jits[0]["labels"] == ["theta-5-1-linux-x86_64"] and gh.jits[0]["work_folder"] == "../work"
                and t.node_legs() == ["linux-x86_64", "win-x86_64"]))
    commands = [c for c, _ in ssh.calls]
    stdin = dict(ssh.calls)
    out.append(("on: prepared before the runner starts, the run named, and the configuration goes by stdin, never on a command line",
                commands.index("bash -s") < commands.index("bash /usr/local/sbin/node_runners.sh 5")
                and b"node_bootstrap" in stdin["bash -s"] and b"\r" not in stdin["bash -s"]
                and stdin["bash /usr/local/sbin/node_runners.sh 5"] == b"JIT1\n"
                and not any("JIT" in c for c in commands)))
    out.append(("on: a node that went well stays on", api.deleted == []))
    return out


def group_no_fallback():
    out = []
    api, gh = FakeTheta(create_status=409), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: a refusal (the price, say) is final: an error, nothing on", got is None and "409" in error and api.nodes == [] and gh.jits == [] and len(api.created) == 1))
    api, gh = FakeTheta(create_status=404), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: the machine is taken: asked again for about 6 minutes, then a message that says so, by its name",
                got is None and "taken" in error and t.MACHINE in error and api.nodes == [] and gh.jits == []
                and len(api.created) == 1 + t.FREE_LIMIT_S // 30 and 300 <= t.FREE_LIMIT_S <= 420))

    class FreedLater(FakeTheta):
        def __call__(self, method, url, headers, body):
            if method == "POST" and len(self.created) < 3:
                self.created.append(json.loads(body))
                return 404, b'{"message":"GPU resource not found"}'
            return super().__call__(method, url, headers, body)
    api = FreedLater()
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("on: a machine we just freed, still 404 in Theta's list, is waited for", got and got["runner"] == "theta-5-1" and len(api.created) == 4))
    api = FakeTheta(balance=1.5)
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: the balance is below $2: an error, nothing created", got is None and "balance" in error and api.created == []))
    api = FakeTheta(create_lost=True)
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: the answer to the create is lost: the node it may have made is deleted", got is None and api.deleted == ["dpl_1"]))
    api = FakeTheta(rows=())
    got, error = provisioned(api, FakeGitHub(), FakeSsh())
    out.append(("fail: a node never Running within the limit is deleted", got is None and api.deleted == ["dpl_1"] and api.nodes == []))
    api = FakeTheta()
    got, error = provisioned(api, FakeGitHub(), FakeSsh({"true": (255, "", "refused")}))
    out.append(("fail: no SSH within the limit: deleted", got is None and api.deleted == ["dpl_1"]))
    api, gh = FakeTheta(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({"bash -s": (1, "", "node_bootstrap: ERROR: missing: git")}))
    out.append(("fail: a bootstrap that refuses: deleted, and nothing registered", got is None and "missing: git" in error and api.deleted == ["dpl_1"] and gh.jits == []))
    api, gh = FakeTheta(), FakeGitHub(jit_status=401)
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: GitHub refuses the registration: deleted", got is None and api.deleted == ["dpl_1"]))
    api, gh = FakeTheta(), FakeGitHub()
    got, error = provisioned(api, gh, FakeSsh({"bash /usr/local/sbin/node_runners.sh 5": (1, "", "no")}))
    out.append(("fail: the runner does not start: deleted, and the registered one removed",
                got is None and api.deleted == ["dpl_1"] and gh.removed == ["100"]))
    api, gh = FakeTheta(), FakeGitHub(online=False)
    got, error = provisioned(api, gh, FakeSsh())
    out.append(("fail: a runner that never comes online fails the run within the limit, it does not wait 24 hours in the queue",
                got is None and "online" in error and api.deleted == ["dpl_1"] and t.RUNNER_ONLINE_LIMIT_S <= 600))
    # The command line: exit 1, and not one output a job could run on.
    for var in ("THETA_API_KEY", "THETA_PROJECT"):
        os.environ.pop(var, None)
    with tempfile.TemporaryDirectory() as tmp:
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            code, said = quiet(t.main, ["provision", "--run", "5", "--attempt", "1", "--access-out", str(Path(tmp) / "a")])
    out.append(("fail: the command exits 1 with an error for the run's page, and prints no runner", code == 1 and sink.getvalue() == "" and "::error::" in said))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("no fallback: nothing in the script names a builder or sends a leg to GitHub's runners",
                "builder" not in source.replace("test_release_builder", "") and "provision_outputs" not in source and "GitHub's runners, nothing" not in source))
    return out


def group_two_runs():
    out = []
    def taken(nodes, run_status):
        api, gh = FakeTheta(), FakeGitHub()
        api.nodes = list(nodes)
        gh.run_status = run_status
        got, error = provisioned(api, gh, FakeSsh())
        return got, error, api, gh
    got, error, api, gh = taken([{"id": "d9", "name": "ci9a1", "status": "running"}], {"9": "in_progress"})
    out.append(("two runs: a live run's node refuses this run at once, naming it, with nothing created",
                got is None and "run 9" in error and "ci9a1" in error and api.created == [] and api.deleted == []))
    got, error, api, gh = taken([{"id": "d9", "name": "ci9a1", "status": "running"}], {"9": "completed"})
    out.append(("two runs: a node whose run has finished goes, and the machine is waited for", got is not None and api.deleted == ["d9"]))
    got, error, api, gh = taken([{"id": "dpl_old", "name": "ci5a0", "status": "running"}], {"5": "in_progress"})
    out.append(("two runs: an earlier attempt of this same run goes", got is not None and api.deleted == ["dpl_old"]))
    by_hand = {"id": "dh", "name": "gpunodegn8rdcmn6agw1", "status": "running", "machine_type": t.MACHINE}
    got, error, api, gh = taken([by_hand], {})
    out.append(("taken: a node of this project made by hand on the machine fails the run at once, by name, untouched",
                got is None and "gpunodegn8rdcmn6agw1" in error and t.MACHINE in error and api.deleted == [] and api.created == []))
    other = {"id": "dh", "name": "cihandbuild1", "status": "running", "machine_type": "community_other"}
    got, error, api, gh = taken([other], {})
    out.append(("taken: a node on another machine is other work, even with a name that starts with ci", got is not None and api.deleted == []))
    return out


def group_off():
    out = []
    clock = Clock()
    api = FakeTheta()
    api.nodes = [{"id": "dpl_1", "name": "ci5a1", "status": "running"}, {"id": "dpl_2", "name": "ci5a2", "status": "running"},
                 {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "status": "running"}]
    theta = t.Theta(PROJECT, KEY, api, clock.sleep)
    deleted = t.turn_off(theta, 5, 1, clock, clock.sleep)
    out.append(("off: this attempt's node only, by name", deleted == ["dpl_1"] and sorted(n["id"] for n in api.nodes) == ["dpl_2", "dpl_lab"]))
    out.append(("off: nothing on is not an error", t.turn_off(t.Theta(PROJECT, KEY, FakeTheta(), clock.sleep), 5, 1, clock, clock.sleep) == []))
    api = FakeTheta(delete_takes=False)
    api.nodes = [{"id": "dpl_1", "name": "ci5a1", "status": "running"}]
    try:
        t.turn_off(t.Theta(PROJECT, KEY, api, clock.sleep), 5, 1, clock, clock.sleep)
        refused = False
    except t.ThetaError:
        refused = True
    out.append(("off: a delete that never takes is an error", refused))

    class Refused(FakeGitHub):
        def __call__(self, method, url, headers, body):
            return 401, b'{"message":"Bad credentials"}'
    got, _ = quiet(t.remove_runners, t.GitHub("o/r", "T", "BAD", Refused(), clock.sleep), "theta-5-1")
    out.append(("off: runners GitHub will not list are a warning, never a failed delete", got == []))

    def run_off(answers, if_legs_done=True):
        api, gh = FakeTheta(), FakeGitHub()
        api.nodes = [{"id": "dpl_1", "name": "ci5a1", "status": "running"}]
        gh.jobs_answers = answers
        clock = Clock()
        from_env, github_from_env = t.from_env, t.github_from_env
        t.from_env = lambda: t.Theta(PROJECT, KEY, api, clock.sleep)
        t.github_from_env = lambda: t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                code, said = quiet(t.run_off, argparse.Namespace(run="5", attempt="1", if_legs_done=if_legs_done))
        finally:
            t.from_env, t.github_from_env = from_env, github_from_env
        return code, api
    code, api = run_off([[RUNNING_LEG, QUEUED_WIN]])
    out.append(("off after a watch that crashed: legs still running, the healthy node stays", code == 0 and api.deleted == []))
    code, api = run_off(["down"])
    out.append(("off after a watch that crashed: GitHub does not answer, which is not a yes", code == 0 and api.deleted == []))
    code, api = run_off(["garbage"])
    out.append(("off after a watch that crashed: nor is an answer that cannot be read", code == 0 and api.deleted == []))
    code, api = run_off([[RUNNING_LEG]])
    out.append(("off: a leg not listed yet is not a finished leg", code == 0 and api.deleted == []))
    code, api = run_off([[GREEN_LINUX, GREEN_WIN]])
    out.append(("off: the legs have finished: deleted", code == 0 and api.deleted == ["dpl_1"]))
    code, api = run_off([[RUNNING_LEG, QUEUED_WIN]], if_legs_done=False)
    out.append(("off without the option (the node job never finished): deleted, asking nothing", code == 0 and api.deleted == ["dpl_1"]))
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
    out.append(("names: run ids of today fit Theta's 20 characters", t.node_name(41647921995, 12) == "ci41647921995a12" and len(t.node_name(41647921995, 12)) <= 20))
    out.append(("labels: one per leg, with the run and the attempt", t.leg_label(41647921995, 2, "win-x86_64") == "theta-41647921995-2-win-x86_64"))
    out.append(("time: Theta's 5 decimals and GitHub's none", t.parse_time("2026-11-03T14:35:38.39446Z") == t.parse_time("2026-11-03T14:35:38Z") and t.parse_time("nonsense") is None))
    return out


def group_github_blip():
    """Finding 1: a blip of GitHub's API deleted a healthy node mid-build."""
    out = []
    finished = [GREEN_LINUX, GREEN_WIN]
    result, gh, api, _, _, _, _ = watched(["down"] * 3 + [finished])
    out.append(("blip: three polls of errors, the watch goes on and the node stays until the legs finish", result == "done" and api.deleted == [] and gh.dispatches == []))
    result, gh, api, _, _, _, _ = watched(["garbage", "garbage", finished])
    out.append(("blip: an answer that is not JSON is the same", result == "done" and api.deleted == []))
    try:
        watched(["down"])
        blind = None
    except t.WatchBlind as exc:
        blind = str(exc)
    out.append(("blind: GitHub silent for the limit stops the watch with its own error", blind and str(t.BLIND_POLLS) in blind))
    code_is_kept = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("blind: the command says no verdict and deletes nothing", "return 3" in code_is_kept
                and 'verdict = watch(' in code_is_kept and "turn_off" not in code_is_kept.split("except WatchBlind")[1].split("finally")[0]))
    return out


def group_false_lost():
    """Finding 2: Theta's list alone said lost, after 5 minutes."""
    out = []
    running_then_done = [[RUNNING_LEG, QUEUED_WIN]] * 40 + [[GREEN_LINUX, GREEN_WIN]]
    result, gh, api, _, _, _, _ = watched(running_then_done, node=False)
    out.append(("list: gone for 40 minutes with sshd answering and a leg running: not lost", result == "done" and gh.dispatches == [] and api.deleted == []))
    result, gh, api, _, _, _, _ = watched(running_then_done, node="pending")
    out.append(("list: another status with sshd answering: not lost", result == "done" and gh.dispatches == []))
    result, gh, api, _, _, _, _ = watched(running_then_done, node="running", rows=(dict(RUNNING, PodPhase="x: Pending"),))
    out.append(("list: a pod not Running with sshd answering: not lost", result == "done" and gh.dispatches == []))
    silent_5 = lambda s: s >= 5 * 60
    result, gh, _, _, _, _, _ = watched(running_then_done, node=False, answering=silent_5)
    out.append(("both: down for the old 5 minutes, then sshd back: not lost", result == "done" and gh.dispatches == []))
    out.append(("the limit is at least 15 minutes", t.DOWN_LIMIT_S >= 15 * 60))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=1)
    almost = t.decide([RUNNING_LEG, QUEUED_WIN], machine, NOW + t.DOWN_LIMIT_S - 1)
    enough = t.decide([RUNNING_LEG, QUEUED_WIN], machine, NOW + t.DOWN_LIMIT_S)
    out.append(("both: not a second before the limit, and lost at it", almost == "wait" and enough == "lost"))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=True, answers=False, steps_done=1)
    out.append(("evidence: a leg running on a machine Theta still lists is left to GitHub, however long sshd is silent",
                t.decide([RUNNING_LEG, QUEUED_WIN], machine, NOW + 3600) == "wait"))
    machine = t.Machine(("h", 1))
    machine.see(NOW, listed=False, answers=False, steps_done=1)
    machine.see(NOW + 600, listed=False, answers=False, steps_done=2)
    out.append(("evidence: a leg that finished a step says the machine lives, and the wait starts again",
                machine.down_since is None and t.decide([RUNNING_LEG, QUEUED_WIN], machine, NOW + t.DOWN_LIMIT_S) == "wait"))
    out.append(("status: the string the API really answers (measured: running), read as it is", t.RUNNING == "running"))
    live = FakeTheta()
    live.nodes = [{"id": "dpl_1", "name": "ci5a1", "status": "running"}]
    clock = Clock()
    machine = t.Machine()
    quiet(t.look_at_machine, t.Theta(PROJECT, KEY, live, clock.sleep), "ci5a1", machine, [], lambda h, p: True, NOW)
    out.append(("status: a node the real API calls running is listed, and its address comes from there",
                machine.listed is True and machine.address == ("1.2.3.4", 30043) and machine.down_since is None))
    live.list_status = 500
    machine = t.Machine(("h", 1))
    quiet(t.look_at_machine, t.Theta(PROJECT, KEY, live, clock.sleep), "ci5a1", machine, [], lambda h, p: True, NOW)
    out.append(("status: the API down says nothing about the machine", machine.listed is None and machine.down_since is None))
    return out


def group_lost_either_order():
    """Finding 7 and GitHub's unknown timing: two witnesses, whichever comes first."""
    out = []
    out.append(("silent: a failed job with no failed step, also one with no steps", t.died_silently(SILENT) and t.died_silently(job(LINUX, conclusion="failure"))))
    out.append(("silent: not a compile error, not green, not skipped",
                not t.died_silently(FAILED_STEP) and not t.died_silently(GREEN_LINUX) and not t.died_silently(job(LINUX, conclusion="skipped"))))
    # Theta and sshd first, GitHub silent for ever: a leg in progress, not listed, nothing advancing.
    result, gh, api, _, _, _, clock = watched([[RUNNING_LEG, QUEUED_WIN]], node=False, answering=False)
    out.append(("first the machine: gone from the list, sshd silent, GitHub still says in progress: lost at the limit",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    # GitHub first: the job ended at once, and the machine must still be silent at the limit.
    result, gh, api, _, _, _, clock = watched([[SILENT, QUEUED_WIN]], node="running", answering=False)
    out.append(("first GitHub: the job failed at once, the list still says running, sshd silent: lost at the limit, not before",
                result == "lost" and len(gh.dispatches) == 1 and t.DOWN_LIMIT_S <= clock.now <= t.DOWN_LIMIT_S + 300))
    result, gh, api, _, _, _, _ = watched([[SILENT, job(WIN, conclusion="cancelled")]], node="running", answering=False)
    out.append(("both legs ended, the machine silent: lost", result == "lost"))
    result, gh, api, _, _, said, _ = watched([[SILENT, GREEN_WIN]])
    out.append(("a leg with no failed step on a machine that answers: not a lost machine, no re-launch, and it says so",
                result == "done" and gh.dispatches == [] and "no re-launch" in said))
    result, gh, _, _, _, _, _ = watched([[FAILED_STEP, GREEN_WIN]], answering=False, node=False)
    out.append(("a compile error is a defect, whatever the machine does: no re-launch", result == "done" and gh.dispatches == []))
    result, gh, _, _, _, _, _ = watched([[job(LINUX, conclusion="cancelled", steps=("success", "cancelled")), job(WIN, conclusion="cancelled")]])
    out.append(("a run cancelled by hand does not come back", result == "done" and gh.dispatches == []))
    machine = t.Machine(("h", 1))
    out.append(("no jobs listed yet is not lost, whatever the machine does", t.decide([], machine, NOW) == "wait"))
    return out


def group_relaunch():
    """Decision 3 and findings 3, 13, 17: once, by value, in the right order."""
    out = []
    result, gh, api, _, log, said, _ = watched([[RUNNING_LEG, QUEUED_WIN]], node=False, answering=False)
    out.append(("re-launch: one, with the run's own inputs by value and its mark",
                result == "lost" and gh.dispatches == [{"ref": "main", "inputs": INPUTS}] and len(gh.cancels) == 1))
    out.append(("re-launch: the node first, seen gone, then the dispatch, this run's cancel last", log[-2:] == ["dispatch", "cancel"]))
    result, gh, api, _, log, _, _ = watched([[SILENT, QUEUED_WIN]], node="running", answering=False)
    out.append(("re-launch: a node still listed is deleted before the dispatch: the re-launch needs the machine free",
                log == ["delete", "dispatch", "cancel"] and api.nodes == []))
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
    result, gh, api, _, log, said, _ = watched([[RUNNING_LEG, QUEUED_WIN]], node=False, answering=False, relaunch_of="4")
    out.append(("second loss: no dispatch, the run fails and says it was the second, and the queued leg does not wait 24 hours",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and "second time" in said))
    titled = [{"display_title": t.relaunch_title(5)}]
    result, gh, api, _, log, said, _ = watched([[RUNNING_LEG, QUEUED_WIN]], node=False, answering=False, past_runs=titled)
    out.append(("double dispatch: a re-launch of this run already there: not another, and this run still ends",
                result == "lost" and gh.dispatches == [] and len(gh.cancels) == 1 and "already" in said))
    result, gh, api, _, log, said, _ = watched([[RUNNING_LEG, QUEUED_WIN]], node=False, answering=False, past_runs=[{"display_title": t.relaunch_title(6)}])
    out.append(("double dispatch: another run's re-launch is not this run's", len(gh.dispatches) == 1))
    return out


def group_stuck():
    """A queued job waits up to 24 hours: the watch and the sweeper end that, in that order."""
    out = []
    second_waits = job(WIN, status="queued", conclusion=None, created="2026-11-03T01:00:00Z")
    running = job(LINUX, status="in_progress", conclusion=None)
    result, gh, _, _, _, _, _ = watched([[running, second_waits], [running, second_waits], [GREEN_WIN, GREEN_LINUX]])
    out.append(("the second leg waiting for hours behind the first is no problem", result == "done" and not gh.dispatches))
    out.append(("stuck: not within the limit", not t.stuck([job(LINUX, completed="2026-11-03T11:50:00Z"), second_waits], NOW)))
    out.append(("stuck: not with a leg running", not t.stuck([running, second_waits], NOW)))
    hosted = dict(second_waits, labels=["ubuntu-24.04"])
    out.append(("stuck: not on a label of someone else's", not t.stuck([hosted], NOW)))
    out.append(("stuck: queued alone past the limit", t.stuck([second_waits], NOW)))
    first_queued = job(LINUX, status="queued", conclusion=None, created="2026-11-03T01:00:00Z")
    result, gh, api, _, log, said, _ = watched([[first_queued, second_waits]])
    out.append(("a leg whose runner never comes, on a machine that answers: the run fails in bounded time, no re-launch, the node off",
                result == "failed" and gh.dispatches == [] and len(gh.cancels) == 1 and api.deleted == ["dpl_1"] and "not coming" in said))
    result, gh, _, _, _, _, _ = watched([[first_queued, second_waits]], node=False, answering=False)
    out.append(("the same with the machine gone: lost", result == "lost" and len(gh.dispatches) == 1))
    out.append(("the sweeper gives the watch time to speak first: well past its stuck limit and its lost limit",
                t.SWEEP_STUCK_S >= t.STUCK_S + t.DOWN_LIMIT_S and t.SWEEP_STUCK_S >= 45 * 60))
    gh = FakeGitHub()
    gh.jobs_answers = [[job(LINUX, status="queued", conclusion=None, created="2026-11-03T11:30:00Z"), QUEUED_WIN]]
    gh.past_runs = [{"id": 7, "run_attempt": 1}]
    clock = Clock()
    got, _ = quiet(t.cancel_stuck, t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep), NOW)
    out.append(("the sweeper: 30 minutes on the queue is still the watch's", got == [] and gh.cancels == []))
    return out


def group_second_runner():
    """Finding 12: registered when due, so a configuration never has to wait."""
    out = []
    ssh = FakeSsh()
    gh = FakeGitHub()
    result, gh, _, ssh, _, _, _ = watched([[RUNNING_LEG, QUEUED_WIN], [RUNNING_LEG, QUEUED_WIN]], limit=120, gh=gh, ssh=ssh)
    out.append(("next runner: nothing registered while the first leg runs", result == "continue" and gh.jits == [] and ssh.calls == []))
    won = [GREEN_LINUX, QUEUED_WIN]
    result, gh, _, ssh, _, _, _ = watched([won, won, won, [GREEN_LINUX, job(WIN, status="in_progress", conclusion=None)], [GREEN_LINUX, GREEN_WIN]])
    out.append(("next runner: registered once when the first leg has finished, for the second leg alone, the config by stdin",
                result == "done" and [j["name"] for j in gh.jits] == ["ci5a1-2"] and gh.jits[0]["labels"] == ["theta-5-1-win-x86_64"]
                and ssh.calls == [("bash /usr/local/sbin/node_runners.sh 5", b"JIT1\n")]))
    gh = FakeGitHub()
    gh.runners = [{"id": 50, "name": "ci5a1-2", "status": "offline", "labels": [{"name": "theta-5-1-win-x86_64"}]}]
    result, gh, _, ssh, _, _, _ = watched([won, [GREEN_LINUX, GREEN_WIN]], gh=gh)
    out.append(("next runner: one an earlier watch job registered is not registered twice", result == "done" and gh.jits == [] and ssh.calls == []))
    flaky = FakeSsh({"bash /usr/local/sbin/node_runners.sh 5": (1, "", "no")})
    result, gh, api, _, _, said, _ = watched([won], ssh=flaky)
    out.append(("next runner: one that will not start is removed and tried again, then the run fails clearly and the node goes off",
                result == "failed" and len(gh.jits) == t.START_TRIES and gh.dispatches == [] and api.deleted == ["dpl_1"] and "did not start" in said
                and len(gh.removed) == t.START_TRIES))
    result, gh, _, _, _, _, _ = watched([[SILENT, QUEUED_WIN]], node="running", answering=lambda s: s > 3000, limit=600)
    out.append(("next runner: not on a machine that does not answer", result == "continue" and gh.jits == []))
    out.append(("next_leg: the first leg's runner came with the node", t.next_leg([job(LINUX, status="queued", conclusion=None), QUEUED_WIN]) is None))
    return out


def group_watch_stages():
    """Finding 11: one job's share of the time, and the next goes on."""
    out = []
    result, gh, api, _, _, _, clock = watched([[RUNNING_LEG, QUEUED_WIN]], limit=t.STAGE_LIMIT_S)
    out.append(("stage: at its limit the watch says continue, and turns nothing off", result == "continue" and api.deleted == [] and clock.now >= t.STAGE_LIMIT_S))
    out.append(("stage: it stops by itself well before the 360 minutes GitHub gives a job", t.STAGE_LIMIT_S <= 340 * 60))
    result, gh, api, _, _, _, clock = watched([[]])
    out.append(("no legs listed at all for a while: nothing to watch", result == "done" and clock.now == (t.NO_LEGS_POLLS - 1) * 60))
    return out


def group_sweep():
    out = []
    def swept(nodes, run_status=None, github=True, jobs=None, **kwargs):
        clock = Clock()
        api, gh = FakeTheta(), FakeGitHub()
        api.nodes = nodes
        gh.run_status = run_status or {}
        gh.jobs_answers = [jobs if jobs is not None else [RUNNING_LEG, QUEUED_WIN]]
        theta = t.Theta(PROJECT, KEY, api, clock.sleep)
        hub = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep) if github else None
        return quiet(t.sweep, theta, hub, NOW, **kwargs)[0], api
    young = "2026-11-03T11:00:00Z"
    old = "2026-11-02T20:00:00Z"
    by_hand = {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "created_at": "2020-01-01T00:00:00Z"}
    other = {"id": "dpl_c", "name": "cinema", "created_at": "2020-01-01T00:00:00Z"}
    hand = {"id": "dpl_h", "name": "cihandbuild1", "created_at": "2020-01-01T00:00:00Z"}
    gone, api = swept([by_hand, other, hand, {"id": "d1", "name": "ci5a1", "created_at": old}], {"5": "in_progress"})
    out.append(("sweep: past the limit ours goes even if its run says it is alive; by hand or another name never, however old",
                gone == ["ci5a1"] and api.deleted == ["d1"]))
    out.append(("sweep: the limit lets two legs of 350 minutes finish", t.GUARD_HOURS * 60 >= 2 * 350 + 45))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": "in_progress"})
    out.append(("sweep: a young node whose run still needs it stays", gone == [] and api.deleted == []))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": "completed"})
    out.append(("sweep: a young node whose run has finished goes", gone == ["ci5a1"] and api.deleted == ["d1"]))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": "in_progress"}, jobs=[GREEN_LINUX, GREEN_WIN])
    out.append(("sweep: the safety net of a dead watch: the run goes on with arm64, its legs have finished, the node goes", gone == ["ci5a1"] and api.deleted == ["d1"]))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": "in_progress"}, jobs=[])
    out.append(("sweep: no legs listed yet is not legs finished", gone == [] and api.deleted == []))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": 500})
    out.append(("sweep: GitHub's error is not 'the run has finished'", gone == [] and api.deleted == []))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}, {"id": "d2", "name": "ci6a1", "created_at": old}], github=False)
    out.append(("sweep: from a machine with no GitHub, the age alone", gone == ["ci6a1"] and api.deleted == ["d2"]))
    gone, api = swept([{"id": "d2", "name": "ci6a1", "created_at": old}], dry_run=True)
    out.append(("sweep: a dry run deletes nothing", gone == ["ci6a1"] and api.deleted == []))
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
    address = t.Theta(PROJECT, KEY, FakeTheta(), clock.sleep).address("dpl_1")
    out.append(("address: host and port of the Running container, nothing else of the row", address == ("1.2.3.4", 30043)))
    pending = dict(RUNNING, PodPhase="x: Pending")
    out.append(("address: a Pending container has none yet", t.Theta(PROJECT, KEY, FakeTheta(rows=(pending,)), clock.sleep).address("dpl_1") is None))
    flaky = []

    def transport(method, url, headers, body):
        flaky.append(url)
        return (503, b"busy") if len(flaky) < 3 else (200, b'{"body":{"balance_usd":3}}')
    out.append(("reads: a busy API is asked again", t.Theta(PROJECT, KEY, transport, clock.sleep).balance_usd() == 3.0 and len(flaky) == 3))
    if not shutil_which("openssl"):
        print("skip  access: openssl is missing here")
        return out
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "access"
        secret = t.access_secret(KEY, 5, 1)
        t.seal_access(path, "PRIVATE-KEY-TEXT", ("1.2.3.4", 30043), secret)
        raw = path.read_bytes()
        got = t.open_access(path, secret)
        out.append(("access: the node's key travels encrypted, and comes back", b"PRIVATE-KEY-TEXT" not in raw and b"1.2.3.4" not in raw
                    and got == {"key": "PRIVATE-KEY-TEXT", "address": ["1.2.3.4", 30043]}))
        try:
            t.open_access(path, t.access_secret("another key", 5, 1))
            refused = False
        except t.ThetaError:
            refused = True
        out.append(("access: without the API key it stays closed", refused and secret != t.access_secret(KEY, 5, 2)))
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


def runners_case(leaks) -> dict:
    """node_runners.sh, as it runs on the node, with the system's commands faked."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        home, bin_dir, scratch = tmp / "home", tmp / "bin", tmp / "tmp"
        (home / "actions-runner").mkdir(parents=True)
        bin_dir.mkdir()
        scratch.mkdir()
        trace, envs = tmp / "trace", tmp / "envs"
        run = home / "actions-runner" / "run.sh"
        run.write_text("#!/bin/bash\n"
                       'echo "start $ACTIONS_RUNNER_INPUT_JITCONFIG work=$(ls "$HOME/work" | wc -l) pgo=$(ls "$HOME/pgo" 2>/dev/null | wc -l) tmp=$(ls "%s" | wc -l)" >> "%s"\n'
                       'env >> "%s"\n'
                       'mkdir -p "$HOME/pgo"; touch "$HOME/work/left" "$HOME/pgo/merged.profdata" "%s/left"\n'
                       'sleep 0.3\necho "end $ACTIONS_RUNNER_INPUT_JITCONFIG" >> "%s"\nexit 3\n' % (scratch, trace, envs, scratch, trace),
                       newline="\n")
        run.chmod(0o755)
        fakes = {"runuser": '#!/bin/bash\nwhile [ "$1" != "--" ]; do shift; done; shift; exec "$@"\n',
                 "pkill": "#!/bin/bash\nexit 1\n",
                 "install": '#!/bin/bash\nfor a; do d="$a"; done; mkdir -p "$d"\n'}
        for name, text in fakes.items():
            (bin_dir / name).write_text(text, newline="\n")
            (bin_dir / name).chmod(0o755)
        log = tmp / "log"
        env = dict(os.environ, PATH="%s%s%s" % (bin_dir, os.pathsep, os.environ["PATH"]), NODE_RUNNER_HOME=str(home),
                   NODE_RUNNER_LOG=str(log), NODE_RUNNER_TMP=str(scratch), **leaks)
        script = str(HERE / "node_runners.sh")

        def start(config, run_id="5"):
            return subprocess.run(["bash", script, run_id], input=config, env=env, capture_output=True, text=True, timeout=30)
        first, second = start("CFG1\n"), start("CFG2\n")
        empty, no_run, bad_run = start(""), subprocess.run(["bash", script], input="CFG\n", env=env, capture_output=True, text=True, timeout=30), start("CFG\n", "5;rm")
        for _ in range(100):
            if log.exists() and log.read_text().count("exited with") >= 2:
                break
            subprocess.run(["sleep", "0.1"])
        guard = home / "job_guard.sh"

        def hooked(run_id):
            return subprocess.run(["bash", "-e", str(guard)], env={"GITHUB_RUN_ID": run_id, "PATH": os.environ["PATH"]}, capture_output=True, text=True)
        return {"codes": (first.returncode, second.returncode), "refused": (empty.returncode, no_run.returncode, bad_run.returncode),
                "trace": trace.read_text().splitlines(), "log": log.read_text(), "env": envs.read_text(),
                "guard_ok": hooked("5").returncode, "guard_other": hooked("6"), "guard_mode": guard.stat().st_mode & 0o111}


def group_runners():
    out = []
    if not sys.platform.startswith("linux"):
        print("skip  runners: node_runners.sh runs on Linux only")
        return out
    leaks = {"CARGO_LEAK": "root", "CONTAINER_SECRET": "x", "ACTIONS_RUNNER_HOOK_JOB_COMPLETED": "/nope"}
    r = runners_case(leaks)
    out.append(("runners: returns at once; no configuration, no run id, or one that is not a number is refused",
                r["codes"] == (0, 0) and all(c != 0 for c in r["refused"])))
    out.append(("runners: the second waits for the first, each config once, the first's tree, profile and temp files gone before the second",
                r["trace"] == ["start CFG1 work=0 pgo=0 tmp=0", "end CFG1", "start CFG2 work=0 pgo=0 tmp=0", "end CFG2"]))
    out.append(("runners: a runner's failure does not stop the next, and it is logged", r["log"].count("exited with 3") == 2))
    names = {l.split("=", 1)[0] for l in r["env"].splitlines() if "=" in l}
    out.append(("runners: an empty environment: nothing of root's reaches the job's start, neither CARGO_* nor anything else",
                not names & set(leaks)
                and names <= {"HOME", "PATH", "LANG", "ACTIONS_RUNNER_HOOK_JOB_STARTED", "ACTIONS_RUNNER_INPUT_JITCONFIG", "PWD", "OLDPWD", "SHLVL", "_"}))
    out.append(("runners: HOME and PATH are the job's", "LANG=C.UTF-8" in r["env"] and "PATH=/home/runner/.cargo/bin:/opt/node/bin:" in r["env"]))
    out.append(("runners: the job of another run is refused before it starts, by the runner's own hook; this run's goes on",
                r["guard_ok"] == 0 and r["guard_other"].returncode != 0 and "serves only run 5" in r["guard_other"].stderr
                and r["guard_mode"] != 0 and "ACTIONS_RUNNER_HOOK_JOB_STARTED" in r["env"]))
    text = (HERE / "node_runners.sh").read_text(encoding="utf-8")
    out.append(("runners: no --preserve-environment, and no flag that the runner's `run` does not take", "--preserve-environment" not in text and "--disableupdate" not in text))
    return out


GROUPS = [group_turning_on, group_no_fallback, group_two_runs, group_off, group_bootstrap, group_github_blip, group_false_lost,
          group_lost_either_order, group_relaunch, group_stuck, group_second_runner, group_watch_stages, group_sweep, group_secrets,
          group_probe, group_runners]


def cases() -> list:
    out = []
    for group in GROUPS:
        try:
            out += group()
        except (Exception, SystemExit) as exc:
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
