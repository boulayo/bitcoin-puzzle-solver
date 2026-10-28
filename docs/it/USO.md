# Guida all'uso

## 1. Comandi

| Comando | Cosa fa |
|---------|---------|
| `pzsolve puzzles [--open\|--solved\|--pubkey]` | elenca i 160 puzzle del database integrato (stato, chiave pubblica nota, indirizzo, sforzo medio); i 161–256, svuotati nel 2017, si vedono con `puzzle N` |
| `pzsolve puzzle N [--rate R]` | dati del puzzle N (indirizzo, chiave pubblica, stato, intervallo) e **stime di tempo** (misura la velocità di questa macchina, oppure usa `--rate` in chiavi/s) |
| `pzsolve hunt …`    | avvia o riprende una campagna |
| `pzsolve list [-d DIR]` | tutte le campagne della cartella: stato, avanzamento, velocità, tempo residuo |
| `pzsolve info -c NOME` | stampa il report di una campagna |
| `pzsolve where -c NOME --key HEX` | dice a che punto del percorso della campagna verrà scansionata una chiave |
| `pzsolve split V1 V2 …` | divide le quote di `--part` fra più computer in proporzione alle loro velocità (per esempio `split 800M 120M`; vedi sez. 8) |
| `pzsolve merge (-c NOME \| -p N) [-d DIR] -f FILE_O_CARTELLA …` | unisce i punti distinti di altri computer a una campagna canguro (vedi sez. 8) |
| `pzsolve doctor [-d DIR] [--full]` | controlla programma, sistema, CPU, GPU, taratura e profilo salvati, campagne, disco, orologio e variabili `PZ_*`, e dice cosa non va; da incollare quando si chiede aiuto |
| `pzsolve devices`   | CPU, set di istruzioni, RAM e GPU (CUDA e OpenCL) rilevate |
| `pzsolve selftest`  | controlli di correttezza integrati (primitive, database, motori SIMD, canguro, BSGS, GPU) |
| `pzsolve bench [--together [--linear] [-t N] [--gpu L] [--prio-gpu P]] [--gpu-only] [--retune]` | velocità di ogni motore CPU (1…N thread), del canguro, del BSGS e delle GPU (con banda e throughput intero); `--together` misura CPU+GPU insieme e dice quanti thread CPU e quali GPU convengono |
| `pzsolve kstat BITS PROVE [THREAD [R [DP]]]` | statistica del costo del canguro su intervalli di 2^BITS chiavi (R salti invece di 256 e dp-bits fissi, senza riavvii: per studiare i cicli) |
| `pzsolve version`   | versione e commit di compilazione |

## 2. Avvio rapido: tutti i puzzle sono già dentro

```sh
pzsolve puzzles --open          # quali puzzle sono ancora irrisolti
pzsolve puzzle 71                 # indirizzo, intervallo e quanto tempo servirebbe su questa macchina
pzsolve hunt -p 71                # parte: intervallo e indirizzo vengono dal database
# … Ctrl-C quando vuoi …
pzsolve hunt -p 71                # lo stesso comando riprende da dove eri arrivato
pzsolve list                      # lo stato di tutte le campagne
```

`hunt -p N` crea la campagna `pN` nella cartella `./campaigns`; rilanciare lo stesso comando la **riprende** (stesso seed,
nessun lavoro rifatto). Per i puzzle con chiave pubblica nota (140, 145, 150, 155, 160 e tutti quelli già risolti) la chiave
pubblica viene usata da sola e il programma sceglie tra BSGS e canguro (sezione 4).

Il database contiene indirizzo, chiave pubblica (se mai comparsa sulla blockchain) e, per i puzzle risolti, la chiave privata (che
serve come banco di prova: `hunt -p 40` ritrova una chiave nota in pochi secondi). Provenienza e verifiche: [`data/README.md`](../../data/README.md).
**Lo stato "risolto/aperto" è quello della data di creazione del database**: prima di lanciare una campagna lunga controlla
sull'indirizzo (esploratore della blockchain) che i fondi ci siano ancora. Il programma avvisa se scegli un puzzle già risolto.

### I puzzle 161–256

