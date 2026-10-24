# Architettura

## 1. Moduli (`src/`)

| File | Ruolo |
|------|-------|
| `common.h`, `util.c` | tipi, log (terminale + `log.txt`), formattazione, file atomici, memoria con controllo errori |
| `platform.h/.c` | **unico punto dipendente dal sistema operativo** (POSIX e Windows): file binari con offset a 64 bit, rinomina atomica, blocco della campagna, memoria grande (mmap/THP, VirtualAlloc/pagine grandi), orologi, entropia, console (sequenze ANSI, larghezza), Ctrl-C / SIGTERM / SIGHUP / chiusura della finestra, librerie dinamiche. Il resto del programma è C11 + pthread; qualche `#ifdef _WIN32` residuo c'è solo dove il codice è comunque specifico (`cl_api.c`, `rt_cuda.c`, `sysinfo.c`, `doctor.c`, `util.c`, `common.h`) |
| `../lib/pzcore/` | **il nucleo crittografico riutilizzabile** (README proprio): `u256`, `fe`, `ec`, `hash`, `address` (Base58Check, bech32, WIF), `hash160_batch`, `perm`, `cpu` e le intestazioni a corsie `fev.h` / `fev52.h`; compilato in `build/libpzcore.a` senza nulla di `src/`. Le righe seguenti per questi file si riferiscono a `lib/pzcore/include/pzcore/` e `lib/pzcore/src/` |
| `u256.h/.c` | interi a 256 bit, ordine del gruppo `n` |
| `fe.h` | campo `GF(p)` (tutto `static inline`, così ogni unità di compilazione ottiene il codice per il *proprio* set di istruzioni; moltiplicazione e addizione/sottrazione in assembly MULX/ADX con fallback C) |
| `fev.h` | campo `GF(p)` **a corsie SIMD** (9 limbi da 29 bit, `vpmuludq`): una corsia = un elemento indipendente; limiti dei limbi documentati nell'intestazione |
| `fev52.h` | la stessa interfaccia con **AVX-512 IFMA** (5 limbi da 52 bit, `vpmadd52luq/huq`); `fev_chain.inc` contiene le parti comuni alle due (catena di inversione, broadcast) |
| `ec.h/.c` | curva secp256k1: somme Jacobiane/affini, moltiplicazione `k·G` con tabella a finestre di 4 bit, (de)serializzazione delle chiavi pubbliche |
| `hash.h/.c`, `hash_tables.h` | SHA-256, RIPEMD-160, hash160, Base58Check, WIF (riferimento scalare) |
| `hash_simd.inc` | SHA-256 + RIPEMD-160 multi-lane (vector extensions GCC), incluso una volta per ISA |
| `engine_impl.inc` + `eng_base.c` / `eng_avx2.c` / `eng_avx512.c` | motore CPU della ricerca lineare con aritmetica scalare, istanziato per ISA con flag diversi (`Makefile`) |
| `engine_vec_impl.inc` + `eng_avx2v.c` / `eng_avx512v.c` / `eng_avx512i.c` | motore CPU con aritmetica della curva su corsie SIMD (N finestre indipendenti per round); `eng_avx512i.c` lo istanzia con `fev52.h` |
| `cpu_common.c` | tabelle condivise (`i·G`, `cpu_gmultiples` per tabelle di ogni taglia), registro dei motori, rilevamento ISA, verifica rapida di ogni motore (chiave nota), autotune |
| `perm.h/.c` | permutazione di Feistel con round SHA-256 (ordine di visita) |
| `jobq.h/.c` | scheduler a intervalli (richieste di lavoro, completamento, checkpoint, ripresa) |
| `campaign.h/.c` | configurazione immutabile della campagna, generazione del seed |
| `puzzles.h/.c`, `puzzles_data.c` | database dei 160 puzzle (generato da `data/puzzles.csv`) più gli indirizzi dei 161–256 svuotati (`data/puzzles_withdrawn.csv`) e sua verifica |
| `campaigns.h/.c` | blocco della campagna (via `platform.h`), comando `list` |
| `eta.h/.c`, `ui.h/.c` | modelli di stima del tempo / probabilità e stimatore della velocità (`rate_est`); rendering del pannello e della riga di stato (puro formato, nessun I/O) |
| `bench_tg.h/.c`, `profile.h/.c`, `doctor.h/.c` | confronto CPU+GPU insieme (`bench --together` e taratura della prima esecuzione), profilo della macchina e velocità a regime ricordata, comando `doctor` |
| `bsgs.h`, `bsgs_priv.h`, `bsgs.c`, `bsgs_isa.inc` (+ `bsgs_base.c`, `bsgs_x86.c`) | BSGS simmetrico: tabella hash dei baby step, passi giganti a gruppi, salvataggio/lettura |
| `bsgs_vec_impl.inc` + `bsgs_avx2v.c` / `bsgs_avx512v.c` / `bsgs_avx512i.c` | le stesse finestre di 1024 punti, 4 o 8 per istruzione (`fev.h`/`fev52.h`); verificate contro quelle scalari prima dell'uso |
| `kangaroo_priv.h`, `kangaroo.c`, `kang_isa.inc`, `kang_base.c`, `kang_x86.c` | canguro: parametri, tabella DP, risoluzione, round scalare dei camminatori, confronto delle varianti (`kang_round_check`) e scelta della più veloce |
| `kang_vec_impl.inc` + `kang_avx2v.c` / `kang_avx512v.c` / `kang_avx512i.c` / `kang_avx512i2.c` | round del canguro a corsie SIMD, identico bit per bit a quello scalare; stato dei camminatori in forma vettoriale tra un round e l'altro |
| `run.h`, `run_common.c` | contesto di esecuzione, chiave trovata, segnali, caricamento dello scheduler, reporter (pannello, file di report) |
| `run_linear.c`, `run_kangaroo.c`, `run_bsgs.c` | orchestrazione delle tre modalità, autotest e benchmark del canguro |
| `gpu.h`, `gpu.c` | GPU: elenco e scelta dei dispositivi, self-test, taratura, worker della ricerca lineare e del canguro. Scritto contro `gpu_rt.h`, quindi identico per CUDA e OpenCL |
| `gpu_rt.h`, `rt.c`, `rt_priv.h` | interfaccia runtime GPU minima (contesto, moduli, kernel, buffer, lancio) e lista combinata dei dispositivi (una scheda NVIDIA vista da entrambi i runtime è riconosciuta per indirizzo PCI e usata una volta) |
| `rt_cuda.c` | implementazione CUDA: *driver API* caricata a runtime (`nvcuda.dll` / `libcuda.so.1`), moduli PTX incorporati, contesto con attesa non attiva |
| `rt_cl.c`, `cl_api.h/.c` | implementazione OpenCL: loader dinamico, kernel compilati dai sorgenti incorporati |
| `kernels/*.cl`, `kernels/pz_cuda.h`, `kernels/ptx/*.ptx` | **un solo sorgente dei kernel**: OpenCL C, compilato dal driver OpenCL oppure, tramite `pz_cuda.h`, da NVRTC in PTX (`tools/gen_ptx.py`, file committati) |
| `sysinfo.h/.c` | CPU/RAM/cache, affinità dei thread |
| `selftest.c`, `main.c` | autotest, benchmark, interfaccia a riga di comando |

