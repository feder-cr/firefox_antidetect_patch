#!/usr/bin/env python3
"""Turn the build nodes on and off for one run of release.yml.

The two x86_64 legs compile at the same time on ONE node: a Latitude.sh server of the plan
PLAN, in the cheapest location that has one when the node is created (Federico,
2026-11-04: always the cheapest), and nowhere else: no other plan, no other kind of
runner. One runner per leg, one job per runner, each leg on its own user, home, mounts
and cgroup (node_runner.sh). A leg's link spends most of its time on one core of 24, so
the other leg fills what it leaves, and the plan is billed by the started hour: one
machine for both is half the price of two (measured 2026-11-04: alone, each leg used
15-17% of its machine's core-minutes, at most 34.5 GB of its 96). Gates and tests stay on
GitHub. The node is created for the run and deleted when both legs have ended: a server
that is off is still billed, and nothing on its disk has to outlive the legs.

    provision --run R --attempt A
                                    create the run's node, prepare it, start one runner
                                    per leg and see them online. Any failure: exit 1, the
                                    node off, and the run has failed.
    watch --run R --attempt A --ref REF
                                    follow both legs, turn the node off when both have ended;
                                    prints verdict= done | lost | failed | continue. A
                                    machine lost mid-build leaves the other leg to finish,
                                    then re-launches the whole run once, on new
                                    nodes, unless someone cancelled it, the
                                    other leg ended red or the branch has moved.
    off --run R --attempt A [--if-legs-done]
                                    delete this run's node, confirm it is gone; with the
                                    option, only once GitHub says both legs have finished
    sweep [--max-age-hours H]       protection against a forgotten node, see sweep()
    status                          our nodes

Runs from any machine, standard library and ssh only. Environment: LATITUDE_API_KEY,
LATITUDE_PROJECT; for what talks to GitHub GITHUB_REPOSITORY, GH_TOKEN, and
RUNNER_ADMIN_TOKEN (the runners' registration). No secret reaches a node except its one
single-job runner configuration, and none reaches a log.

Its cases: scripts/ci/test_latitude_node.py.
"""
from __future__ import annotations

import argparse
import calendar
import http.client
import json
import os
import re
import shlex
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

# Measured 2026-11-04 with the same program on each machine, a compiler's kind of load, on
# one core: this plan's EPYC 4484PX (12 cores, 96 GB, 128 MB of L3, a whole machine) 9.2 s,
# a 14900KF 17.4 s, the Threadripper 2970WX it replaces 34.5 s. Most of a link is one core.
PLAN = "f4-metal-small"
# The newest Ubuntu LTS it offers that GitHub's runners also use.
OPERATING_SYSTEM = "ubuntu_24_04_x64_lts"
API = "https://api.latitude.sh"
GITHUB_API = "https://api.github.com"
# Its dearest location was $1.48 an hour when read, and the plan is billed by the started hour.
MAX_PRICE_USD_HOUR = 1.50
# What the API answers when a location no longer has what it listed a moment ago, or
# refuses that location: the next cheapest is asked. Anything else is final.
TRY_NEXT_SITE = ("HTTP 400", "HTTP 409", "HTTP 422")
# Measured: 21 s, from a pool of ready machines. A real install takes longer.
UP_LIMIT_S = 900
# Measured from a hosted runner to a node: 1440 and below carry everything. Kept lower, for paths not measured.
PATH_MTU = 1200
BOOTSTRAP_LIMIT_S = 1200
# ONE deadline for provision(), from the create to the last runner online: the node job has 45 minutes (release.yml) and GitHub kills a job past them
# without letting its script delete anything. wait_up (900), the bootstrap with its retries (900 + one attempt of up to 1200) and the runners (2 x 300) add
# up to more; each phase gets what is left of these 40 minutes and a phase with nothing left fails, so the node is deleted by this script and the
# job never reaches the kill.
PROVISION_DEADLINE_S = 2400
# When `off` is asked about a run whose node job did not succeed (a cancel or a kill, perhaps in the middle of the create), the first list of the API may be
# empty a few seconds before a server it was asked to make appears in it: look again once, this long after.
OFF_RECHECK_S = 45
# A bootstrap that fails for the network (Ubuntu's archive or GitHub answering 502, 503, 504, a name not resolving, a download that stops) is asked
# AGAIN on the same server, which is already paid for the hour: the other way is a new server for every run that is started again, and the
# runs of 2026-11-04 and 2026-11-08 paid seven hours for 502 and 504 answers that ended minutes later. No new attempt starts once this many seconds
# have gone since the first, and the node is deleted as before. A failure that is not the network (a missing package, a refused sha256 of our own
# archive, an HTTP 4xx such as a deleted asset) is final at once: node_bootstrap.sh has already asked three times, from two addresses, before it says
# an archive does not match its sha256, and a wrong pin would otherwise be asked again for fifteen minutes on a server paid by the hour.
BOOTSTRAP_RETRY_CAP_S = 900
BOOTSTRAP_RETRY_PAUSE_S = 60
TRANSIENT = re.compile(r"Failed to fetch|Temporary failure|Could not resolve|Connection (?:reset|timed out|refused)|Operation timed out|Timeout was reached|"
                       r"error: 5\d\d|returned error: 5\d\d|\b50[0-9] (?:Bad Gateway|Service Unavailable|Gateway Time-?out)|"
                       r"Bad Gateway|Service Unavailable|Gateway Time-?out|curl: \((?:6|7|28|35|52|56)\)|"
                       r"Network is unreachable|Undetermined Error", re.I)
