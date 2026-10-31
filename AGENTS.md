# Il motore come serie di patch

Questo repository (feder-cr/firefox_antidetect_patch, ramo `main`; in locale `C:/ff/engine`)
è il nostro Firefox scritto come **differenza da un tag di Mozilla**: una cartella
per funzione, applicate nell'ordine di `SERIES`. Il tag e il ramo sorgente sono in
`UPSTREAM.json`.

## Chi è la fonte

Su GitHub c'è solo questa serie: nessun ramo contiene l'albero di Firefox. Il codice si
modifica **nel sorgente locale**, `C:/ff/source`, sul ramo `source_branch` di
`UPSTREAM.json` (oggi `port/159-from-d28c866`, che esiste solo in locale; serve un
clone che abbia il tag `mozilla_tag`). Se non c'è, o è vecchio, lo si ricrea dalla
serie: `python tools/engine.py apply --head port/159-from-d28c866 --force`, che dà
sempre lo stesso commit, perché le date sono quelle del tag. La serie si
**genera** dal ramo con `export` e si **dimostra** identica con `verify`. La CI rifà
lo stesso percorso, `materialize`: tag di Mozilla, poi serie, poi l'albero dichiarato
in `UPSTREAM.json` (`exported_tree`). Si modificano a mano soltanto:

- `FEATURE.md` di ogni cartella (cosa fa, perché, test);
- `tools/layout.py` (in quale cartella va una funzione o un nostro file);
- `tools/inventory.json` (una voce per ogni file Mozilla che tocchiamo);
- `tools/hook-budget.json` e `tools/removed-budget.json` (solo verso il basso, vedi sotto);
- `AGENTS.md`, `tools/`, `.github/`.

`hooks.patch`, `src/`, `MANIFEST.json`, `SERIES`, `exported_from`/`exported_tree` e i
workflow alla radice (`.github/workflows/`, copia di `platform/ci/src/.github/workflows/`
tranne `series.yml`) li scrive l'export: **non si modificano a mano**. Un workflow si
cambia nel sorgente, come ogni altro file.

## Comandi

```
python tools/engine.py status   # il sorgente si è spostato dall'ultimo export?
python tools/engine.py apply --head <ramo>   # nel sorgente: il tag + un commit per cartella (la pila a strati)
python tools/engine.py export   # riscrive la serie dal sorgente
python tools/engine.py verify   # la ricostruisce sul tag e la confronta: "identical" o rosso
python tools/engine.py check    # regole della serie, senza Firefox (gira anche in CI)
python tools/engine.py shrink-budget   # abbassa i tetti dopo aver spostato logica
python tools/engine.py drift --to <tag>  # quali nostri file Mozilla ha cambiato fino al tag nuovo
python tools/engine.py port --to <tag>   # tutte le cartelle a tre vie sul tag nuovo, tutti i conflitti in una corsa
python -m pytest -q tools/tests
```

## Il flusso di ogni modifica

1. In `C:/ff/source`, sul ramo sorgente: la modifica, con il suo test, e il commit.
2. Qui: `export`, poi `verify` (deve dire *identical*), poi `check`.
3. Se `export` si ferma su un file Mozilla nuovo: aggiungi la sua voce all'inventario
   e la funzione a `layout.FEATURES`; se si ferma su un nostro file nuovo, aggiungi il
   percorso a `layout.OWN_FILES`. Se `check` chiede un tetto per un file nuovo: un
   gancio nuovo è al massimo 5 righe.
4. Commit qui con lo stesso titolo del commit nel sorgente, poi push di entrambi.

## Le tre regole del prodotto (Federico, 31/10/2026)

1. **Niente dall'host.** Ogni valore che una pagina può leggere viene dall'identità
   dichiarata o dal codice di Firefox, mai dalla macchina o dal sistema su cui gira il
   browser. Un valore che nasce dall'host si toglie dove nasce (dichiarato, o calcolato
   con lo stesso codice su ogni sistema), non si corregge a valle.
2. **Stesso seed, stessi byte su ogni macchina.** La stessa identità su host diversi
   (Windows, Linux x86_64, Linux arm64) dà lo stesso risultato: testo, canvas, WebGL,
   audio, media.
3. **Il riferimento è Windows retail, e si va solo verso di lui.** Il risultato è uguale,
   o coerente, con Firefox ufficiale su Windows. Linux si porta verso Windows, mai il
   contrario: uniformare gli host su qualcosa che nessun Windows vero produce (per
   esempio FreeType anche su Windows) è vietato.

Le misura il gate host del wrapper (`scripts/ci_host_gate.py`): ogni differenza fra host
è una fuga, ogni differenza dal retail lo è anche; le sole eccezioni sono rumore, ingressi
di rete e difetti noti con il lavoro che li chiude.

## Le regole del codice

- **Logica nei nostri file, ganci nei file di Mozilla.** In un file di Mozilla si
  scrivono da 1 a 5 righe che chiamano una funzione nostra, marcate
  `// STEALTH-HOOK(<cartella>)`. La logica va in un file nostro (`Stealth*.cpp/.h`
  accanto al gancio), registrato in `layout.OWN_FILES`. Ogni conflitto del passaggio
  151 → 157 (53 file) era logica nostra dentro file di Mozilla.
