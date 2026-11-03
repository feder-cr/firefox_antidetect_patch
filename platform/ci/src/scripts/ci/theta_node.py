#!/usr/bin/env python3
"""Turn the Theta node on and off for one run of release.yml.

The two x86_64 legs compile on one community machine, MACHINE, the only place
that names it; gates and tests stay on GitHub. A node is created for a run and
deleted after it. Stop and Start were measured against this on 2026-11-03 and
give nothing more: Start makes a new container with an empty disk, on another IP
and port, and keeps only the SSH key given at creation, so Stop would need a
long-lived key and leave no name saying which run owns the machine.

    provision --run R --attempt A   create, prepare, register, start the runners;
                                    prints runner= attempt= node= for $GITHUB_OUTPUT,
                                    runner= empty (and a warning) if anything failed,
                                    and the legs then run on GitHub's runners
    watch --run R --attempt A ...   wait for the legs; one lost runner reruns the
                                    whole run once with builder=github
    off --run R --attempt A         delete this run's node, confirm it is gone
    sweep [--max-age-hours H]       protection against a forgotten node, see sweep()
    status                          balance and our nodes

Runs from any machine, standard library and ssh only. Environment: THETA_API_KEY,
THETA_PROJECT; for what talks to GitHub GITHUB_REPOSITORY, GH_TOKEN, and
RUNNER_ADMIN_TOKEN (the runners' registration). No secret reaches the node except
the two single-job runner configurations, and none reaches a log.

Its cases: scripts/ci/test_theta_node.py.
"""
from __future__ import annotations

import argparse
import calendar
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import legs  # noqa: E402

MACHINE = "community_18a9f767a3e940349d2a634e173598f2"
API = "https://controller.thetaedgecloud.com"
GITHUB_API = "https://api.github.com"
TEMPLATE = "img_n67q5y3xu9pcyqqcajz5cd72263n"
# The only Ubuntu image the template accepts: the API answers 400 to any other
# ("container_image is not allowed by the GPU Node template"; ubuntu:24.04, measured).
IMAGE = "thetalabsorg/ubuntu-sshd:latest"
# The ceiling Theta enforces at creation; the machine was $0.156/hour when measured.
MAX_PRICE_USD_HOUR = 0.30
MIN_BALANCE_USD = 2.0
# Theta's list of machines lags: a machine just stopped or deleted answered 404
# ("GPU resource not found") for up to about 4 minutes (measured). Asked again, so a
# run right after another does not lose the node for that. A machine another
# customer holds answers the same, and costs this wait before GitHub's runners.
FREE_LIMIT_S = 360
UP_LIMIT_S = 600
BOOTSTRAP_LIMIT_S = 1200
GONE_LIMIT_S = 180
# Nobody needs a node older than this: stop-node's own job ends within 6 hours.
GUARD_HOURS = 8
# A queued leg with no other leg running on the node: the runner is not coming.
STUCK_S = 1200
# Polls in a row the API must deny the node before it counts as lost.
NODE_LOST_POLLS = 5
NAME = re.compile(r"^ci(\d+)a(\d+)$")


class ThetaError(Exception):
    pass


def urllib_transport(method, url, headers, body):
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


class Client:
    """JSON over HTTP. Reads retry; a create or a delete is the caller's to judge,
    because the request may have happened when the answer was lost."""

    def __init__(self, base, headers, secrets, transport=urllib_transport, sleep=time.sleep):
        self.base, self.headers, self.secrets = base, headers, [s for s in secrets if s]
        self.transport, self.sleep = transport, sleep

    def scrub(self, text):
        for secret in self.secrets:
            text = text.replace(secret, "<secret>")
        return text

    def call(self, method, path, payload=None, ok=(200, 201, 202, 204), tries=None):
        tries = tries or (4 if method == "GET" else 1)
        body = json.dumps(payload).encode() if payload is not None else None
        problem = ""
        for attempt in range(tries):
            if attempt:
                self.sleep(5 * attempt)
            try:
                status, raw = self.transport(method, self.base + path, self.headers, body)
            except OSError as exc:
                problem = "%s %s: %s" % (method, path, exc)
                continue
            if status in ok:
                return json.loads(raw) if raw.strip() else None, status
            problem = "%s %s: HTTP %d %s" % (method, path, status, raw[:300].decode("utf-8", "replace"))
            if status < 500 and status != 429:
                break
        raise ThetaError(self.scrub(problem))


