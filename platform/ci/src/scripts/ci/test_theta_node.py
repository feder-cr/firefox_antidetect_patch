#!/usr/bin/env python3
"""theta_node.py against a fake Theta API, a fake GitHub API and a fake SSH.

  turning on
    1. one machine, named in one place, under the run's name, with this run's key;
    2. prepared, then one single-job runner per x86_64 leg on one label;
    3. the machine taken, the balance low, the API down, no SSH, a bootstrap or a
       registration that fails: a warning, nothing left on, and GitHub's runners;
    4. a create whose answer is lost still deletes the node it may have made;
  turning off
    5. by name, this run's only, until the API no longer lists it;
    6. a delete that never takes is an error, not a success;
  the watcher
    7. a job failed with no failed step is a lost runner; a failed step or a cancel
       is not;
    8. lost means one rerun with builder=github, with the run's own inputs, and a
       cancel of what is left; a compile error means neither;
    9. the second leg waiting behind the first is not stuck; with nothing running
       for 20 minutes it is;
  the sweeper
   10. only our names, only with no run to serve: a node made by hand is never
       touched, however old;
   11. a GitHub error is not "the run has finished";
  secrets
   12. neither the key nor the project reaches an error message, and a node's
       password does not leave the address call;
  the runners on the node
   13. one after the other, the tree of the first gone before the second (Linux).

    python3 scripts/ci/test_theta_node.py
"""
from __future__ import annotations

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


class FakeTheta:
    def __init__(self, balance=10.5, create_status=201, create_lost=False, rows=(RUNNING,), delete_takes=True):
        self.balance, self.create_status, self.create_lost = balance, create_status, create_lost
        self.rows, self.delete_takes = list(rows), delete_takes
        self.nodes, self.created, self.deleted = [], [], []

    def __call__(self, method, url, headers, body):
        assert headers["x-api-key"] == KEY
        path = url.split("controller.thetaedgecloud.com", 1)[1]
        if path.endswith("/billing/balance"):
            return 200, json.dumps({"body": {"balance_usd": self.balance}}).encode()
        if path.startswith("/deployments/list"):
            return 200, json.dumps({"body": self.rows}).encode()
        if path.endswith("/gpu-deployments") and method == "GET":
            return 200, json.dumps({"body": {"deployments": self.nodes}}).encode()
        if path.endswith("/gpu-deployments") and method == "POST":
            sent = json.loads(body)
            self.created.append(sent)
            if self.create_lost:
                self.nodes.append({"id": "dpl_1", "name": sent["name"], "status": "unknown"})
                raise OSError("connection reset")
            if self.create_status != 201:
                return self.create_status, b'{"message":"machine not available"}'
            self.nodes.append({"id": "dpl_1", "name": sent["name"], "status": "running"})
            return 201, json.dumps({"body": {"deployment": {"id": "dpl_1"}}}).encode()
        if method == "DELETE":
            node_id = path.rsplit("/", 1)[1]
            self.deleted.append(node_id)
            if self.delete_takes:
                self.nodes = [n for n in self.nodes if n["id"] != node_id]
            return 200, b"{}"
        raise AssertionError("unexpected %s %s" % (method, path))


class FakeGitHub:
    def __init__(self, jit_status=201):
        self.jit_status = jit_status
        self.jits, self.dispatches, self.cancels, self.removed = [], [], [], []
        self.jobs_answers, self.run_status = [], {}

    def __call__(self, method, url, headers, body):
        path = url.split("api.github.com", 1)[1]
        if path.endswith("generate-jitconfig"):
            self.jits.append(json.loads(body))
            if self.jit_status != 201:
                return self.jit_status, b'{"message":"Bad credentials"}'
            return 201, json.dumps({"encoded_jit_config": "JIT%d" % len(self.jits)}).encode()
        if "/actions/runners?" in path:
            return 200, json.dumps({"runners": [
                {"id": 7, "name": "ci5a1-2", "busy": False, "labels": [{"name": "theta-5"}]},
                {"id": 8, "name": "other", "busy": False, "labels": [{"name": "self-hosted"}]}]}).encode()
        if method == "DELETE" and "/actions/runners/" in path:
            self.removed.append(path.rsplit("/", 1)[1])
            return 204, b""
        if "/jobs" in path:
            answer = self.jobs_answers.pop(0) if len(self.jobs_answers) > 1 else self.jobs_answers[0]
            return 200, json.dumps({"jobs": answer}).encode()
        if path.endswith("/dispatches"):
            self.dispatches.append(json.loads(body))
            return 204, b""
        if path.endswith("/cancel"):
            self.cancels.append(path)
            return 202, b""
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


