# Versionamento e rilasci

## Regole

* Schema **MAJOR.MINOR.PATCH** ([SemVer](https://semver.org/lang/it/)). Fonte unica: il file `VERSION`.
  * **PATCH**: correzioni di bug, ottimizzazioni, documentazione. Nessun cambiamento nei file di campagna.
  * **MINOR**: nuove funzioni compatibili (nuove opzioni, nuovi motori, nuovi comandi). Le campagne create con versioni
    precedenti della stessa MAJOR continuano a funzionare.
  * **MAJOR**: cambiamenti incompatibili (formato di `campaign.conf`/`state.dat`/`dp.bin`, opzioni rimosse o rinominate).
* Ogni rilascio ha un tag git annotato `vX.Y.Z` e una voce in `CHANGELOG.md`.
* Il formato dei file di campagna ha un proprio numero (`format=` in `campaign.conf`, `PZSCHED n`, `PZDPn`): il programma rifiuta
  con un messaggio chiaro i formati che non conosce, invece di interpretarli male. `campaign.conf` registra anche la versione
  che ha creato la campagna (`created_with`).
* `pzsolve version` stampa la versione e il commit sorgente da cui è stato compilato il binario.

## Dove sta cosa

```
VERSION                versione corrente
CHANGELOG.md           storia delle modifiche
dist/                  binari della versione corrente: pzsolve, pzsolve.exe, VERSION, SHA256SUMS, RELEASE.txt
archive/vX.Y.Z/        versioni precedenti: binario, sorgenti (tar.gz da git archive), RELEASE.txt, SHA256SUMS
tools/release.sh       automatizza il rilascio
```

## Come si rilascia

1. Sviluppa e committa le modifiche.
2. Decidi il nuovo numero secondo le regole sopra e scrivi in `CHANGELOG.md` la sezione `## [X.Y.Z] — data` con le modifiche.
3. Esegui `tools/release.sh X.Y.Z`. Lo script:
   * verifica che l'albero git sia pulito e che il numero sia coerente col `CHANGELOG.md`;
   * aggiorna `VERSION` e committa;
   * ricompila da zero ed esegue `make test` e `tests/e2e.sh` (si ferma al primo errore);
   * **archivia la versione precedente**: sposta `dist/*` in `archive/vOLD/` e aggiunge i sorgenti di quel tag;
   * copia in `dist/` il binario Linux e, se MinGW-w64 è installato, l'exe Windows (`pzsolve.exe`), con `SHA256SUMS` e `RELEASE.txt`;
   * committa `Release vX.Y.Z` e crea il tag annotato `vX.Y.Z`.
4. Pubblica: `git push origin <branch> --follow-tags`.

Senza argomenti (`tools/release.sh`) rilascia la versione già scritta in `VERSION` (così è stato fatto per la 1.0.0).
La variabile d'ambiente opzionale `COMMIT_TRAILERS` aggiunge righe (es. `Co-Authored-By: …`) ai messaggi dei due commit creati dallo script.
`VERIFY_MODE=full` esegue l'end-to-end completo invece di quello rapido.

## Prima di ogni rilascio

`tools/verify.sh` (usato anche da `tools/release.sh`) esegue in sequenza:

0. ricontrollo del database dei puzzle con l'implementazione indipendente (`tools/gen_puzzles.py --check`);
1. compilazione da zero e `make test` (unitari + autotest integrato);
2. `tests/e2e.sh` (campagne reali su puzzle piccoli; `E2E_QUICK=1`, o `verify.sh quick`, ne usa la versione breve);
   poi il percorso CUDA con un driver simulato (autotest, bench, profilo della macchina, `--prio-gpu`, ripresa dopo un errore della GPU), `doctor`, la
   sospensione (processo congelato), più computer (`merge` del canguro e `--part` della ricerca lineare), e l'exe Windows sotto Wine;
3. build con **AddressSanitizer + UndefinedBehaviorSanitizer**: autotest, **tutti i test unitari** (inclusi quelli di robustezza su
   file corrotti, `t_fuzz`) e campagne lineare, BSGS (anche con tabella su file), canguro e canguro con tabella minuscola;
4. build con **ThreadSanitizer**: campagne lineare, BSGS, canguro e interruzione con Ctrl-C;
5. **Valgrind** (memcheck con controllo completo delle perdite) su una campagna lineare, una BSGS e una di canguro, sul percorso
   portabile (Valgrind non conosce AVX-512/ADX, che sono coperti dai punti precedenti).

Il rilascio si ferma al primo errore. Se hai una GPU OpenCL, esegui anche `pzsolve selftest` e `pzsolve bench` e
controlla che il self-test della GPU sia superato.

## Compatibilità dei file tra versioni

| Da → a | Campagne esistenti |
|--------|--------------------|
| 1.0.x → 1.0.y / 1.y | riprendono senza modifiche (verificato: campagne lineari e canguro create dalla 1.0.0 riprendono con la 1.1.0) |
| 1.x → 2.0 | verrà fornito (se serve) uno strumento di migrazione; in caso contrario il programma lo segnala all'avvio |

Se vuoi continuare una campagna con una versione specifica, usa il binario in `archive/vX.Y.Z/`.