# How long a delete the API accepted may stay in its list before we stop looking.
GONE_LIMIT_S = 180
# How long the API may not answer while a node has to go off. A last call can start just
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
# What the API says of a server that runs.
RUNNING = "on"
WAITING = ("queued", "waiting", "pending", "requested")
NAME = re.compile(r"^ci(\d+)a(\d+)([a-z])$")
# A leg in the queue cannot be failed from outside, so a run we end, ends as cancelled.
NOT_A_CANCEL = ("This is a failure and not a cancel by a person: the run shows as cancelled only because "
                "a queued leg can end no other way.")
HOST_KEY_LINE = re.compile(r"^node_bootstrap: host key: (ssh-ed25519 [A-Za-z0-9+/=]+)$", re.M)
# The name a node's host key is pinned under.
HOST_ALIAS = "build-node"
# The image's user: it has sudo, and root takes no SSH (measured 2026-11-04).
USER = "ubuntu"


class NodeError(Exception):
    pass


class NoAnswer(NodeError):
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
                raise NodeError(self.scrub(problem))
        raise NoAnswer(self.scrub(problem))


class Latitude:
    def __init__(self, project, key, transport=urllib_transport, sleep=time.sleep, exclude=()):
        self.project = project
        # Sites not to take for now, from the environment's LATITUDE_EXCLUDE_SITES: on
        # 04/11 nodes in Dallas fetched from GitHub at 1.5-2 MB/s and three candidates
        # died or crawled there, while San Jose, at the same price, ran at full speed.
        # A knob of the environment, so that it goes without a change of code.
        self.exclude = {s.strip().upper() for s in exclude if s.strip()}
        self.http = Client(API, {"Authorization": "Bearer %s" % key, "Accept": "application/json",
                                 "Content-Type": "application/json", "User-Agent": "firefox-ci"},
                           [project, key], transport, sleep)

    def keep_quiet_about(self, node_id):
        """A server's id reaches no message: it is in the URL of a failed delete, and an
        answer can repeat it."""
        if node_id and str(node_id) not in self.http.secrets:
            self.http.secrets.append(str(node_id))

    def nodes(self):
        body, _ = self.http.call("GET", "/servers?filter[project]=%s&page[size]=100" % self.project)
        # An answer with no list at all is not "nothing on".
        if not isinstance(body.get("data"), list):
            raise NoAnswer("the list of nodes came without its list")
        listed = []
        for server in body["data"]:
            attributes = server.get("attributes") or {}
            self.keep_quiet_about(server.get("id"))
            listed.append({"id": server.get("id"), "name": attributes.get("hostname"), "status": attributes.get("status"),
                           "created_at": attributes.get("created_at")})
        return listed

    def sites(self):
        """The locations that have the plan now, as (price, site), the cheapest first.
        Among equal prices the United States first: GitHub's cache is closer."""
        body, _ = self.http.call("GET", "/plans?filter[slug]=%s" % PLAN)
        found = []
        for plan in body.get("data") or []:
            for region in (plan.get("attributes") or {}).get("regions") or []:
                price = ((region.get("pricing") or {}).get("USD") or {}).get("hour")
                for site in (region.get("locations") or {}).get("in_stock") or []:
                    if price is not None and site.upper() not in self.exclude:
                        found.append((float(price), region.get("name") != "United States", site))
        return [(price, site) for price, _, site in sorted(found)]

    def create(self, name, public_key):
        """A server in the cheapest location that takes the order. A lost answer may mean
        one exists: it goes up, and the delete goes by name."""
        sites = self.sites()
        if not sites:
            raise NodeError("no location has the plan %s now" % PLAN)
        body, _ = self.http.call("POST", "/projects/%s/ssh_keys" % self.project, {
            "data": {"type": "ssh_keys", "attributes": {"name": name, "public_key": public_key}}})
        key_id = body["data"]["id"]
        refused = []
        for price, site in sites:
            if price > MAX_PRICE_USD_HOUR:
                refused.append("%s costs $%.2f an hour, above the ceiling of $%.2f" % (site, price, MAX_PRICE_USD_HOUR))
                break
            try:
                body, _ = self.http.call("POST", "/servers", {"data": {"type": "servers", "attributes": {
                    "project": self.project, "plan": PLAN, "site": site, "operating_system": OPERATING_SYSTEM,
                    "hostname": name, "ssh_keys": [key_id], "billing": "hourly"}}})
            except NoAnswer:
                raise
            except NodeError as exc:
                if not any(code in str(exc) for code in TRY_NEXT_SITE):
                    raise
                refused.append("%s: %s" % (site, exc))
                continue
            self.keep_quiet_about(body["data"]["id"])
            say("%s: at %s, $%.2f an hour" % (name, site, price))
            return body["data"]["id"]
        raise NodeError("no location took the order for %s: %s" % (PLAN, "; ".join(refused)))

    def delete(self, node_id):
        self.keep_quiet_about(node_id)
        self.http.call("DELETE", "/servers/%s" % node_id, ok=(200, 202, 204, 404), tries=4)

    def address(self, node_id):
        """(host, ssh port) once the server has an address, else None."""
        self.keep_quiet_about(node_id)
        body, status = self.http.call("GET", "/servers/%s" % node_id, ok=(200, 404))
        host = ((body.get("data") or {}).get("attributes") or {}).get("primary_ipv4") if status == 200 else None
        return (str(host), 22) if host else None

    def ssh_keys(self):
        body, _ = self.http.call("GET", "/projects/%s/ssh_keys" % self.project)
        return [(k.get("id"), (k.get("attributes") or {}).get("name")) for k in body.get("data") or []]

    def drop_keys(self, wanted) -> list:
        """The registered public keys of a node's name for which wanted(name) says yes. Only a
        new server reads them, so none has to stay. A key left behind costs nothing and
        opens nothing: its private half died with its job."""
        dropped = []
        for key_id, name in self.ssh_keys():
            if name and wanted(name):
                self.http.call("DELETE", "/projects/%s/ssh_keys/%s" % (self.project, key_id), ok=(200, 202, 204, 404), tries=3)
                dropped.append(name)
        return dropped


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
    """Root's shell on the node, through the image's own user and sudo. The connection that
    runs node_bootstrap.sh, which carries no secret, takes the host key it finds; pin() holds
    every later one to the key that script made. What comes back ends up in public logs, so
    the address stops here."""

    def __init__(self, host, port, key, known_hosts=None):
        self.host, self.port, self.key = str(host), str(port), key
        verify = (["-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=%s" % known_hosts,
                   "-o", "GlobalKnownHostsFile=%s" % os.devnull, "-o", "HostKeyAlias=%s" % HOST_ALIAS,
                   "-o", "HostKeyAlgorithms=ssh-ed25519"] if known_hosts else
                  ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=%s" % os.devnull])
        self.argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=15", *verify,
                     "-o", "LogLevel=ERROR", "-o", "IdentitiesOnly=yes", "-i", str(key), "-p", self.port,
                     "%s@%s" % (USER, host)]

    def pin(self, host_key):
        known_hosts = Path(str(self.key) + ".known_hosts")
        known_hosts.write_text("%s %s\n" % (HOST_ALIAS, host_key), encoding="utf-8")
        return Ssh(self.host, self.port, self.key, known_hosts)

    def said(self, raw):
        # The address, and nothing else: the port is 22, and "22" is in versions and in host keys.
        return raw.decode("utf-8", "replace").replace(self.host, "<node>")

    def __call__(self, command, stdin=None, timeout=60):
        try:
            done = subprocess.run(self.argv + ["sudo -n bash -c %s" % shlex.quote(command)], input=stdin, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "", "timed out after %d s" % timeout
        return done.returncode, self.said(done.stdout), self.said(done.stderr)


def clamp_path(host, run=subprocess.run):
    """From a hosted runner, packets over about 1500 bytes never reach a node and nothing
    says so: ssh hangs at the key exchange, and any larger transfer with it (measured:
    run 41715927503). Sized for this one address, so that the runner's other traffic
    stays as it is. Says nothing of the address, its output ends in public logs."""
    found = run(["ip", "route", "get", str(host)], capture_output=True, text=True)
    words = found.stdout.split()
    if found.returncode != 0 or "dev" not in words:
        raise NodeError("no route to the node to size")
    via = ["via", words[words.index("via") + 1]] if "via" in words else []
    set_it = run(["sudo", "-n", "ip", "route", "replace", "%s/32" % host, *via, "dev", words[words.index("dev") + 1],
                  "mtu", "lock", str(PATH_MTU)], capture_output=True, text=True)
    if set_it.returncode != 0:
        raise NodeError("could not size the path to the node: ssh would hang on it")


def new_key(directory: Path) -> Path:
    key = directory / "node_key"
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "ci-node", "-f", str(key)],
                   check=True, stdin=subprocess.DEVNULL)
    return key


