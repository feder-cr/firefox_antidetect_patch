#!/usr/bin/env python3
"""Turn the Theta nodes on and off for one run of release.yml.

The two x86_64 legs compile at the same time, each on a node of its own, on its own
fixed community machine (MACHINES, the only place that names them), and nowhere else:
no other machine, no swapping between the two, no other kind of runner. One node, one
runner, one job. Gates and tests stay on GitHub. A node is created for a leg and deleted
when that leg ends. Stop and Start were measured against this on 2026-11-03 and give
nothing more: Start makes a new container with an empty disk, on another IP and port,
and keeps only the SSH key given at creation, so Stop would need a long-lived key and
leave no name saying which run owns the machine.

    provision --run R --attempt A --leg L
                                    create this leg's node, prepare it, start its runner
                                    and see it online. Any failure: exit 1, this node off,
                                    and the run has failed; stop-node takes the other.
    watch --run R --attempt A --ref REF
                                    follow both legs, turn each node off when its leg ends;
                                    prints verdict= done | lost | failed | continue. A
                                    machine lost mid-build leaves the other leg to finish,
                                    then re-launches the whole run once, on new nodes of
                                    the same machines, unless someone cancelled it, the
                                    other leg ended red or the branch has moved.
    off --run R --attempt A [--if-legs-done]
                                    delete this run's nodes, each whatever the other does,
                                    confirm they are gone; with the option, only those
                                    whose leg GitHub says has finished
    sweep [--max-age-hours H]       protection against a forgotten node, see sweep()
    status                          balance and our nodes

Runs from any machine, standard library and ssh only. Environment: THETA_API_KEY,
THETA_PROJECT; for what talks to GitHub GITHUB_REPOSITORY, GH_TOKEN, and
RUNNER_ADMIN_TOKEN (the runners' registration). No secret reaches a node except its one
single-job runner configuration, and none reaches a log.

Its cases: scripts/ci/test_theta_node.py.
"""
from __future__ import annotations

import argparse
import calendar
import http.client
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import legs  # noqa: E402

# Each leg always on the same PC. Both are 48 threads and about 67 GB, so which goes where
# only has to be fixed.
MACHINES = {"linux-x86_64": "community_18a9f767a3e940349d2a634e173598f2",
            "win-x86_64": "community_921db380bf634e279a706d1d1311c129"}
API = "https://controller.thetaedgecloud.com"
GITHUB_API = "https://api.github.com"
TEMPLATE = "img_n67q5y3xu9pcyqqcajz5cd72263n"
# The only Ubuntu image the template accepts: the API answers 400 to any other
# ("container_image is not allowed by the GPU Node template"; ubuntu:24.04, measured).
IMAGE = "thetalabsorg/ubuntu-sshd:latest"
# The ceiling Theta enforces at creation; the machines were $0.156 and $0.182/hour when read.
MAX_PRICE_USD_HOUR = 0.30
# Both nodes at their age limit, GUARD_HOURS: 8 hours of $0.338 is $2.70.
MIN_BALANCE_USD = 3.0
# After our own Delete the machine answers 404 and stays out of gpu-resources for 2 to 4
# minutes (measured). One someone else holds does the same, and a missing template answers
# 404 too: the wait covers our own delete, and at its end gpu-resources says which it is.
FREE_LIMIT_S = 360
UP_LIMIT_S = 600
BOOTSTRAP_LIMIT_S = 1200
# How long a delete Theta accepted may stay in its list before we stop looking. It left
# both lists within 7 s, and sshd refused within 6 s (measured).
GONE_LIMIT_S = 180
# How long Theta may not answer while a node has to go off. A last call can start just
# before this and take 270 s (four tries of 60 s): release.yml's stop-node step leaves
# room for both (test_release_builder.py adds them up).
OFF_ANSWER_LIMIT_S = 420
# A runner that has not come online by then never will; a queued leg would wait for
# it up to 24 hours (GitHub's limit), holding the run.
RUNNER_ONLINE_LIMIT_S = 300
# A leg of 350 minutes (build's timeout-minutes), the 45 of the node job and the 20 its
# runner may keep it queued make 415: a healthy node is not older than this.
GUARD_HOURS = 8
# The machine does not answer for this long, and a leg's job agrees, before it counts
# as lost. GitHub failed a dead runner's job 9m27s after the kill, one sample: nothing
# below waits for that, and nothing below assumes it has not happened yet.
DOWN_LIMIT_S = 900
# A queued leg with nothing running on a machine that answers, this long after its
# runner is known to exist: that runner is not coming.
STUCK_S = 1200
# The sweeper's own limit for the same thing, for a run whose watch has gone: well
# after the watch's, which is the one that can still re-launch.
SWEEP_STUCK_S = 3600
# Nothing usable for this many polls in a row, for any reason: the watch stops, and
# leaves the node on.
BLIND_POLLS = 30
# Polls with no leg listed at all (a run cancelled before its matrix): nothing to wait for.
NO_LEGS_POLLS = 15
# A re-launch asked for again only if it is not listed after this long.
DISPATCH_TRIES = 3
DISPATCH_SEEN_S = 120
# One watch job. GitHub stops a job on its runners at 360 minutes, and a leg has 350 from
# when its runner takes it, up to 20 later: the watch is two jobs in a row, which also
# leaves one to go on if the other dies (test_release_builder.py adds them up).
STAGE_LIMIT_S = 335 * 60
# What gpu-deployments says of a node that runs (measured 2026-11-03; PodPhase in
# /deployments/list says "<pod>: Running"). No other string has been seen.
RUNNING = "running"
WAITING = ("queued", "waiting", "pending", "requested")
NAME = re.compile(r"^ci(\d+)a(\d+)([a-z])$")
# A leg in the queue cannot be failed from outside, so a run we end, ends as cancelled.
NOT_A_CANCEL = ("This is a failure and not a cancel by a person: the run shows as cancelled only because "
                "a queued leg can end no other way.")
