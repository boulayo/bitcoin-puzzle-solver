# Guida all'uso

## 1. Comandi

| Comando | Cosa fa |
|---------|---------|
| `pzsolve puzzles [--open\|--solved] [--pubkey]` | elenca i puzzle 1-160 del database integrato (stato, chiave pubblica nota, indirizzo, sforzo medio); i puzzle 161-256, svuotati nel 2017, si raggiungono con `puzzle N` e `hunt -p N` |
| `pzsolve puzzle N [--rate R]` | dati del puzzle N (indirizzo, chiave pubblica, stato, intervallo) e **stime di tempo** (misura la velocità della CPU di questa macchina, oppure usa `--rate` in chiavi/s, per esempio `800M` o `1.5G`) |
| `pzsolve hunt …`    | avvia o riprende una campagna |
| `pzsolve list [-d DIR]` | tutte le campagne della cartella: stato, avanzamento, velocità, tempo residuo |
| `pzsolve info (-c NOME \| -p N)` | stampa il report di una campagna (la riga `State` è quella che mostra `list`: `FOUND` se esiste `FOUND.txt`, `RUNNING` solo mentre un processo tiene il blocco della campagna, quindi una campagna uccisa risulta `STOPPED`) |
| `pzsolve where (-c NOME \| -p N) --key HEX` | dice in che punto dell'ordine di visita di una campagna lineare o BSGS arriva una chiave, e se è già stata scandita |
| `pzsolve split V1 V2 …` | divide le quote di `--part` fra più computer in proporzione alle loro velocità (per esempio `split 800M 120M`; vedi sezione 8.2) |
| `pzsolve merge (-c NOME \| -p N) -f FILE_O_CARTELLA … [-d DIR] [--mem GiB]` | unisce i punti distinti di altri computer a una campagna canguro (vedi sezione 8.1) |
| `pzsolve doctor [-d DIR] [--full]` | controlla programma, sistema, CPU, GPU, taratura e profilo salvati, campagne, disco, orologio e variabili `PZ_*`, e dice cosa non va; da incollare quando si chiede aiuto |
| `pzsolve devices`   | CPU, set di istruzioni, RAM e GPU (CUDA e OpenCL) rilevate |
| `pzsolve selftest`  | controlli di correttezza integrati (primitive, database, motori SIMD, canguro, BSGS, GPU) |
| `pzsolve bench [--together [--linear] [-t N] [--gpu L] [--prio-gpu P]] [--gpu-only] [--retune]` | velocità di ogni motore CPU (1…N thread), del canguro, del BSGS e delle GPU (con banda e throughput intero); `--together`: sezione 7.9 |
| `pzsolve kstat BITS TRIALS [THREADS [R [DP]]]` | statistica del costo del canguro su intervalli di 2^BITS chiavi (R salti invece di 256 e dp-bits fissi, senza riavvii: per studiare i cicli) |
| `pzsolve version`   | versione e commit di compilazione |

`pzsolve help COMANDO` (o `pzsolve COMANDO --help`) mostra le opzioni di un comando. Il programma scrive i suoi messaggi in inglese: in questa
guida sono citati così come appaiono, e il testo italiano li spiega. Le righe lunghe vanno a capo alla larghezza del terminale (al massimo 128 colonne).

## 2. Avvio rapido: tutti i puzzle sono già dentro

`hunt -p N` crea la campagna `pN` nella cartella `./campaigns`; rilanciare lo stesso comando la **riprende** (stesso seed,
nessun lavoro rifatto). Per i puzzle con chiave pubblica nota (140, 145, 150, 155, 160 e tutti quelli già risolti) la chiave
pubblica viene usata da sola e il programma sceglie tra BSGS e canguro (sezione 4).

