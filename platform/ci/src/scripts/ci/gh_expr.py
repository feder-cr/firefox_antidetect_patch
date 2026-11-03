"""GitHub Actions' expressions and job graph, for the tests that read release.yml.

An expression's meaning is not what it looks like: `cond && '' || '-node'` is `-node`
whatever cond is, because '' is falsy, and a job without a status function has an
implicit success() that reads its ancestors, not only what it needs. So the tests
evaluate the workflow's own expressions, with GitHub's documented semantics:
https://docs.github.com/en/actions/reference/workflows-and-actions/expressions

    evaluate("a.b == 'x' && 'yes' || 'no'", {"a": {"b": "X"}})   -> "yes"
    render("key-${{ a.b }}-end", {"a": {"b": "x"}})              -> "key-x-end"
    jobs_after(doc["jobs"], {"build": "failure"}, context)       -> {job: result}
"""
from __future__ import annotations

import json
import math
import re

TOKEN = re.compile(r"""\s*(?:
    (?P<string>'(?:[^']|'')*')
  | (?P<number>-?\d+(?:\.\d+)?)
  | (?P<op>&&|\|\||==|!=|<=|>=|<|>|!|\(|\)|,)
  | (?P<name>[A-Za-z_][\w-]*(?:\.[A-Za-z_*][\w-]*)*)
)""", re.X)
STATUS = re.compile(r"\b(?:always|cancelled|failure|success)\s*\(")
EXPRESSION = re.compile(r"\$\{\{(.*?)\}\}", re.S)


def truthy(value) -> bool:
    return value not in (None, False, 0, "") and not (isinstance(value, float) and math.isnan(value))


def _number(value) -> float:
    if value is None:
        return 0.0
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        if not value.strip():
            return 0.0
        try:
            return float(value)
        except ValueError:
            return math.nan
    return math.nan


def _equal(a, b) -> bool:
    if isinstance(a, str) and isinstance(b, str):
        return a.lower() == b.lower()
    if isinstance(a, (dict, list)) or isinstance(b, (dict, list)):
        return a is b
    return _number(a) == _number(b)


def to_text(value) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float) and value == int(value):
        return str(int(value))
    return value if isinstance(value, str) else json.dumps(value)


class _Parser:
    def __init__(self, text: str, context: dict):
        self.context, self.tokens, at = context, [], 0
        text = text.strip()
        while at < len(text):
            m = TOKEN.match(text, at)
            if not m or m.end() == at:
                raise ValueError("cannot read %r at %d" % (text, at))
            self.tokens.append((m.lastgroup, m.group(m.lastgroup)))
            at = m.end()
        self.at = 0

    def peek(self):
        return self.tokens[self.at] if self.at < len(self.tokens) else (None, None)

    def take(self, value=None):
        kind, text = self.peek()
        if value is not None and text != value:
            raise ValueError("expected %r, found %r" % (value, text))
        self.at += 1
        return kind, text

    def parse(self):
        value = self.or_()
        if self.at != len(self.tokens):
            raise ValueError("left over: %r" % (self.tokens[self.at:],))
        return value

    def or_(self):
        value = self.and_()
        while self.peek()[1] == "||":
            self.take()
            right = self.and_()
            value = value if truthy(value) else right
        return value

    def and_(self):
        value = self.compare()
        while self.peek()[1] == "&&":
            self.take()
            right = self.compare()
            value = right if truthy(value) else value
        return value

    def compare(self):
        value = self.unary()
        while self.peek()[1] in ("==", "!=", "<", "<=", ">", ">="):
            op = self.take()[1]
            right = self.unary()
            if op == "==":
                value = _equal(value, right)
            elif op == "!=":
                value = not _equal(value, right)
            else:
                a, b = _number(value), _number(right)
                value = {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]
        return value

    def unary(self):
        if self.peek()[1] == "!":
            self.take()
            return not truthy(self.unary())
        return self.atom()

    def atom(self):
        kind, text = self.take()
        if text == "(":
            value = self.or_()
            self.take(")")
            return value
        if kind == "string":
            return text[1:-1].replace("''", "'")
        if kind == "number":
            return float(text)
        if kind != "name":
            raise ValueError("unexpected %r" % (text,))
        if self.peek()[1] == "(":
            self.take()
            args = []
            while self.peek()[1] != ")":
                args.append(self.or_())
                if self.peek()[1] == ",":
                    self.take()
            self.take(")")
            return self.call(text.lower(), args)
        lowered = text.lower()
        if lowered in ("true", "false"):
            return lowered == "true"
        if lowered == "null":
            return None
        value = self.context
        for part in text.split("."):
            value = value.get(part) if isinstance(value, dict) else None
        return value

    def call(self, name, args):
        if name in ("always", "cancelled", "failure", "success"):
            status = self.context.get("__status", {})
            return True if name == "always" else status.get(name, name == "success")
        if name == "format":
            return re.sub(r"\{(\d+)\}", lambda m: to_text(args[1 + int(m.group(1))]), to_text(args[0]))
        if name == "fromjson":
            return json.loads(args[0])
        if name == "tojson":
            return json.dumps(args[0])
        if name == "contains":
            hay, needle = args
            if isinstance(hay, list):
                return any(_equal(x, needle) for x in hay)
            return to_text(needle).lower() in to_text(hay).lower()
        if name == "startswith":
            return to_text(args[0]).lower().startswith(to_text(args[1]).lower())
        if name == "endswith":
            return to_text(args[0]).lower().endswith(to_text(args[1]).lower())
        if name == "hashfiles":
            return "HASH"
        raise ValueError("no function %s" % name)