class Theta:
    def __init__(self, project, key, transport=urllib_transport, sleep=time.sleep):
        self.project = project
        self.http = Client(API, {"x-api-key": key, "Accept": "application/json",
                                 "Content-Type": "application/json", "User-Agent": "firefox-ci"},
                           [project, key], transport, sleep)
        self.deployments = "/v1/projects/%s/gpu-deployments" % project

    def balance_usd(self):
        body, _ = self.http.call("GET", "/v1/projects/%s/billing/balance" % self.project)
        return float(body["body"]["balance_usd"])

    def nodes(self):
        body, _ = self.http.call("GET", self.deployments)
        return body["body"]["deployments"]

    def create(self, name, public_key, machine=MACHINE):
        body, _ = self.http.call("POST", self.deployments, {
            "name": name, "deployment_template_id": TEMPLATE, "resource_id": machine,
            "container_image": IMAGE, "ssh_public_key": public_key, "ssh_port": 22,
            "max_price_per_hour_usd": MAX_PRICE_USD_HOUR})
        return body["body"]["deployment"]["id"]

    def delete(self, node_id):
        self.http.call("DELETE", "%s/%s" % (self.deployments, node_id), ok=(200, 202, 204, 404), tries=4)

    def address(self, node_id):
        """(host, ssh port) once the container runs, else None. The only endpoint
        with both; it also carries passwords, which stop here."""
        body, _ = self.http.call("GET", "/deployments/list?project_id=%s" % self.project)
        for row in body.get("body") or []:
            if row.get("BaseID") == node_id:
                port = (row.get("PortMappings") or {}).get("22")
                host = row.get("NodePublicIP")
                if host and port and str(row.get("PodPhase", "")).endswith(": Running"):
                    return host, int(port)
        return None


class GitHub:
    def __init__(self, repo, token="", admin_token="", transport=urllib_transport, sleep=time.sleep):
        self.repo = repo
        base = {"Accept": "application/vnd.github+json", "User-Agent": "firefox-ci",
                "X-GitHub-Api-Version": "2022-11-28", "Content-Type": "application/json"}
        self.http = Client(GITHUB_API, dict(base, Authorization="Bearer %s" % token), [token, admin_token],
                           transport, sleep)
        self.admin = Client(GITHUB_API, dict(base, Authorization="Bearer %s" % admin_token), [token, admin_token],
                            transport, sleep)

    def jit_config(self, name, label):
        body, _ = self.admin.call("POST", "/repos/%s/actions/runners/generate-jitconfig" % self.repo, {
            "name": name, "runner_group_id": 1, "labels": [label],
            # relative to the runner's directory: /home/runner/work, as on GitHub's runners
            "work_folder": "../work"})
        return body["encoded_jit_config"]

    def remove_runners(self, label):
        body, _ = self.admin.call("GET", "/repos/%s/actions/runners?per_page=100" % self.repo)
        removed = []
        for runner in body.get("runners", []):
            if label in [l.get("name") for l in runner.get("labels", [])] and not runner.get("busy"):
                self.admin.call("DELETE", "/repos/%s/actions/runners/%d" % (self.repo, runner["id"]), tries=3)
                removed.append(runner["name"])
        return removed

    def jobs(self, run, attempt):
        body, _ = self.http.call("GET", "/repos/%s/actions/runs/%s/attempts/%s/jobs?per_page=100" % (self.repo, run, attempt))
        return body.get("jobs", [])

    def run_finished(self, run):
        """Whether a run needs nothing any more. A run GitHub does not know needs nothing."""
        body, status = self.http.call("GET", "/repos/%s/actions/runs/%s" % (self.repo, run), ok=(200, 404))
        return status == 404 or body.get("status") == "completed"

    def active_runs(self):
        found = []
        for status in ("in_progress", "queued"):
            body, _ = self.http.call("GET", "/repos/%s/actions/workflows/release.yml/runs?status=%s&per_page=30" % (self.repo, status))
            found += body.get("workflow_runs", [])
        return found

    def cancel(self, run):
        self.http.call("POST", "/repos/%s/actions/runs/%s/cancel" % (self.repo, run), ok=(200, 202, 409), tries=3)

    def dispatch_release(self, ref, inputs):
        self.http.call("POST", "/repos/%s/actions/workflows/release.yml/dispatches" % self.repo,
                       {"ref": ref, "inputs": inputs}, tries=3)