HOST_KEY_LINE = re.compile(r"^node_bootstrap: host key: (ssh-ed25519 [A-Za-z0-9+/=]+)$", re.M)
# The name a node's host key is pinned under: Theta may move its address.
HOST_ALIAS = "theta-node"


class ThetaError(Exception):
    pass


class NoAnswer(ThetaError):
    """A service or the machine gave no usable answer, whatever its shape. It says nothing
    about what they would have answered, so it is never read as a refusal."""


class WatchBlind(Exception):
    """Nothing usable for BLIND_POLLS polls: nothing is known of the legs."""


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
            except (OSError, http.client.HTTPException) as exc:
                problem = "%s %s: %s %s" % (method, path, type(exc).__name__, exc)
                continue
            if status in ok:
                try:
                    answer = json.loads(raw) if raw.strip() else None
                except ValueError:
                    problem = "%s %s: HTTP %d, and an answer that is not JSON" % (method, path, status)
                    continue
                if method == "GET" and status == 200 and not isinstance(answer, dict):
                    problem = "%s %s: HTTP %d, and an answer with nothing in it" % (method, path, status)
                    continue
                return answer, status
            problem = "%s %s: HTTP %d %s" % (method, path, status, raw[:300].decode("utf-8", "replace"))
            if status < 500 and status != 429 and not (status == 403 and b"rate limit" in raw.lower()):
                raise ThetaError(self.scrub(problem))
        raise NoAnswer(self.scrub(problem))


class Theta:
    def __init__(self, project, key, transport=urllib_transport, sleep=time.sleep):
        self.project = project
        self.http = Client(API, {"x-api-key": key, "Accept": "application/json",
                                 "Content-Type": "application/json", "User-Agent": "firefox-ci"},
                           [project, key], transport, sleep)
        self.deployments = "/v1/projects/%s/gpu-deployments" % project

    def keep_quiet_about(self, node_id):
        """A deployment id reaches no message: it is in the URL of a failed delete, and an
        answer can repeat it."""
        if node_id and str(node_id) not in self.http.secrets:
            self.http.secrets.append(str(node_id))

    def balance_usd(self):
        body, _ = self.http.call("GET", "/v1/projects/%s/billing/balance" % self.project)
        return float(body["body"]["balance_usd"])

    def nodes(self):
        body, _ = self.http.call("GET", self.deployments)
        inner = body.get("body")
        # An empty list may come as null. An answer with no list at all is not "nothing on".
        if not isinstance(inner, dict) or "deployments" not in inner:
            raise NoAnswer("the list of nodes came without its list")
        listed = inner["deployments"] or []
        for node in listed:
            self.keep_quiet_about(node.get("id"))
        return listed

    def create(self, name, leg, public_key):
        body, _ = self.http.call("POST", self.deployments, {
            "name": name, "deployment_template_id": TEMPLATE, "resource_id": MACHINES[leg],
            "container_image": IMAGE, "ssh_public_key": public_key, "ssh_port": 22,
            "max_price_per_hour_usd": MAX_PRICE_USD_HOUR})
        self.keep_quiet_about(body["body"]["deployment"]["id"])
        return body["body"]["deployment"]["id"]

    def machine_listed(self, leg) -> bool:
        """Whether this leg's machine can be had now, asked without creating anything."""
        body, _ = self.http.call("GET", "/v1/projects/%s/gpu-resources" % self.project)
        inner = body.get("body")
        if not isinstance(inner, dict):
            raise NoAnswer("the list of machines came without its list")
        return any(r.get("resource_id") == MACHINES[leg] for r in inner.get("resources") or [])

    def delete(self, node_id):
        self.keep_quiet_about(node_id)
        self.http.call("DELETE", "%s/%s" % (self.deployments, node_id), ok=(200, 202, 204, 404), tries=4)

    def address(self, node_id):
        """(host, ssh port) once the container runs, else None. The only endpoint
        with both; it also carries passwords, which stop here."""
        self.keep_quiet_about(node_id)
        body, _ = self.http.call("GET", "/deployments/list?project_id=%s" % self.project)
        for row in body.get("body") or []:
            if row.get("BaseID") == node_id:
                port = (row.get("PortMappings") or {}).get("22")
                host = row.get("NodePublicIP")
                if host and port and str(row.get("PodPhase", "")).endswith(": Running"):
                    if not str(port).isdigit():
                        raise NoAnswer("the node's port cannot be read")
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
        if not isinstance(body, dict) or not body.get("encoded_jit_config"):
            raise NoAnswer("the runner's registration came without its configuration")
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

    def remove_runners(self, wanted):
        """The idle runners that carry one of these labels."""
        removed = []
        for runner in self.runners():
            labels = [l.get("name") or "" for l in runner.get("labels", [])]
            if set(labels) & set(wanted) and not runner.get("busy"):
                # 422: it has a job after all, and a dead runner keeps it until GitHub ends it.
                _, status = self.admin.call("DELETE", "/repos/%s/actions/runners/%d" % (self.repo, runner["id"]),
                                            ok=(200, 204, 404, 422), tries=3)
                if status != 422:
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
                       {"ref": ref, "inputs": inputs})

    def relaunched(self, run):
        """The re-launch of this run if one exists (its row): release.yml's run-name says so."""
        body, _ = self.http.call("GET", "/repos/%s/actions/workflows/release.yml/runs?event=workflow_dispatch&per_page=100" % self.repo)
        return next((r for r in body.get("workflow_runs", []) if r.get("display_title") == relaunch_title(run)), None)

    def cancelled(self, run, attempt) -> bool:
        """Whether someone cancelled this run, from its jobs: the run's own status says
        nothing while one of them goes (it read `queued` with a job running, measured)."""
        return cancel_marks(self.jobs(run, attempt))

    def branch_head(self, ref):
        """The commit a dispatch on this branch would build, None if there is no such branch."""
        body, status = self.http.call("GET", "/repos/%s/git/ref/heads/%s" % (self.repo, urllib.parse.quote(ref, safe="/")), ok=(200, 404))
        return (body.get("object") or {}).get("sha") if status == 200 else None