# The node's mark: it serves every leg of NODE_LEGS. A runner's mark is its leg's letter.
NODE_MARK = "x"


def node_name(run, attempt) -> str:
    """Our mark on the node, its hostname: the run that owns it."""
    name = "ci%sa%s%s" % (run, attempt, NODE_MARK)
    if not NAME.match(name) or len(name) > 20:
        raise ValueError("not a node name: %r" % name)
    return name


def node_names(run, attempt) -> list:
    return [node_name(run, attempt)]


def runner_name(run, attempt, leg) -> str:
    """A leg's runner: the run, the attempt and the leg's letter."""
    return "ci%sa%s%s" % (run, attempt, leg[0])


def leg_label(run, attempt, leg) -> str:
    """One label, one leg, one single-job runner. release.yml's plan and runs-on say the
    same (test_release_builder.py holds them together)."""
    return "latitude-%s-%s-%s" % (run, attempt, leg)


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


def remaining(deadline, clock):
    """Seconds left until `deadline` (a time of `clock`, or None for no deadline); NodeError when there are none."""
    if deadline is None:
        return None
    left = deadline - clock()
    if left <= 0:
        raise NodeError("the node was not ready within %d s of its creation: it is deleted, not waited for until the job is killed" % PROVISION_DEADLINE_S)
    return left


