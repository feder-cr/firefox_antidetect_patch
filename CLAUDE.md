@AGENTS.md

# Questo albero non e' mozilla-central: e' il nostro Firefox patchato

Ramo `stealth/151`. Le istruzioni di Mozilla qui sopra valgono; queste vengono
prima quando si contraddicono.

## Sei nell'unico posto dove il sorgente si modifica

`C:/ff/source` (da WSL `/mnt/c/ff/source`). Non cercarne un altro e non
crearne: **`~/ff-build/firefox-150` e' uno specchio di build**, esiste solo
perche' Firefox si compila nativamente su ciascun sistema, e un hook di
pre-commit li' rifiuta i commit. Si allinea con `git fetch win stealth/151`.
macOS non ha checkout locale, lo costruisce la CI dalla stessa storia.

Un solo `.mozconfig`, alla radice, identico per le due macchine - e decide
anche cosa costruisce la CI, che non ne ha uno suo e ci appende solo le opzioni
per target.

Verifica: `python scripts/check_tree_parity.py` dal workbench.

## Compilare

`./mach build`. E' incrementale. **Mai `mach clobber`**, mai cancellare
l'objdir: 15-60 minuti contro 3-4 ore, e il clobber quasi mai risolve qualcosa
che non risolva un `mach build`.

**Il costo dipende da COSA tocchi, non da quanto scrivi**, e sono ordini di
grandezza diversi: un `.cpp` da solo circa 2 minuti; `dom/base/Element.h`, che
e' incluso quasi ovunque, 38 minuti per tre righe. Quando la stessa modifica si
puo' fare senza toccare un header molto incluso, quella scelta vale mezz'ora e
va fatta scrivendo la patch, non dopo. Toccare `.mozconfig` forza un
reconfigure.

La cache di compilazione e' gia' dichiarata nel `.mozconfig` (`sccache`, anche
per Rust) e si usa da sola: non c'e' niente da lanciare prima. Si controlla con
`sccache --show-stats`. Il primo giro dopo un reconfigure e' piu' LENTO con la
cache che senza, perche' deve popolarsi.

## Le due regole che rompono il prodotto se ignorate

1. **Ogni spoof passa da una pref** `zoom.stealth.*` o `stealthfox.*` (o
   `STEALTHFOX_*` per il contratto binario/Juggler). Mai valori cablati in C++
   o in Python, e **mai rinominare quelle pref**: sono il contratto fra il
   binario e il wrapper.
2. **Sembrare reale, non "non perdere niente".** Un segnale soppresso, bloccato
   o vuoto e' esso stesso un indizio e vale come fallimento. Prima di scrivere
   "i browser veri fanno cosi'", provalo su un Firefox retail della stessa
   major che dichiariamo.

## Dove sta scritto il resto

I documenti NON sono in questo albero: stanno nel workbench, in
`c:/src/firefox-stealth/docs/firefox-stealth-architecture/`. Si entra da
`INDEX.md`, che dice quale file risponde a quale domanda. Le patch e il loro
perche' stanno in `20-our-patches.md`, i bug aperti in `70-known-bugs.md`.

**Ogni modifica al sorgente aggiorna quei documenti nello stesso lavoro.** Le
regole di comportamento complete stanno in `c:/src/firefox-stealth/CLAUDE.md`.