class Ssh:
    """The node's root shell. The image's host key is the same on every node
    (measured), so there is no host to verify: the node is not trusted anyway."""

    def __init__(self, host, port, key):
        self.argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=15",
                     "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=%s" % os.devnull,
                     "-o", "LogLevel=ERROR", "-o", "IdentitiesOnly=yes", "-i", str(key), "-p", str(port),
                     "root@%s" % host]

    def __call__(self, command, stdin=None, timeout=60):
        try:
            done = subprocess.run(self.argv + [command], input=stdin, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "", "timed out after %d s" % timeout
        return done.returncode, done.stdout.decode("utf-8", "replace"), done.stderr.decode("utf-8", "replace")


def new_key(directory: Path) -> Path:
    key = directory / "node_key"
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "ci-node", "-f", str(key)],
                   check=True, stdin=subprocess.DEVNULL)
    return key


def node_name(run, attempt) -> str:
    """Our mark on a node, and the run that owns it. Theta allows 1-20 alphanumerics."""
    name = "ci%sa%s" % (run, attempt)
    if not NAME.match(name) or len(name) > 20:
        raise ValueError("not a node name: %r" % name)
    return name


def runner_label(run) -> str:
    return "theta-%s" % run


def node_legs() -> list:
    return [leg.leg for leg in legs.LEGS if leg.node]


def say(text):
    print(text, file=sys.stderr, flush=True)


def script_bytes(name) -> bytes:
    # A Windows checkout may hold CRLF, which bash does not read.
    return (HERE / name).read_bytes().replace(b"\r\n", b"\n")


def create_when_free(theta, name, public_key, machine, clock=time.monotonic, sleep=time.sleep, limit=FREE_LIMIT_S):
    start = clock()
    while True:
        try:
            return theta.create(name, public_key, machine)
        except ThetaError as exc:
            if "HTTP 404" not in str(exc) or clock() - start >= limit:
                raise
            say("the machine is not available yet, asking again: %s" % exc)
        sleep(30)


def wait_up(theta, node_id, make_ssh, clock=time.monotonic, sleep=time.sleep, limit=UP_LIMIT_S):
    start = clock()
    while clock() - start < limit:
        address = theta.address(node_id)
        if address:
            ssh = make_ssh(*address)
            if ssh("true", timeout=30)[0] == 0:
                say("node up after %.0f s at %s:%d" % (clock() - start, address[0], address[1]))
                return ssh
        sleep(5)
    raise ThetaError("the node did not answer on SSH within %d s" % limit)


def turn_off(theta, run, attempt, clock=time.monotonic, sleep=time.sleep, limit=GONE_LIMIT_S) -> list:
    """Delete every node of this run and attempt, and see them gone. By name, so a
    node whose id nobody kept (a cancel during the create) goes too."""
    name, deleted = node_name(run, attempt), []
    start = clock()
    while True:
        mine = [n for n in theta.nodes() if n.get("name") == name]
        if not mine:
            return deleted
        for node in mine:
            theta.delete(node["id"])
            if node["id"] not in deleted:
                deleted.append(node["id"])
        if clock() - start >= limit:
            raise ThetaError("%s still listed %d s after its delete" % (name, limit))
        sleep(10)