def wait_up(latitude, node_id, make_ssh, clock=time.monotonic, sleep=time.sleep, limit=UP_LIMIT_S):
    start = clock()
    last = "the API gives no address yet"
    said = None
    while clock() - start < limit:
        address = latitude.address(node_id)
        if address:
            ssh = make_ssh(*address)
            code, _, err = ssh("true", timeout=30)
            if code == 0:
                say("node up after %.0f s" % (clock() - start))
                return ssh
            last = "ssh exited %d: %s" % (code, err.strip()[-300:])
        else:
            last = "the API gives no address yet"
        # Each change as it happens: a node that never comes up has to say why.
        if last != said:
            say("%.0f s: %s" % (clock() - start, last))
            said = last
        sleep(5)
    raise NodeError("the node did not answer on SSH within %d s; last: %s" % (limit, last))


def turn_off(latitude, names, clock=time.monotonic, sleep=time.sleep, limit=GONE_LIMIT_S,
             answer_limit=OFF_ANSWER_LIMIT_S, recheck=0) -> list:
    """Delete the nodes of these names and see them gone, each whatever happens to the
    other. By name, so a node whose id nobody kept (a cancel during the create) goes too.
    The API's failures are asked again up to answer_limit, then NodeError naming what may
    still be on. A delete it accepted and a node it goes on listing is a warning, never
    seen: what comes next waits by itself."""
    deleted, accepted, left = [], set(), set(names)
    start = clock()
    settled_at = None
    rechecked = not recheck
    while True:
        problem = None
        try:
            mine = [n for n in latitude.nodes() if n.get("name") in names]
            left = {n.get("name") for n in mine}
            if not mine:
                if not deleted and not rechecked:
                    # `recheck` seconds, once: nothing listed yet is not nothing made (a create cut by a cancel can still be answered)
                    rechecked = True
                    say("nothing is listed under %s yet: looking again in %d s" % (", ".join(sorted(names)), recheck))
                    sleep(recheck)
                    continue
                return deleted
        except NodeError as exc:
            problem, mine = exc, []
        for node in mine:
            try:
                latitude.delete(node["id"])
                accepted.add(node.get("name"))
                if node["id"] not in deleted:
                    deleted.append(node["id"])
            except NodeError as exc:
                problem = exc
        if problem:
            say("the API did not do it, asking again: %s" % problem)
            if clock() - start >= answer_limit:
                refused = sorted(left - accepted)
                if refused:
                    raise NodeError("%s could not be deleted, the API failed for %d s: %s"
                                     % (", ".join(refused), answer_limit, problem)) from None
                say("::warning::%s: delete accepted, and the API does not answer to say it is gone: %s" % (", ".join(sorted(accepted)), problem))
                return deleted
        else:
            settled_at = clock() if settled_at is None else settled_at
            if clock() - settled_at >= limit:
                say("::warning::%s: delete accepted, still listed %d s later" % (", ".join(sorted(left)), limit))
                return deleted
        sleep(10)


def off_now(latitude, name) -> bool:
    """One pass and no waiting, for legs that have ended: what confirms it is the
    turn_off at the end."""
    try:
        for node in latitude.nodes():
            if node.get("name") == name:
                latitude.delete(node["id"])
        return True
    except NodeError as exc:
        say("%s: the API did not delete it, asking again at the next poll: %s" % (name, exc))
        return False


def clear_leftovers(latitude, github, run, attempt):
    """What no run needs any more goes. That covers an earlier attempt of this run, a node
    of this one a failed provision left, and a run already re-launched, whose watch has
    deleted the node, whatever the list still says. Another run's live node is that run's:
    there is no fixed machine to share, so two runs do not get in each other's way. A node
    made by hand has another name and is never touched."""
    for node in latitude.nodes():
        name = node.get("name") or ""
        ours = NAME.match(name)
        if ours and ours.group(1) == str(run):
            say("an earlier attempt of this run left %s: deleting it" % name)
            latitude.delete(node["id"])
        elif ours and (github.run_finished(ours.group(1)) or github.relaunched(ours.group(1))):
            say("run %s needs nothing any more and left %s: deleting it" % (ours.group(1), name))
            latitude.delete(node["id"])


def start_runner(github, ssh, run, attempt, leg):
    config = github.jit_config(runner_name(run, attempt, leg), leg_label(run, attempt, leg))
    code, out, err = ssh("bash /usr/local/sbin/node_runner.sh %s %s" % (run, leg[0]), stdin=(config + "\n").encode(), timeout=120)
    if code != 0:
        raise NodeError("node_runner.sh exited %d: %s" % (code, (err or out)[-300:]))
    say("runner started, for %s" % leg)


