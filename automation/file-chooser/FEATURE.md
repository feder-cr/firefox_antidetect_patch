# automation/file-chooser

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/html/HTMLInputElement.cpp` (HTMLInputElement::InitFilePicker, already-hook): When Juggler asked to intercept the file chooser, InitFilePicker returns before the native dialog opens, without touching mPickerRunning. The JS side gets the element through the notification fired just above. Reads nsIDocShell::GetFileInputInterceptionEnabled, which is in the docshell group (not checked here). Adds #include nsIDocShell.h. The comment is in Italian and dates a fix to 2026-08-25. V

## dom/html/HTMLInputElement.cpp: HTMLInputElement::InitFilePicker

Hook placed after the notification that InitFilePicker fires for the JS side, and before
the `!oldFiles.IsEmpty()` branch.

Stealthfox: quando l'automazione ha chiesto di intercettare il dialogo, il
dialogo NATIVO non si apre - e la notifica che InitFilePicker invia poco prima e' gia' l'evento che
il lato JS ascolta (juggler/content/PageAgent.js), con l'elemento come
subject, che e' esattamente cio' che gli serve.

Il flag lo scrive juggler/content/main.js su richiesta di
`Page.setInterceptFileChooserDialog`. Fino al 2026-08-25 nessuno lo
LEGGEVA: era memorizzato e basta (il commento in nsDocShell.cpp lo diceva,
"storage only"; oggi la nota e' in docshell/base/nsIDocShell.idl), quindi `expect_file_chooser()` restava appeso mentre una
finestra "Apri file" di Windows si apriva davvero e rubava il focus.

Si esce PRIMA di entrambi i rami che aprono il picker, e senza toccare
mPickerRunning/SetOpenState: nessun picker sta girando, e dichiarare il
contrario impedirebbe l'apertura successiva ("Just one nsIFilePicker is
allowed").