def provision(theta, github, run, attempt, make_ssh, make_key, clock=time.monotonic, sleep=time.sleep,
              machine=MACHINE, runners=None) -> dict:
    """The node ready with its runners, or ThetaError with nothing left on."""
    balance = theta.balance_usd()
    if balance < MIN_BALANCE_USD:
        raise ThetaError("the Theta balance is $%.2f, below $%.2f" % (balance, MIN_BALANCE_USD))
    name, label = node_name(run, attempt), runner_label(run)
    key = make_key()
    public_key = Path(str(key) + ".pub").read_text(encoding="utf-8").strip()
    # The create inside the try: a lost answer can mean a node exists, and the
    # delete goes by name.
    try:
        node_id = create_when_free(theta, name, public_key, machine, clock, sleep)
        say("created %s on %s" % (name, machine))
        ssh = wait_up(theta, node_id, make_ssh, clock, sleep)
        code, out, err = ssh("bash -s", stdin=script_bytes("node_bootstrap.sh"), timeout=BOOTSTRAP_LIMIT_S)
        if code != 0:
            raise ThetaError("node_bootstrap.sh exited %d: %s" % (code, (err or out)[-600:]))
        say(out.strip().splitlines()[-1] if out.strip() else "prepared")
        configs = [github.jit_config("%s-%d" % (name, n + 1), label)
                   for n in range(runners or len(node_legs()))]
        code, out, err = ssh("cat > /usr/local/sbin/node_runners.sh", stdin=script_bytes("node_runners.sh"), timeout=60)
        if code != 0:
            raise ThetaError("could not put node_runners.sh on the node: %s" % err[-300:])
        code, out, err = ssh("bash /usr/local/sbin/node_runners.sh", stdin=("\n".join(configs) + "\n").encode(), timeout=60)
        if code != 0:
            raise ThetaError("node_runners.sh exited %d: %s" % (code, (err or out)[-300:]))
        return {"runner": label, "attempt": str(attempt), "node": node_id}
    except Exception:
        off_after_failure(theta, github, run, attempt, label, clock, sleep)
        raise


def remove_runners(github, label) -> list:
    """Housekeeping: GitHub removes a single-job runner by itself in the end. Its
    failure never makes a deleted node look like a failed delete."""
    try:
        return github.remove_runners(label)
    except ThetaError as exc:
        say("::warning::runners left registered: %s" % exc)
        return []


def off_after_failure(theta, github, run, attempt, label, clock, sleep):
    try:
        turn_off(theta, run, attempt, clock, sleep)
    except ThetaError as exc:
        say("::warning::the node could not be deleted, the sweeper will: %s" % exc)
    remove_runners(github, label)


