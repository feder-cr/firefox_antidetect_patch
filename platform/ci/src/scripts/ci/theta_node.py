#!/usr/bin/env python3
"""Turn the Theta node on and off for one run of release.yml.

The two x86_64 legs compile on one community machine, MACHINE, the only place
that names it, and nowhere else: there is no other machine and no other kind of
runner for them. Gates and tests stay on GitHub. A node is created for a run and
deleted after it. Stop and Start were measured against this on 2026-11-03 and
give nothing more: Start makes a new container with an empty disk, on another IP
and port, and keeps only the SSH key given at creation, so Stop would need a
long-lived key and leave no name saying which run owns the machine.

    provision --run R --attempt A --access-out F
                                    create, prepare, start the first leg's runner and
                                    see it online; prints runner= attempt= node= for
                                    $GITHUB_OUTPUT and writes F, this node's SSH key
                                    encrypted for the watch. Any failure: exit 1, nothing
                                    left on, and the run has failed.
    watch --run R --attempt A --ref REF --access F
                                    wait for the legs, start each next leg's runner when
                                    the last has finished; prints verdict= done | lost |
                                    failed | continue. A machine lost mid-build re-launches
                                    the whole run once, on a new node of the same machine.
    off --run R --attempt A [--if-legs-done]
                                    delete this run's node, confirm it is gone; with the
                                    option, only once GitHub says the legs have finished
    sweep [--max-age-hours H]       protection against a forgotten node, see sweep()
    status                          balance and our nodes

Runs from any machine, standard library, ssh and openssl only. Environment:
THETA_API_KEY, THETA_PROJECT; for what talks to GitHub GITHUB_REPOSITORY, GH_TOKEN,
and RUNNER_ADMIN_TOKEN (the runners' registration). No secret reaches the node except
one single-job runner configuration at a time, and none reaches a log.

Its cases: scripts/ci/test_theta_node.py.
"""
from __future__ import annotations

import argparse
import calendar
import hashlib
import hmac
import json
import os
import re
import shutil
import socket
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
# After our own Delete the machine answers 404 ("GPU resource not found") for 2 to 4
# minutes (measured). A machine someone else holds answers the same, so the wait is
# what tells them apart, and it is bounded: past it the machine is taken.
FREE_LIMIT_S = 360
UP_LIMIT_S = 600
BOOTSTRAP_LIMIT_S = 1200
GONE_LIMIT_S = 180
# A runner that has not come online by then never will; a queued leg would wait for
# it up to 24 hours (GitHub's limit), holding the run.
RUNNER_ONLINE_LIMIT_S = 300
# Two legs of 350 minutes each (build's timeout-minutes) and the 40-odd minutes of
# the node job: a healthy run is not older than this.
GUARD_HOURS = 13
# The machine does not answer for this long, and a leg's job agrees, before it counts
# as lost. How long GitHub takes to fail a dead runner's job is not established, so
# nothing below waits for that and nothing below assumes it has not happened yet.
DOWN_LIMIT_S = 900
# A queued leg with nothing running on a machine that answers: its runner is not coming.
STUCK_S = 1200
# The sweeper's own limit for the same thing, for a run whose watch has gone: well
# after the watch's, which is the one that can still re-launch.
SWEEP_STUCK_S = 3600
# GitHub's API down for this many polls in a row: the watch stops, and leaves the node on.
BLIND_POLLS = 30
# Polls with no leg listed at all (a run cancelled before its matrix): nothing to wait for.
NO_LEGS_POLLS = 15
START_TRIES = 5
# One watch job. GitHub stops a job on its runners at 360 minutes, and two legs can
# take 700: the watch is three jobs in a row (test_release_builder.py adds them up).
STAGE_LIMIT_S = 335 * 60
# What gpu-deployments says of a node that runs (measured 2026-11-03; PodPhase in
# /deployments/list says "<pod>: Running"). No other string has been seen.
RUNNING = "running"
WAITING = ("queued", "waiting", "pending", "requested")
NAME = re.compile(r"^ci(\d+)a(\d+)$")


class ThetaError(Exception):
    pass