def wait_online(github, name, clock=time.monotonic, sleep=time.sleep, limit=RUNNER_ONLINE_LIMIT_S):
    start = clock()
    while True:
        if github.runner_status(name) == "online":
            return
        if clock() - start >= limit:
            raise NodeError("the runner %s did not come online within %d s: the leg would wait for it up to GitHub's "
                             "24-hour limit" % (name, limit))
        sleep(10)


#: Where both legs find the series applied once (tools/engine.py materialize --shared) and
#: the node's own fetch of the Mozilla tag, which starts right after node_bootstrap.sh,
#: before the runners exist: on 04/11 the first leg spent 110-180 s fetching it from
#: GitHub after its job had started.
SHARED = "/srv/shared"


def prefetch_script(repository, tag, shared=SHARED) -> str:
    """Root's fetch of the tag into SHARED/mozilla.git, in the background; a file says when
    it is whole (fetched-<tag>) or failed (failed-<tag>). Whoever waits on it falls back to
    GitHub on its own (engine.py, _wait_for_mirror)."""
    mirror = shared + "/mozilla.git"
    return ("set -eu\ninstall -d -m 1777 %(shared)s\nrm -rf %(m)s\ngit init -q --bare %(m)s\n"
            "nohup sh -c 'git -C \"$0\" fetch -q --depth 1 --no-tags \"$1\" \"refs/tags/$2:refs/tags/$2\" > \"$0.log\" 2>&1"
            " && touch \"$0/fetched-$2\" || touch \"$0/failed-$2\"' %(m)s %(r)s %(t)s > /dev/null 2>&1 < /dev/null &\n"
            % {"shared": shlex.quote(shared), "m": shlex.quote(mirror), "r": shlex.quote(repository), "t": shlex.quote(tag)})


def bootstrap(ssh, clock=time.monotonic, sleep=time.sleep, deadline=None):
    """node_bootstrap.sh on the node, asked again for as long as the failure is the network's and BOOTSTRAP_RETRY_CAP_S has not gone by since the
    first attempt (the script is made to be run again: it removes what it made and `id runner ||` its user). Returns its output; NodeError with
    the last failure otherwise, and the caller deletes the node."""
    start = clock()
    attempts = 0
    # no new attempt starts past this time: the cap since the first, or the deadline of provision() when that comes first
    stop_at = start + BOOTSTRAP_RETRY_CAP_S if deadline is None else min(start + BOOTSTRAP_RETRY_CAP_S, deadline)
    while True:
        attempts += 1
        left = remaining(deadline, clock)
        code, out, err = ssh("bash -s", stdin=script_bytes("node_bootstrap.sh"), timeout=BOOTSTRAP_LIMIT_S if left is None else max(30, min(BOOTSTRAP_LIMIT_S, left)))
        if code == 0:
            if attempts > 1:
                say("node_bootstrap.sh succeeded at attempt %d, %.0f s in" % (attempts, clock() - start))
            return out
        text = (err or out)[-600:]
        transient = code == 255 or TRANSIENT.search(text) is not None
        if not transient or clock() + BOOTSTRAP_RETRY_PAUSE_S >= stop_at:
            raise NodeError("node_bootstrap.sh exited %d: %s%s" % (code, text, (" (after %d attempts in %.0f s)" % (attempts, clock() - start)) if attempts > 1 else ""))
        say("::warning::node_bootstrap.sh exited %d at attempt %d, %.0f s in, the network's: asked again in %d s on the same node"
            % (code, attempts, clock() - start, BOOTSTRAP_RETRY_PAUSE_S))
        sleep(BOOTSTRAP_RETRY_PAUSE_S)


def provision(latitude, github, run, attempt, make_ssh, make_key, clock=time.monotonic, sleep=time.sleep, upstream=None):
    """The run's node ready with one runner per leg online, or NodeError with the node off."""
    clear_leftovers(latitude, github, run, attempt)
    name = node_name(run, attempt)
    key = make_key()
    public_key = Path(str(key) + ".pub").read_text(encoding="utf-8").strip()
    deadline = clock() + PROVISION_DEADLINE_S
    # The create inside the try: a lost answer can mean a node exists, and the
    # delete goes by name.
    try:
        node_id = latitude.create(name, public_key)
        ssh = wait_up(latitude, node_id, make_ssh, clock, sleep, limit=min(UP_LIMIT_S, remaining(deadline, clock)))
        drop_key_of(latitude, name)
        out = bootstrap(ssh, clock, sleep, deadline=deadline)
        say(out.strip().splitlines()[-1] if out.strip() else "prepared")
        host_key = HOST_KEY_LINE.search(out)
        if not host_key:
            raise NodeError("node_bootstrap.sh named no host key: the runner's configuration would go to an unverified host")
        ssh = ssh.pin(host_key.group(1))
        if upstream:
            # Only a head start: a failure here costs the legs their own fetch, nothing more.
            code, out, err = ssh("bash -s", stdin=prefetch_script(upstream["mozilla_repository"], upstream["mozilla_tag"]).encode(),
                                 timeout=60)
            say("the tag's fetch on the node: %s" % ("started" if code == 0 else "not started (%s), the legs fetch it" % (err or out)[-200:]))
        code, out, err = ssh("cat > /usr/local/sbin/node_runner.sh", stdin=script_bytes("node_runner.sh"), timeout=60)
        if code != 0:
            raise NodeError("could not put node_runner.sh on the node: %s" % err[-300:])
        for leg in node_legs():
            start_runner(github, ssh, run, attempt, leg)
        for leg in node_legs():
            wait_online(github, runner_name(run, attempt, leg), clock, sleep, limit=min(RUNNER_ONLINE_LIMIT_S, remaining(deadline, clock)))
    except BaseException:
        off_after_failure(latitude, github, run, attempt, clock, sleep)
        raise