class Ssh:
    """The node's root shell. The image's host key is the same on every node (measured),
    so the connection that runs node_bootstrap.sh, which carries no secret, takes what it
    finds; pin() holds every later one to the key that script made. What comes back ends
    up in public logs, so the address stops here."""

    def __init__(self, host, port, key, known_hosts=None):
        self.host, self.port, self.key = str(host), str(port), key
        verify = (["-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=%s" % known_hosts,
                   "-o", "GlobalKnownHostsFile=%s" % os.devnull, "-o", "HostKeyAlias=%s" % HOST_ALIAS,
                   "-o", "HostKeyAlgorithms=ssh-ed25519"] if known_hosts else
                  ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=%s" % os.devnull])
        self.argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=15", *verify,
                     "-o", "LogLevel=ERROR", "-o", "IdentitiesOnly=yes", "-i", str(key), "-p", self.port,
                     "root@%s" % host]

    def pin(self, host_key):
        known_hosts = Path(str(self.key) + ".known_hosts")
        known_hosts.write_text("%s %s\n" % (HOST_ALIAS, host_key), encoding="utf-8")
        return Ssh(self.host, self.port, self.key, known_hosts)

    def said(self, raw):
        text = raw.decode("utf-8", "replace").replace(self.host, "<node>")
        return re.sub(r"(?<!\d)%s(?!\d)" % re.escape(self.port), "<port>", text)

    def __call__(self, command, stdin=None, timeout=60):
        try:
            done = subprocess.run(self.argv + [command], input=stdin, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "", "timed out after %d s" % timeout
        return done.returncode, self.said(done.stdout), self.said(done.stderr)


def new_key(directory: Path) -> Path:
    key = directory / "node_key"
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "ci-node", "-f", str(key)],
                   check=True, stdin=subprocess.DEVNULL)
    return key


def node_name(run, attempt, leg) -> str:
    """Our mark on a node: the run that owns it, and the leg, which says its machine. Theta
    allows 1-20 alphanumerics."""
    name = "ci%sa%s%s" % (run, attempt, leg[0])
    if not NAME.match(name) or len(name) > 20:
        raise ValueError("not a node name: %r" % name)
    return name


def node_names(run, attempt) -> list:
    return [node_name(run, attempt, leg) for leg in node_legs()]


def leg_of(mark) -> str:
    return next(leg for leg in node_legs() if leg[0] == mark)


def leg_label(run, attempt, leg) -> str:
    """One label, one leg, one single-job runner. release.yml's plan and runs-on say the
    same (test_release_builder.py holds them together)."""
    return "theta-%s-%s-%s" % (run, attempt, leg)


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


def why_404(theta, leg, limit, exc) -> str:
    """A create that has answered 404 for longer than our own delete can explain."""
    try:
        listed = theta.machine_listed(leg)
    except ThetaError as problem:
        return "%s: the create still answers 404 after %d s, and Theta's list of machines does not say why (%s; %s)" % (leg, limit, exc, problem)
    if listed:
        return ("%s: the machine %s is free, and the create still answers 404 after %d s: something else is missing, the "
                "template %s or the project (%s)" % (leg, MACHINES[leg], limit, TEMPLATE, exc))
    return ("%s: the machine %s is not available, taken by someone else or its PC is off: Theta does not list it, %d s on, "
            "longer than our own recent delete can explain, and there is no other machine for this leg (%s)" % (leg, MACHINES[leg], limit, exc))


def create_when_free(theta, name, leg, public_key, clock=time.monotonic, sleep=time.sleep, limit=FREE_LIMIT_S):
    start = clock()
    while True:
        try:
            return theta.create(name, leg, public_key)
        except ThetaError as exc:
            if "HTTP 404" not in str(exc):
                raise
            if clock() - start >= limit:
                raise ThetaError(why_404(theta, leg, limit, exc)) from None
            say("%s: its machine is not free yet, asking again: %s" % (leg, exc))
        sleep(30)