Il database contiene indirizzo, chiave pubblica (se mai comparsa sulla blockchain) e, per i puzzle risolti, la chiave privata (che
serve come banco di prova: `hunt -p 40` ritrova una chiave nota in pochi secondi, e `puzzle N` di un puzzle risolto dice
`A test run (the key is known): …`, cioè una corsa di prova). Provenienza e verifiche: [`data/README.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/data/README.md).
**Lo stato "risolto/aperto" è quello della data di creazione del database**: prima di lanciare una campagna lunga controlla
sull'indirizzo (esploratore della blockchain) che i fondi ci siano ancora. Il programma avvisa se scegli un puzzle già risolto.

### I puzzle 161-256

La transazione originale del 2015 aveva **256** uscite, una per puzzle (il puzzle `n` riceveva `n` millesimi di BTC). Nel luglio
2017 (blocco 475240) il creatore ha speso con un'unica transazione le uscite 161-256 e ha usato i fondi per aumentare i premi dei
puzzle 1-160. Gli indirizzi 161-256 quindi esistono ma **sono vuoti**. `puzzles` non li elenca; `puzzle 161` ne mostra l'indirizzo e
`hunt -p 161` lo cerca lo stesso (con un avviso): utile solo come esercizio, dato che `2^160` chiavi non si esauriscono. Il puzzle 256 si
ferma a `n − 1` (l'ordine del gruppo), perché le chiavi oltre non esistono. Provenienza e verifiche degli indirizzi: `data/README.md`.

## 3. `hunt`: opzioni

`pzsolve hunt --help` elenca tutte le opzioni con il loro valore predefinito. Quello che richiede più di una riga dell'aiuto:

* `-p N` con `-r S:E` prende il bersaglio del puzzle N e l'intervallo di `-r`. Una chiave fuori dall'intervallo di `-r` non viene segnalata dalla
  ricerca lineare, nemmeno quando l'ultimo blocco la raggiunge (i motori scandiscono corsie intere).
* `-s` usa 64 cifre hex così come sono e fa l'hash di qualsiasi altro testo; senza, o con `auto`, il seed è casuale.
* `--mem` è in GiB (`--mem 0.25` sono 256 MiB). Senza, il canguro prende il 40 % della RAM libera per la tabella dei punti distinti
  (almeno 256 MiB, al massimo 64 GiB) e il BSGS sceglie una tabella che stia nel 70 % della RAM libera.
* Un `--chunk-bits` sotto il predefinito rallenta la CPU e soprattutto le GPU, perché ogni lancio riceve un solo blocco.

Un valore che non si può usare ferma il comando con un messaggio come `--limit: invalid value '100' (expected an integer between 1 and 99)`,
e un'opzione sconosciuta con `unrecognized option '--foo' (see 'pzsolve help hunt')`.

Esempi:

```sh
# CPU + tutte le GPU, ricerca lineare (indirizzo noto, nessuna chiave pubblica)
pzsolve hunt -p 71

# due computer sulla stessa ricerca: stesso seed, ciascuno la sua metà dei blocchi (vedi «Più computer sulla ricerca lineare»)
pzsolve hunt -p 71 -s 5f3a…(64 cifre) --part 0/2
pzsolve hunt -p 71 -s 5f3a…(stesso)   --part 1/2

# sola GPU numero 1 e 2, seed scelto da te per poter ricreare la stessa campagna altrove
pzsolve hunt -p 71 -c p71b --no-cpu --gpu 1,2 -s "la mia frase"

# chiave pubblica nota, BSGS con tabella in RAM salvata su SSD e riusabile
pzsolve hunt -p 60 --mode bsgs --table /ssd/tabella-2e29.bin

# bersaglio personalizzato
pzsolve hunt -r 40000000000:7fffffffff -a <INDIRIZZO> -c zona1
```

## 4. Quale algoritmo? (`--mode auto`)

Termini usati da qui in poi: **W** è l'ampiezza dell'intervallo (2^(n-1) per il puzzle n). Una **chiave** è una candidata della ricerca lineare,
un **passo** un salto del canguro o un passo gigante del BSGS. Un **punto distinto** è un punto del cammino di un canguro che viene memorizzato,
circa uno su 2^dp-bits (`--dp-bits`). Un **work-item** è un thread della GPU; nel canguro muove `NK` camminatori.

| Situazione | Scelta |
|------------|--------|
| solo indirizzo | `linear`: scansione di tutto l'intervallo (CPU e GPU) |
| chiave pubblica nota, intervallo < 2^20 | `linear` (istantaneo) |
| chiave pubblica nota, una tabella BSGS che non costa più del canguro, e uno fra: nessuna GPU utilizzabile (o `--no-gpu`), `--bsgs-entries` dato, il file di `--table` esiste, il lavoro dura pochi secondi | `bsgs`: costo ≈ 1,0·√W, deterministico |
| chiave pubblica nota, altrimenti | `kangaroo`: ≈ 1,06·√W, usa anche le GPU |

La tabella BSGS è quella di `--bsgs-entries` o del file di `--table` esistente; altrimenti quella ottima, o la più grande che sta nel 70 % della
RAM libera (o in `--mem`). Un file di `--table` che non esiste ancora non conta: con `bsgs` viene costruito e salvato.

Il BSGS usa solo CPU e RAM; con una GPU potente il canguro può essere più veloce nonostante il costo maggiore: se sai che è
così, forza `--mode kangaroo`. La modalità resta fissata nella campagna (`campaign.conf`). Senza chiave pubblica vale solo la ricerca lineare:
`-m kangaroo` o `-m bsgs` si fermano con `puzzle 71 has no known public key: only -m linear applies`. Il canguro accetta intervalli da 2^16 a 2^188
chiavi (i puzzle dal 190 in su vengono rifiutati con un messaggio; gli esponenti dei suoi camminatori sono numeri a 192 bit); un intervallo
troppo grande per il BSGS finisce con `: use -m kangaroo`.

La tabella del BSGS occupa circa 5,7 byte per baby step (celle da 4 byte, usate al 70 %); `--bsgs-entries` conta i baby step, non i byte.
Tabelle di altre versioni: [`RELEASING.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RELEASING.md#compatibility-of-the-files-between-versions) (in inglese), tabella di compatibilità.

## 5. La cartella di una campagna

```
campaigns/NOME/
  campaign.conf   parametri immutabili (intervallo, bersaglio, modalità, seed, chunk-bits, baby step)
  lock            blocco (flock) che impedisce due processi sulla stessa campagna
  state.dat       checkpoint della ricerca (insieme di intervalli completati, con checksum; .bak = precedente)
  dp.bin          canguro: log append-only dei punti distinti (per riprendere)
  session.txt     tempo totale e sessioni
  report.txt      report leggibile, riscritto ogni 5 s (la prima volta 2 s dopo l'avvio)
  status.json     stato per programmi esterni (include tempi residui e probabilità), riscritto ogni 5 s
  stats.csv       serie storica (una riga al minuto)
  log.txt         eventi con data e ora (anche l'errore che ha fermato una corsa)
  FOUND.txt       solo a chiave trovata (permessi 0600)
```

* **Fermarsi**: Ctrl-C, `kill` o chiusura del terminale (SIGHUP; sotto `nohup` viene ignorato). Lo stato viene salvato. Un secondo
  Ctrl-C dopo mezzo secondo esce subito (e perde al massimo l'ultimo minuto di lavoro).
* **Riprendere**: lo stesso comando (`hunt -p N`, oppure `hunt -c NOME`). Il seed e i parametri che definiscono la ricerca non
  possono cambiare: se dai `-s`, `-r`, `-a`, `-k`, `-H`, `-m`, `--chunk-bits` o `--bsgs-entries` diversi da quelli della campagna
  esistente il programma **rifiuta** con un errore (non li ignora in silenzio); puoi invece cambiare thread, GPU, ISA, memoria e
  aspetto del pannello. **`pzsolve list`** mostra tutte le campagne e il loro stato.
* **Nome della campagna** (senza `-c`): `pN` per un puzzle intero; per un sotto-intervallo (`-r`) o un bersaglio personalizzato
  `pN-xxxxxxxx` / `custom-xxxxxxxx`, dove `xxxxxxxx` dipende da puzzle, intervallo e bersaglio: **lo stesso comando ritrova sempre
  la stessa campagna**. Per due campagne indipendenti sullo stesso intervallo usa `-c` con nomi diversi (e seed diversi). I nomi
  ammettono lettere, cifre, `-`, `_`, `.` (non all'inizio né alla fine), al massimo 80 caratteri, e non un nome di dispositivo di Windows (`con`, `prn`,
  `aux`, `nul`, `com0`-`com9`, `lpt0`-`lpt9`, anche con un'estensione): la cartella di una campagna si copia fra computer, quindi la regola è la stessa su ogni sistema.
  Una campagna con un bersaglio personalizzato mostra il suo nome dove un puzzle mostra `#N` (`list` mostra `-` nella colonna del puzzle).
* **Codici di uscita di `hunt`**, per gli script: 0 = chiave trovata (anche quando la campagna l'aveva già trovata: vedi `FOUND.txt`);
  1 = la ricerca è finita senza trovare la chiave (tutto l'intervallo, o tutta la `--part`, è stato coperto);
  130 = interrotto con Ctrl-C, `kill` o chiusura del terminale (lo stato è salvato, lo stesso comando riprende);
  2 = errore: opzioni non valide o incoerenti, campagna in uso, file illeggibili, salvataggio finale dello stato non riuscito, o un
  arresto che nessuno ha chiesto (tutte le GPU in errore e CPU spenta).
  Gli altri comandi escono con 0 se riescono e con 2 in caso di errore; `selftest` e `doctor` escono con 1 se un controllo è fallito e
  `merge` con 1 se qualche file non è stato unito (esce con 0 sia quando l'unione ha trovato la chiave, che mostra e salva in
  `FOUND.txt`, sia quando non c'è ancora nessuna collisione).
* Un riavvio perde al massimo il lavoro dei blocchi in corso (pochi secondi per worker); il resto non viene mai rifatto. Nel BSGS la
  tabella viene ricostruita (pochi minuti) a meno che tu non usi `--table`.
* Una campagna che ha già trovato la chiave non riparte: mostra `FOUND.txt`.

## 6. Il pannello di avanzamento

Su un terminale il programma disegna **sul posto** un pannello di una decina di righe, aggiornato una volta al secondo
(le righe di log scorrono sopra, il pannello resta in basso). Un esempio (una ricerca lineare su una macchina di prova a 4 core, mezzo minuto
dopo l'avvio; `pzsolve X.Y.Z` è la versione):

```
pzsolve X.Y.Z | campaign p71 | puzzle #71 | linear | seed id 330eae54f8407a09 | RUNNING
Target        1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU   range of 2^70 keys
Progress      [..............................] 5.435e-11%   (641.71 M / 1.181e+21 keys)
Speed         23.64 M keys/s   (peak 24.13 M, session average 20.34 M)
              CPU x2: 22.69 M  |  GPU[0]: 943.39 k
Time          this session 31.5s | total 31.5s | session no. 1
Time left     50%: 788,333 years | 90%: 1.4 million years | 100%: 1.6 million years
Chance        1h: 7.2e-09% | 24h: 1.7e-07% | 7d: 1.2e-06% | 30d: 5.2e-06% | 365d: 6.3e-05%
              provisional (settles in ~34 s)
              random order: blocks of 2^36 keys
Ctrl-C: stop and save progress; run the same command again to resume.
```

* **Progress** (avanzamento): frazione coperta (lineare, BSGS). Nel canguro la riga è `Success prob.`: la **probabilità stimata di aver già
  trovato la chiave**, con i passi fatti e quelli attesi in media.
* **Time left** (tempo residuo): tempo ancora necessario, a velocità costante, per arrivare alla probabilità 50 % / 90 % / 100 % (nel canguro
  99 % invece di 100 %, che non esiste). Se manca meno di un anno compaiono anche data e ora previste. La velocità usata è pensata per essere
  realistica fin dall'inizio (vedi sotto); nel BSGS resta una media mobile di ≈ 30 s. Oltre 5 volte lo sforzo atteso senza una collisione il
  canguro avvisa una volta (`kangaroo: 5x the expected effort without a collision: check the public key (-k) and the range`: controlla la chiave
  pubblica e l'intervallo) e la riga dice `Time left     unknown (beyond the model)`.
* **Stima provvisoria**. Nei primi secondi la velocità mostrata inganna: una GPU viene contata solo quando finisce il suo primo lancio, i clock
  salgono, e un portatile dopo qualche minuto sotto carico rallenta. Per questo il tempo residuo non compare finché ogni motore non ha prodotto
  qualcosa (la riga dice `Time left     measuring...` e la velocità porta l'annotazione `[starting: not all engines are counted yet]`), e poi parte
  dalla velocità a regime di una corsa precedente della stessa configurazione, se c'è, altrimenti da quella misurata dalla taratura (ALGORITHMS.md
  sezione 11, in inglese). Finché la stima si assesta una riga lo dice: `provisional (from the remembered speed, settles in ~2 min)` (dalla velocità
  ricordata), `provisional (from the tuning run, settles in ~40 s)` (dalla taratura), oppure `provisional (settles in ~34 s)` quando non c'è nulla
  da cui partire. Con `--no-tune` le velocità ricordate non si usano né si aggiornano; `pzsolve doctor` le elenca.
* **Chance** (probabilità): probabilità di successo entro la prossima ora, il prossimo giorno, settimana, mese e anno a questo ritmo,
  *supponendo che la chiave sia nell'intervallo* (formule in [`ALGORITHMS.md`](../en/ALGORITHMS.md) sezione 11, in inglese).
* Nel BSGS, durante la costruzione della tabella, il pannello mostra la fase 1 di 2 e quanti passi giganti seguiranno.
* Quando la chiave viene trovata il pannello finisce con `KEY FOUND`, e un riquadro sotto dà la chiave (`key (hex): 0x…`) e il percorso di
  `FOUND.txt`.

Modalità: `--ui full` (pannello), `--ui line` (una sola riga dinamica), `--ui log` (una riga di riepilogo al minuto nel log,
adatta a file e `nohup`), `--ui auto` (predefinito: pannello su un terminale, altrimenti log). La riga singola è così (il nome della campagna
prende il posto di `#135` per un bersaglio personalizzato; `(provisional)` è l'annotazione di sopra):

```
#135 [....................] 0.0000% 25.55 M steps/s | 50%: 178,231 years 90%: 324,846 years 99%: 459,402 years | P(24h)=1.6e-14% (provisional)
```

e l'ultima riga nel log di una sessione finita dice come è finita, per esempio `final state: #26 FOUND, 18.3350% covered`.
Fuori da un terminale non vengono mai emessi codici di controllo. `NO_COLOR=1` toglie il grassetto. Il file `status.json` contiene gli stessi
numeri per altri programmi.

## 7. GPU: CUDA e OpenCL

### 7.1 Runtime

Il programma usa due runtime, entrambi caricati all'avvio solo se presenti (senza nessuno dei due lavora con la CPU):

* **CUDA** (schede NVIDIA): il *driver* CUDA (`nvcuda.dll` su Windows, `libcuda.so.1` su Linux), che si installa con il normale
  driver grafico NVIDIA (versione **525 o successiva**). **Non serve il toolkit CUDA**: i kernel sono già dentro il programma in forma
  PTX (generata da `tools/gen_ptx.py` con NVRTC dagli stessi sorgenti dei kernel OpenCL) e il driver li compila per la tua scheda.
  Serve una GPU Maxwell o più recente (compute capability ≥ 5.2).
* **OpenCL** (`OpenCL.dll` / `libOpenCL.so.1`): AMD (driver Adrenalin su Windows, ROCm `rocm-opencl-runtime` su Linux), Intel
  (driver grafico / `intel-opencl-icd`); anche il driver NVIDIA include OpenCL. **Solo prove su CPU**: `pocl-opencl-icd`; i dispositivi
  CPU non vengono usati di default (`PZ_OPENCL_ALL=1 pzsolve selftest` oppure `--gpu N`).

### 7.2 Memoria dichiarata e memoria usata

`devices` e l'avvio mostrano la memoria che il driver dichiara. Per una scheda con memoria propria (le NVIDIA, le AMD discrete) è la VRAM; per
una **GPU integrata** (Intel UHD/Iris, le APU AMD) il driver OpenCL dichiara la quota di RAM di sistema che la scheda può indirizzare, di solito
più della metà della RAM (`59.58 GiB, shared system RAM`): non è memoria riservata né allocata. Il programma ne usa solo quanto serve ai suoi
buffer, e a ogni avvio del canguro lo dice (`GPU[0]: 131072 kangaroos (1024 work-items x 128), memory used 72.08 MiB of 1.00 GiB`; su una GPU
integrata, `… MiB of system RAM`). Il canguro ne usa ≈ 40 MiB per il buffer dei punti distinti più 256 byte per canguro (stato e storia dei
salti); il branco è limitato a un sesto della memoria dichiarata. Le schede NVIDIA hanno meno memoria dichiarata ma più velocità: la banda, non la capacità, è il limite.

### 7.3 Scelta dei dispositivi e self-test

`pzsolve devices` elenca tutto con un indice per `--gpu`; una scheda NVIDIA compare due volte (CUDA e OpenCL) e di default viene
usata **una volta sola, tramite CUDA**. Se la via CUDA non supera il self-test il programma prova la stessa scheda tramite OpenCL.
`--gpu-api opencl` (o `PZ_GPU_API=opencl`) forza OpenCL, `--gpu-api cuda` solo CUDA. Ogni messaggio su un dispositivo lo nomina con il suo
indice, `GPU[n]`.

All'avvio ogni dispositivo esegue un **self-test bit-esatto contro la CPU** (aritmetica, hash160, somma di punti, ricerca lineare in
tutte le varianti, passi del canguro con punti distinti e riavvii); chi non lo supera viene escluso con un messaggio e il programma
prosegue con gli altri. I dispositivi partono insieme, un thread ciascuno, e ognuno scrive una riga quando è pronto, nell'ordine dell'elenco,
per esempio `GPU[0]: NAME via CUDA, 20 SM, 4.00 GiB, window 256, self-test passed`. Se le prove si fermano per un errore del driver (non per un
risultato diverso da quello della CPU: quello esclude subito il dispositivo) vengono ripetute, fino a tre tentativi con una breve pausa, perché un
errore passeggero come un reset dello schermo costerebbe altrimenti una scheda a tutta la campagna (il log dice `attempt 1 of 3`). Ogni chiave
candidata viene comunque riverificata sulla CPU.

### 7.4 Prima esecuzione e taratura dei kernel

Il driver compila i kernel per la tua scheda (con CUDA circa 10 s per modulo; con la taratura delle varianti
del kernel lineare circa un minuto, poi le varianti del canguro la prima volta che lo usi) e conserva il risultato nella sua cache:
dalle volte successive l'avvio è immediato. Con più schede la taratura dei dispositivi gira insieme, e le schede dello stesso tipo si misurano una
volta sola (le altre leggono il risultato: `from earlier tuning`). La taratura è memorizzata in `%LOCALAPPDATA%\pzsolve\gpu_tune.txt` su Windows,
`~/.cache/pzsolve/gpu_tune.txt` su Linux, e si ripete quando cambia la versione del programma. La variante predefinita resta, a meno che un'altra
non sia più veloce di oltre il 3 %.

Le varianti (finestra, camminatori per thread, branchi dimensionati sulla L2) sono elencate in [`ALGORITHMS.md`](../en/ALGORITHMS.md) sezione 5
(in inglese). `bench` mostra, per ogni GPU, la variante scelta e la sua velocità. Su un **portatile** CPU e GPU condividono alimentazione e
raffreddamento: `bench --together` (sezione 7.9) trova quanti thread CPU e quali GPU convengono.

### 7.5 `bench` e le GPU

Per ogni GPU il `bench` stampa, oltre alla ricerca lineare e al canguro (con la memoria usata), la banda di copia, il throughput delle istruzioni
intere a 32 bit e la cache L2, e per le NVIDIA i due tetti del canguro che ne derivano: come leggerli è in [`RESEARCH.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RESEARCH.md) sezione 8.4 (in inglese).

`bench --gpu-only` salta le prove della CPU. `bench --retune` ignora la scelta salvata delle varianti dei kernel
(`gpu_tune.txt`) e la misura di nuovo: serve dopo un aggiornamento del driver o per vedere i numeri di tutte le varianti.

### 7.6 Profilo automatico della macchina

La prima volta che lanci `hunt` con una versione nuova, su una macchina con una GPU utilizzabile, il programma misura da solo quanti thread CPU e
quali GPU funzionano meglio (il confronto di `bench --together`, sezione 7.9, circa 2 minuti, per il canguro o per la ricerca lineare secondo la
campagna), salva il risultato e da allora lo usa come predefinito. Non misura nel BSGS, né con `-t`, `--gpu`, `--no-cpu`, `--no-gpu` o `--limit`.
Il profilo è nella cartella della cache, accanto alla taratura dei kernel (`%LOCALAPPDATA%\pzsolve\machine_profile.txt` su Windows,
`~/.cache/pzsolve/machine_profile.txt` su Linux) ed è legato alla versione del programma, al processore, al numero di core, al passo/motore usato e
alle GPU con il loro driver: se cambia una di queste cose si rimisura. Il log dice sempre cosa è stato applicato
(`profile of this machine from 2026-10-22: 11 CPU threads, GPU 0 (-t and --gpu override them; --retune measures again)`). Si cambia idea così:
`-t`/`--gpu`/`--no-cpu`/`--no-gpu` sostituiscono il profilo per quella esecuzione; `--retune` lo rimisura; `--no-tune` (o `PZ_NO_PROFILE=1`) non lo
misura né lo usa. Se nessuna configurazione batte quella predefinita di più del rumore della misura, il profilo salvato è "i valori predefiniti sono
già i migliori" e non si rimisura a ogni avvio. Ctrl-C durante la misura termina subito il programma (la ricerca non parte, non si salva nulla e la
cartella della misura viene rimossa). Senza una cartella di cache scrivibile (né `HOME` né `XDG_CACHE_HOME`, come in un servizio) la misura viene
saltata con un solo avviso, perché non si potrebbe conservare.

### 7.7 Priorità dei thread che servono le schede

Ogni GPU è servita da un thread che lancia i kernel, raccoglie i risultati e rilancia. Quando tutti i core sono occupati dal calcolo, quel thread
può aspettare il suo turno mentre la scheda resta ferma; sui portatili l'effetto si confonde con il limite di potenza condiviso. La taratura
confronta priorità normale e alta con tutti i thread CPU e tiene quella alta solo se è misurabilmente più veloce. L'esito entra nel profilo (`prio=1`) e il log lo dice
(`GPU: the threads that serve the GPUs run at high priority (from the machine profile)`). `--prio-gpu yes|no` la impone o la esclude (e non si
misura); `--prio-gpu auto` è il predefinito. Su Windows è `ABOVE_NORMAL` per i soli thread delle schede (che dormono quasi sempre in attesa del
kernel: non tolgono tempo al calcolo); su Linux è `nice -5` e serve root o `CAP_SYS_NICE`: senza, la misura viene saltata e `doctor` lo dice. Non
è una promessa di velocità: se la causa è la potenza del portatile e non l'attesa del thread, la misura darà ~0 % e la priorità resta normale.

### 7.8 Usare solo una parte della macchina: `--limit X`

Spento di default. `hunt -p 71 --limit 50` usa circa metà della macchina per scaldare meno e consumare meno, accettando di andare più piano. Non
ci sono pause né cicli «lavora e riposa»: il carico resta costante, così la ventola non parte e si ferma a ondate. Si ottiene con meno unità di
calcolo, tutte al massimo:

* **CPU**: il X % dei thread (arrotondato al più vicino, almeno uno). Con `-t` il X % si applica ai thread indicati. Come sempre (sezione 8) i thread
  vanno prima su core fisici diversi: su 8 core e 16 thread, `--limit 50` sono 8 thread su 8 core, `--limit 25` sono 4 thread su 4 core,
  `--limit 75` sono 12 thread.
* **GPU**: il X % dei work-item di ogni lancio (arrotondato a blocchi di 128, almeno uno) sia nella ricerca lineare sia nel canguro: la scheda lavora
  sempre, ma con meno blocchi in corso. Il canguro ha di conseguenza meno canguri. Il log lo scrive
  (`limit 40%: CPU 6 of 16 threads, GPUs at 40% of the work-items`, poi `GPU[0]: limit 40%: 3328 work-items per launch`).
* **BSGS**: solo i thread CPU (anche la costruzione della tabella usa meno thread e dura di più). Le GPU non partecipano comunque.
* La taratura della macchina (circa 2 minuti a pieno carico) e il profilo non vengono usati né aggiornati; la velocità a regime di una corsa limitata
  si ricorda a parte (`pzsolve doctor` la elenca con "limit N%"), così le stime del tempo restano realistiche. `--retune` con `--limit` è rifiutato.
* Il limite non è salvato nella campagna: ogni esecuzione sceglie il suo, e riprendendo senza `--limit` si torna a tutta la macchina. Il pannello
  scrive "limit X%" davanti alle velocità e `report.txt` e `status.json` (`limit_percent`) lo riportano.

Il consumo non scala esattamente come X: con meno core attivi la CPU può alzare la frequenza di quelli rimasti, e una parte del consumo (RAM, scheda
in attesa, ventole) resta. Se vuoi un tetto più deciso, prova valori più bassi e guarda temperature e ventola (HWiNFO, `nvidia-smi`). Su Windows
anche «Stato massimo del processore» nelle opzioni risparmio energia (impostato al 99 % disattiva di solito il turbo) frena la frequenza senza
toccare il programma. Le prove su GPU in questo progetto sono fatte con un driver simulato: l'effetto sui watt di una scheda vera non è stato
misurato.

### 7.9 `bench --together`

Avvia come in una campagna (puzzle 140) i thread CPU e tutte le GPU e misura la velocità totale con vari numeri di thread CPU (tutti, tutti meno
uno per GPU CUDA, tre quarti, uno per core fisico, la metà di questi, nessuno), poi spegne a turno ogni GPU più lenta (restituendo alla CPU il core che le serve). Ogni
configurazione dura 10 s (2 di assestamento e 8 di misura) più una ripetizione finale della prima che stima il rumore. Alla fine indica la
configurazione migliore, per esempio `-t 11 --gpu 0`, se supera quella predefinita di più del rumore; una GPU la cui perdita non si misura (entro
l'1 %, confermato in coppie alternate) viene lasciata fuori, perché costa un core e, su un portatile, energia che i core della CPU e l'altra GPU userebbero. `-t N` prova solo N thread CPU, `--gpu LISTA` limita le GPU, `--linear` misura la ricerca lineare invece del canguro. Il primo
`hunt` di una versione fa lo stesso confronto da solo (sezione 7.6).

### 7.10 Variabili d'ambiente

Il programma le legge una volta sola, all'avvio. Quelle a due stati (come `PZ_NO_CUDA`) valgono con qualunque valore tranne vuoto e `0`; una
variabile impostata alla stringa vuota conta come non impostata. Un valore che non si può usare (un numero che non è un intero decimale nei limiti
indicati qui sotto, una parola che non è fra quelle ammesse) viene segnalato una volta sola, come `NAME=TEXT ignored (...)`, e il programma prosegue
come se la variabile non ci fosse.

| Variabile | Effetto |
|-----------|---------|
| `PZ_GPU_API=cuda\|opencl` | come `--gpu-api` (che ha la precedenza) |
| `PZ_NO_CUDA=1`, `PZ_NO_OPENCL=1` | non caricare quel runtime |
| `PZ_GPU_HALF=N` | fissa la finestra del kernel lineare (salta la taratura; i valori sono in ALGORITHMS.md sezione 5); un numero che il dispositivo non offre viene segnalato per ogni dispositivo |
| `PZ_GPU_NK=N` | fissa la variante del kernel del canguro (salta la taratura): camminatori per thread con i suffissi di ALGORITHMS.md sezione 5, es. `128`, `128b`, `64l` |
| `PZ_GPU_TUNE=0` | nessuna taratura; `=1` la fa anche sui dispositivi che non sono GPU, per i test (ogni altro valore è rifiutato) |
| `PZ_OPENCL_ALL=1` | usa anche i dispositivi OpenCL su CPU (sezione 7.1) |
| `PZ_NO_PROFILE=1` | come `--no-tune` |
| `PZ_NO_LARGE_PAGES=1` | niente pagine grandi (Windows, sezione 10) |
| `PZ_BENCH_SCALE=0.2` | accorcia a un quinto le misure di `bench --together` e del profilo della macchina, per i test (un numero da 0,01 a 100) |
| `PZ_KANG_MIX=0\|1` | sui core con hyper-threading il primo thread può usare il passo vettoriale del canguro e l'altro quello scalare BMI2/ADX, che usa il moltiplicatore intero invece dell'unità vettoriale. Senza la variabile il canguro misura le due soluzioni all'avvio e usa quella mista quando è più veloce almeno del 3% (lo dice una riga del log); `=0` non la usa mai, `=1` sempre. `bench` mostra il guadagno e se è in uso (riga "mixed") |
| `XDG_CACHE_HOME` | cartella della cache (anche su Windows); senza, `%LOCALAPPDATA%` su Windows, poi `HOME` (`~/.cache/pzsolve`) |
| `TMPDIR` | cartella delle campagne di prova delle misure e dei test (Linux e macOS; senza, `/tmp`) |
| `NO_COLOR` | qualunque valore non vuoto: niente grassetto nel pannello |
| `TERM=dumb` | il terminale non sa spostare il cursore: niente pannello (`--ui log`) |

Le variabili per i test e la diagnostica (`PZ_KANG_ROUND`, `PZ_KANG_VERBOSE`, `PZ_BSGS_SCALAR`, `PZ_TEST_*`, `PZ_ASSUME_IFMA`, `PZ_EXPECT_SHANI`
e quelle del driver CUDA simulato) sono descritte in [`ARCHITECTURE.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/ARCHITECTURE.md) sezione 5b (in inglese).

## 8. Consigli prestazionali

* Lancia `pzsolve bench` una volta: confronta i motori e il numero di thread; con una GPU lancia anche `pzsolve bench --together` (sezione 7.9).
  `--isa auto` sceglie il motore più veloce; `bench` li mostra tutti. Ogni motore, prima di essere usato, supera una verifica rapida con una
  chiave nota: un motore che sbagliasse verrebbe escluso.
* I thread di calcolo vanno prima su core fisici diversi, uno per core, e solo oltre il numero dei core sul secondo thread SMT di un core (su
  Windows l'assegnazione è un suggerimento al sistema, che può spostare i thread).
* Il canguro sulla CPU usa le stesse corsie SIMD della ricerca lineare; la variante viene scelta misurando, dopo un confronto bit per bit con
  quella di riferimento. `bench` indica quale è in uso e misura ogni variante con 1 thread, un thread per core fisico e tutti i thread logici.
  Sui core con hyper-threading la modalità **mista** (il passo a corsie sul primo thread di ogni core, quello scalare sul suo gemello SMT) viene
  scelta da sola quando misura più veloce; `PZ_KANG_MIX=0` o `=1` la spegne o la impone.
* Il canguro con più RAM spreca meno (`--mem` grande consente `dp-bits` più bassi); il BSGS è limitato soltanto dalla RAM.
* **Pagine grandi**: per il BSGS conviene `echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled` (spesso già così).
* **Portatile: la velocità a regime è più bassa del picco.** Nei primi secondi CPU e GPU corrono alla massima potenza, poi il raffreddamento e il
  limite di potenza condivisa le rallentano (su un portatile la GPU è passata da ~790 M a ~606 M passi/s in 3 minuti); spesso la GPU integrata
  dà meno di quanto toglie. La taratura automatica dura pochi secondi per configurazione e vede soprattutto il picco: per scegliere `-t` giudica
  la riga "Speed" dopo 5-10 minuti di campagna, confrontando per esempio `hunt -p 140` e `hunt -p 140 -t 8`.
* Non usare più processi sulla stessa campagna (il blocco lo impedisce: il secondo si ferma con un errore).
* Per sessioni lunghe: `nohup pzsolve hunt -p N --ui log > /dev/null &` e controlla `pzsolve list` o `report.txt`.
* **Realismo**: `pzsolve puzzle N` stima il tempo sulla CPU di questa macchina; le GPU si sommano. I puzzle 71+ senza chiave pubblica
  richiedono 10^20…10^40 chiavi in media: leggi quei numeri prima di lanciare una campagna.

### 8.1 Più computer sulla stessa campagna canguro

Il canguro si divide fra computer meglio di qualunque altro algoritmo: i punti distinti di macchine diverse si incontrano come quelli dei
thread di una macchina sola, e la velocità si somma. Servono tre cose uguali su tutti i computer e una diversa:
* **uguali**: il puzzle (intervallo e chiave pubblica), il **seed** (`-s`, 64 cifre esadecimali: determina la tabella dei salti, e due
  camminate con tabelle diverse non si incontrano mai) e, per efficienza, `--dp-bits`;
* **diversa**: `--node N` (0 sul primo, 1 sul secondo, …): fa partire i canguri da punti diversi. Senza, due computer con lo stesso seed rifarebbero
  gli stessi passi.

```
computer A:  pzsolve hunt -p 140 -s 5f3a…(64 cifre) --dp-bits 43 --node 0
computer B:  pzsolve hunt -p 140 -s 5f3a…(stesso)   --dp-bits 43 --node 1
```
Il seed della campagna di A è nella riga «Seed» di `campaigns/p140/report.txt` (e in `campaign.conf`). Con `--node` l'avvio mostra
l'**identificativo della tabella dei salti** (`kangaroo: jump table id 402ef43c (computers working on the same campaign must show the same id)`;
`log.txt` lo contiene sempre): deve essere lo stesso su tutti i computer. Ogni tanto si porta il `dp.bin` di B su A e, **con la campagna di A
ferma**, si unisce: `pzsolve merge -p 140 -f percorso/di/B/p140` (un file `dp.bin`, oppure la cartella della campagna, o più `-f`). Il comando
dice quanti punti sono nuovi e se c'è una collisione fra un punto "domato" e uno "selvatico": in quel caso trova la chiave, la scrive in
`FOUND.txt` e la mostra. Si può anche unire in senso inverso (A su B) o in entrambi: i punti già noti vengono riconosciuti. Dopo l'unione `hunt`
riprende da dove era, con la tabella arricchita.

Un file che `merge` rifiuta viene nominato con il motivo (sezione 9.3). Per la ricerca lineare vedi la sezione 8.2.

### 8.2 Più computer sulla ricerca lineare

La ricerca lineare visita i blocchi dell'intervallo in un ordine casuale deciso dal seed. Con `--part Q/T` quell'ordine si divide in `T`
quote, a turno (il blocco in posizione 0 alla quota 0, quello in posizione 1 alla quota 1, …, poi di nuovo da capo), e il computer scandisce
solo le quote `Q` che gli spettano: le quote di computer diversi non si sovrappongono mai e insieme coprono tutto l'intervallo, senza
che i computer comunichino. Il blocco corto di fondo intervallo, se c'è, è contato esattamente.

```
computer A:  pzsolve hunt -p 71 -s 5f3a…(64 cifre) --part 0/2
computer B:  pzsolve hunt -p 71 -s 5f3a…(stesso)   --part 1/2
```
* **Uguali su tutti**: puzzle o intervallo, seed (`-s`: lo trovi nella riga «Seed» di `report.txt`, e con `--part` nel log all'avvio) e
  `--chunk-bits` (se non lo indichi dipende solo dall'intervallo, quindi è già uguale). **Diverse**: le quote.
* **Computer di velocità diverse**: dai più quote al più veloce. Con `pzsolve split 800M 120M` ottieni la divisione in proporzione
  (qui `0-86/100` e `87-99/100`); `--part 0-86/100` è lo stesso di `0,1,2,…,86/100`. Una quota per computer basta se sono uguali.
* **La parte è della campagna**: si decide alla creazione, non si cambia (`--part` diverso su una campagna esistente è rifiutato, usa `-c` per
  un'altra) e riprendendo senza `--part` resta quella. Anche il file di stato la ricorda, quindi non si può caricare per sbaglio uno stato
  di un'altra quota. Per far continuare una parte a un altro computer basta copiare la cartella della campagna.
* **Cosa vedi**: l'avanzamento è quello della *tua* parte (al 100 % hai finito le tue quote, non l'intervallo). Le probabilità sono
  calcolate sull'intero intervallo e contano solo il lavoro di questo computer; i tempi oltre la sua parte sono "n/a" (da solo non ci arriva).
  `where` dice se una chiave cade nella tua parte.
* **Se un computer si ferma**, le sue quote restano ferme: rilancia la sua campagna o copiala altrove. **Quando la parte finisce** il
  programma lo dice e si ferma; la chiave, se non è lì, è in una delle altre quote.
* Solo per la ricerca lineare. Con la chiave pubblica nota (canguro) si usa `--node` e `merge`; il BSGS non si divide.
* Servono almeno tanti blocchi quante sono le quote (`--chunk-bits` più piccolo se l'intervallo è corto).

## 9. Risoluzione dei problemi

### 9.1 Prima di tutto: `pzsolve doctor`

Controlla in una pagina il programma, il sistema, la CPU (verifica rapida di ogni motore con una chiave
nota), le GPU (apertura del dispositivo e confronto bit per bit con la CPU), la taratura e il profilo salvati, le campagne della cartella
(ferma o in esecuzione, ultimo salvataggio, sospensioni subite), lo spazio su disco e la scrittura atomica, l'orologio e le variabili `PZ_*`.
Le righe `[XX]` sono errori, `[!!]` avvisi. `doctor --full` esegue anche l'autotest intero; `-d DIR` indica la cartella delle campagne.

### 9.2 Sospensione e ibernazione

Se il computer va in sospensione o in ibernazione mentre una campagna gira:
* il programma si accorge del salto di tempo (il pannello non si ferma né mostra velocità assurde), non conta il tempo sospeso né nei tempi
  né nelle velocità, scrive nel log `system suspension detected (about … without computing): not counted in times and speeds; saving the state`
  e salva subito lo stato;
* se dopo il risveglio il driver ha azzerato la GPU e un lancio fallisce, la GPU viene riaperta, ricontrollata con lo stesso autotest dell'avvio
  e rimessa al lavoro (`GPU[0]: device resumed (…)`; il branco del canguro sulla GPU riparte da posizioni nuove: si perde solo il percorso
  dall'ultimo punto distinto, i punti già trovati sono al sicuro). I lanci che seguono un fallimento sono grandi un quarto di prima (una scheda
  condivisa con un gioco o con il desktop può rallentare tanto che un lancio supera i 2 s del watchdog del sistema, e fallirebbe di nuovo) e
  ricrescono da soli; il log dice la nuova dimensione. L'attesa prima di un nuovo tentativo (fino a 30 s) finisce subito con Ctrl-C. La GPU viene
  esclusa dopo più di 3 fallimenti di fila, o più di 40 in tutto (dieci minuti di lanci buoni dopo un fallimento azzerano il conteggio, così un
  portatile che va in sospensione ogni notte tiene la sua scheda);
* `hunt --keep-awake` impedisce a Windows (e a macOS, con `caffeinate`) di sospendersi finché la campagna gira (la sospensione torna possibile alla fine). Su Linux: `systemd-inhibit`.

Lo stato su disco non è mai a rischio: ogni scrittura è atomica e il checkpoint è ogni 60 s.

### 9.3 Sintomi

| Sintomo | Causa probabile |
|---------|-----------------|
| `the campaign in … is already running` | un altro processo usa la cartella: fermalo o usa un altro nome (`-c`) |
| `the existing campaign uses mode …` / `--range does not match the range of the existing campaign` / `the given seed does not match the one of the existing campaign` | hai cambiato in ripresa un parametro che definisce la campagna: toglilo (si riprende com'era) o usa un altro nome con `-c` |
| `final state save FAILED` | disco pieno o cartella non scrivibile: libera spazio e rilancia (il checkpoint precedente è intatto) |
| `main checkpoint unusable` / `no usable checkpoint` | `state.dat` corrotto: viene tentato `.bak`; altrimenti rimuovilo (si ricomincia) |
| `the checkpoint belongs to a different split between computers` / `the checkpoint was written by a campaign with another seed` (seguiti da `the state … is intact but belongs to another campaign, another seed or another part of the range`) | lo stato è integro ma è di un'altra `--part`, di un altro seed o di un'altra campagna: non toccarlo, usa la campagna giusta o un altro nome con `-c` |
| `file … does not belong to this campaign` | `dp.bin` di un'altra campagna: spostalo |
| `file … was written with another jump table (id …, this campaign: …)` | `dp.bin` dello stesso intervallo scritto con un altro seed (o con la configurazione di un altro computer): spostalo per ricominciare i punti distinti |
| `file … was written by a newer version of pzsolve` | il `dp.bin` della campagna è di una versione successiva: non viene toccato; usa quella versione |
| `the BSGS table needs … but only … of RAM are available` | poca RAM: usa `--mem`, `--bsgs-entries` più piccolo (alla creazione) o `--mode kangaroo` |
| `table … is not usable: …` / `table header corrupted` | file `--table` troncato o di un altro programma: cancellalo, verrà ricostruito |
| `self-test failed, device excluded` | driver difettoso o compilatore del kernel sbagliato: aggiorna il driver; per una NVIDIA il programma prova anche OpenCL; segnala il caso |
| `CUDA: the driver is too old for this PTX` | aggiorna il driver NVIDIA (525 o successivo), oppure usa `--gpu-api opencl` |
| `CUDA driver not found` (in `devices`) | normale senza scheda NVIDIA; con una NVIDIA, reinstalla il driver grafico |
| `the CPU does not support the instruction set …` | usa `--isa auto` o `--isa baseline` |
| `NAME=TEXT ignored (…)` | una variabile d'ambiente ha un valore che il programma non può usare e viene ignorata: correggila o toglila (sezione 7.10) |
| `distinguished-point table full (RAM limit): halving the density of stored points` | atteso in circa una corsa su cinque quando è la memoria a fissare i dp-bits (ALGORITHMS.md sezione 4.4, in inglese): la ricerca prosegue con una collisione un po' più tardiva; un `--mem` più grande aiuta solo se la RAM c'è |
| velocità molto bassa | controlla `devices` (quante corsie/GPU usate) e che nessun altro processo sia in esecuzione |
| prima esecuzione del BSGS lenta | la RAM appena allocata viene "toccata" per la prima volta (su alcune macchine virtuali costa secondi per GiB) |

## 10. Windows

`pzsolve.exe` (dall'archivio Windows della release) è un eseguibile a 64 bit per Windows 10/11, statico: usa solo DLL di sistema e non va
installato. Si usa da **PowerShell** o dal Prompt dei comandi, con gli stessi comandi e opzioni di Linux (`.\pzsolve.exe hunt -p 71`, ecc.).
Con un doppio clic mostra l'aiuto e attende Invio.

* **Cartelle**: le campagne vanno in `campaigns\` nella cartella da cui lanci il comando (o in quella indicata con `-d`); funzionano
  anche percorsi con spazi tra virgolette (`-d "D:\Le mie campagne"`). I file di testo (`report.txt`, `status.json`, ...) si aprono con
  il Blocco note.
* **Fermarsi**: Ctrl-C (o Ctrl-Interr) salva e chiude. Anche **chiudere la finestra**, disconnettersi o spegnere il PC: Windows concede
  qualche secondo e il programma lo usa per salvare lo stato (l'ultimo minuto di lavoro al massimo può andare perso).
* **Sessioni lunghe**: lascia aperta la finestra di PowerShell; il pannello si aggiorna sul posto (Windows Terminal o la console di
  Windows 10 1607+). Per impedire che il PC vada in sospensione durante la notte: Impostazioni → Sistema → Alimentazione (oppure usa
  `--keep-awake`, sezione 9.2). Sui portatili tieni l'alimentatore collegato e la modalità "Prestazioni migliori".
* **Memoria**: i limiti sono calcolati sulla RAM disponibile riportata da Windows (come su Linux: BSGS al massimo il 70 %, canguro il
  40 %, modificabili con `--mem GiB`). Windows deve poter "impegnare" la memoria: con file di paging disattivato o piccolo una tabella
  BSGS molto grande può essere rifiutata (il programma lo dice); lascia il file di paging gestito dal sistema.
* **Pagine grandi** (opzionali, utili solo per il BSGS con tabelle ≥ 1 GiB): servono il diritto "Blocco di pagine in memoria"
  (`secpol.msc` → Criteri locali → Assegnazione diritti utente → *Blocco di pagine in memoria*: aggiungi il tuo utente, poi esci e
  rientra) e l'avvio di PowerShell come amministratore. Senza, il programma usa le pagine normali; `PZ_NO_LARGE_PAGES=1` le disattiva.
* **Antivirus**: un programma che usa a lungo tutta la CPU può insospettire alcuni antivirus. Il codice sorgente è nel repository e
  `SHA256SUMS` permette di verificare che l'exe sia quello pubblicato; se Windows Defender lo blocca puoi aggiungere un'esclusione
  per la cartella.
* **Compilarlo da sé**: da Linux con MinGW-w64, `make windows` (risultato in `build/win/pzsolve.exe`); `make wintest` esegue i test
  dell'exe sotto Wine.
