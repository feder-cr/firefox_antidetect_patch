# platform/prefs

Category: other, prefs
Prefs: browser.translations.select.enable, font.size.monospace.ar, font.size.monospace.el, font.size.monospace.he, font.size.monospace.x-cyrillic, font.size.monospace.x-unicode, font.size.monospace.x-western, invisible_firefox.usage_ping.enabled, invisible_firefox.usage_ping.url, juggler.locale.override, juggler.timezone.override, network.proxy.socks_password, network.proxy.socks_username, termsofuse.acceptedDate, termsofuse.acceptedVersion, ui.font.-moz-button, ui.font.-moz-button.size, ui.font.-moz-field, ui.font.-moz-field.size, ui.font.-moz-list, ui.font.-moz-list.size, ui.font.-moz-pull-down-menu, ui.font.-moz-pull-down-menu.size, ui.font.caption, ui.font.caption.size, ui.font.icon, ui.font.icon.size, ui.font.menu, ui.font.menu.size, ui.font.message-box, ui.font.message-box.size, ui.font.small-caption, ui.font.small-caption.size, ui.font.status-bar, ui.font.status-bar.size, zoom.stealth., zoom.stealth.audio.fp_noise, zoom.stealth.audio.max_channel_count, zoom.stealth.audio.output_latency_ms, zoom.stealth.audio.sample_rate, zoom.stealth.branding.archive, zoom.stealth.canvas.edge_tolerance, zoom.stealth.canvas.substitute_pixels, zoom.stealth.focus.ignore_os_deactivation, zoom.stealth.font.fontlist, zoom.stealth.font.system_ui, zoom.stealth.fonts.generics, zoom.stealth.fonts.manifest, zoom.stealth.fpp.hw_seed, zoom.stealth.hw_concurrency, zoom.stealth.max_touch_points, zoom.stealth.media.decode_support, zoom.stealth.media.decoding_info, zoom.stealth.media.enumeration_ms, zoom.stealth.pointer.all, zoom.stealth.pointer.primary, zoom.stealth.screen.avail_device_height, zoom.stealth.screen.avail_device_width, zoom.stealth.screen.color_depth, zoom.stealth.screen.frame_border, zoom.stealth.screen.height, zoom.stealth.screen.maximized_ui, zoom.stealth.screen.popup_position, zoom.stealth.screen.popup_ui, zoom.stealth.screen.taskbar_px, zoom.stealth.screen.width, zoom.stealth.storage.quota_mb, zoom.stealth.text.coverage_ladder, zoom.stealth.text.freetype_antialias, zoom.stealth.text.freetype_hintstyle, zoom.stealth.text.system_quality, zoom.stealth.voices.list, zoom.stealth.webgl.extensions, zoom.stealth.webgl.float_params, zoom.stealth.webgl.int2_params, zoom.stealth.webgl.int_params, zoom.stealth.webgl.renderer, zoom.stealth.webgl.shader_output_language, zoom.stealth.webgl.shader_precisions, zoom.stealth.webgl.vendor, zoom.stealth.webgl2.extensions, zoom.stealth.widget.occlusion_tracking.enabled
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `browser/app/profile/firefox.js` ((top-level pref() defaults), already-hook): Pure pref defaults, but three sub-changes with different meanings (see pref_groups). (a) the terms-of-use acceptance moved to the core (see First run). (b) stillborn: the browser.translations.enable override line is deleted (it is not in the 
- `modules/libpref/Preferences.cpp` (sDynamicPrefOverrideList (static array), already-hook): Adds a zoom.stealth. entry to the override list so string prefs of that family reach content processes. Any future zoom.stealth.* string pref becomes visible in content processes automatically. Not a pref default, hence not category prefs.
- `modules/libpref/init/StaticPrefList.yaml` ((YAML pref list: new 'juggler.' group before 'layers.'; zoom.stealth.* block before 'End of prefs'), already-hook): Pure definitions (51 entries, list matches the added '- name:' lines exactly), but they declare the whole spoof surface: screen and window geometry, pointer, WebGL, fonts, canvas, text rendering, media capabilities, voices, branding, storage quota, WebRTC host IP, and the juggler session locale and timezone (mirror always, so any thread reads them). Many are read by C++ elsewhere. Misplaced-commen
- `modules/libpref/init/all.js` ((top-level pref defaults: ui.font block near ui.scrollToClick; proxy block; font.size.monospace block), already-hook): Pure defaults with mixed sub-categories (see pref_groups). spoof: ui.font.* system font keywords pinned to Segoe UI 12 on every host (unconditional, placed after the #endif at all.js:446), so Linux no longer reveals 'Sans 13.33px' (values quoted on purpose: float read); font.size.monospace.ar/el/he/x-cyrillic/x-unicode/x-western changed 12 -> 13; they sit inside the Unix-only block '#if !defined(A

## Values changed on Mozilla's own lines (2026-10-29)

No pref of ours is declared twice: a Mozilla pref whose value we change is
changed on the line where Mozilla declares it, marked
`// STEALTH-HOOK(platform/prefs)` at the end of the line, and our files declare
only prefs Mozilla does not have (modules/libpref/init/StealthPrefs.js, included
by greprefs.js; StealthPrefList.yaml for static prefs). Enforced by
modules/libpref/test/test_stealth_prefs_declared_once.py.

### First run (terms of use, data-reporting notice)

Moved to the core on 2026-11-02: invisible_core declares the first-run state on every host (browser.preonboarding.enabled false, termsofuse.acceptedVersion 4, termsofuse.acceptedDate, in user.js before the window exists; measured on official 159: with the two termsofuse prefs neither the terms-of-use modal nor the legacy data-reporting notice appears). The engine leaves Mozilla's lines: tools/tests/test_first_run_is_the_cores.py.

### browser/app/profile/firefox.js: browser.translations.enable and browser.translations.select.enable

firefox.js declared both true; the two lines are replaced by a marker, so all.js's
`false` is the only declaration.

STEALTHFOX: le traduzioni restano al default di Gecko, che e' SPENTO
(`modules/libpref/init/all.js` le dichiara false; e' firefox.js ad
accenderle). Un browser che pilota una pagina non traduce niente.

Cosa costavano, misurato il 2026-08-14 su Linux con about:memory dumpato via
SIGRTMIN. TranslationsParent rileva la lingua di OGNI pagina caricata, e per
farlo istanzia il worker del rilevatore CLD2: `cld-worker.js` tiene **17,75 MB**
di ArrayBuffer nel processo padre, ed e' 17,3 dei 19,4 MB di tutti i worker
chrome messi insieme. Tre coppie di misure interlacciate sull'heap del padre -
255,0/232,3, 276,0/276,0->229,0, 307,3/291,8 - danno tre differenze tutte
negative, media **-28,4 MB**, e la piu' piccola (-15,5) e' della dimensione del
worker stesso.

⛔ E qui la forma della correzione e' una PREF e non un taglio nel codice, al
contrario di about:newtab, per una ragione misurata e non per comodita': la
bandiera funziona. Con la pref al default il worker non nasce - 0,00 MB in tre
giri su tre - mentre browser.newtabpage.enabled lasciava about:home caricare
l'add-on lo stesso. Dove il flag di Firefox copre davvero la feature, tagliare
piu' a fondo aggiungerebbe divergenza dall'upstream per zero MB in piu'.

Non osservabile da una pagina: la traduzione e' interfaccia chrome, nessuna API
web la espone, e non rilevare la lingua non cambia niente di cio' che la pagina
puo' leggere.

### modules/libpref/init/all.js: font.size.monospace.{ar,el,he,x-cyrillic,x-unicode,x-western} 13

In the `#if !defined(ANDROID) && !defined(XP_MACOSX) && defined(XP_UNIX)` block.

STEALTH: all.js sets these to 12 on desktop Unix while Windows keeps the
global 13. This build claims Windows, and the difference is directly
observable - a font-size-less monospace span measured 121.583px on
Windows vs 112.233 on Linux (ratio 13/12). Kept at the Windows value, under
the same condition as the Mozilla lines they replace.