def wait_up(theta, node_id, make_ssh, clock=time.monotonic, sleep=time.sleep, limit=UP_LIMIT_S):
    start = clock()
    last = "Theta gives no address yet"
    said = None
    while clock() - start < limit:
        address = theta.address(node_id)
        if address:
            ssh = make_ssh(*address)
            code, _, err = ssh("true", timeout=30)
            if code == 0:
                say("node up after %.0f s" % (clock() - start))
                return ssh
            last = "ssh exited %d: %s" % (code, err.strip()[-300:])
        else:
            last = "Theta gives no address yet"
        # Each change as it happens: a node that never comes up has to say why.
        if last != said:
            say("%.0f s: %s" % (clock() - start, last))
            said = last
        sleep(5)
    raise ThetaError("the node did not answer on SSH within %d s; last: %s" % (limit, last))


def turn_off(theta, names, clock=time.monotonic, sleep=time.sleep, limit=GONE_LIMIT_S,
             answer_limit=OFF_ANSWER_LIMIT_S) -> list:
    """Delete the nodes of these names and see them gone, each whatever happens to the
    other. By name, so a node whose id nobody kept (a cancel during the create) goes too.
    Theta's failures are asked again up to answer_limit, then ThetaError naming what may
    still be on. A delete it accepted and a node it goes on listing is a warning, never
    seen: what comes next waits by itself."""
    deleted, accepted, left = [], set(), set(names)
    start = clock()
    settled_at = None
    while True:
        problem = None
        try:
            mine = [n for n in theta.nodes() if n.get("name") in names]
            left = {n.get("name") for n in mine}
            if not mine:
                return deleted
        except ThetaError as exc:
            problem, mine = exc, []
        for node in mine:
            try:
                theta.delete(node["id"])
                accepted.add(node.get("name"))
                if node["id"] not in deleted:
                    deleted.append(node["id"])
            except ThetaError as exc:
                problem = exc
        if problem:
            say("Theta did not do it, asking again: %s" % problem)
            if clock() - start >= answer_limit:
                refused = sorted(left - accepted)
                if refused:
                    raise ThetaError("%s could not be deleted, Theta failed for %d s: %s"
                                     % (", ".join(refused), answer_limit, problem)) from None
                say("::warning::%s: delete accepted, and Theta does not answer to say it is gone: %s" % (", ".join(sorted(accepted)), problem))
                return deleted
        else:
            settled_at = clock() if settled_at is None else settled_at
            if clock() - settled_at >= limit:
                say("::warning::%s: delete accepted, still listed %d s later" % (", ".join(sorted(left)), limit))
                return deleted
        sleep(10)


def off_now(theta, name) -> bool:
    """One pass and no waiting, for a leg that has ended, while the other is being watched:
    what confirms it is the turn_off at the end."""
    try:
        for node in theta.nodes():
            if node.get("name") == name:
                theta.delete(node["id"])
        return True
    except ThetaError as exc:
        say("%s: Theta did not delete it, asking again at the next poll: %s" % (name, exc))
        return False


def refuse_if_taken(theta, github, run, attempt, leg):
    """One machine, one run. A live run's node on this leg's machine, or any node of this
    project on it, refuses this leg at once. What no run needs any more goes, and
    create_when_free waits out its 404: that covers a run already re-launched, whose
    watch has deleted the node, whatever the list still says, and which may go on for a
    while with its always() jobs. This run's other leg, and any node on the other machine,
    are not this leg's to touch: the other machine has its own provision. An earlier
    attempt of this run has no use for either machine."""
    for node in theta.nodes():
        name = node.get("name") or ""
        ours = NAME.match(name)
        if ours and ours.group(1) == str(run):
            if ours.group(2) != str(attempt) or ours.group(3) == leg[0]:
                say("an earlier attempt of this run left %s: deleting it" % name)
                theta.delete(node["id"])
        elif ours and ours.group(3) != leg[0]:
            continue
        elif ours and (github.run_finished(ours.group(1)) or github.relaunched(ours.group(1))):
            say("run %s needs nothing any more and left %s: deleting it" % (ours.group(1), name))
            theta.delete(node["id"])
        elif ours:
            raise ThetaError("run %s holds the machine %s with %s, and two runs cannot share it: "
                             "wait for it or cancel it, then run this again" % (ours.group(1), MACHINES[leg], name))
        elif node.get("machine_type") == MACHINES[leg]:
            raise ThetaError("the machine %s of %s is taken by %s, a node of this project that CI did not start: "
                             "there is no other machine for this leg" % (MACHINES[leg], leg, name))


def start_runner(github, ssh, run, attempt, leg):
    config = github.jit_config(node_name(run, attempt, leg), leg_label(run, attempt, leg))
    code, out, err = ssh("bash /usr/local/sbin/node_runner.sh %s" % run, stdin=(config + "\n").encode(), timeout=60)
    if code != 0:
        raise ThetaError("node_runner.sh exited %d: %s" % (code, (err or out)[-300:]))
    say("runner started, for %s" % leg)


def wait_online(github, name, clock=time.monotonic, sleep=time.sleep, limit=RUNNER_ONLINE_LIMIT_S):
    start = clock()
    while True:
        if github.runner_status(name) == "online":
            return
        if clock() - start >= limit:
            raise ThetaError("the runner %s did not come online within %d s: the leg would wait for it up to GitHub's "
                             "24-hour limit" % (name, limit))
        sleep(10)