La transazione originale del 2015 aveva **256** uscite, una per puzzle (il puzzle `n` riceveva `n` millesimi di BTC). Nel luglio
2017 (blocco 475240) il creatore ha speso con un'unica transazione le uscite 161–256 e ha usato i fondi per aumentare i premi dei
puzzle 1–160. Gli indirizzi 161–256 quindi esistono ma **sono vuoti**. `puzzle 161` ne mostra l'indirizzo e `hunt -p 161` li
cerca lo stesso (con un avviso): utile solo come esercizio, dato che `2^160` chiavi non si esauriscono. Il puzzle 256 si ferma a
`n − 1` (l'ordine del gruppo), perché le chiavi oltre non esistono. Provenienza e verifiche degli indirizzi: `data/README.md`.

## 3. `hunt`: opzioni

```
-p, --puzzle N          puzzle 1..256: intervallo, indirizzo e (se nota) chiave pubblica dal database integrato
                        (161..256: solo l'indirizzo, fondi ritirati nel 2017, vedi sotto). Con -r sceglie solo il
                        bersaglio, l'intervallo è quello di -r
-r, --range S:E         intervallo personalizzato (esadecimale, inclusivo): p. es. una sola parte del puzzle. Una chiave fuori
                        dall'intervallo non viene segnalata dalla ricerca lineare, nemmeno quando l'ultimo blocco la raggiunge
                        (i motori scandiscono corsie intere)
-a, --address ADDR      indirizzo mainnet diverso da quello del database: P2PKH (`1...`, Base58Check) o SegWit nativo P2WPKH (`bc1q...`, bech32)
-H, --hash160 HEX       alternativa all'indirizzo (40 cifre hex)
-k, --pubkey HEX        chiave pubblica nota (33 byte compressa o 65 non compressa)
-c, --campaign NOME     nome della campagna: cartella <dir>/<NOME>; se esiste già viene ripresa [pN con -p N]
-d, --dir DIR           cartella base delle campagne [./campaigns]
-s, --seed SEED         64 cifre hex (usate verbatim) o testo qualsiasi (hashato); default o `auto`: casuale
-m, --mode M            auto | linear | kangaroo | bsgs   [auto, vedi sotto]
    --table FILE        bsgs: legge la tabella dei baby step da FILE se esiste, altrimenti la costruisce e la salva
    --bsgs-entries N    bsgs: baby step della tabella (alla creazione della campagna)
    --part Q/T         ricerca lineare su più computer: i blocchi dell'ordine di visita sono divisi in T quote e questo computer scandisce le quote Q
                        (numerate da 0; anche più d'una: `0,1/4`, `0-6/9`). Vedi «Più computer sulla ricerca lineare»
    --node N            canguro su più computer: numero di questo computer (0..65535); vedi «Più computer sulla stessa campagna canguro»
    --prio-gpu P        auto | yes | no: thread che servono le schede con priorità più alta dei thread di calcolo [auto: la taratura lo prova]
    --limit X          usa solo il X % della macchina (1..99), sempre a pieno ritmo e senza pause: meno thread CPU e meno work-item sulle GPU
                        [spento]; vedi «Usare solo una parte della macchina»
    --retune            misura di nuovo la combinazione migliore di thread CPU e GPU di questa macchina
    --no-tune       non misurare e non usare il profilo della macchina né la velocità ricordata
    --keep-awake           Windows: impedisce al sistema di andare in sospensione finché la campagna gira
-t, --threads N         thread CPU [core disponibili; con una GPU ne resta riservato uno per ciascuna]
    --no-cpu / --no-gpu disabilita CPU o GPU
    --gpu LISTA         indici (vedi `devices`) separati da virgola; permette anche dispositivi OpenCL su CPU
    --gpu-api A         auto | cuda | opencl   [auto: CUDA per le NVIDIA, OpenCL come riserva e per le altre schede]
    --isa I             auto | baseline | avx2 | avx512 | avx2v | avx512v | avx512i   [auto: micro-benchmark]
                        su arm64: auto | baseline | neon | neon2 (SHA-256 hardware della CPU, 4 o 8 chiavi alla volta)
    --chunk-bits B      blocchi di 2^B chiavi (20..40) [36, ridotto per intervalli piccoli]; blocchi più piccoli
                        rallenterebbero la CPU e soprattutto le GPU (ogni lancio riceve un solo blocco)
    --dp-bits B         canguro: bit dei punti distinti [automatico]
    --mem GB            canguro e bsgs: RAM massima per la tabella [canguro: 40 % della RAM libera; bsgs: 70 %]
    --ui U              auto | full | line | log   (vedi sezione 6)
    --report-interval S secondi tra gli aggiornamenti del pannello [1]
    --checkpoint-interval S  secondi tra i salvataggi [60]
-q / -v                 meno / più messaggi
    --new / --resume    fallisce se la campagna esiste / non esiste
```

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

| Situazione | Scelta |
|------------|--------|
| solo indirizzo | `linear`: scansione di tutto l'intervallo (CPU e GPU) |
| chiave pubblica nota, intervallo < 2^20 | `linear` (istantaneo) |
| chiave pubblica nota, la tabella BSGS ottima (o la massima che sta nel 70 % della RAM libera) costa meno del canguro, **e non ci sono GPU** (oppure il lavoro dura pochi secondi, oppure hai già una `--table`) | `bsgs`: costo ≈ 1,0·√W, deterministico |
| chiave pubblica nota, altrimenti | `kangaroo`: ≈ 1,06·√W (campagne iniziate prima della 1.7.9: vedi «Più computer sulla stessa campagna canguro»), usa anche le GPU |

Il BSGS usa solo CPU e RAM; con una GPU potente il canguro può essere più veloce nonostante il costo maggiore: se sai che è
così, forza `--mode kangaroo`. La modalità resta fissata nella campagna (`campaign.conf`). Il canguro accetta intervalli da 2^16 a 2^188
chiavi (i puzzle dal 190 in su vengono rifiutati con un messaggio; gli esponenti dei suoi camminatori sono numeri a 192 bit).

La tabella del BSGS occupa circa 11,4 byte per baby step (celle da 8 byte, usate al 70 %); `--bsgs-entries` conta i baby step, non i byte.
Una tabella salvata da una versione precedente (file `PZBSGS01`, 12,3 byte per baby step) viene letta e usata così com'è, con la RAM che richiede;
le tabelle salvate ora hanno un formato nuovo (`PZBSGS02`) che quelle versioni non sanno leggere.

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
  log.txt         eventi con data e ora
  FOUND.txt       solo a chiave trovata (permessi 0600)
```

* **Fermarsi**: Ctrl-C, `kill` o chiusura del terminale (SIGHUP; sotto `nohup` viene ignorato) — lo stato viene salvato; un secondo
  Ctrl-C dopo mezzo secondo esce subito (e perde al massimo l'ultimo minuto di lavoro).
* **Riprendere**: lo stesso comando (`hunt -p N`, oppure `hunt -c NOME`). Il seed e i parametri che definiscono la ricerca non
  possono cambiare: se dai `-s`, `-r`, `-a`, `-k`, `-H`, `-m`, `--chunk-bits` o `--bsgs-entries` diversi da quelli della campagna
  esistente il programma **rifiuta** con un errore (non li ignora in silenzio); puoi invece cambiare thread, GPU, ISA, memoria e
  aspetto del pannello. **`pzsolve list`** mostra tutte le campagne e il loro stato; `info -p N` / `where -p N --key K` accettano
  `-p` al posto di `-c pN`.
* **Nome della campagna** (senza `-c`): `pN` per un puzzle intero; per un sotto-intervallo (`-r`) o un obiettivo personalizzato
  `pN-xxxxxxxx` / `custom-xxxxxxxx`, dove `xxxxxxxx` dipende da puzzle, intervallo e obiettivo: **lo stesso comando ritrova sempre
  la stessa campagna**. Per due campagne indipendenti sullo stesso intervallo usa `-c` con nomi diversi (e seed diversi). I nomi
  ammettono lettere, cifre, `-`, `_`, `.` (non all'inizio).
* **Codici di uscita di `hunt`**, per gli script: 0 = chiave trovata (anche quando la campagna l'aveva già trovata: vedi `FOUND.txt`);
  1 = la ricerca è finita senza trovare la chiave (tutto l'intervallo, o tutta la `--part`, è stato coperto);
  130 = interrotto con Ctrl-C, `kill` o chiusura del terminale (lo stato è salvato, lo stesso comando riprende);
  2 = errore: opzioni non valide o incoerenti, campagna in uso, file illeggibili, salvataggio finale dello stato non riuscito, o un
  arresto che nessuno ha chiesto (tutte le GPU in errore e CPU spenta), oppure uso errato della riga di comando.
  Gli altri comandi escono con 0 se riescono e con 2 in caso di errore; `selftest` e `doctor` escono con 1 se un controllo è fallito e
  `merge` con 1 se qualche file non è stato unito (esce con 0 sia quando l'unione ha trovato la chiave, che mostra e salva in
  `FOUND.txt`, sia quando non c'è ancora nessuna collisione).
* Un riavvio perde al massimo il lavoro dei blocchi in corso (pochi secondi per worker); il resto non viene mai rifatto. Nel BSGS la
  tabella viene ricostruita (pochi minuti) a meno che tu non usi `--table`.
* Una campagna che ha già trovato la chiave non riparte: mostra `FOUND.txt`.
* **Più computer**: nella ricerca lineare usa `--part` (sez. 8, «Più computer sulla ricerca lineare»): niente doppioni e niente scambio di
  dati; nel canguro `--node` e `merge` (sez. 8). Con seed diversi e senza `--part` le campagne si sovrappongono a caso.

## 6. Il pannello di avanzamento

Su un terminale il programma disegna **sul posto** un pannello di una decina di righe, aggiornato una volta al secondo
(le righe di log scorrono sopra, il pannello resta in basso). Esempio *illustrativo* (i numeri non sono misure):

```
pzsolve 1.1.0 | campagna p71 | puzzle #71 | linear | seed 1b0f37cb | IN CORSO
Obiettivo     1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU   intervallo di 2^70 chiavi
Avanzamento   [..............................] 0.0123%   (7.28e19 / 1.181e+21 chiavi)
Velocita'     312.40 M chiavi/s   (picco 330.1 M, media sessione 308 M)
              CPU x16: 120.1 M  |  GPU0: 192.3 M
Tempo         questa sessione 12m 3s | totale 3g 4h 1m | sessione n. 4
Tempo residuo   50%: 4.1 milioni di anni | 90%: 7.4 milioni di anni | 100%: 8.2 milioni di anni
Prob. di trovarla da ora: 1h 1.2e-12% | 24h 2.9e-11% | 7g 2e-10% | 30g 8.8e-10% | 365g 1.1e-08%
              ordine casuale: blocchi da 2^36 chiavi, 12 intervalli aperti
Ctrl-C: ferma e salva i progressi; rilancia lo stesso comando per riprendere.
```

* **Avanzamento**: frazione coperta (lineare, BSGS) oppure **probabilità stimata di aver già trovato la chiave** (canguro).
* **Tempo residuo**: tempo ancora necessario, a velocità costante, per arrivare alla probabilità 50 % / 90 % / 100 % (nel canguro 99 %
  invece di 100 %, che non esiste). Se mancano meno di un anno compare anche l'ora prevista. Dalla 1.7.4 la velocità usata è
  pensata per essere realistica fin dall'inizio (vedi sotto); nel BSGS resta una media mobile di ≈ 30 s.
* **Stima provvisoria** (dalla 1.7.4). Nei primi secondi la velocità mostrata inganna: una GPU viene contata solo quando finisce il
  suo primo lancio, i clock salgono, e un portatile dopo qualche minuto sotto carico rallenta (su un portatile con GPU NVIDIA: 1,03 G passi/s
  misurati a freddo dalla taratura, 834 M dopo tre minuti). Per questo il tempo residuo: (1) non compare finché ogni motore non ha
  prodotto qualcosa (la riga dice «in misura» e la velocità porta l'etichetta «in avvio»); (2) usa poi la media su una finestra
  scorrevole fino a 2 minuti, non una media mobile partita dal primo campione; (3) parte da una **velocità ricordata**, se c'è: quella
  *a regime* raggiunta da una corsa precedente della stessa configurazione (stessa macchina, versione, motore, thread, GPU e priorità;
  salvata nel profilo ogni 5 minuti e alla fine, solo dopo almeno 3,5 minuti di corsa), altrimenti quella misurata a freddo dalla
  taratura; la misura prende il posto del ricordo in circa 2 minuti. Finché la stima si assesta compare la riga «stima provvisoria».
  Con `--no-tune` la memoria non si usa né si aggiorna. `pzsolve doctor` elenca le velocità ricordate.
* **Prob. di trovarla da ora**: probabilità di successo nelle prossime ore/giorni a questo ritmo, *supponendo che la chiave sia
  nell'intervallo* (formule in [`ALGORITHMS.md`](../en/ALGORITHMS.md) sezione 11, in inglese).
* Nel BSGS, durante la costruzione della tabella il pannello mostra la fase 1 di 2 e quanti giganti seguiranno.

Modalità: `--ui full` (pannello), `--ui line` (una sola riga dinamica), `--ui log` (una riga di riepilogo al minuto nel log,
adatta a file e `nohup`), `--ui auto` (default: pannello su un terminale, altrimenti log). Fuori da un terminale non vengono mai
emessi codici di controllo. `NO_COLOR=1` toglie i grassetti. Il file `status.json` contiene gli stessi numeri per altri programmi.

## 7. GPU: CUDA e OpenCL

Il programma usa due runtime, entrambi caricati all'avvio solo se presenti (senza nessuno dei due lavora con la CPU):

* **CUDA** (schede NVIDIA): il *driver* CUDA (`nvcuda.dll` su Windows, `libcuda.so.1` su Linux), che si installa con il normale
  driver grafico NVIDIA (versione **525 o successiva**). **Non serve il toolkit CUDA**: i kernel sono già dentro il programma in forma
  PTX (generata da `tools/gen_ptx.py` con NVRTC dagli stessi sorgenti dei kernel OpenCL) e il driver li compila per la tua scheda.
  Serve una GPU Maxwell o più recente (compute capability ≥ 5.2).
* **OpenCL** (`OpenCL.dll` / `libOpenCL.so.1`): AMD (driver Adrenalin su Windows, ROCm `rocm-opencl-runtime` su Linux), Intel
  (driver grafico / `intel-opencl-icd`); anche il driver NVIDIA include OpenCL. **Solo prove su CPU**: `pocl-opencl-icd`; i dispositivi
  CPU non vengono usati di default (`PZ_OPENCL_ALL=1 pzsolve selftest` oppure `--gpu N`).

**Memoria dichiarata e memoria usata.** `devices` e l'avvio mostrano la memoria che il driver dichiara. Per una scheda con memoria
propria (le NVIDIA, le AMD discrete) è la VRAM; per una **GPU integrata** (Intel UHD/Iris, le APU AMD) il driver OpenCL dichiara la
quota di RAM di sistema che la scheda può indirizzare, di solito più della metà della RAM (`NN.NN GiB, RAM di sistema condivisa`):
non è memoria riservata né allocata. Il programma ne usa solo quanto serve ai suoi buffer, e a ogni avvio del canguro lo dice
(`GPU[2]: 54272 canguri (…), memoria usata 29.8 MiB della RAM di sistema`; sulle NVIDIA, `… MiB su 8.00 GiB`). Il canguro ne usa
≈ 55 MiB per ogni 1000 work-item di 64 canguri (≈ 230 byte per canguro compresa la storia dei salti), nel limite di un sesto
della memoria dichiarata. Le schede NVIDIA hanno meno memoria dichiarata ma più velocità: la banda, non la capacità, è il limite.

`pzsolve devices` elenca tutto con un indice per `--gpu`; una scheda NVIDIA compare due volte (CUDA e OpenCL) e di default viene
usata **una volta sola, tramite CUDA**. Se la via CUDA non supera il self-test il programma prova la stessa scheda tramite OpenCL.
`--gpu-api opencl` (o `PZ_GPU_API=opencl`) forza OpenCL, `--gpu-api cuda` solo CUDA.

All'avvio ogni dispositivo esegue un **self-test bit-esatto contro la CPU** (aritmetica, hash160, somma di punti, ricerca lineare in
tutte le varianti, passi del canguro con punti distinti e riavvii); chi non lo supera viene escluso con un messaggio e il programma
prosegue con gli altri. Ogni chiave candidata viene comunque riverificata sulla CPU.

**Prima esecuzione.** Il driver compila i kernel per la tua scheda (con CUDA circa 10 s per modulo; con la taratura delle sei varianti
del kernel lineare circa un minuto, poi tre varianti del canguro la prima volta che lo usi) e conserva il risultato nella sua cache:
dalle volte successive l'avvio è immediato. La taratura è memorizzata in `%LOCALAPPDATA%\pzsolve\gpu_tune.txt` su Windows,
`~/.cache/pzsolve/gpu_tune.txt` su Linux, e si ripete quando cambia la versione del programma.

Le varianti provate dalla taratura sono: ricerca lineare con CUDA finestra 128, 256 o 512 × registri liberi o limitati a 128, con
OpenCL finestra 64, 128, 256 o 512; canguro con 64, 128 o 256 camminatori per thread (CUDA) o 32, 64, 128 (OpenCL), ciascuno in due
forme (dalla 1.7.0): semplice e con i prodotti del lotto tenuti a coppie (`b`, meno traffico di memoria). Sulle schede con una cache L2 di
almeno 8 MiB (RTX 30/40 non portatili: 6–72 MiB) la taratura prova anche un **branco dimensionato sulla L2**: meno work-item (e NK 16 o 32)
in modo che gli array privati dei camminatori stiano in mezza L2, dove il traffico che limita il kernel non arriva alla memoria della
scheda (`NK 64 (branco nella L2)` nel log; `PZ_GPU_NK=64l`). Viene scelto solo se misurato più veloce. `bench` mostra
la velocità di entrambi per ogni GPU. Su un **portatile** CPU e GPU condividono alimentazione e raffreddamento: vale la pena confrontare per un paio di
minuti la velocità totale del pannello con le impostazioni predefinite, con `-t 8` (solo i core fisici: l'hyper-threading aggiunge
poco nella ricerca lineare e scalda) e con `--no-cpu`, e tenere la più veloce. Dalla 1.7.1 lo fa `bench --together` (sotto).

**`bench` e le GPU.** Per ogni GPU il `bench` stampa, oltre alla ricerca lineare e al canguro (con la memoria usata):
* la **banda di copia** (lettura + scrittura coalescente, in GB/s, con la banda teorica per le NVIDIA): è il tetto del canguro, il
  cui traffico è ≈ 215 byte per passo;
* il **throughput delle istruzioni intere a 32 bit** (`mad.lo`, `mad.hi`, somma a 3 ingressi) in operazioni al secondo e per
  unità e per ciclo al clock dichiarato: è l'altro tetto, perché il canguro esegue ≈ 2200 istruzioni per passo;
* la **cache L2** e la memoria condivisa per SM, con il rapporto tra il branco del canguro e la L2 (se il branco non sta nella L2, ogni
  passo legge e scrive la memoria della scheda: è il caso di tutte le GPU attuali per i branchi da centinaia di migliaia di canguri);
* per le NVIDIA, i due tetti calcolati con quei numeri (`banda … passi/s, intere … passi/s`): il canguro misurato non può superare
  il più basso, e se gli è vicino il kernel è già al limite di quella risorsa (una rielaborazione software non lo velocizza più).
`bench --gpu-only` salta le prove della CPU. `bench --retune` ignora la scelta salvata delle varianti dei kernel
(`gpu_tune.txt`) e la misura di nuovo: serve dopo un aggiornamento del driver o per vedere i numeri di tutte le varianti.

**Profilo automatico della macchina (dalla 1.7.1).** La prima volta che lanci `hunt` con una versione nuova, su una macchina con GPU e
senza `-t`, `--gpu`, `--no-cpu` o `--no-gpu`, il programma esegue da solo lo stesso confronto di `bench --together` (circa 2 minuti, per il
canguro o per la ricerca lineare secondo la campagna), salva il risultato e da allora lo usa come predefinito: il numero di thread CPU
e le GPU che conviene tenere accese. Il profilo è nella cartella della cache, accanto alla taratura dei kernel (`%LOCALAPPDATA%\pzsolve\machine_profile.txt`
su Windows, `~/.cache/pzsolve/machine_profile.txt` su Linux) ed è legato alla versione del programma, al processore, al numero di
core, al passo/motore usato e alle GPU con il loro driver: se cambia una di queste cose si rimisura. Il log dice sempre cosa è stato
applicato (`profilo di questa macchina del AAAA-MM-GG: 8 thread CPU, GPU 0`). Si cambia idea così: `-t`/`--gpu`/`--no-cpu`/`--no-gpu`
sostituiscono il profilo per quella esecuzione; `--retune` lo rimisura; `--no-tune` (o `PZ_NO_PROFILE=1`) non lo misura né lo usa.
Se nessuna configurazione batte quella predefinita di più del rumore della misura, il profilo salvato è "i valori predefiniti sono già i
migliori" e non si rimisura a ogni avvio.

**Priorità dei thread che servono le schede (dalla 1.7.4).** Ogni GPU è servita da un thread che lancia i kernel, raccoglie i risultati
e rilancia. Quando tutti i core sono occupati dal calcolo, quel thread può aspettare il suo turno mentre la scheda resta ferma; sui
portatili l'effetto si confonde con il limite di potenza condiviso. Nella taratura, subito dopo il riscaldamento e con tutti i thread
CPU, il programma confronta priorità normale e alta a coppie alternate (normale, alta, alta, normale: una deriva termica non può
falsare il confronto; la seconda coppia si fa solo se la prima favorisce la priorità alta di almeno l'1 %) e la tiene solo se vince di
almeno il 2 % in complesso e in entrambe le coppie. L'esito entra nel profilo (`prio=1`) e il log lo dice (`GPU: thread che servono le
schede con priorità alta`). `--prio-gpu yes|no` la impone o la esclude (e non si misura); `--prio-gpu auto` è il predefinito. Su Windows
è `ABOVE_NORMAL` per i soli thread delle schede (che dormono quasi sempre in attesa del kernel: non tolgono tempo al calcolo); su
Linux è `nice -5` e serve root o `CAP_SYS_NICE`: senza, la misura viene saltata e `doctor` lo dice. Non è una promessa di velocità:
se la causa è la potenza del portatile e non l'attesa del thread, la misura darà ~0 % e la priorità resta normale. Per i test: `PZ_BENCH_SCALE=0.2` riduce a un quinto la durata delle misure.

**Usare solo una parte della macchina: `--limit X` (dalla 1.7.6).** Spento di default. `hunt -p 71 --limit 50` usa circa metà della macchina per
scaldare meno e consumare meno, accettando di andare più piano. Non ci sono pause né cicli «lavora e riposa»: il carico resta costante, così la ventola
non parte e si ferma a ondate. Si ottiene con meno unità di calcolo, tutte al massimo:

* **CPU**: il X % dei thread (arrotondato al più vicino, almeno uno). Con `-t` il X % si applica ai thread indicati. I thread vanno prima su core
  fisici diversi, uno per core, e solo oltre il numero dei core sul secondo thread SMT dello stesso core: su 8 core e 16 thread, `--limit 50` sono
  8 thread su 8 core, `--limit 25` sono 4 thread su 4 core, `--limit 75` sono 12 thread. Su Windows l'assegnazione è un suggerimento al sistema, che può spostare i thread.
* **GPU**: il X % dei work-item di ogni lancio (arrotondato a blocchi di 128, almeno uno) sia nella ricerca lineare sia nel canguro: la scheda lavora
  sempre, ma con meno blocchi in corso. Il canguro ha di conseguenza meno canguri. Il log lo scrive (`limite 40%: 3328 work-item per lancio`).
* **BSGS**: solo i thread CPU (anche la costruzione della tabella usa meno thread e dura di più). Le GPU non partecipano comunque.
* La taratura della macchina (circa 2 minuti a pieno carico) e il profilo non vengono usati né aggiornati; la velocità a regime di una corsa limitata
  si ricorda a parte (`pzsolve doctor` la elenca con «limite N%»), così le stime del tempo restano realistiche. `--retune` con `--limit` è rifiutato.
* Il limite non è salvato nella campagna: ogni esecuzione sceglie il suo, e riprendendo senza `--limit` si torna a tutta la macchina. Il pannello
  scrive «limite X%» davanti alle velocità e `report.txt` e `status.json` (`limit_percent`) lo riportano.

Il consumo non scala esattamente come X: con meno core attivi la CPU può alzare la frequenza di quelli rimasti, e una parte del consumo (RAM, scheda
in attesa, ventole) resta. Se vuoi un tetto più deciso, prova valori più bassi e guarda temperature e ventola (HWiNFO, `nvidia-smi`). Su Windows
anche «Stato massimo del processore» nelle opzioni risparmio energia (impostato al 99 % disattiva di solito il turbo) frena la frequenza senza toccare il programma.
Le prove su GPU in questo progetto sono fatte con un driver simulato: l'effetto sui watt di una scheda vera non è stato misurato.

**`bench --together`** avvia come in una campagna (puzzle 140) i thread CPU e tutte le GPU e misura la velocità totale con
5 numeri di thread CPU (tutti, tre quarti, uno per core fisico, metà, nessuno), poi spegne a turno ogni GPU più lenta
(restituendo alla CPU il core che le serve). Ogni configurazione dura 10 s (2 di assestamento e 8 di misura) più una ripetizione
finale della prima che stima il rumore. Alla fine indica la configurazione migliore, per esempio `-t 11 --gpu 0`, se supera
quella predefinita di più del rumore. Su un portatile CPU, GPU integrata e GPU discreta condividono potenza e raffreddamento:
spesso la GPU integrata dà meno di quanto toglie. `-t N` prova solo N thread CPU, `--gpu LISTA` limita le GPU, `--linear` misura la ricerca lineare invece del canguro.

Variabili d'ambiente utili (quelle a due stati, come `PZ_NO_CUDA`, valgono con qualunque valore tranne vuoto e `0`): `PZ_GPU_API=cuda|opencl`, `PZ_NO_CUDA=1`, `PZ_NO_OPENCL=1`, `PZ_GPU_HALF=128|256|512` (CUDA) o
`64|128|256|512` (OpenCL) e `PZ_GPU_NK=64|128|256` (CUDA) o `32|64|128` (OpenCL), con suffisso facoltativo `b` per i prodotti a coppie (es. `128b`) (saltano la taratura), `PZ_GPU_TUNE=0` (nessuna
taratura; `=1` la fa anche sui dispositivi che non sono GPU, per i test). Per i test e la diagnostica: `PZ_KANG_VERBOSE=1` (velocità di ogni passo del canguro scelto), `PZ_TEST_DP_CAP=N` (tabella dei punti distinti minuscola), `PZ_MOCK_FAIL_FILE=percorso` (il driver CUDA simulato fallisce un lancio quando il file compare), `PZ_TEST_KTHREAD=N` e `PZ_TEST_JUMP=N` (solo per le prove: camminatori per thread CPU e salto medio in unità di √W), `XDG_CACHE_HOME` (cartella della cache, anche su Windows). Per la CPU, a scopo diagnostico: `PZ_KANG_ROUND=base|x86|avx2v|avx512v|avx512i|avx512i2` impone il passo del canguro (`avx512i2`: IFMA a coppie di vettori, dalla 1.7.7),
`PZ_BSGS_SCALAR=1` disattiva le finestre BSGS a corsie. `PZ_KANG_MIX=1` (sperimentale): sui core con hyper-threading il primo
thread usa il passo vettoriale e l'altro quello scalare BMI2/ADX, che usa il moltiplicatore intero invece dell'unità vettoriale;
`bench` misura se conviene (riga "misto") e lo suggerisce.

## 8. Consigli prestazionali

* Lancia `pzsolve bench` una volta: confronta i motori e il numero di thread; con una GPU lancia anche
  `pzsolve bench --together` (circa 2 minuti), che misura la combinazione CPU+GPU vera e suggerisce `-t` e `--gpu`. `--isa auto` sceglie da solo tra
  `baseline`, `avx2`, `avx512`, le varianti `avx2v`/`avx512v` (aritmetica della curva su corsie SIMD) e `avx512i` (la stessa con
  AVX-512 **IFMA**, sui processori che la hanno: Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids, Zen 4 e
  successivi; è circa 1,5 volte `avx512v`). Ogni motore, prima di essere usato, supera una verifica rapida
  con una chiave nota: un motore che sbagliasse verrebbe escluso.
* Il canguro sulla CPU usa le stesse corsie SIMD della ricerca lineare (con IFMA ≈ 4 volte il passo scalare della 1.4.0); la
  variante viene scelta misurando, dopo un confronto bit per bit con quella di riferimento. `bench` indica quale è in uso e
  misura ogni variante con 1 thread, un thread per core fisico e tutti i thread logici, più la combinazione "mista" su
  hyper-threading (vedi `PZ_KANG_MIX`).
* Il canguro con più RAM spreca meno (`--mem` grande consente `dp-bits` più bassi); il BSGS è limitato soltanto dalla RAM.
* **Pagine grandi**: per il BSGS conviene `echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled` (spesso già così).
* **Portatile: la velocità a regime è più bassa del picco.** Nei primi secondi CPU e GPU corrono alla massima potenza, poi il raffreddamento e il
  limite di potenza condivisa le rallentano (per esempio una GPU da portatile è passata da ~790 M a ~606 M passi/s in 3 minuti). La taratura
  automatica dura pochi secondi per configurazione e vede soprattutto il picco: per scegliere `-t` giudica la riga "Velocita'" dopo 5-10 minuti di
  campagna, confrontando per esempio `hunt -p 140` e `hunt -p 140 -t 8`.
* Non usare più processi sulla stessa campagna (il blocco lo impedisce: il secondo si ferma con un errore).
* Per sessioni lunghe: `nohup pzsolve hunt -p N --ui log > /dev/null &` e controlla `pzsolve list` o `report.txt`.
* **Realismo**: `pzsolve puzzle N` stima il tempo sulla CPU di questa macchina; le GPU si sommano. I puzzle 71+ senza chiave pubblica
  richiedono 10^20…10^40 chiavi in media: leggi quei numeri prima di lanciare una campagna.

### Più computer sulla stessa campagna canguro (dalla 1.7.3)

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
Il seed della campagna di A è nella riga «Seed» di `campaigns/p140/report.txt` (e in `campaign.conf`). Ogni tanto si porta il `dp.bin` di B su A e, **con la campagna di A
ferma**, si unisce: `pzsolve merge -p 140 -f percorso\di\B\p140` (un file `dp.bin`, oppure la cartella della campagna, o più `-f`). Il comando
dice quanti punti sono nuovi e se c'è una collisione fra un punto "domato" e uno "selvatico": in quel caso trova la chiave, la scrive in
`FOUND.txt` e la mostra. Si può anche unire in senso inverso (A su B) o in entrambi: i punti già noti vengono riconosciuti. Dopo l'unione `hunt`
riprende da dove era, con la tabella arricchita. Un registro di un'altra campagna (intervallo o chiave diversi) viene rifiutato, e lo stesso vale per uno scritto da una
vecchia disposizione del canguro (versioni fino alla 1.9.0 nelle campagne con meno di 256 canguri, cioè intervalli piccoli): `hunt` lo conserva come `dp.bin.vN` e prosegue con un registro nuovo, `merge` spiega perché non può usarlo. Per la ricerca lineare vedi la sezione che segue.

### Più computer sulla ricerca lineare (dalla 1.7.5)

La ricerca lineare visita i blocchi dell'intervallo in un ordine casuale deciso dal seed. Con `--part Q/T` quell'ordine si divide in `T`
quote, a turno (il blocco in posizione 0 alla quota 0, quello in posizione 1 alla quota 1, …, poi di nuovo da capo), e il computer scandisce
solo le quote `Q` che gli spettano: le quote di computer diversi non si sovrappongono mai e insieme coprono tutto l'intervallo, senza
che i computer comunichino. Il blocco corto di fondo intervallo, se c'è, è contato esattamente.

```
computer A:  pzsolve hunt -p 71 -s 5f3a…(64 cifre) --part 0/2
computer B:  pzsolve hunt -p 71 -s 5f3a…(stesso)   --part 1/2
```
* **Uguali su tutti**: puzzle o intervallo, seed (`-s`: lo trovi nella riga «Seed» di `report.txt` e nel log all'avvio) e `--chunk-bits`
  (se non lo indichi dipende solo dall'intervallo, quindi è già uguale). **Diverse**: le quote.
* **Computer di velocità diverse**: dai più quote al più veloce. Con `pzsolve split 800M 120M` ottieni la divisione in proporzione
  (qui `0-86/100` e `87-99/100`); `--part 0-86/100` è lo stesso di `0,1,2,…,86/100`. Una quota per computer basta se sono uguali.
* **La parte è della campagna**: si decide alla creazione, non si cambia (`--part` diverso su una campagna esistente è rifiutato, usa `-c` per
  un'altra) e riprendendo senza `--part` resta quella. Anche il file di stato la ricorda, quindi non si può caricare per sbaglio uno stato
  di un'altra quota. Per far continuare una parte a un altro computer basta copiare la cartella della campagna.
* **Cosa vedi**: l'avanzamento è quello della *tua* parte (al 100 % hai finito le tue quote, non l'intervallo). Le probabilità sono
  calcolate sull'intero intervallo e contano solo il lavoro di questo computer; i tempi oltre la sua parte sono «n/d» (da solo non ci arriva).
  `where` dice se una chiave cade nella tua parte.
* **Se un computer si ferma**, le sue quote restano ferme: rilancia la sua campagna o copiala altrove. **Quando la parte finisce** il
  programma lo dice e si ferma; la chiave, se non è lì, è in una delle altre quote.
* Solo per la ricerca lineare. Con la chiave pubblica nota (canguro) si usa `--node` e `merge`; il BSGS non si divide.
* Servono almeno tanti blocchi quante sono le quote (`--chunk-bits` più piccolo se l'intervallo è corto).

## 9. Risoluzione dei problemi

**Prima di tutto: `pzsolve doctor`.** Controlla in una pagina il programma, il sistema, la CPU (verifica rapida di ogni motore con una chiave
nota), le GPU (apertura del dispositivo e confronto bit per bit con la CPU), la taratura e il profilo salvati, le campagne della cartella
(ferma o in esecuzione, ultimo salvataggio, sospensioni subite), lo spazio su disco e la scrittura atomica, l'orologio e le variabili `PZ_*`.
Le righe `[XX]` sono errori, `[!!]` avvisi. `doctor --full` esegue anche l'autotest intero; `-d DIR` indica la cartella delle campagne.

**Sospensione e ibernazione.** Se il computer va in sospensione o in ibernazione mentre una campagna gira, dalla 1.7.2:
* il programma si accorge del salto di tempo (il pannello non si ferma ne' mostra velocità assurde), non conta il tempo sospeso né nei tempi
  né nelle velocità, scrive nel log `sospensione del sistema rilevata (circa …)` e salva subito lo stato;
* se dopo il risveglio il driver ha azzerato la GPU e un lancio fallisce, la GPU viene riaperta, ricontrollata con lo stesso autotest dell'avvio
  e rimessa al lavoro (`dispositivo ripreso`; il branco del canguro sulla GPU riparte da posizioni nuove: si perde solo il percorso dall'ultimo
  punto distinto, i punti già trovati sono al sicuro). Fino a 3 errori di fila e 40 in tutto, poi la GPU viene esclusa come prima;
* `hunt --keep-awake` impedisce a Windows di sospendersi finché la campagna gira (la sospensione torna possibile alla fine). Su Linux: `systemd-inhibit`.
Lo stato su disco non è mai a rischio: ogni scrittura è atomica e il checkpoint è ogni 60 s.

| Sintomo | Causa probabile |
|---------|-----------------|
| `la campagna in … e' gia' in esecuzione` | un altro processo usa la cartella: fermalo o usa un altro nome (`-c`) |
| `la campagna esistente usa la modalita' …` / `--range non coincide …` / `il seed indicato non coincide …` | hai cambiato in ripresa un parametro che definisce la campagna: toglilo (si riprende com'era) o usa un altro nome con `-c` |
| `salvataggio finale dello stato FALLITO` | disco pieno o cartella non scrivibile: libera spazio e rilancia (il checkpoint precedente è intatto) |
| `checkpoint principale non utilizzabile` / `nessun checkpoint utilizzabile` | `state.dat` corrotto: viene tentato `.bak`; altrimenti rimuovilo (si ricomincia) |
| `il checkpoint e' di un'altra ripartizione fra computer` / `lo stato … appartiene a un'altra campagna o a un'altra parte` | lo stato è integro ma è di un'altra `--part` o di un'altra campagna: non toccarlo, usa la campagna giusta o un altro nome con `-c` |
| `il file dei punti distinti … non appartiene a questa campagna` | `dp.bin` di un'altra campagna: spostalo |
| `la tabella BSGS richiede … ma sono disponibili solo …` | poca RAM: usa `--mem`, `--bsgs-entries` più piccolo (alla creazione) o `--mode kangaroo` |
| `tabella … non utilizzabile` / `corrotta` | file `--table` troncato o di un'altra versione: cancellalo, verrà ricostruito |
| `self-test fallito, dispositivo escluso` | driver difettoso o compilatore del kernel sbagliato: aggiorna il driver; per una NVIDIA il programma prova anche OpenCL; segnala il caso |
| `CUDA: il driver e' troppo vecchio per questo PTX` | aggiorna il driver NVIDIA (525 o successivo), oppure usa `--gpu-api opencl` |
| `driver CUDA non trovato` (in `devices`) | normale senza scheda NVIDIA; con una NVIDIA, reinstalla il driver grafico |
| `la CPU non supporta il set di istruzioni …` | usa `--isa auto` o `--isa baseline` |
| `tabella dei punti distinti piena … dimezzo la densità` | il limite di RAM è stato raggiunto in una corsa molto più lunga del previsto: la campagna prosegue con meno efficienza; riprendi con `--mem` più grande per evitarlo |
| velocità molto bassa | controlla `devices` (quante lane/GPU usate) e che nessun altro processo sia in esecuzione |
| prima esecuzione del BSGS lenta | la RAM appena allocata viene "toccata" per la prima volta (su alcune macchine virtuali costa secondi per GiB) |

