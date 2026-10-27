# automation/enterprise-policies

Category: juggler
Prefs: browser.policies.alternatePath
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/enterprisepolicies/EnterprisePoliciesParent.sys.mjs` (module scope, new class PlaywrightPoliciesProvider after class JSONPoliciesProvider (line 688 in the Mozilla base), movable): The class is defined but NOT registered in this file: the provider list at line 176 still pushes only the stock providers, so the class is dead code. JSONPoliciesProvider is module-local, so moving the class to our own file needs an export or a shim. The dump() string contains a product name (Playwright) in code, check it against the no-vendor-names rule. Verified dead: git grep over the whole tra