def provision(theta, github, run, attempt, leg, make_ssh, make_key, clock=time.monotonic, sleep=time.sleep):
    """This leg's node ready with its runner online, or ThetaError with this node off. The
    other leg's is the other provision's, and stop-node's if that one is gone."""
    refuse_if_taken(theta, github, run, attempt, leg)
    balance = theta.balance_usd()
    if balance < MIN_BALANCE_USD:
        raise ThetaError("the Theta balance is $%.2f, below $%.2f" % (balance, MIN_BALANCE_USD))
    name = node_name(run, attempt, leg)
    key = make_key()
    public_key = Path(str(key) + ".pub").read_text(encoding="utf-8").strip()
    # The create inside the try: a lost answer can mean a node exists, and the
    # delete goes by name.
    try:
        node_id = create_when_free(theta, name, leg, public_key, clock, sleep)
        say("created %s on %s" % (name, MACHINES[leg]))
        ssh = wait_up(theta, node_id, make_ssh, clock, sleep)
        code, out, err = ssh("bash -s", stdin=script_bytes("node_bootstrap.sh"), timeout=BOOTSTRAP_LIMIT_S)
        if code != 0:
            raise ThetaError("node_bootstrap.sh exited %d: %s" % (code, (err or out)[-600:]))
        say(out.strip().splitlines()[-1] if out.strip() else "prepared")
        host_key = HOST_KEY_LINE.search(out)
        if not host_key:
            raise ThetaError("node_bootstrap.sh named no host key: the runner's configuration would go to an unverified host")
        ssh = ssh.pin(host_key.group(1))
        code, out, err = ssh("cat > /usr/local/sbin/node_runner.sh", stdin=script_bytes("node_runner.sh"), timeout=60)
        if code != 0:
            raise ThetaError("could not put node_runner.sh on the node: %s" % err[-300:])
        start_runner(github, ssh, run, attempt, leg)
        wait_online(github, name, clock, sleep)
    except Exception:
        off_after_failure(theta, github, run, attempt, leg, clock, sleep)
        raise


def remove_runners(github, labels) -> list:
    """Housekeeping: GitHub removes a single-job runner by itself in the end. Its
    failure never makes a deleted node look like a failed delete."""
    try:
        return github.remove_runners(labels)
    except (ThetaError, OSError, ValueError, KeyError) as exc:
        say("::warning::runners left registered: %s" % exc)
        return []


def off_after_failure(theta, github, run, attempt, leg, clock, sleep):
    try:
        turn_off(theta, [node_name(run, attempt, leg)], clock, sleep)
    except (ThetaError, OSError, ValueError, KeyError) as exc:
        say("::warning::the node could not be deleted, stop-node and the sweeper will: %s" % exc)
    remove_runners(github, [leg_label(run, attempt, leg)])


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


def jobs_by_leg(jobs) -> dict:
    """The jobs of the legs that compile on a node, by the name release.yml gives them."""
    names = {"build-%s" % leg: leg for leg in node_legs()}
    return {names[j["name"]]: j for j in jobs if j.get("name") in names}


def died_silently(job, down_since=None) -> bool:
    """A leg that ended as a dead runner's does (measured): `failure`, its step still
    in_progress and the rest pending, none concluded. `cancelled` is not that: someone
    cancelled. A compile error or a red test fails a step while the machine answers, and
    the runner concludes the steps after it, so a failed step counts as a dead runner's
    only with steps left unconcluded, or when it ended after the machine went silent. This
    alone never means a lost machine: it is one half of the evidence, and leg_state() asks
    the machine for the other."""
    if job.get("status") != "completed" or job.get("conclusion") in ("success", "skipped", "cancelled"):
        return False
    steps = job.get("steps") or []
    failed = [step for step in steps if step.get("conclusion") == "failure"]
    if not failed or any(step.get("conclusion") is None for step in steps):
        return True
    ended = parse_time(failed[-1].get("completed_at")) or parse_time(job.get("completed_at"))
    return down_since is not None and ended is not None and ended >= down_since


def cancel_marks(jobs) -> bool:
    """Whether a job ended as cancelled, which means someone cancelled the run: a dead
    runner's job ends as failure, a queued leg goes cancelled at once, a leg on a dead
    runner at the cancel + 5:00 (measured). A watch can reach its own limit: that says nothing."""
    return any(j.get("conclusion") == "cancelled" and not (j.get("name") or "").startswith("watch-") for j in jobs)


def stuck(jobs, now, limit=STUCK_S) -> bool:
    """A leg queued on one of our labels for too long, and no leg running that a cancel
    would cut. From GitHub's times alone, for the sweeper, which has nothing else."""
    mine = [j for j in jobs_by_leg(jobs).values() if any(str(l).startswith("theta-") for l in j.get("labels") or [])]
    queued = [j for j in mine if j.get("status") in WAITING]
    if not queued or any(j.get("status") == "in_progress" for j in mine):
        return False
    since = [parse_time(j.get("created_at")) for j in queued]
    since = [t for t in since if t is not None]
    return bool(since) and now - max(since) >= limit


class Machine:
    """What is known of a leg's node between two polls. Down: sshd does not answer, or,
    with no address to ask, Theta does not list it as running. Theta's list alone never
    says down when sshd answers, and a leg that finishes another step says it is up."""

    def __init__(self, address=None):
        self.address = tuple(address) if address else None
        self.listed = None      # True, False, None when the API did not answer
        self.answers = None     # True, False, None with no address
        self.down_since = None
        self.steps_done = None  # none seen yet: the first look is not progress
        self.runner_since = None  # since when a leg waits for a runner known to exist

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

    def expect_runner(self, expected, now):
        """The clock of a runner that is not coming runs while its leg waits, on a machine
        that answers. The runner exists from before the leg can be queued: provision saw it
        online. So each watch job starts this clock at its first look, never before that."""
        if expected and self.down_since is None:
            self.runner_since = now if self.runner_since is None else self.runner_since
        else:
            self.runner_since = None

    def runner_waited(self, now) -> float:
        return now - self.runner_since if self.runner_since is not None else -1