## 10. Windows

`pzsolve.exe` (dalla pagina Releases) è un eseguibile a 64 bit per Windows 10/11, statico: usa solo DLL di sistema e non va installato. Si usa da
**PowerShell** o dal Prompt dei comandi, con gli stessi comandi e opzioni di Linux (`.\pzsolve.exe hunt -p 71`, ecc.). Con un doppio
clic mostra l'aiuto e attende Invio.

* **Cartelle**: le campagne vanno in `campaigns\` nella cartella da cui lanci il comando (o in quella indicata con `-d`); funzionano
  anche percorsi con spazi tra virgolette (`-d "D:\Le mie campagne"`). I file di testo (`report.txt`, `status.json`, ...) si aprono con
  il Blocco note.
* **Fermarsi**: Ctrl-C (o Ctrl-Interr) salva e chiude. Anche **chiudere la finestra**, disconnettersi o spegnere il PC: Windows concede
  qualche secondo e il programma lo usa per salvare lo stato (l'ultimo minuto di lavoro al massimo può andare perso).
* **Sessioni lunghe**: lascia aperta la finestra di PowerShell; il pannello si aggiorna sul posto (Windows Terminal o la console di
  Windows 10 1607+). Per impedire che il PC vada in sospensione durante la notte: Impostazioni → Sistema → Alimentazione.
  Sui portatili tieni l'alimentatore collegato e la modalità "Prestazioni migliori".
* **Memoria**: i limiti sono calcolati sulla RAM disponibile riportata da Windows (come su Linux: BSGS al massimo il 70 %, canguro il
  40 %, modificabili con `--mem GB`). Windows deve poter "impegnare" la memoria: con file di paging disattivato o piccolo una tabella
  BSGS molto grande può essere rifiutata (il programma lo dice); lascia il file di paging gestito dal sistema.
* **Pagine grandi** (opzionali, utili solo per il BSGS con tabelle ≥ 1 GiB): servono il diritto "Blocco di pagine in memoria"
  (`secpol.msc` → Criteri locali → Assegnazione diritti utente → *Blocco di pagine in memoria*: aggiungi il tuo utente, poi esci e
  rientra) e l'avvio di PowerShell come amministratore. Senza, il programma usa le pagine normali; `PZ_NO_LARGE_PAGES=1` le disattiva.
* **Antivirus**: un programma che usa a lungo tutta la CPU può insospettire alcuni antivirus. Il codice sorgente è nel repository e
  `SHA256SUMS` permette di verificare che l'exe sia quello pubblicato; se Windows Defender lo blocca puoi aggiungere un'esclusione
  per la cartella.
* **Compilarlo da sé**: da Linux con MinGW-w64, `make windows` (risultato in `build/win/pzsolve.exe`); `make wintest` esegue i test
  dell'exe sotto Wine.