def provision_outputs(theta, github, run, attempt, make_ssh, make_key, clock=time.monotonic, sleep=time.sleep,
                      machine=MACHINE) -> dict:
    """provision(), where every failure means the same: a warning, and GitHub's runners."""
    try:
        return provision(theta, github, run, attempt, make_ssh, make_key, clock, sleep, machine)
    except (ThetaError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        say("::warning::the Theta node is not available, the x86_64 legs run on GitHub's runners: %s" % exc)
        return {"runner": "", "attempt": str(attempt), "node": ""}


def runner_lost(job) -> bool:
    """GitHub fails the job of a runner that stopped answering, with no failed step.
    A compile error or a red test fails a step, and a cancel is not a failure."""
    return job.get("conclusion") == "failure" and not any(
        step.get("conclusion") == "failure" for step in job.get("steps") or [])


def parse_time(text):
    """Seconds since the epoch of an API time. Theta's carries from 0 to 6 decimals."""
    m = re.match(r"^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(?:\.\d+)?Z$", text or "")
    return calendar.timegm(tuple(int(g) for g in m.groups()) + (0, 0, 0)) if m else None


def stuck(jobs, label, now, limit=STUCK_S) -> bool:
    """A leg queued on the node's label with nothing running there, for too long.
    The second leg waiting behind the first is not stuck: it waits for hours."""
    mine = [j for j in jobs if label in (j.get("labels") or [])]
    queued = [j for j in mine if j.get("status") in ("queued", "waiting", "pending")]
    if not queued or any(j.get("status") == "in_progress" for j in mine):
        return False
    since = [parse_time(j.get("created_at")) for j in queued]
    since += [parse_time(j.get("completed_at")) for j in mine if j.get("status") == "completed"]
    since = [t for t in since if t is not None]
    return bool(since) and now - max(since) >= limit


def decide(jobs, label, node_missing_polls, now) -> str:
    """wait | done | lost: what the watcher does with what it sees."""
    mine = [j for j in jobs if label in (j.get("labels") or [])]
    if mine and all(j.get("status") == "completed" for j in mine):
        return "lost" if any(runner_lost(j) for j in mine) else "done"
    if node_missing_polls >= NODE_LOST_POLLS or stuck(jobs, label, now):
        return "lost"
    return "wait"


def retry_inputs(kind, tag, compile_cache, core_ref, wrapper_ref, gtests, gtest_filter) -> dict:
    """The same run on GitHub's runners. builder=github is what makes it the only
    retry: that run has no node to lose."""
    inputs = {"kind": kind, "builder": "github", "compile_cache": compile_cache}
    if tag:
        inputs["release_tag"] = tag
    for key, value in (("core_ref", core_ref), ("wrapper_ref", wrapper_ref), ("gtests", gtests),
                       ("gtest_filter", gtest_filter)):
        if value:
            inputs[key] = value
    return inputs


def watch(theta, github, run, attempt, ref, inputs, clock=time.time, sleep=time.sleep, poll=60) -> str:
    """Until the legs on the node are finished. Returns done or lost; lost has
    already asked for the rerun, and cancelled what was left of this run."""
    label, name = runner_label(run), node_name(run, attempt)
    missing = 0
    no_jobs = 0
    while True:
        jobs = github.jobs(run, attempt)
        try:
            present = any(n.get("name") == name and n.get("status") == "running" for n in theta.nodes())
            missing = 0 if present else missing + 1
        except ThetaError as exc:
            say("the Theta API did not answer, not read as a lost node: %s" % exc)
        verdict = decide(jobs, label, missing, clock())
        if not any(label in (j.get("labels") or []) for j in jobs):
            no_jobs += 1
            if no_jobs >= 10:
                return "done"
        if verdict == "done":
            return "done"
        if verdict == "lost":
            say("::warning::the node's runner was lost: the whole run goes again once, on GitHub's runners")
            github.dispatch_release(ref, inputs)
            github.cancel(run)
            return "lost"
        sleep(poll)


def sweep(theta, github, now, max_age_hours=GUARD_HOURS, dry_run=False) -> list:
    """Delete the nodes we started that no run needs: our name, and either older
    than the limit or owned by a run that has finished. A node with any other name
    was made by hand and is never touched. Without GitHub, the age alone."""
    deleted = []
    for node in theta.nodes():
        m = NAME.match(node.get("name") or "")
        if not m:
            continue
        created = parse_time(node.get("created_at"))
        old = created is not None and now - created >= max_age_hours * 3600
        finished = False
        if github and not old:
            try:
                finished = github.run_finished(m.group(1))
            except ThetaError as exc:
                say("run %s unknown for now, its node stays: %s" % (m.group(1), exc))
        if old or finished:
            say("%s %s: %s" % ("would delete" if dry_run else "deleting", node["name"],
                               "older than %d h" % max_age_hours if old else "its run has finished"))
            if not dry_run:
                theta.delete(node["id"])
            deleted.append(node["name"])
    return deleted


def cancel_stuck(github, now, dry_run=False) -> list:
    """Runs whose leg waits on a label no runner will come for, and whose stop-node
    is not there to see it."""
    cancelled = []
    for run in github.active_runs():
        label = runner_label(run["id"])
        if stuck(github.jobs(run["id"], run.get("run_attempt", 1)), label, now):
            say("%s run %s: queued on %s with no runner" % ("would cancel" if dry_run else "cancelling", run["id"], label))
            if not dry_run:
                github.cancel(run["id"])
            cancelled.append(run["id"])
    return cancelled


def from_env():
    missing = [v for v in ("THETA_API_KEY", "THETA_PROJECT") if not os.environ.get(v)]
    if missing:
        raise ThetaError("missing from the environment: %s" % ", ".join(missing))
    return Theta(os.environ["THETA_PROJECT"], os.environ["THETA_API_KEY"])


def github_from_env():
    repo = os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        return None
    return GitHub(repo, os.environ.get("GH_TOKEN", ""), os.environ.get("RUNNER_ADMIN_TOKEN", ""))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the Theta node of a release run")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("provision", "off", "watch"):
        p = sub.add_parser(name)
        p.add_argument("--run", required=True)
        p.add_argument("--attempt", required=True)
        if name == "provision":
            p.add_argument("--machine", default=MACHINE, help="for a test of this script on another machine")
        if name == "watch":
            for option in ("ref", "kind", "tag", "compile-cache", "core-ref", "wrapper-ref", "gtests", "gtest-filter"):
                p.add_argument("--" + option, default="")
    s = sub.add_parser("sweep")
    s.add_argument("--max-age-hours", type=float, default=GUARD_HOURS)
    s.add_argument("--dry-run", action="store_true")
    sub.add_parser("status")
    a = ap.parse_args(argv)

    if a.cmd == "provision":
        # Whatever goes wrong, even here, the legs have GitHub's runners to go to.
        try:
            theta, github = from_env(), github_from_env()
            if github is None:
                raise ThetaError("GITHUB_REPOSITORY is not set")
        except ThetaError as exc:
            say("::warning::the Theta node is not available, the x86_64 legs run on GitHub's runners: %s" % exc)
            print("runner=\nattempt=%s\nnode=" % a.attempt)
            return 0
        work = Path(tempfile.mkdtemp(prefix="theta-node-"))
        try:
            outputs = provision_outputs(theta, github, a.run, a.attempt,
                                        lambda host, port: Ssh(host, port, work / "node_key"),
                                        lambda: new_key(work), machine=a.machine)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        for key, value in outputs.items():
            print("%s=%s" % (key, value))
        return 0

    try:
        theta = from_env()
        github = github_from_env()
        if a.cmd == "off":
            print("deleted: %s" % (", ".join(turn_off(theta, a.run, a.attempt)) or "nothing was on"))
            if github and os.environ.get("RUNNER_ADMIN_TOKEN"):
                print("runners removed: %s" % (", ".join(remove_runners(github, runner_label(a.run))) or "none"))
        elif a.cmd == "watch":
            if github is None:
                raise ThetaError("GITHUB_REPOSITORY is not set")
            inputs = retry_inputs(a.kind, a.tag, a.compile_cache, a.core_ref, a.wrapper_ref, a.gtests, a.gtest_filter)
            print("legs: %s" % watch(theta, github, a.run, a.attempt, a.ref, inputs))
        elif a.cmd == "sweep":
            now = time.time()
            print("deleted: %s" % (", ".join(sweep(theta, github, now, a.max_age_hours, a.dry_run)) or "nothing"))
            if github:
                print("cancelled: %s" % (", ".join(str(r) for r in cancel_stuck(github, now, a.dry_run)) or "nothing"))
        elif a.cmd == "status":
            print("balance: $%.2f" % theta.balance_usd())
            for node in theta.nodes():
                print("%s %s %s" % (node.get("name"), node.get("status"), node.get("created_at")))
    except ThetaError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