def look_at_machines(theta, run, attempt, machines, by_leg, probe, now):
    rows = None
    try:
        rows = {n.get("name"): n for n in theta.nodes()}
    except ThetaError as exc:
        say("the Theta API did not answer, not read as a lost node: %s" % exc)
    for leg, machine in machines.items():
        listed = None
        if rows is not None:
            try:
                row = rows.get(node_name(run, attempt, leg))
                listed = bool(row) and str(row.get("status", "")).lower() == RUNNING
                if row:
                    address = theta.address(row["id"])
                    machine.address = address or machine.address
                    listed = listed and address is not None
            except ThetaError as exc:
                listed = None
                say("the Theta API did not answer, not read as a lost node: %s" % exc)
        answers = probe(*machine.address) if machine.address else None
        done = sum(1 for step in (by_leg.get(leg) or {}).get("steps") or [] if step.get("conclusion") == "success")
        machine.see(now, listed, answers, done)


def leg_state(job, machine, now, cancelled=False):
    """ended | lost | stuck, or None while nothing is decided, from a leg's job and its machine.

    Lost needs two witnesses, whichever spoke first: the machine silent for
    DOWN_LIMIT_S, and the leg's job that agrees. It agrees when GitHub ended it with no
    failed step of a live runner, when it waits in the queue, or when it still runs without
    finishing a step: GitHub failed a dead runner's job 9m27s after the kill, one sample,
    and nothing waits up to the job's limit for it. Only with Theta's API silent as well is
    a running leg left alone: that may be Theta and not the machine. After a cancel nothing
    waits for the limit: once the leg has ended, its node can go."""
    if job is None:
        return None
    status = job.get("status")
    gone_long = machine.down_for(now) >= DOWN_LIMIT_S
    if status == "completed":
        if cancelled or machine.down_since is None or not died_silently(job, machine.down_since):
            return "ended"
        return "lost" if gone_long else None
    if gone_long and (status in WAITING or machine.listed is not None):
        return "lost"
    if status in WAITING and machine.down_since is None and machine.runner_waited(now) >= STUCK_S:
        return "stuck"
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


def seen_relaunch(github, run, clock, sleep, limit=DISPATCH_SEEN_S):
    """The re-launch's row once GitHub lists it, None if not by the limit. A list that
    does not answer is raised: not known is not "not there"."""
    start = clock()
    while True:
        problem = None
        try:
            row = github.relaunched(run)
            if row:
                return row
        except NoAnswer as exc:
            problem = exc
        if clock() - start >= limit:
            if problem:
                raise problem
            return None
        sleep(15)


def dispatch_once(github, run, ref, inputs, clock, sleep):
    """The re-launch, and its row if GitHub lists it by then. A dispatch whose answer was
    lost may have happened, so it is asked again only once the list says it did not."""
    lost = None
    for _ in range(DISPATCH_TRIES):
        lost = None
        try:
            github.dispatch_release(ref, inputs)
        except NoAnswer as exc:
            lost = exc
            say("the answer to the re-launch was lost, looking for it before asking again: %s" % exc)
        try:
            row = seen_relaunch(github, run, clock, sleep)
        except NoAnswer:
            if lost:
                raise
            return None
        if row or not lost:
            return row
    raise lost


def named(row) -> str:
    return "run %s (%s)" % (row.get("id"), row.get("html_url"))


def after_loss(theta, github, run, attempt, lost, ref, sha, inputs, relaunch_of, clock, sleep) -> str:
    """Both legs have ended or are lost. The nodes go first, seen gone: a re-launch has to
    find both machines free. This run's cancel comes last. A failure on the way comes back
    at the next poll, then at the next watch job: each step here looks at what the last one
    did. A cancel by a person is looked for after the nodes have gone and again right
    before the dispatch, and a re-launch happens only if GitHub says there was none."""
    turn_off(theta, node_names(run, attempt), clock, sleep)
    which = "the machine of %s" % " and ".join(lost)
    cancelled = "%s is silent and someone cancelled this run: the nodes are off, and no re-launch" % which
    if github.cancelled(run, attempt):
        say("::notice::%s" % cancelled)
        return "done"
    if relaunch_of:
        say("::error::%s was lost, a second loss (this run re-launched run %s), no third try. %s" % (which, relaunch_of, NOT_A_CANCEL))
        github.cancel(run)
        return "failed"
    earlier = github.relaunched(run)
    if earlier:
        say("::warning::%s was lost, and this run has been re-launched already as %s: not again" % (which, named(earlier)))
    else:
        # A dispatch names a branch, not a commit: it would build whatever is there now.
        head = github.branch_head(ref)
        if not sha or head != sha:
            say("::error::%s was lost, and no re-launch: %s is at %s and this run built %s, so it would not be the "
                "same code. Start it by hand if that is what you want. %s" % (which, ref, head or "(no such branch)", sha or "(unknown)", NOT_A_CANCEL))
            github.cancel(run)
            return "failed"
        if github.cancelled(run, attempt):
            say("::notice::%s" % cancelled)
            return "done"
        say("%s was lost: the whole run goes again once, on new nodes of the same machines, with the cache already saved" % which)
        try:
            row = dispatch_once(github, run, ref, inputs, clock, sleep)
        except NoAnswer:
            raise
        except ThetaError as exc:
            say("::error::%s was lost, and GitHub refuses the re-launch: %s. %s" % (which, exc, NOT_A_CANCEL))
            github.cancel(run)
            return "failed"
        say("::warning::%s was lost: this run is replaced by %s, which goes again once, on new nodes of the same "
            "machines, with the cache already saved" % (which, named(row) if row else 'the run named "%s", not listed yet' % relaunch_title(run)))
    github.cancel(run)
    return "lost"


