# platform/script-cache

Category: other
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `js/xpconnect/loader/ScriptPreloader.cpp` (ScriptPreloader::CollectReports; ScriptPreloader::WalkCacheHeader (new template); ScriptPreloader::InitCacheInternal; ScriptPreloader::AdoptWrittenCache (new); ScriptPreloader::WriteCache, hard): Not a fingerprint change: a memory optimisation. After writing the cache file it maps that file and points the entries into the mapping, instead of keeping heap copies (the comment reports about 16.5 MB kept on fresh profiles). Hard for two reasons. First, InitCacheInternal's own body is rewritten into a template (WalkCacheHeader) shared by two callers, so any upstream change to that function will
- `js/xpconnect/loader/ScriptPreloader.h` (ScriptPreloader::CachedStencil (AdoptMappedRange, inline); class ScriptPreloader (WalkCacheHeader and AdoptWrittenCache declarations; mWrittenCacheData member), hard): Hard because the new data member changes ScriptPreloader's layout and the template declaration must sit in the class. Only the body of AdoptMappedRange (about 6 lines, inline) could move to a .cpp of ours. Most of the 52 added lines are Italian comments explaining the design; the comments are the rationale for the change and should travel with it.
