# automation/element-atoms

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `remote/marionette/atom.sys.mjs` (module level, after const ATOMS = {, already-hook): Adds an export so Juggler's Page.elementAtom can run the atom sources itself. Breaks silently if upstream renames ATOMS. Comment text is part of the diff and should move to our own file if the series allows. Verified: the consumer is juggler/content/PageAgent.js:445 (ATOM_SOURCES[atom] run via Cu.evalInSandbox at line 449).
