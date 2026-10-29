# Il motore come serie di patch

Questo repository (feder-cr/firefox_antidetect_patch, ramo `main`; in locale `C:/ff/engine`)
è il nostro Firefox scritto come **differenza da un tag di Mozilla**: una cartella
per funzione, applicate nell'ordine di `SERIES`. Il tag e il ramo sorgente sono in
`UPSTREAM.json`.

## Chi è la fonte

Su GitHub c'è solo questa serie: nessun ramo contiene l'albero di Firefox. Il codice si
modifica **nel sorgente locale**, `C:/ff/source`, sul ramo `source_branch` di
`UPSTREAM.json` (oggi `work/157`, che esiste solo in locale). Se non c'è, o è vecchio,
lo si ricrea dalla serie: `python tools/engine.py apply --head work/157 --force`, che
dà sempre lo stesso commit, perché le date sono quelle del tag. La serie si
**genera** dal ramo con `export` e si **dimostra** identica con `verify`. La CI rifà
lo stesso percorso, `materialize`: tag di Mozilla, poi serie, poi l'albero dichiarato
in `UPSTREAM.json` (`exported_tree`). Si modificano a mano soltanto:

- `FEATURE.md` di ogni cartella (cosa fa, perché, test);
- `tools/layout.py` (in quale cartella va una funzione o un nostro file);
- `tools/inventory.json` (una voce per ogni file Mozilla che tocchiamo);
- `tools/hook-budget.json` (solo verso il basso, vedi sotto);
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

## Le regole del codice

- **Logica nei nostri file, ganci nei file di Mozilla.** In un file di Mozilla si
  scrivono da 1 a 5 righe che chiamano una funzione nostra, marcate
  `// STEALTH-HOOK(<cartella>)`. La logica va in un file nostro (`Stealth*.cpp/.h`
  accanto al gancio), registrato in `layout.OWN_FILES`. Ogni conflitto del passaggio
  151 → 157 (53 file) era logica nostra dentro file di Mozilla.
- **Il tetto per file scende soltanto.** `tools/hook-budget.json` dice quante righe
  nostre porta ogni file di Mozilla; `check` fallisce se una cresce. Dopo aver spostato
  logica: `shrink-budget`. Un tetto non si alza mai: si sposta la logica.
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
- Release e push di `stealth/157` solo dopo averne parlato con Federico.

## Lavori grandi: struttura a sub-agenti

Per spostare logica su molti file: Haiku 5.5 mappa un file o una funzione a testa,
in sola lettura, con riga e file per ogni affermazione. Sonnet 5.5 verifica ogni
mappa contro il sorgente. Chi integra scrive il codice. Un Sonnet separato rivede il
diff. Opus decide solo i casi contesi.

## Dove siamo

1. Fatto: la serie riproduce `stealth/157` identica, ricostruita e controllata in CI.
2. In corso: spostare la logica nei ganci, su un ramo sorgente dedicato, una funzione
   alla volta, dai file con più righe nostre (`hook-budget.json`).
3. Poi: la build della release parte dalla serie applicata al tag, non dal ramo.
4. Nuova versione di Firefox: solo dal tag della release stabile ufficiale, mai da
   una beta. In un clone che ha i due tag: `drift --to <nuovo>` dice quali nostri
   file Mozilla sono cambiati, `port --to <nuovo>` fa il ramo `port/<nuovo>` con una
   fusione a tre vie per cartella e l'elenco di tutti i conflitti. Si risolvono i
   marcatori su quel ramo, si cambia `mozilla_tag` e si esporta.