class WatchBlind(Exception):
    """GitHub has not answered for BLIND_POLLS polls: nothing is known of the legs."""


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

    def create(self, name, public_key):
        body, _ = self.http.call("POST", self.deployments, {
            "name": name, "deployment_template_id": TEMPLATE, "resource_id": MACHINE,
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
        # Whether GitHub adds labels of its own to a JIT runner is not established: said in the log.
        say("%s registered with labels: %s" % (name, ", ".join(
            l.get("name", "") for l in (body.get("runner") or {}).get("labels", [])) or "(not in the answer)"))
        return body["encoded_jit_config"]

    def runners(self):
        body, _ = self.admin.call("GET", "/repos/%s/actions/runners?per_page=100" % self.repo)
        return body.get("runners", [])

    def runner_status(self, name):
        """online, offline, or None if no runner has that name."""
        for runner in self.runners():
            if runner.get("name") == name:
                return runner.get("status") or "offline"
        return None

    def remove_runners(self, prefix):
        """The idle runners of this run and attempt, whichever leg they were for."""
        removed = []
        for runner in self.runners():
            labels = [l.get("name") or "" for l in runner.get("labels", [])]
            if any(l.startswith(prefix + "-") for l in labels) and not runner.get("busy"):
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

    def relaunched(self, run) -> bool:
        """Whether a re-launch of this run already exists: release.yml's run-name says so."""
        body, _ = self.http.call("GET", "/repos/%s/actions/workflows/release.yml/runs?event=workflow_dispatch&per_page=100" % self.repo)
        return any(r.get("display_title") == relaunch_title(run) for r in body.get("workflow_runs", []))


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


def runner_prefix(run, attempt) -> str:
    """Every label of this run and attempt starts with it, and no other's does."""
    return "theta-%s-%s" % (run, attempt)


def leg_label(run, attempt, leg) -> str:
    """One label, one leg, one single-job runner."""
    return "%s-%s" % (runner_prefix(run, attempt), leg)


def relaunch_title(run) -> str:
    """The name of the run that re-launches this one (release.yml's run-name)."""
    return "release, relaunch of %s" % run


def node_legs() -> list:
    return [leg.leg for leg in legs.LEGS if leg.node]


def say(text):
    print(text, file=sys.stderr, flush=True)


def script_bytes(name) -> bytes:
    # A Windows checkout may hold CRLF, which bash does not read.
    return (HERE / name).read_bytes().replace(b"\r\n", b"\n")


def create_when_free(theta, name, public_key, clock=time.monotonic, sleep=time.sleep, limit=FREE_LIMIT_S):
    start = clock()
    while True:
        try:
            return theta.create(name, public_key)
        except ThetaError as exc:
            if "HTTP 404" not in str(exc):
                raise
            if clock() - start >= limit:
                raise ThetaError("the machine %s is taken: it still answers 404 after %d s, longer than our own recent "
                                 "delete can explain, and there is no other machine for this run (%s)"
                                 % (MACHINE, limit, exc)) from None
            say("the machine is not free yet, asking again: %s" % exc)
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


def refuse_if_taken(theta, github, run):
    """One machine, one run. A live run's node, or any node of this project on the
    machine, refuses this run at once. What no run needs any more goes, and
    create_when_free waits out its 404. A node with another name on another machine
    is other work and is not looked at."""
    for node in theta.nodes():
        name = node.get("name") or ""
        ours = NAME.match(name)
        if ours and ours.group(1) == str(run):
            say("an earlier attempt of this run left %s: deleting it" % name)
            theta.delete(node["id"])
        elif ours and github.run_finished(ours.group(1)):
            say("run %s has finished and left %s: deleting it" % (ours.group(1), name))
            theta.delete(node["id"])
        elif ours:
            raise ThetaError("run %s holds the machine with %s, and two runs cannot share it: "
                             "wait for it or cancel it, then run this again" % (ours.group(1), name))
        elif node.get("machine_type") == MACHINE:
            raise ThetaError("the machine %s is taken by %s, a node of this project that CI did not start: "
                             "there is no other machine for this run" % (MACHINE, name))


def start_runner(github, ssh, run, attempt, n):
    """Register leg number n (from 1) now and start its runner. Registered only when
    it is due, because how long an unused configuration stays valid is not established.
    The node waits for the runner before it by itself."""
    leg = node_legs()[n - 1]
    config = github.jit_config("%s-%d" % (node_name(run, attempt), n), leg_label(run, attempt, leg))
    code, out, err = ssh("bash /usr/local/sbin/node_runners.sh %s" % run, stdin=(config + "\n").encode(), timeout=60)
    if code != 0:
        raise ThetaError("node_runners.sh exited %d: %s" % (code, (err or out)[-300:]))
    say("runner %d started, for %s" % (n, leg))


def wait_online(github, name, clock=time.monotonic, sleep=time.sleep, limit=RUNNER_ONLINE_LIMIT_S):
    start = clock()
    while True:
        if github.runner_status(name) == "online":
            return
        if clock() - start >= limit:
            raise ThetaError("the runner %s did not come online within %d s: the leg would wait for it up to GitHub's "
                             "24-hour limit" % (name, limit))
        sleep(10)


def provision(theta, github, run, attempt, make_ssh, make_key, clock=time.monotonic, sleep=time.sleep) -> dict:
    """The node ready with the first leg's runner online, or ThetaError with nothing left on."""
    refuse_if_taken(theta, github, run)
    balance = theta.balance_usd()
    if balance < MIN_BALANCE_USD:
        raise ThetaError("the Theta balance is $%.2f, below $%.2f" % (balance, MIN_BALANCE_USD))
    name, prefix = node_name(run, attempt), runner_prefix(run, attempt)
    key = make_key()
    public_key = Path(str(key) + ".pub").read_text(encoding="utf-8").strip()
    # The create inside the try: a lost answer can mean a node exists, and the
    # delete goes by name.
    try:
        node_id = create_when_free(theta, name, public_key, clock, sleep)
        say("created %s on %s" % (name, MACHINE))
        ssh = wait_up(theta, node_id, make_ssh, clock, sleep)
        code, out, err = ssh("bash -s", stdin=script_bytes("node_bootstrap.sh"), timeout=BOOTSTRAP_LIMIT_S)
        if code != 0:
            raise ThetaError("node_bootstrap.sh exited %d: %s" % (code, (err or out)[-600:]))
        say(out.strip().splitlines()[-1] if out.strip() else "prepared")
        code, out, err = ssh("cat > /usr/local/sbin/node_runners.sh", stdin=script_bytes("node_runners.sh"), timeout=60)
        if code != 0:
            raise ThetaError("could not put node_runners.sh on the node: %s" % err[-300:])
        start_runner(github, ssh, run, attempt, 1)
        wait_online(github, "%s-1" % name, clock, sleep)
        return {"runner": prefix, "attempt": str(attempt), "node": node_id}
    except Exception:
        off_after_failure(theta, github, run, attempt, prefix, clock, sleep)
        raise


def remove_runners(github, prefix) -> list:
    """Housekeeping: GitHub removes a single-job runner by itself in the end. Its
    failure never makes a deleted node look like a failed delete."""
    try:
        return github.remove_runners(prefix)
    except (ThetaError, OSError, ValueError, KeyError) as exc:
        say("::warning::runners left registered: %s" % exc)
        return []


def off_after_failure(theta, github, run, attempt, prefix, clock, sleep):
    try:
        turn_off(theta, run, attempt, clock, sleep)
    except (ThetaError, OSError, ValueError, KeyError) as exc:
        say("::warning::the node could not be deleted, the sweeper will: %s" % exc)
    remove_runners(github, prefix)


def access_secret(api_key, run, attempt) -> str:
    """What encrypts a node's SSH key between two jobs: only a job with the API key,
    which could delete the node anyway, can read it."""
    return hmac.new(api_key.encode(), ("node-access:%s:%s" % (run, attempt)).encode(), hashlib.sha256).hexdigest()


def seal_access(path: Path, private_key: str, address, secret: str):
    """What the watch needs to start the next runner. It travels as an artifact, and
    anyone can read the artifacts of a public repository: never in clear."""
    text = json.dumps({"key": private_key, "address": list(address) if address else None})
    subprocess.run(["openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-salt", "-pass", "env:NODE_ACCESS", "-out", str(path)],
                   input=text.encode(), env=dict(os.environ, NODE_ACCESS=secret), check=True)


def open_access(path: Path, secret: str) -> dict:
    done = subprocess.run(["openssl", "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-pass", "env:NODE_ACCESS", "-in", str(path)],
                          capture_output=True, env=dict(os.environ, NODE_ACCESS=secret))
    try:
        # A wrong key passes openssl's padding check about once in 256 times.
        if done.returncode == 0:
            return json.loads(done.stdout)
    except ValueError:
        pass
    raise ThetaError("the node's access file cannot be read")


def ssh_answers(host, port, timeout=15) -> bool:
    """The machine's own word, with no key and no API in the way: sshd's banner."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            return sock.recv(4) == b"SSH-"
    except OSError:
        return False


def parse_time(text):
    """Seconds since the epoch of an API time. Theta's carries from 0 to 6 decimals."""
    m = re.match(r"^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(?:\.\d+)?Z$", text or "")
    return calendar.timegm(tuple(int(g) for g in m.groups()) + (0, 0, 0)) if m else None


def node_jobs(jobs) -> list:
    """The jobs of the legs that compile on the node, by the name release.yml gives them,
    in the order they run."""
    names = ["build-%s" % leg for leg in node_legs()]
    return sorted((j for j in jobs if j.get("name") in names), key=lambda j: names.index(j["name"]))


def died_silently(job) -> bool:
    """A leg that ended with nothing to show for it: not green, and no step failed. A
    compile error or a red test fails a step. What GitHub's job object looks like for a
    dead runner is not established, so this alone never means a lost machine: it is one
    half of the evidence, and decide() asks the machine for the other."""
    return job.get("status") == "completed" and job.get("conclusion") not in ("success", "skipped") \
        and not any(step.get("conclusion") == "failure" for step in job.get("steps") or [])


def stuck(jobs, now, limit=STUCK_S) -> bool:
    """A leg queued on one of our labels with nothing running there, for too long."""
    mine = [j for j in node_jobs(jobs) if any(str(l).startswith("theta-") for l in j.get("labels") or [])]
    queued = [j for j in mine if j.get("status") in WAITING]
    if not queued or any(j.get("status") == "in_progress" for j in mine):
        return False
    since = [parse_time(j.get("created_at")) for j in queued]
    since += [parse_time(j.get("completed_at")) for j in mine if j.get("status") == "completed"]
    since = [t for t in since if t is not None]
    return bool(since) and now - max(since) >= limit


class Machine:
    """What is known of the node between two polls. Down: sshd does not answer, or,
    with no address to ask, Theta does not list it as running. Theta's list alone never
    says down when sshd answers, and a leg that finishes another step says it is up."""

    def __init__(self, address=None):
        self.address = tuple(address) if address else None
        self.listed = None      # True, False, None when the API did not answer
        self.answers = None     # True, False, None with no address
        self.down_since = None
        self.steps_done = None  # none seen yet: the first look is not progress

    def see(self, now, listed, answers, steps_done):
        self.listed, self.answers = listed, answers
        advanced = self.steps_done is not None and steps_done > self.steps_done
        self.steps_done = max(steps_done, self.steps_done or 0)
        if advanced:
            self.down_since = None
        elif answers is True or (answers is None and listed is True):
            self.down_since = None
        elif (answers is False or (answers is None and listed is False)) and self.down_since is None:
            self.down_since = now

    def down_for(self, now) -> float:
        return now - self.down_since if self.down_since is not None else -1


def look_at_machine(theta, name, machine, jobs, probe, now):
    listed = None
    try:
        row = next((n for n in theta.nodes() if n.get("name") == name), None)
        listed = bool(row) and str(row.get("status", "")).lower() == RUNNING
        if row:
            address = theta.address(row["id"])
            machine.address = address or machine.address
            listed = listed and address is not None
    except (ThetaError, OSError, ValueError, KeyError) as exc:
        say("the Theta API did not answer, not read as a lost node: %s" % exc)
    answers = probe(*machine.address) if machine.address else None
    done = sum(1 for j in node_jobs(jobs) for step in j.get("steps") or [] if step.get("conclusion") == "success")
    machine.see(now, listed, answers, done)


def decide(jobs, machine, now) -> str:
    """wait | done | lost | failed, from the node's legs and the machine.

    Lost needs two witnesses, whichever spoke first: the machine silent for
    DOWN_LIMIT_S, and a leg's job that agrees. It agrees when GitHub ended it with no
    failed step, when a leg waits with none running, or when a leg still runs without
    finishing a step and Theta does not list the machine either. A leg running on a
    machine Theta does list is left to GitHub, up to the job's own limit."""
    legs_on_node = node_jobs(jobs)
    if len(legs_on_node) < len(node_legs()):
        return "wait"
    silent = any(died_silently(j) for j in legs_on_node)
    running = any(j.get("status") == "in_progress" for j in legs_on_node)
    waiting = any(j.get("status") in WAITING for j in legs_on_node)
    gone_long = machine.down_for(now) >= DOWN_LIMIT_S
    if not running and not waiting:
        if not silent or machine.down_since is None:
            return "done"
        return "lost" if gone_long else "wait"
    if gone_long and (silent or (waiting and not running) or (running and machine.listed is False)):
        return "lost"
    if machine.down_since is None and stuck(jobs, now):
        return "failed"
    return "wait"


def next_leg(jobs):
    """The number (from 1) of the leg whose runner is due: the first not finished, once
    a leg has finished before it and it is still waiting. The first leg's runner came
    with the node."""
    legs_on_node = node_jobs(jobs)
    if len(legs_on_node) < len(node_legs()):
        return None
    for n, job in enumerate(legs_on_node, 1):
        if job.get("status") != "completed":
            return n if n > 1 and job.get("status") in WAITING else None
    return None


def relaunch_inputs(run, kind, tag, compile_cache, core_ref, wrapper_ref, gtests="", gtest_filter="") -> dict:
    """The same run, once more: every input it ran with, by value, and its mark. Never
    a default left to chance: the refs are what its gate would have run against."""
    if not core_ref or not wrapper_ref:
        raise ThetaError("a re-launch must name core_ref and wrapper_ref (got %r and %r)" % (core_ref, wrapper_ref))
    inputs = {"kind": kind, "compile_cache": compile_cache, "core_ref": core_ref, "wrapper_ref": wrapper_ref,
              "relaunch_of": str(run)}
    for key, value in (("release_tag", tag), ("gtests", gtests), ("gtest_filter", gtest_filter)):
        if value:
            inputs[key] = value
    return inputs


def after_loss(theta, github, run, attempt, ref, inputs, relaunch_of, clock, sleep) -> str:
    """The node goes first, seen gone: a re-launch is refused while a live run's node
    exists, and has to find the machine free. This run's cancel comes last, so that a
    failure on the way leaves it to the next watch job to try again."""
    turn_off(theta, run, attempt, clock, sleep)
    if relaunch_of:
        say("::error::the machine was lost a second time (this run re-launched run %s): the run fails, no third try" % relaunch_of)
        github.cancel(run)
        return "failed"
    if github.relaunched(run):
        say("::warning::run %s has been re-launched already: not again" % run)
    else:
        say("::warning::the machine was lost: the whole run goes again once, on a new node of the same machine, with the cache already saved")
        github.dispatch_release(ref, inputs)
    github.cancel(run)
    return "lost"


def give_up(theta, github, run, attempt, why, clock, sleep) -> str:
    say("::error::%s: the run fails" % why)
    turn_off(theta, run, attempt, clock, sleep)
    github.cancel(run)
    return "failed"


def watch(theta, github, run, attempt, ref, inputs, node_ssh, relaunch_of="", probe=ssh_answers, address=None,
          clock=time.time, sleep=time.sleep, poll=60, limit=STAGE_LIMIT_S) -> str:
    """Until the legs on the node have finished, or this job's share of the time has.
    Returns done, lost (re-launched), failed, or continue for the next watch job."""
    name = node_name(run, attempt)
    machine = Machine(address)
    begun = clock()
    blind = no_legs = start_failures = 0
    started = set()
    while clock() - begun < limit:
        try:
            jobs = github.jobs(run, attempt)
            blind = 0
        except Exception as exc:  # a blip of any shape: a healthy build goes on without us
            blind += 1
            say("GitHub did not answer (%d in a row): %s" % (blind, exc))
            if blind >= BLIND_POLLS:
                raise WatchBlind("GitHub did not answer for %d polls" % blind) from None
            sleep(poll)
            continue
        look_at_machine(theta, name, machine, jobs, probe, clock())
        no_legs = 0 if node_jobs(jobs) else no_legs + 1
        if no_legs >= NO_LEGS_POLLS:
            return "done"
        verdict = decide(jobs, machine, clock())
        if verdict == "done":
            if any(died_silently(j) for j in node_jobs(jobs)):
                say("::warning::a leg ended with no failed step on a machine that still answers: "
                    "not a lost machine, so no re-launch")
            return "done"
        if verdict == "lost":
            return after_loss(theta, github, run, attempt, ref, inputs, relaunch_of, clock, sleep)
        if verdict == "failed":
            return give_up(theta, github, run, attempt,
                           "a leg waits for a runner that is not coming, on a machine that answers", clock, sleep)
        due = next_leg(jobs) if machine.down_since is None else None
        if due and due not in started:
            try:
                # An earlier watch job may have started it before it ran out of time.
                if github.runner_status("%s-%d" % (name, due)) is None:
                    start_runner(github, node_ssh(), run, attempt, due)
                started.add(due)
                start_failures = 0
            except Exception as exc:
                start_failures += 1
                say("the next leg's runner did not start (%d of %d): %s" % (start_failures, START_TRIES, exc))
                remove_runners(github, runner_prefix(run, attempt))
                if start_failures >= START_TRIES:
                    return give_up(theta, github, run, attempt, "the next leg's runner did not start: %s" % exc, clock, sleep)
        sleep(poll)
    return "continue"


def legs_finished(github, run, attempt) -> bool:
    jobs = node_jobs(github.jobs(run, attempt))
    return len(jobs) == len(node_legs()) and all(j.get("status") == "completed" for j in jobs)


def sweep(theta, github, now, max_age_hours=GUARD_HOURS, dry_run=False) -> list:
    """Delete the nodes we started that no run needs: our name, and older than the
    limit, or owned by a run that has finished, or whose legs have finished (what a
    watch that died leaves behind). A node with any other name was made by hand and is
    never touched. Without GitHub, the age alone."""
    deleted = []
    for node in theta.nodes():
        m = NAME.match(node.get("name") or "")
        if not m:
            continue
        created = parse_time(node.get("created_at"))
        old = created is not None and now - created >= max_age_hours * 3600
        why = "older than %d h" % max_age_hours if old else ""
        if github and not old:
            try:
                if github.run_finished(m.group(1)):
                    why = "its run has finished"
                elif legs_finished(github, m.group(1), m.group(2)):
                    why = "its legs have finished"
            except (ThetaError, OSError, ValueError, KeyError) as exc:
                say("run %s unknown for now, its node stays: %s" % (m.group(1), exc))
        if why:
            say("%s %s: %s" % ("would delete" if dry_run else "deleting", node["name"], why))
            if not dry_run:
                theta.delete(node["id"])
            deleted.append(node["name"])
    return deleted


def cancel_stuck(github, now, dry_run=False, limit=SWEEP_STUCK_S) -> list:
    """Runs whose leg waits on a label no runner will come for, long after their own
    watch would have seen it (a queued job otherwise holds a run for 24 hours)."""
    cancelled = []
    for run in github.active_runs():
        if stuck(github.jobs(run["id"], run.get("run_attempt", 1)), now, limit):
            say("%s run %s: a leg queued on the node's label with no runner" % ("would cancel" if dry_run else "cancelling", run["id"]))
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


def write_private(path: Path, text: str):
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)


def run_provision(a) -> int:
    work = Path(tempfile.mkdtemp(prefix="theta-node-"))
    try:
        theta, github = from_env(), github_from_env()
        if github is None:
            raise ThetaError("GITHUB_REPOSITORY is not set")
        outputs = provision(theta, github, a.run, a.attempt,
                            lambda host, port: Ssh(host, port, work / "node_key"), lambda: new_key(work))
        seal_access(a.access_out, (work / "node_key").read_text(encoding="utf-8"), theta.address(outputs["node"]),
                    access_secret(os.environ["THETA_API_KEY"], a.run, a.attempt))
    except (ThetaError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        # Without the node the x86_64 legs have nowhere to run: the run has failed.
        print("::error::no Theta node, the run fails: %s" % exc, file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for key, value in outputs.items():
        print("%s=%s" % (key, value))
    return 0


def run_watch(a) -> int:
    theta, github = from_env(), github_from_env()
    if github is None:
        raise ThetaError("GITHUB_REPOSITORY is not set")
    env = os.environ.get
    inputs = relaunch_inputs(a.run, env("KIND", ""), env("TAG", ""), env("COMPILE_CACHE", ""), env("CORE_REF", ""),
                             env("WRAPPER_REF", ""), env("GTESTS", ""), env("GTEST_FILTER_INPUT", ""))
    access = {}
    try:
        access = open_access(a.access, access_secret(os.environ["THETA_API_KEY"], a.run, a.attempt))
    except (ThetaError, OSError, ValueError) as exc:
        say("::warning::no access to the node, the next leg's runner cannot start: %s" % exc)
    work = Path(tempfile.mkdtemp(prefix="theta-node-"))

    def node_ssh():
        if not access:
            raise ThetaError("no access to the node")
        key = work / "node_key"
        if not key.exists():
            write_private(key, access["key"])
        node_id = next(n["id"] for n in theta.nodes() if n.get("name") == node_name(a.run, a.attempt))
        host, port = theta.address(node_id) or access["address"]
        return Ssh(host, port, key)

    try:
        verdict = watch(theta, github, a.run, a.attempt, a.ref, inputs, node_ssh, env("RELAUNCH_OF", ""),
                        address=access.get("address"))
    except WatchBlind as exc:
        # No verdict: the legs may be running, so nothing here deletes the node. The
        # next watch job takes over, and the sweeper is behind it.
        print("::error::%s" % exc, file=sys.stderr)
        return 3
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("verdict=%s" % verdict)
    return 1 if verdict == "failed" else 0


def run_off(a) -> int:
    theta, github = from_env(), github_from_env()
    if a.if_legs_done:
        # The watch has ended, in whatever way; only this says the legs have too. An
        # unanswered question is not a yes: the sweeper deletes the node when they have.
        try:
            finished = github is not None and legs_finished(github, a.run, a.attempt)
        except Exception as exc:
            print("::warning::GitHub did not say whether the legs have finished, the node stays for the sweeper: %s" % exc,
                  file=sys.stderr)
            return 0
        if not finished:
            print("::warning::the legs have not finished, the node stays: the sweeper deletes it when they have", file=sys.stderr)
            return 0
    print("deleted: %s" % (", ".join(turn_off(theta, a.run, a.attempt)) or "nothing was on"))
    if github and os.environ.get("RUNNER_ADMIN_TOKEN"):
        print("runners removed: %s" % (", ".join(remove_runners(github, runner_prefix(a.run, a.attempt))) or "none"))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the Theta node of a release run")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("provision", "off", "watch"):
        p = sub.add_parser(name)
        p.add_argument("--run", required=True)
        p.add_argument("--attempt", required=True)
        if name == "provision":
            p.add_argument("--access-out", type=Path, required=True)
        if name == "watch":
            p.add_argument("--ref", required=True)
            p.add_argument("--access", type=Path, required=True)
        if name == "off":
            p.add_argument("--if-legs-done", action="store_true")
    s = sub.add_parser("sweep")
    s.add_argument("--max-age-hours", type=float, default=GUARD_HOURS)
    s.add_argument("--dry-run", action="store_true")
    sub.add_parser("status")
    a = ap.parse_args(argv)

    if a.cmd == "provision":
        return run_provision(a)
    try:
        if a.cmd == "off":
            return run_off(a)
        if a.cmd == "watch":
            return run_watch(a)
        theta, github = from_env(), github_from_env()
        if a.cmd == "sweep":
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