def drop_key_of(latitude, name):
    """Housekeeping, like the runners: the sweeper finds what this leaves."""
    try:
        latitude.drop_keys(lambda key: key == name)
    except (NodeError, OSError, ValueError, KeyError) as exc:
        say("::warning::the node's public key stays registered: %s" % exc)


def remove_runners(github, labels) -> list:
    """Housekeeping: GitHub removes a single-job runner by itself in the end. Its
    failure never makes a deleted node look like a failed delete."""
    try:
        return github.remove_runners(labels)
    except (NodeError, OSError, ValueError, KeyError) as exc:
        say("::warning::runners left registered: %s" % exc)
        return []


def off_after_failure(latitude, github, run, attempt, clock, sleep):
    try:
        turn_off(latitude, node_names(run, attempt), clock, sleep)
    except (NodeError, OSError, ValueError, KeyError) as exc:
        say("::warning::the node could not be deleted, stop-node and the sweeper will: %s" % exc)
    remove_runners(github, [leg_label(run, attempt, leg) for leg in node_legs()])


def ssh_answers(host, port, timeout=15) -> bool:
    """The machine's own word, with no key and no API in the way: sshd's banner."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            return sock.recv(4) == b"SSH-"
    except OSError:
        return False


def parse_time(text):
    """Seconds since the epoch of an API time, UTC. GitHub ends it with Z, Latitude with +00:00."""
    m = re.match(r"^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(?:\.\d+)?(?:Z|\+00:00)$", text or "")
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
    mine = [j for j in jobs_by_leg(jobs).values() if any(str(l).startswith("latitude-") for l in j.get("labels") or [])]
    queued = [j for j in mine if j.get("status") in WAITING]
    if not queued or any(j.get("status") == "in_progress" for j in mine):
        return False
    since = [parse_time(j.get("created_at")) for j in queued]
    since = [t for t in since if t is not None]
    return bool(since) and now - max(since) >= limit


class Machine:
    """What is known of a leg's node between two polls. Down: sshd does not answer, or,
    with no address to ask, the API does not list it as running. The API's list alone never
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


def look_at_machines(latitude, run, attempt, machines, by_leg, probe, now):
    rows = None
    try:
        rows = {n.get("name"): n for n in latitude.nodes()}
    except NodeError as exc:
        say("the API did not answer, not read as a lost node: %s" % exc)
    for leg, machine in machines.items():
        listed = None
        if rows is not None:
            try:
                row = rows.get(node_name(run, attempt))
                listed = bool(row) and str(row.get("status", "")).lower() == RUNNING
                if row:
                    address = latitude.address(row["id"])
                    machine.address = address or machine.address
                    listed = listed and address is not None
            except NodeError as exc:
                listed = None
                say("the API did not answer, not read as a lost node: %s" % exc)
        answers = probe(*machine.address) if machine.address else None
        done = sum(1 for step in (by_leg.get(leg) or {}).get("steps") or [] if step.get("conclusion") == "success")
        machine.see(now, listed, answers, done)