- **Dove si mette il gancio.** Git fonde senza conflitto se fra la nostra modifica e
  quella di Mozilla resta anche una sola riga invariata. Quindi mai dentro una lista
  di Mozilla né in fondo a un blocco, perché è lì che Mozilla aggiunge a ogni release.
  Nei `moz.build` la voce va dopo la `]` di chiusura:
  `VAR += ["StealthX.cpp"]  # STEALTH-HOOK(<cartella>)`, una riga per cartella. Nel
  codice, meglio una chiamata in testa a una funzione che Mozilla lascia com'è che una
  condizione dentro un `if` che Mozilla riscrive. La prova è
  `port --to <tag nuovo>`: ogni conflitto è un gancio da spostare o una riga che
  Mozilla riscrive davvero.
- **Niente valori nostri negli spazi numerati di Mozilla** (bit di flag, valori di
  enum, costanti IDL). Mozilla li assegna alla release dopo e la fusione non lo segnala:
  in 159 il bit 25 di `nsIWebBrowserChrome`, che era il nostro flag, diventa
  `CHROME_MEDIA_PIP`. Le nostre costanti stanno nei nostri file, su valori che
  Mozilla lascia liberi, con un test che lo controlla
  (`juggler/tests/explicit_size_flag_is_free.test.mjs`).
- **Prima di ogni build di prova**, nel sorgente: `python build/stealth_check_sources.py`
  e `python build/stealth_check_build_files.py`. In circa mezzo minuto trovano ciò che
  la CI trova dopo un'ora:
  - un nome `mozilla::` che non sta in quel namespace;
  - una macro degli header di Windows;
  - un livello di log di nICEr fuori da `namespace mozilla`;
  - una lista di build non valida;
  - un nostro sorgente che nessuno compila.
- **Il tetto per file scende soltanto.** `tools/hook-budget.json` dice quante righe
  nostre porta ogni file di Mozilla; `check` fallisce se una cresce. Dopo aver spostato
  logica: `shrink-budget`. Un tetto non si alza mai: si sposta la logica.
- **Le righe di Mozilla restano come le ha scritte Mozilla.** `tools/removed-budget.json`
  dice quante righe di Mozilla togliamo o riscriviamo in ogni file (un file assente: 0).
  Una riga tolta va in conflitto al port come una aggiunta, e se la sua logica finisce
  copiata in un nostro file smette di seguire Mozilla. Se un blocco di Mozilla non deve
  girare o va esteso, il blocco resta intatto e il gancio gli sta accanto (esempio:
  il caricamento delle mappe dei caratteri in `InitializeFamily`). Stesso tetto, solo verso
  il basso, abbassato da `shrink-budget`.
- **Un test di comportamento per ogni gancio**: si imposta la pref e si controlla il
  valore che la pagina vede. Il nome del test va in `FEATURE.md` (riga `Test:`).
- **Ogni falsificazione passa da una pref** `zoom.stealth.*` / `stealthfox.*`; non si
  rinominano: sono il contratto con il core.
- **Mai i nomi dei sistemi studiati** (controlli dei siti, servizi di impronta, siti di
  test) né di browser concorrenti: né nel codice, né nei commenti, né nei commit. Si
  descrive il controllo.
- `stillborn/` è ciò che spegniamo alla nascita (aggiornamenti, telemetria, chiavi):
  le richieste che un Firefox vero fa restano, si spegne solo ciò che cambierebbe il
  browser installato.

## Commit e push

- Identità: `feder-cr <85809106+feder-cr@users.noreply.github.com>`, già impostata nel
  repo. Mai un'email personale nei commit.
- Prima di ogni push: `pytest`, `check`, `verify`; poi `git log -p origin/main..`
  cercando email e nomi dei sistemi studiati. Questi controlli sono locali e restano
  fuori da GitHub.
- Push: `git push origin master:main`. La CI (`.github/workflows/series.yml`) rifà
  le regole e la ricostruzione sul tag.
- Una build di prova: `gh workflow run release.yml --ref <ramo> -f kind=candidate`
  (compila tutte e tre le gambe, LTO+PGO, con la cache; non pubblica nulla).
- **La cache di compilazione appartiene a un ramo.** GitHub dà a una build solo le
  cache del suo ramo e quelle di `main`. Un ramo nuovo legge la cache di `main`; un
  ramo che non parte da `main` compila a freddo (ore). Le build di prova si lanciano da
  `main` o da rami nati da `main`, mai da un ramo isolato.
- Release e push del motore solo dopo averne parlato con Federico.

## Lavori grandi: struttura a sub-agenti

Per spostare logica su molti file: Haiku 5.5 mappa un file o una funzione a testa,
in sola lettura, con riga e file per ogni affermazione. Sonnet 5.5 verifica ogni
mappa contro il sorgente. Chi integra scrive il codice. Un Sonnet separato rivede il
diff. Opus decide solo i casi contesi.

## Dove siamo

1. Fatto: la serie riproduce l'albero dichiarato in `UPSTREAM.json`, ricostruita e
   controllata in CI. Oggi è Firefox 159.0, la release firefox-54.
2. Fatto: la build della release parte dalla serie applicata al tag (`materialize`).
3. In corso: spostare la logica nei ganci, su un ramo sorgente dedicato, una funzione
   alla volta, dai file con più righe nostre (`hook-budget.json`).
4. Nuova versione di Firefox: solo dal tag della release stabile ufficiale, mai da
   una beta. In un clone che ha i due tag: `drift --to <nuovo>` dice quali nostri
   file Mozilla sono cambiati, `port --to <nuovo>` fa il ramo `port/<nuovo>` con una
   fusione a tre vie per cartella e l'elenco di tutti i conflitti. Si risolvono i
   marcatori su quel ramo, si cambia `mozilla_tag` e si esporta. Fatto così il
   passaggio 157 → 159 (firefox-54).