## 2. Flusso di una campagna lineare

```
main ─ cmd_hunt ─ crea/carica campaign.conf ─ run_linear
                        │ sched_new (perm + intervalli) ─ sched_load (state.dat / .bak)
                        ├─ thread CPU i:  loop { sched_claim → engine.scan → sched_complete }
                        ├─ thread GPU j:  loop { sched_claim → kernel pz_search → hit → sched_complete }
                        └─ thread principale: reporter (1 Hz) → terminale, report.txt, status.json, stats.csv,
                                              checkpoint ogni 60 s
Ogni hit (CPU o GPU) → run_submit_key: ricalcola k·G, confronta hash160/pubkey → FOUND.txt → stop globale
```

Lo scheduler distribuisce porzioni contigue di un blocco (≈ 3 s di lavoro per le CPU, ≈ 0,35 s per lancio sulle GPU,
regolate dalla velocità misurata). Un worker interrotto restituisce il prefisso completato; il resto rimane un "buco" nel
checkpoint e viene ripreso per primo alla sessione successiva.

## 2b. Flusso del BSGS

```
run_bsgs ─ bsgs_params_init (c, T, S, A, Ng) ─ run_load_sched (giganti 1..Ng, stesso scheduler del lineare)
         ├─ thread "driver": tabella (bsgs_table_load da --table, oppure bsgs_table_build con N thread e salvataggio)
         │                   → N thread di ricerca: loop { sched_claim → bsgs_scan → sched_complete }
         └─ thread principale: reporter (fase 1/2 durante la tabella, poi giganti)
bsgs_scan: 1, 4 o 8 gruppi di 1024 giganti (inversione batch, a corsie SIMD se disponibili) → ascisse → prefetch → sondaggio della tabella → candidato → verifica k·G = P → run_submit_key
```

## 3. Flusso del canguro

```
run_kangaroo ─ (GPU: apre i dispositivi, taratura di NK, self-test) ─ kang_create (intervallo, base, Q', zone)
             ─ kang_configure (K, dp_bits, tabella, salti da seed) ─ kang_load_log (dp.bin)
             ─ thread CPU: kang_cpu_worker_main { round (variante più veloce tra scalare e a corsie, verificata) → DP → tabella }
             ─ thread GPU: gpu_kang_thread { kernel pz_kang_step (NK camminatori per thread, fino a 64 round per lancio, storia dei salti in
                                              un buffer di 16 bit per passo; d e i DP si ricostruiscono rigiocandola) → DP/riavvii → tabella }
             ─ reporter: probabilità di successo, DP in tabella, riavvii
kang_insert_dps (sotto mutex): inserisce o confronta con la voce esistente → solve_pair → run_submit_key
```

## 4. Formati su disco

