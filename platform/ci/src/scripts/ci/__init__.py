"""The release workflow's logic, out of the YAML.

release.yml says WHEN each thing runs; this package says WHAT it does, so
every rule lives in one place and has a test (scripts/ci/test_ci.py):

  legs.py      the three legs as data (Strategy): runner, target, asset name
               derived from the version the tree builds
  mozconfig.py the CI's .mozconfig, composed in layers (Builder), each layer
               with the reason it exists written into the file it produces
  phase.py     one phase of a leg run as a command that records events
               (start, end, seconds, exit) and, on failure, the first error
               lines, into the step summary (Command + Observer)
  report.py    every leg's events in one table, the run's summary

    python3 scripts/ci <command> ...      (see __main__.py)
"""