def run_provision(theta_api, github_api, ssh):
    clock = Clock()
    theta = t.Theta(PROJECT, KEY, theta_api, clock.sleep)
    github = t.GitHub("o/r", "TOKEN", "ADMIN", github_api, clock.sleep)
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "node_key"
        key.write_text("private", encoding="utf-8")
        Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA ci-node\n", encoding="utf-8")
        stderr, out = sys.stderr, None
        sink = open(os.devnull, "w", encoding="utf-8")
        sys.stderr = sink
        try:
            out = t.provision_outputs(theta, github, 5, 1, lambda host, port: ssh, lambda: key, clock, clock.sleep)
        finally:
            sys.stderr = stderr
            sink.close()
    return out


def job(name, status="completed", conclusion="success", steps=(), created="2026-11-03T10:00:00Z", completed=None, label="theta-5"):
    return {"name": name, "status": status, "conclusion": conclusion, "labels": [label], "created_at": created,
            "completed_at": completed, "steps": [{"name": "s", "conclusion": c} for c in steps]}


NOW = t.parse_time("2026-11-03T12:00:00Z")


def quiet(fn, *args, **kwargs):
    stderr = sys.stderr
    sink = open(os.devnull, "w", encoding="utf-8")
    sys.stderr = sink
    try:
        return fn(*args, **kwargs)
    finally:
        sys.stderr = stderr
        sink.close()