def give_up(theta, github, run, attempt, why, clock, sleep) -> str:
    say("::error::%s: the run fails. %s" % (why, NOT_A_CANCEL))
    try:
        turn_off(theta, node_names(run, attempt), clock, sleep)
    except ThetaError as exc:
        say("::warning::the nodes could not be deleted, the sweeper will: %s" % exc)
    github.cancel(run)
    return "failed"


def why_not_coming(github, run, attempt, stuck_legs) -> str:
    why = "%s waits for a runner that is not coming, on a machine that answered" % " and ".join(stuck_legs)
    try:
        if any(github.runner_status(node_name(run, attempt, leg)) is None for leg in stuck_legs):
            why += ", and the runner has gone without taking it, as one does when the node refuses another run's job: that uses it up"
    except ThetaError as exc:
        say("the list of runners did not answer: %s" % exc)
    return why


def conclude(theta, github, run, attempt, settled, by_leg, cancelled, ref, sha, inputs, relaunch_of, clock, sleep) -> str:
    """Every leg has ended or is lost or stuck: what becomes of the run. A healthy leg has
    had its time, and its cache: nothing here runs before it has ended."""
    lost = [leg for leg in node_legs() if settled[leg] == "lost"]
    stuck_legs = [leg for leg in node_legs() if settled[leg] == "stuck"]
    ended = [leg for leg in node_legs() if settled[leg] == "ended"]
    for leg in ended:
        if died_silently(by_leg[leg]):
            say("::warning::%s ended with no failed step on a machine that still answers: not a lost machine, so no re-launch" % leg)
    if not lost and not stuck_legs:
        return "done"
    if cancelled:
        say("::notice::someone cancelled this run: no re-launch")
        return "done"
    if stuck_legs:
        return give_up(theta, github, run, attempt, why_not_coming(github, run, attempt, stuck_legs), clock, sleep)
    red = [leg for leg in ended if by_leg[leg].get("conclusion") not in ("success", "skipped")]
    if red:
        return give_up(theta, github, run, attempt, "the machine of %s was lost, and %s ended red, which a new run would not "
                       "change: no re-launch" % (" and ".join(lost), " and ".join(red)), clock, sleep)
    return after_loss(theta, github, run, attempt, lost, ref, sha, inputs, relaunch_of, clock, sleep)


def watch(theta, github, run, attempt, ref, inputs, relaunch_of="", probe=ssh_answers,
          clock=time.time, sleep=time.sleep, poll=60, limit=STAGE_LIMIT_S, sha="") -> str:
    """Until both legs are settled, or this job's share of the time has run out. Returns
    done, lost (re-launched), failed, or continue for the next watch job, which reads the
    same from GitHub and Theta and needs nothing from this one. A leg that settles has its
    node deleted at once, and the other goes on. A poll that fails, in whatever way, is
    only a poll: a healthy build goes on without us, and after BLIND_POLLS in a row the
    watch stops and says what the last one was."""
    machines = {leg: Machine() for leg in node_legs()}
    settled, to_turn_off = {}, set()
    begun = clock()
    blind = no_legs = 0

    def look():
        nonlocal no_legs
        jobs = github.jobs(run, attempt)
        by_leg = jobs_by_leg(jobs)
        no_legs = 0 if by_leg else no_legs + 1
        if no_legs >= NO_LEGS_POLLS:
            return "done"
        cancelled = cancel_marks(jobs)
        open_legs = {leg: machine for leg, machine in machines.items() if leg not in settled}
        look_at_machines(theta, run, attempt, open_legs, by_leg, probe, clock())
        for leg, machine in open_legs.items():
            job = by_leg.get(leg)
            state = leg_state(job, machine, clock(), cancelled)
            if state:
                settled[leg] = state
                # A dead runner's job on a machine that answers stays with its node: the next
                # watch job, finding it deleted, could only read that as a lost machine.
                if state != "ended" or cancelled or not died_silently(job):
                    to_turn_off.add(leg)
            else:
                machine.expect_runner(bool(job) and job.get("status") in WAITING, clock())
        for leg in sorted(to_turn_off):
            if off_now(theta, node_name(run, attempt, leg)):
                to_turn_off.discard(leg)
        if len(settled) < len(machines):
            return None
        return conclude(theta, github, run, attempt, settled, by_leg, cancelled, ref, sha, inputs, relaunch_of, clock, sleep)

    while clock() - begun < limit:
        try:
            verdict = look()
            blind = 0
            if verdict:
                return verdict
        except Exception as exc:  # a blip of any shape, and a bug would look the same from here
            blind += 1
            # A runner cannot take a job from a GitHub that does not answer: that time is not its fault.
            for machine in machines.values():
                machine.runner_since = None
            what = "%s: %s" % (type(exc).__name__, exc)
            say("this poll saw nothing (%d in a row): %s" % (blind, what))
            if not isinstance(exc, ThetaError):
                say("::warning::an error that is not a service's:\n%s" % traceback.format_exc())
            if blind >= BLIND_POLLS:
                raise WatchBlind("nothing usable for %d polls, the last: %s" % (blind, what)) from None
        sleep(poll)
    return "continue"