def evaluate(expression: str, context: dict):
    return _Parser(expression, context).parse()


def render(text: str, context: dict) -> str:
    return EXPRESSION.sub(lambda m: to_text(evaluate(m.group(1), context)), text)


def bare(condition) -> str:
    """An `if:` may be written with or without ${{ }}."""
    text = str(condition).strip()
    m = re.fullmatch(r"\$\{\{(.*)\}\}", text, re.S)
    return m.group(1) if m else text


def condition(cond) -> str:
    """The `if:` GitHub evaluates: success() unless it names a status function."""
    if cond is None or cond is True:
        return "success()"
    text = bare(cond)
    return text if STATUS.search(text) else "success() && (%s)" % text


def ancestors(jobs: dict, name: str) -> set:
    seen, todo = set(), list(_needs(jobs[name]))
    while todo:
        need = todo.pop()
        if need not in seen:
            seen.add(need)
            todo += _needs(jobs[need])
    return seen


def _needs(job: dict) -> list:
    needs = job.get("needs", [])
    return [needs] if isinstance(needs, str) else list(needs)


def status_of(results) -> dict:
    results = list(results)
    return {"success": all(r == "success" for r in results), "failure": "failure" in results}


def jobs_after(jobs: dict, forced: dict, context: dict, outputs=None, cancelled=False) -> dict:
    """What each job ends as, when the jobs in `forced` ended so and the others ran and
    went green if their `if:` let them. A skipped or failed ancestor counts against
    success() at any distance: reported, and what a `needs.x.result` check exists for."""
    outputs = outputs or {}
    done = {}

    def result(name):
        if name in done:
            return done[name]
        for need in _needs(jobs[name]):
            result(need)
        if name in forced:
            done[name] = forced[name]
            return done[name]
        here = dict(context)
        here["needs"] = {n: {"result": done[n], "outputs": outputs.get(n, {})} for n in _needs(jobs[name])}
        status = status_of(done[a] for a in ancestors(jobs, name))
        status["cancelled"] = cancelled
        status["success"] = status["success"] and not cancelled
        here["__status"] = status
        ran = truthy(evaluate(condition(jobs[name].get("if")), here))
        done[name] = "success" if ran else "skipped"
        return done[name]

    for name in jobs:
        result(name)
    return done


def step_runs(step: dict, context: dict, failed=False, cancelled=False) -> bool:
    """Whether a step runs, at a point of its job where steps have failed or not, or the
    run was cancelled."""
    here = dict(context, __status={"success": not failed and not cancelled, "failure": failed, "cancelled": cancelled})
    return truthy(evaluate(condition(step.get("if")), here))