def runners_case() -> tuple:
    """node_runners.sh, as it runs on the node, with the system's commands faked."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        home, bin_dir, scratch = tmp / "home", tmp / "bin", tmp / "tmp"
        (home / "actions-runner").mkdir(parents=True)
        bin_dir.mkdir()
        scratch.mkdir()
        trace = tmp / "trace"
        run = home / "actions-runner" / "run.sh"
        run.write_text("#!/bin/bash\n"
                       'echo "start $ACTIONS_RUNNER_INPUT_JITCONFIG work=$(ls "$HOME/work" | wc -l) pgo=$(ls "$HOME/pgo" 2>/dev/null | wc -l) tmp=$(ls "%s" | wc -l)" >> "%s"\n'
                       'mkdir -p "$HOME/pgo"; touch "$HOME/work/left" "$HOME/pgo/merged.profdata" "%s/left"\n'
                       'sleep 0.3\necho "end $ACTIONS_RUNNER_INPUT_JITCONFIG" >> "%s"\nexit 3\n' % (scratch, trace, scratch, trace),
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
                   NODE_RUNNER_LOG=str(log), NODE_RUNNER_TMP=str(scratch))
        script = str(HERE / "node_runners.sh")
        done = subprocess.run(["bash", script], input="CFG1\nCFG2\n", env=env, capture_output=True, text=True, timeout=30)
        empty = subprocess.run(["bash", script], input="", env=env, capture_output=True, text=True, timeout=30)
        for _ in range(100):
            if log.exists() and "all runners done" in log.read_text():
                break
            subprocess.run(["sleep", "0.1"])
        return done.returncode, empty.returncode, trace.read_text().splitlines(), log.read_text(), sorted(p.name for p in (home / "work").iterdir())


def cases() -> list:
    out = []

    api, gh, ssh = FakeTheta(), FakeGitHub(), FakeSsh()
    got = run_provision(api, gh, ssh)
    sent = api.created[0] if api.created else {}
    out.append(("on: the one machine, under the run's name, with this run's key and a price ceiling",
                sent.get("resource_id") == t.MACHINE and sent.get("name") == "ci5a1"
                and sent.get("ssh_public_key") == "ssh-ed25519 AAAA ci-node" and sent.get("ssh_port") == 22
                and sent.get("container_image") == t.IMAGE and sent.get("max_price_per_hour_usd") == t.MAX_PRICE_USD_HOUR))
    source = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("on: the machine is named in one place", source.count(t.MACHINE) == 1))
    out.append(("on: the outputs name one label, for this attempt",
                got == {"runner": "theta-5", "attempt": "1", "node": "dpl_1"}))
    out.append(("on: one single-job runner per x86_64 leg, on one label, working in ~/work",
                [j["name"] for j in gh.jits] == ["ci5a1-1", "ci5a1-2"]
                and all(j["labels"] == ["theta-5"] and j["work_folder"] == "../work" for j in gh.jits)
                and t.node_legs() == ["linux-x86_64", "win-x86_64"]))
    commands = [c for c, _ in ssh.calls]
    stdin = dict(ssh.calls)
    out.append(("on: prepared before the runners start, and the configurations go by stdin, never on a command line",
                commands.index("bash -s") < commands.index("bash /usr/local/sbin/node_runners.sh")
                and b"node_bootstrap" in stdin["bash -s"] and b"\r" not in stdin["bash -s"]
                and stdin["bash /usr/local/sbin/node_runners.sh"] == b"JIT1\nJIT2\n"
                and not any("JIT" in c for c in commands)))
    out.append(("on: a node that went well stays on", api.deleted == []))

    fallback = {"runner": "", "attempt": "1", "node": ""}
    api, gh = FakeTheta(create_status=409), FakeGitHub()
    got = run_provision(api, gh, FakeSsh())
    out.append(("on: a refusal (the price, say) is final: GitHub's runners at once, nothing on",
                got == fallback and api.nodes == [] and gh.jits == [] and len(api.created) == 1))
    api, gh = FakeTheta(create_status=404), FakeGitHub()
    got = run_provision(api, gh, FakeSsh())
    out.append(("on: the machine is held by someone else: asked again for the limit, then GitHub's runners",
                got == fallback and api.nodes == [] and gh.jits == [] and len(api.created) == 1 + t.FREE_LIMIT_S // 30))

    class FreedLater(FakeTheta):
        def __call__(self, method, url, headers, body):
            if method == "POST" and len(self.created) < 3:
                self.created.append(json.loads(body))
                return 404, b'{"message":"GPU resource not found"}'
            return super().__call__(method, url, headers, body)
    api = FreedLater()
    got = run_provision(api, FakeGitHub(), FakeSsh())
    out.append(("on: a machine just freed, still 404 in Theta's list, is waited for", got.get("runner") == "theta-5" and len(api.created) == 4))
    api = FakeTheta(balance=1.5)
    got = run_provision(api, FakeGitHub(), FakeSsh())
    out.append(("on: the balance is below $2: GitHub's runners, nothing created", got == fallback and api.created == []))
    api = FakeTheta(create_lost=True)
    got = run_provision(api, FakeGitHub(), FakeSsh())
    out.append(("on: the answer to the create is lost: the node it may have made is deleted", got == fallback and api.deleted == ["dpl_1"]))
    api = FakeTheta(rows=())
    got = run_provision(api, FakeGitHub(), FakeSsh())
    out.append(("on: a node never Running within the limit is deleted", got == fallback and api.deleted == ["dpl_1"] and api.nodes == []))
    api = FakeTheta()
    got = run_provision(api, FakeGitHub(), FakeSsh({"true": (255, "", "refused")}))
    out.append(("on: no SSH within the limit: deleted", got == fallback and api.deleted == ["dpl_1"]))
    api, gh = FakeTheta(), FakeGitHub()
    got = run_provision(api, gh, FakeSsh({"bash -s": (1, "", "node_bootstrap: ERROR: missing: git")}))
    out.append(("on: a bootstrap that refuses: deleted, and nothing registered", got == fallback and api.deleted == ["dpl_1"] and gh.jits == []))
    api, gh = FakeTheta(), FakeGitHub(jit_status=401)
    got = run_provision(api, gh, FakeSsh())
    out.append(("on: GitHub refuses the registration: deleted", got == fallback and api.deleted == ["dpl_1"]))
    api, gh = FakeTheta(), FakeGitHub()
    got = run_provision(api, gh, FakeSsh({"bash /usr/local/sbin/node_runners.sh": (1, "", "no")}))
    out.append(("on: the runners do not start: deleted, and the registered ones removed",
                got == fallback and api.deleted == ["dpl_1"] and gh.removed == ["7"]))

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
    out.append(("off: runners GitHub will not list are a warning, never a failed delete",
                quiet(t.remove_runners, t.GitHub("o/r", "T", "BAD", Refused(), clock.sleep), "theta-5") == []))
    bootstrap = (HERE / "node_bootstrap.sh").read_text(encoding="utf-8")
    snapshot = bootstrap.index("snapshot.ubuntu.com")
    out.append(("bootstrap: CA certificates before the snapshot, whose redirect to HTTPS needs them (a fresh node found no package)",
                0 < bootstrap.index("--no-install-recommends ca-certificates") < snapshot))
    out.append(("bootstrap: a failed index stops it, by name, both times",
                bootstrap.count("apt-get update -qq -o APT::Update::Error-Mode=any") == 2 and "apt-get update -qq\n" not in bootstrap))
    out.append(("bootstrap: every download by version and sha256, nothing else fetched",
                all(v in bootstrap for v in ("RUNNER_SHA256=", "NODE_SHA256=", "RUSTUP_SHA256=", "GLSLANG_SHA256="))
                and bootstrap.count("  fetch ") == 0 or bootstrap.count("fetch \"https://") == 4))
    as_runner = next(l for l in bootstrap.splitlines() if l.startswith("as_runner()"))
    out.append(("bootstrap: its checks see the PATH the job will have, where glslang 14 comes before Ubuntu's 11.8",
                "/usr/local/bin:" in as_runner and as_runner.index("/usr/local/bin") < as_runner.index("/usr/bin")
                and "/usr/local/bin:/usr/sbin:/usr/bin" in bootstrap.split("> $R/.path")[0].rsplit("echo ", 1)[1]))
    out.append(("names: run ids of today fit Theta's 20 characters", t.node_name(41647921995, 12) == "ci41647921995a12" and len(t.node_name(41647921995, 12)) <= 20))
    out.append(("time: Theta's 5 decimals and GitHub's none", t.parse_time("2026-11-03T14:35:38.39446Z") == t.parse_time("2026-11-03T14:35:38Z") and t.parse_time("nonsense") is None))

    failed_step = job("build-linux-x86_64", conclusion="failure", steps=("success", "failure", "skipped"))
    lost = job("build-linux-x86_64", conclusion="failure", steps=("success", "cancelled"))
    no_steps = job("build-linux-x86_64", conclusion="failure")
    cancelled = job("build-linux-x86_64", conclusion="cancelled")
    out.append(("lost: a failed job with no failed step, also one with no steps at all", t.runner_lost(lost) and t.runner_lost(no_steps)))
    out.append(("lost: not a compile error, not a cancel", not t.runner_lost(failed_step) and not t.runner_lost(cancelled)))

    def watched(answers, nodes_on=True):
        clock = Clock()
        api = FakeTheta()
        if nodes_on:
            api.nodes = [{"id": "dpl_1", "name": "ci5a1", "status": "running"}]
        gh = FakeGitHub()
        gh.jobs_answers = answers
        theta, github = t.Theta(PROJECT, KEY, api, clock.sleep), t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep)
        inputs = t.retry_inputs("release", "firefox-55", "off", "main", "", "true", "*")
        result = quiet(t.watch, theta, github, 5, 1, "main", inputs, lambda: NOW + clock.now, clock.sleep)
        return result, gh

    ok_win = job("build-win-x86_64", completed="2026-11-03T11:00:00Z")
    result, gh = watched([[failed_step, ok_win]])
    out.append(("watch: a compile error is a defect: no rerun, no cancel", result == "done" and not gh.dispatches and not gh.cancels))
    result, gh = watched([[lost, ok_win]])
    out.append(("watch: a lost runner reruns once with builder=github, with the run's inputs",
                result == "lost" and gh.dispatches == [{"ref": "main", "inputs": {
                    "kind": "release", "builder": "github", "compile_cache": "off", "release_tag": "firefox-55",
                    "core_ref": "main", "gtests": "true", "gtest_filter": "*"}}]))
    result, gh = watched([[cancelled, job("build-win-x86_64", conclusion="cancelled")]])
    out.append(("watch: a run cancelled by hand does not come back", result == "done" and not gh.dispatches))
    second_waits = job("build-win-x86_64", status="queued", conclusion=None, created="2026-11-03T01:00:00Z")
    running = job("build-linux-x86_64", status="in_progress", conclusion=None)
    result, gh = watched([[running, second_waits], [running, second_waits], [ok_win, job("build-linux-x86_64", completed="2026-11-03T11:30:00Z")]])
    out.append(("watch: the second leg waiting for hours behind the first is no problem", result == "done" and not gh.dispatches))
    result, gh = watched([[running, second_waits]], nodes_on=False)
    out.append(("watch: the node gone from the API: rerun, and what is left of the run cancelled", result == "lost" and len(gh.dispatches) == 1 and len(gh.cancels) == 1))
    result, gh = watched([[job("build-linux-x86_64", completed="2026-11-03T11:30:00Z"), second_waits]])
    out.append(("watch: the first leg finished and no runner for the second in 20 minutes: rerun", result == "lost" and len(gh.dispatches) == 1))
    out.append(("stuck: not within the limit", not t.stuck([job("a", completed="2026-11-03T11:50:00Z"), second_waits], "theta-5", NOW)))
    out.append(("stuck: not with a leg running", not t.stuck([running, second_waits], "theta-5", NOW)))
    out.append(("stuck: not on another label", not t.stuck([second_waits], "theta-9", NOW)))
    out.append(("stuck: queued alone past the limit", t.stuck([second_waits], "theta-5", NOW)))
    out.append(("retry: a candidate carries no tag and no empty inputs",
                t.retry_inputs("candidate", "", "gha", "", "", "", "") == {"kind": "candidate", "builder": "github", "compile_cache": "gha"}))

    def swept(nodes, run_status=None, github=True, **kwargs):
        clock = Clock()
        api, gh = FakeTheta(), FakeGitHub()
        api.nodes = nodes
        gh.run_status = run_status or {}
        theta = t.Theta(PROJECT, KEY, api, clock.sleep)
        hub = t.GitHub("o/r", "TOKEN", "ADMIN", gh, clock.sleep) if github else None
        return quiet(t.sweep, theta, hub, NOW, **kwargs), api

    young = "2026-11-03T11:00:00Z"
    old = "2026-11-03T01:00:00Z"
    by_hand = {"id": "dpl_lab", "name": "gpunodegn8rdcmn6agw1", "created_at": "2020-01-01T00:00:00Z"}
    cinema = {"id": "dpl_c", "name": "cinema", "created_at": "2020-01-01T00:00:00Z"}
    gone, api = swept([by_hand, cinema, {"id": "d1", "name": "ci5a1", "created_at": old}], {"5": "in_progress"})
    out.append(("sweep: past the limit ours goes even if its run says it is alive; by hand or another prefix never, however old",
                gone == ["ci5a1"] and api.deleted == ["d1"]))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": "in_progress"})
    out.append(("sweep: a young node whose run still needs it stays", gone == [] and api.deleted == []))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": "completed"})
    out.append(("sweep: a young node whose run has finished goes", gone == ["ci5a1"] and api.deleted == ["d1"]))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}], {"5": 500})
    out.append(("sweep: GitHub's error is not 'the run has finished'", gone == [] and api.deleted == []))
    gone, api = swept([{"id": "d1", "name": "ci5a1", "created_at": young}, {"id": "d2", "name": "ci6a1", "created_at": old}], github=False)
    out.append(("sweep: from a machine with no GitHub, the age alone", gone == ["ci6a1"] and api.deleted == ["d2"]))
    gone, api = swept([{"id": "d2", "name": "ci6a1", "created_at": old}], dry_run=True)
    out.append(("sweep: a dry run deletes nothing", gone == ["ci6a1"] and api.deleted == []))

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

    if sys.platform.startswith("linux"):
        code, empty, trace, log, left = runners_case()
        out.append(("runners: returns at once, and refuses no configurations", code == 0 and empty != 0))
        out.append(("runners: one after the other, each config once, the first's tree, profile and temp files gone before the second",
                    trace == ["start CFG1 work=0 pgo=0 tmp=0", "end CFG1", "start CFG2 work=0 pgo=0 tmp=0", "end CFG2"]))
        out.append(("runners: a runner's failure does not stop the next, and the end leaves nothing",
                    "runner 1 exited with 3" in log and "runner 2 exited with 3" in log and left == []))
    else:
        print("skip  runners: node_runners.sh runs on Linux only")
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