def finished_legs(github, run, attempt) -> list:
    by_leg = jobs_by_leg(github.jobs(run, attempt))
    return [leg for leg in node_legs() if leg in by_leg and by_leg[leg].get("status") == "completed"]


def sweep(theta, github, now, max_age_hours=GUARD_HOURS, dry_run=False) -> list:
    """Delete the nodes we started that no run needs: our name, and older than the
    limit, or owned by a run that has finished, or whose own leg has finished (what a
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
                elif leg_of(m.group(3)) in finished_legs(github, m.group(1), m.group(2)):
                    why = "its leg has finished"
            except (ThetaError, OSError, ValueError, KeyError) as exc:
                say("run %s unknown for now, its node stays: %s" % (m.group(1), exc))
        if why:
            say("%s %s: %s" % ("would delete" if dry_run else "deleting", node["name"], why))
            if not dry_run:
                theta.delete(node["id"])
            deleted.append(node["name"])
    return deleted


def watch_alive(jobs) -> bool:
    return any((j.get("name") or "").startswith("watch-") and j.get("status") != "completed" for j in jobs)


def cancel_stuck(github, now, dry_run=False, limit=SWEEP_STUCK_S) -> list:
    """Runs whose leg waits on a label no runner will come for, long after their own
    watch would have seen it (a queued job otherwise holds a run for 24 hours). A watch
    still there is left to speak."""
    cancelled = []
    for run in github.active_runs():
        jobs = github.jobs(run["id"], run.get("run_attempt", 1))
        if stuck(jobs, now, limit) and not watch_alive(jobs):
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


def run_provision(a) -> int:
    work = Path(tempfile.mkdtemp(prefix="theta-node-"))
    try:
        theta, github = from_env(), github_from_env()
        if github is None:
            raise ThetaError("GITHUB_REPOSITORY is not set")
        provision(theta, github, a.run, a.attempt, a.leg,
                  lambda host, port: Ssh(host, port, work / "node_key"), lambda: new_key(work))
    except (ThetaError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        # Without its node the leg has nowhere to run: the run has failed.
        print("::error::no Theta node for %s, the run fails: %s" % (a.leg, exc), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


def run_watch(a) -> int:
    theta, github = from_env(), github_from_env()
    if github is None:
        raise ThetaError("GITHUB_REPOSITORY is not set")
    env = os.environ.get
    inputs = relaunch_inputs(a.run, env("KIND", ""), env("TAG", ""), env("COMPILE_CACHE", ""), env("CORE_REF", ""),
                             env("WRAPPER_REF", ""), env("GTESTS", ""), env("GTEST_FILTER_INPUT", ""))
    try:
        verdict = watch(theta, github, a.run, a.attempt, a.ref, inputs, env("RELAUNCH_OF", ""), sha=env("GITHUB_SHA", ""))
    except WatchBlind as exc:
        # No verdict: the legs may be running, so nothing here deletes a node. The
        # next watch job takes over, and the sweeper is behind it.
        print("::error::%s" % exc, file=sys.stderr)
        return 3
    print("verdict=%s" % verdict)
    return 1 if verdict == "failed" else 0


def run_off(a) -> int:
    theta, github = from_env(), github_from_env()
    going = node_legs()
    if a.if_legs_done:
        # The watch has ended, in whatever way; only this says a leg has too. An
        # unanswered question is not a yes: the sweeper deletes a node when its leg has.
        try:
            going = finished_legs(github, a.run, a.attempt) if github is not None else []
        except Exception as exc:
            print("::warning::GitHub did not say whether the legs have finished, the nodes stay for the sweeper: %s" % exc,
                  file=sys.stderr)
            return 0
        staying = [leg for leg in node_legs() if leg not in going]
        if staying:
            print("::warning::%s has not finished, its node stays: the sweeper deletes it when it has" % " and ".join(staying), file=sys.stderr)
    code = 0
    names = [node_name(a.run, a.attempt, leg) for leg in going]
    try:
        print("deleted: %s" % (", ".join(names) if turn_off(theta, names) else "nothing was on"))
    except ThetaError as exc:
        # Red, because it may be billing and only a person reading this can know. Nothing
        # needs this job: the legs, the gate and the publish keep their own results.
        print("::error::a node may still be on, and billing; the sweeper tries again every 30 minutes. This job says "
              "nothing about the build, which has its own results: %s" % exc, file=sys.stderr)
        code = 1
    if github and os.environ.get("RUNNER_ADMIN_TOKEN"):
        print("runners removed: %s" % (", ".join(remove_runners(github, [leg_label(a.run, a.attempt, leg) for leg in going])) or "none"))
    return code


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the Theta nodes of a release run")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("provision", "off", "watch"):
        p = sub.add_parser(name)
        p.add_argument("--run", required=True)
        p.add_argument("--attempt", required=True)
        if name == "provision":
            p.add_argument("--leg", required=True, choices=node_legs())
        if name == "watch":
            p.add_argument("--ref", required=True)
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