* `campaign.conf`: testo `chiave=valore` (`format=1`; dalla 1.1.0 anche `bsgs_entries`, opzionale: le campagne 1.0.x si leggono ancora); `format` cambia solo con modifiche incompatibili.
* `state.dat`: testo; intestazione `PZSCHED 1`, `cbits`, `v_limit`, `keys_done`, righe `iv inizio fine` (hex), riga finale
  `crc=` (primi 8 byte di SHA-256 del contenuto). Scrittura atomica (`.tmp` → fsync → rename); il file precedente, se valido,
  diventa `state.dat.bak`.
* `dp.bin`: `"PZDP2\n"` + impronta SHA-256 (base, W, `"PZDPLOG2"`, pubkey) + record da 32 byte `{tagk, d[3]}` little-endian
  (formato 2 dalla 1.6.0, base = inizio − W/32; un file `"PZDP1"` della stessa campagna, base al centro, viene convertito
  alla ripresa e conservato come `dp.bin.v1`)
  (`tagk` = ascissa a 62 bit ∥ tipo a 2 bit; `d` = esponente con segno a 192 bit). Dalla 1.7.8 le campagne nuove (schema 2 del canguro)
  scrivono `"PZDP3\n"` + impronta (inizio, W, `"PZDPLOG3"`, pubkey) e `tagk` = ascissa a 61 bit ∥ tipo a 3 bit (0 domato, 1/2 selvatico su `Q`
  col segno, 3/4 selvatico su `3Q` col segno); lo schema di una campagna esistente si deduce dall'intestazione del suo `dp.bin`.
* Tabella BSGS (`--table`): intestazione di 4 KiB (`PZBSGS01`, SHA-256, `T`, ascissa di `T·G`, somma di controllo) + celle da 8 byte little-endian.
* `session.txt`, `stats.csv`, `status.json`, `report.txt`: vedi [`USO.md`](USO.md).

## 5. Concorrenza e memoria

* Un solo mutex per lo scheduler e uno per la tabella DP: gli accessi sono rari rispetto al lavoro (blocchi di secondi, DP ogni
  `2^dp_bits` passi). Contatori di avanzamento atomici *relaxed*; flag di arresto atomico.
* Ogni thread alloca i propri buffer (nessuna condivisione falsa); i thread sono vincolati ai core con `sched_setaffinity` (Windows: processore "ideale", un suggerimento allo scheduler).
* Le sole scritture condivise a caldo sono i contatori; ThreadSanitizer non segnala nulla sulle campagne CPU, GPU e miste.
* Memoria: ricerca lineare ≈ pochi MiB; canguro: `32 byte × voci` (limite `--mem`) più ≈ 0,4 KiB per camminatore.

## 6. Test e verifica

| Livello | Dove |
|---------|------|
| Vettori da riferimento indipendente (`tools/ref.py`, Python puro) | `tests/t_core.c`, `src/selftest.c` |
| Campo/assembly | `tests/t_fe.c` (due build: C e MULX/ADX); `tests/t_fev.c` (AVX2 e AVX-512: limiti dei limbi, valori estremi, mutazioni verificate) |
| BSGS, ETA, database | `tests/t_bsgs.c`, `t_eta.c`, `t_puzzles.c` |
| Aritmetica dei kernel GPU | `tests/t_gpufe.c`: il campo di `kernels/pz_common.cl` compilato per l'host, variante portabile e variante a catene di riporto PTX (con il flag di riporto emulato secondo l'ISA PTX), contro la CPU su 5 milioni di combinazioni con tutti i casi limite |
| Permutazione, scheduler, motori SIMD | `tests/t_perm.c`, `t_jobq.c`, `t_engine.c` |
| Campagne complete | `tests/e2e.sh` |
| Percorso CUDA senza GPU | `tests/mock_cuda/`: un **finto driver CUDA** (`libcuda.so.1` / `nvcuda.dll`) con le stesse API, che riconosce i moduli PTX incorporati ed esegue gli stessi sorgenti dei kernel sulla CPU, controllando ogni chiamata (puntatori, copie fuori dai buffer, forma dei lanci, argomenti). Verifica tutto il codice host CUDA; la compilazione PTX→GPU resta al driver vero ed è coperta dal self-test all'avvio |
| Windows | `make windows` / `make wintest`: exe statico MinGW-w64, test unitari, autotest e campagne eseguiti sotto Wine (`tools/verify.sh`) |
| Sanitizer / Valgrind | `make asan`, `make tsan`, `valgrind` (vedi `docs/it/VERSIONAMENTO.md`, sezione "prima di un rilascio") |
| Parametri del canguro | `tools/ksim.c` (modello astratto) e `pzsolve kstat` (curva reale) |

## 7. Roadmap

* Kernel OpenCL con limbi a 64 bit per AMD (richiedono hardware per essere tarati). Fatti: kernel CUDA in PTX con catene di riporto, aritmetica a corsie con AVX-512 IFMA, BSGS e canguro sulle corsie SIMD.
* SHA-NI per SHA-256 su CPU che lo supportano.
* Distribuzione multi-macchina con un coordinatore. Oggi: ricerca lineare divisa a quote fisse (`--part`, `split`), canguro con `--node` e `merge`; manca solo il bilanciamento dinamico fra macchine.
* Supporto indirizzi non compressi.