def leg_state(job, machine, now, cancelled=False):
    """ended | lost | stuck, or None while nothing is decided, from a leg's job and its machine.

    Lost needs two witnesses, whichever spoke first: the machine silent for
    DOWN_LIMIT_S, and the leg's job that agrees. It agrees when GitHub ended it with no
    failed step of a live runner, when it waits in the queue, or when it still runs without
    finishing a step: GitHub failed a dead runner's job 9m27s after the kill, one sample,
    and nothing waits up to the job's limit for it. Only with the API silent as well is
    a running leg left alone: that may be the API and not the machine. After a cancel nothing
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
        raise NodeError("a re-launch must name core_ref and wrapper_ref (got %r and %r)" % (core_ref, wrapper_ref))
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


def after_loss(latitude, github, run, attempt, lost, ref, sha, inputs, relaunch_of, clock, sleep) -> str:
    """Both legs have ended or are lost. The nodes go first, seen gone: This run's cancel comes last. A failure on the way comes back
    at the next poll, then at the next watch job: each step here looks at what the last one
    did. A cancel by a person is looked for after the nodes have gone and again right
    before the dispatch, and a re-launch happens only if GitHub says there was none."""
    turn_off(latitude, node_names(run, attempt), clock, sleep)
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
        say("%s was lost: the whole run goes again once, on new nodes, with the cache already saved" % which)
        try:
            row = dispatch_once(github, run, ref, inputs, clock, sleep)
        except NoAnswer:
            raise
        except NodeError as exc:
            say("::error::%s was lost, and GitHub refuses the re-launch: %s. %s" % (which, exc, NOT_A_CANCEL))
            github.cancel(run)
            return "failed"
        say("::warning::%s was lost: this run is replaced by %s, which goes again once, on new nodes, "
            "with the cache already saved" % (which, named(row) if row else 'the run named "%s", not listed yet' % relaunch_title(run)))
    github.cancel(run)
    return "lost"


def give_up(latitude, github, run, attempt, why, clock, sleep) -> str:
    say("::error::%s: the run fails. %s" % (why, NOT_A_CANCEL))
    try:
        turn_off(latitude, node_names(run, attempt), clock, sleep)
    except NodeError as exc:
        say("::warning::the nodes could not be deleted, the sweeper will: %s" % exc)
    github.cancel(run)
    return "failed"


def why_not_coming(github, run, attempt, stuck_legs) -> str:
    why = "%s waits for a runner that is not coming, on a machine that answered" % " and ".join(stuck_legs)
    try:
        if any(github.runner_status(runner_name(run, attempt, leg)) is None for leg in stuck_legs):
            why += ", and the runner has gone without taking it, as one does when the node refuses another run's job: that uses it up"
    except NodeError as exc:
        say("the list of runners did not answer: %s" % exc)
    return why


def conclude(latitude, github, run, attempt, settled, by_leg, cancelled, ref, sha, inputs, relaunch_of, clock, sleep) -> str:
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
        return give_up(latitude, github, run, attempt, why_not_coming(github, run, attempt, stuck_legs), clock, sleep)
    red = [leg for leg in ended if by_leg[leg].get("conclusion") not in ("success", "skipped")]
    if red:
        return give_up(latitude, github, run, attempt, "the machine of %s was lost, and %s ended red, which a new run would not "
                       "change: no re-launch" % (" and ".join(lost), " and ".join(red)), clock, sleep)
    return after_loss(latitude, github, run, attempt, lost, ref, sha, inputs, relaunch_of, clock, sleep)


def watch(latitude, github, run, attempt, ref, inputs, relaunch_of="", probe=ssh_answers,
          clock=time.time, sleep=time.sleep, poll=60, limit=STAGE_LIMIT_S, sha="") -> str:
    """Until both legs are settled, or this job's share of the time has run out. Returns
    done, lost (re-launched), failed, or continue for the next watch job, which reads the
    same from GitHub and the API and needs nothing from this one. The node is deleted as
    soon as every leg has settled and none of them needs it kept. A poll that fails, in whatever way, is
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
        look_at_machines(latitude, run, attempt, open_legs, by_leg, probe, clock())
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
        # One node for every leg: it goes once all of them are done with it.
        if to_turn_off and to_turn_off >= set(machines):
            if off_now(latitude, node_name(run, attempt)):
                to_turn_off.clear()
        if len(settled) < len(machines):
            return None
        return conclude(latitude, github, run, attempt, settled, by_leg, cancelled, ref, sha, inputs, relaunch_of, clock, sleep)

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
            if not isinstance(exc, NodeError):
                say("::warning::an error that is not a service's:\n%s" % traceback.format_exc())
            if blind >= BLIND_POLLS:
                raise WatchBlind("nothing usable for %d polls, the last: %s" % (blind, what)) from None
        sleep(poll)
    return "continue"


def finished_legs(github, run, attempt) -> list:
    by_leg = jobs_by_leg(github.jobs(run, attempt))
    return [leg for leg in node_legs() if leg in by_leg and by_leg[leg].get("status") == "completed"]


def sweep(latitude, github, now, max_age_hours=GUARD_HOURS, dry_run=False) -> list:
    """Delete the nodes we started that no run needs: our name, and older than the
    limit, or owned by a run that has finished, or whose legs have all finished (what a
    watch that died leaves behind). A node with any other name was made by hand and is
    never touched. Without GitHub, the age alone."""
    deleted = []
    listed = latitude.nodes()
    for node in listed:
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
                elif set(node_legs()) <= set(finished_legs(github, m.group(1), m.group(2))):
                    why = "its legs have finished"
            except (NodeError, OSError, ValueError, KeyError) as exc:
                say("run %s unknown for now, its node stays: %s" % (m.group(1), exc))
        if why:
            say("%s %s: %s" % ("would delete" if dry_run else "deleting", node["name"], why))
            if not dry_run:
                latitude.delete(node["id"])
            deleted.append(node["name"])
    if not dry_run:
        # A key whose node never came up, or was lost before provision got to it.
        on = {node.get("name") for node in listed} - set(deleted)
        for name in latitude.drop_keys(lambda key: bool(NAME.match(key)) and key not in on):
            say("the public key of %s: dropped" % name)
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
    missing = [v for v in ("LATITUDE_API_KEY", "LATITUDE_PROJECT") if not os.environ.get(v)]
    if missing:
        raise NodeError("missing from the environment: %s" % ", ".join(missing))
    exclude = os.environ.get("LATITUDE_EXCLUDE_SITES", "").replace(",", " ").split()
    if exclude:
        say("sites left out by LATITUDE_EXCLUDE_SITES: %s" % ", ".join(exclude))
    return Latitude(os.environ["LATITUDE_PROJECT"], os.environ["LATITUDE_API_KEY"], exclude=exclude)


def github_from_env():
    repo = os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        return None
    return GitHub(repo, os.environ.get("GH_TOKEN", ""), os.environ.get("RUNNER_ADMIN_TOKEN", ""))


def node_ssh(work: Path, clamp=clamp_path):
    def make(host, port):
        clamp(host)
        return Ssh(host, port, work / "node_key")
    return make


def run_provision(a) -> int:
    work = Path(tempfile.mkdtemp(prefix="build-node-"))
    try:
        latitude, github = from_env(), github_from_env()
        if github is None:
            raise NodeError("GITHUB_REPOSITORY is not set")
        upstream = None
        if a.upstream:
            # Only a head start: without it the legs fetch the tag themselves, as before.
            try:
                upstream = json.loads(Path(a.upstream).read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                say("::warning::no tag fetched ahead on the node, %s unreadable: %s" % (a.upstream, exc))
        provision(latitude, github, a.run, a.attempt, node_ssh(work), lambda: new_key(work), upstream=upstream)
    except (NodeError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        # Without its node the leg has nowhere to run: the run has failed.
        print("::error::no node for %s, the run fails: %s" % (" and ".join(node_legs()), exc), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


def run_watch(a) -> int:
    latitude, github = from_env(), github_from_env()
    if github is None:
        raise NodeError("GITHUB_REPOSITORY is not set")
    env = os.environ.get
    inputs = relaunch_inputs(a.run, env("KIND", ""), env("TAG", ""), env("COMPILE_CACHE", ""), env("CORE_REF", ""),
                             env("WRAPPER_REF", ""), env("GTESTS", ""), env("GTEST_FILTER_INPUT", ""))
    try:
        verdict = watch(latitude, github, a.run, a.attempt, a.ref, inputs, env("RELAUNCH_OF", ""), sha=env("GITHUB_SHA", ""))
    except WatchBlind as exc:
        # No verdict: the legs may be running, so nothing here deletes a node. The
        # next watch job takes over, and the sweeper is behind it.
        print("::error::%s" % exc, file=sys.stderr)
        return 3
    print("verdict=%s" % verdict)
    return 1 if verdict == "failed" else 0


def run_off(a) -> int:
    latitude, github = from_env(), github_from_env()
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
            print("::warning::%s has not finished, the node stays: the sweeper deletes it when it has" % " and ".join(staying), file=sys.stderr)
    code = 0
    # One node for every leg: it goes only when none is left on it.
    names = node_names(a.run, a.attempt) if set(node_legs()) <= set(going) else []
    try:
        if not names:
            print("deleted: nothing, the node stays for the legs still on it")
        else:
            # a node job that did not succeed (--if-legs-done absent) may have been cut in the middle of the create: look twice before "nothing was on"
            print("deleted: %s" % (", ".join(names) if turn_off(latitude, names, recheck=0 if a.if_legs_done else OFF_RECHECK_S) else "nothing was on"))
    except NodeError as exc:
        # Red, because it may be billing and only a person reading this can know. Nothing
        # needs this job: the legs, the gate and the publish keep their own results.
        print("::error::a node may still be on, and billing; the sweeper tries again every 30 minutes. This job says "
              "nothing about the build, which has its own results: %s" % exc, file=sys.stderr)
        code = 1
    if github and os.environ.get("RUNNER_ADMIN_TOKEN"):
        print("runners removed: %s" % (", ".join(remove_runners(github, [leg_label(a.run, a.attempt, leg) for leg in going])) or "none"))
    return code


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the build nodes of a release run")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("provision", "off", "watch"):
        p = sub.add_parser(name)
        p.add_argument("--run", required=True)
        p.add_argument("--attempt", required=True)
        if name == "watch":
            p.add_argument("--ref", required=True)
        if name == "off":
            p.add_argument("--if-legs-done", action="store_true")
        if name == "provision":
            p.add_argument("--upstream", default="", help="the series' UPSTREAM.json: the node fetches its tag at once")
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
        latitude, github = from_env(), github_from_env()
        if a.cmd == "sweep":
            now = time.time()
            print("deleted: %s" % (", ".join(sweep(latitude, github, now, a.max_age_hours, a.dry_run)) or "nothing"))
            if github:
                print("cancelled: %s" % (", ".join(str(r) for r in cancel_stuck(github, now, a.dry_run)) or "nothing"))
        elif a.cmd == "status":
            for node in latitude.nodes():
                print("%s %s %s" % (node.get("name"), node.get("status"), node.get("created_at")))
    except NodeError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
