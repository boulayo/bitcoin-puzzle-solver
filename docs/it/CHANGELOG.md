# Changelog

Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.1.0/); versionamento [semantico](https://semver.org/lang/it/).
Le versioni precedenti restano consultabili (binario + sorgenti) in `archive/vX.Y.Z/` e tramite i tag git `vX.Y.Z`.

## [1.7.9] — 2026-10-23

### Canguro: schema 3, circa il 25 % di passi in meno sui puzzle aperti
- **Difetto di stima trovato**: con le classi `±` le camminate non hanno deriva e restano locali (regione ≈ `√n·s`). Con camminate lunghe e pochi
  camminatori conta il numero di coppie domato–selvatico che possono incrociarsi, `P ≈ (salto/W^¾)·K^1,5`. Sul puzzle 135 con una GPU e la CPU
  (`K ≈ 2^19,4`) e il salto di `12·√W` degli schemi 1 e 2 vale ≈ 0,6: in un modello con gli stessi parametri adimensionali il costo è **1,34–1,38·√W**,
  non 1,10–1,15 (sulla curva vera, 64 camminatori su `2^40`: 1,548 contro 1,299 con salti lunghi). Le stime di 8.7 e 8.8 valevano solo per camminate corte.
- **Schema 3** per le campagne nuove: salto medio `W^¾/16` (`P ≈ K^1,5/16`: 2^25 sul puzzle 135) e zone di partenza divise in fasce con quote ottimizzate
  (51 fasce, `src/kang_spec3.inc`). Nel modello vicino al caso reale: **1,018** (schema 2 con gli stessi salti 1,049, schema 2 attuale 1,380). Sulla curva vera,
  da `2^32` a `2^44`: **1,060** in media (schema 2 1,100, schema 1 1,164); `KANG_C_MEAN` = 1,06.
- Il passo non cambia (CPU e GPU). Ogni camminatore tiene il suo gruppo di specie e residuo e a ogni partenza estrae la fascia: con pochi camminatori una
  fascia fissa lasciava scoperti alcuni residui (trovato dall'autotest: chiave non trovata su `2^24` con 32 camminatori).
- Registro in formato `PZDP4`; le campagne degli schemi 1 e 2 restano come sono. Se il loro `P` è sotto 64 il programma avverte all'avvio che il costo atteso è
  molto più alto e consiglia una campagna nuova. `PZ_KANG_SCHEMA=1|2|3`; `merge` riconosce lo schema 3.
- Provato e scartato: una quarta specie (nel modello ideale l'ottimizzatore le dà quota zero); le ripartenze periodiche dei camminatori (aiutano solo con `P`
  piccolo, che lo schema 3 evita).
- Strumenti: `tools/kbands.py` (fasce), `tools/kspec_table.py` (tabella C dal file di disposizione), `tools/kang_schema3.txt`; `ksim` con fino a 256
  specie, `rsmin=N` (ripartenze) e conteggio di fusioni e ripartenze. Variabili di prova `PZ_TEST_KTHREAD` e `PZ_TEST_JUMP`.
- Autotest: chiavi trovate con tutti e tre gli schemi, unione di registri dello schema 3, soluzione delle collisioni con lo schema 3.

### Analisi (nessun cambiamento del comportamento)
- **RAM e precalcolo contro il fattore lineare** (`docs/it/ALGORITMI.md` 8.9, `tools/blsim.c`). Per una sola chiave la memoria abbassa il fattore solo
  quando è dell'ordine di √W: BSGS 1,0, intercalato 0,94, strutturato 0,82, con `2^66` voci per il puzzle 135. Il precalcolo alla
  Bernstein–Lange (tabella di `T` punti distinti, poi ricerca da √(W/T)) è misurato nel modello: precalcolo × ricerca ≥ ≈ 2,5·W a 2^32, 2^36 e
  2^40 (letteratura 2,34·W), quindi per una chiave il totale minimo è ≈ 3·√W, il triplo del canguro. Per il puzzle 135 con 64 GiB la ricerca
  scenderebbe a ≈ 70 giorni dopo un precalcolo di ≈ 2,6·10^8 anni (il canguro: ≈ 4 700 anni). Conviene solo da circa 9 chiavi della stessa
  ampiezza in su; i puzzle aperti con chiave pubblica sono 6, di ampiezze molto diverse.

## [1.7.8] — 2026-10-23

### Canguro: meno passi con lo schema 2 (specie con residui fissi mod 12)
- Le campagne canguro **nuove** usano tre specie di camminatori invece di due: domati vicino allo zero, selvatici su `Q` e selvatici su `3Q`
  (partenza `u·G + 3·Q'`), ognuna con le partenze in classi di residui fisse mod 12 e **tutti i salti multipli di 12**, così il residuo di un
  camminatore non cambia mai. Le specie sono scelte perché, per ogni residuo della chiave, ci siano più coppie di camminatori che possono
  incontrarsi con una soluzione intera. Nel modello di copertura casuale il costo scende da 1,085 a **1,045·√W**; nel modello delle camminate, con
  molti camminatori (il regime dei puzzle aperti), da 1,092 a 1,044. Sulla curva vera (`kstat`): da 1,164 a **1,100·√W** in media da `2^32` a `2^44` (a `2^44`: 1,188 → 1,071), cioè −5,5 %.
  Dettagli, tabella delle specie e misure in `docs/it/ALGORITMI.md` 8.8.
- **Il passo non cambia** (né su CPU né su GPU, kernel compresi): cambiano tabella dei salti, partenze e soluzione delle collisioni
  (`(c₁ − c₂)·x = d₂ − d₁` con `c ∈ {0, ±1, ±3}`, divisione esatta). Il codice di tipo a 3 bit è ricavato sull'host dal codice a 2 bit del passo.
- Con meno di 256 camminatori (intervalli sotto `2^32` sulla sola CPU) lo schema 2 costa di più (64 camminatori a `2^28`: 1,31 contro 1,22·√W,
  lo stesso nel modello): lì una campagna nuova usa ancora lo schema 1.
- **Compatibilità**: lo schema è fissato alla creazione della campagna e letto dall'intestazione di `dp.bin`. Le campagne iniziate con la 1.7.7 o prima
  restano sullo schema 1, identico a prima. Lo schema 2 scrive il formato 3 del registro (`PZDP3`, base = inizio intervallo). I punti dei due schemi
  non si incontrano: `merge` rifiuta un registro dell'altro schema con la spiegazione; `PZ_KANG_SCHEMA=1|2` sceglie lo schema di una campagna nuova
  (serve per aggiungere un computer a una campagna dello schema 1). La riga «canguro: schema N» del log dice quale è in uso.
- Stime di tempo: `KANG_C_MEAN` 1,10 per lo schema 2 (misurato), 1,15 per lo schema 1.
- Strumenti: `tools/ksim.c` accetta `design=FILE` (specie, residui, zone, quote; `tools/kang_schema2.txt` è lo schema 2), `tools/kspecies.py` cerca le
  specie nel modello esatto di copertura.
- Autotest: soluzione delle collisioni per tutte le 20 coppie di tipi con coefficiente diverso (divisioni per 1, 2, 3, 4, 6) su punti veri, unione di
  due registri in entrambi gli schemi, chiavi trovate con entrambi gli schemi (anche agli estremi dell'intervallo). `verify.sh`: una campagna dello
  schema 1 resta sullo schema 1 alla ripresa, una nuova usa lo schema 2, il `merge` fra schemi diversi è rifiutato.

### Correzione
- Il profilo della macchina (e la velocità ricordata) era salvato sotto una chiave che conteneva il passo CPU del canguro **scelto cronometrando**
  (dalla 1.7.7 IFMA a corsie o a coppie, spesso a meno del 3 % l'uno dall'altro): se una esecuzione successiva sceglieva l'altra forma, il profilo
  non veniva ritrovato e la taratura ripartiva. Ora la chiave usa l'insieme di istruzioni della CPU (fisso per la macchina); lo stesso per il
  motore della ricerca lineare. Emerso come prova instabile di `verify.sh` («profilo riusato (canguro)»).

## [1.7.7] — 2026-10-23

### Canguro CPU con IFMA: seconda forma del passo, scelta dal programma
- `kang_round_avx512i2` (`KV_PAIR` in `kang_vec_impl.inc`): la seconda metà del passo (inverso a ritroso, pendenza, quadrato, sottrazioni, prodotti,
  normalizzazione, distanza, eventi) si esegue per **due vettori affiancati** invece di uno. Ipotesi: la catena di un vettore (≈ 1000 istruzioni
  dipendenti) supera la finestra fuori ordine e le unità IFMA aspettano le latenze (`llvm-mca`: ≈ 550 cicli per vettore contro ≈ 260 di capacità);
  i ≈ 12 ns sopra il minimo aritmetico di 8.4 potrebbero venire di lì. **Non misurato su una CPU con IFMA** (non ne ho una): per questo il programma
  cronometra le due forme al primo uso, con tutte le CPU logiche occupate, e adotta quella a coppie solo se vince di almeno il 3 %; senza IFMA non cambia
  nulla. Stessi calcoli, stessi eventi nello stesso ordine (identica bit per bit al passo di riferimento). `PZ_KANG_ROUND=avx512i|avx512i2` la forza; `bench` la elenca.
  Provata anche su AVX-512 senza IFMA e su AVX2: nessun vantaggio (9,7–10,7 contro 10,2–10,6 M passi/s), non aggiunta.
- `tools/ifma_emul.h` emula `vpmadd52luq/huq` con AVX-512F: `verify.sh` esegue autotest e una campagna di canguro con il passo a corsie, quello a coppie e il motore
  `avx512i` su qualunque CPU con AVX-512, IFMA o no (prima solo con IFMA o sotto SDE).

### Analisi (nessun cambiamento del comportamento)
- **Karatsuba sul prodotto GPU a 32 bit: misurato, perde** (+35 % additivo, +39 % sottrattivo di istruzioni sul prodotto nudo, con `ptxas`/`nvdisasm` 12.0 per sm_86). Il prodotto
  per righe con una catena di riporti toglie il 4 % al prodotto ma nulla alla moltiplicazione completa; il quadrato −15 %, cioè ≈ −1 % del passo: sotto il 2 % chiesto, non adottato.
  Con IFMA Karatsuba non entra (somme di limbi da 52 bit). `tools/sass_count.py` e `tools/sass/` rendono le misure ripetibili. Dettagli in `docs/it/ALGORITMI.md` 8.6.
- **Limite per conteggio delle coppie rifatto con valori interi** (`docs/it/ALGORITMI.md` 8.7, `tools/bounds.py`): l'ottimo del modello con specie `b = 0, 1, …` è 0,8165·√W
  (frazione tame 1/3, quella già usata), non 0,67; con copertura casuale 1,0854·√W, a cui il modello astratto (`ksim`) tende al crescere dei camminatori (1,17 con 64, 1,10 con 4096 senza ritardo dei punti distinti; il
  sovraccosto dei punti distinti è ≈ 1–2·K·2^dp/√W, trascurabile alle scale dei puzzle aperti): il costo atteso a quelle scale sarebbe ≈ 1,09, non 1,15 (da confermare: sulla curva vera
  non si possono misurare `K` e `W` così diversi). BSGS intercalato 0,9428·√W solo con più del doppio della memoria. Nessuna costruzione sotto 1,085 con memoria sotto-lineare.

## [1.7.6] — 2026-10-23

### Usare solo una parte della macchina: `--limit X`
- `hunt --limit X` (1..99, spento di default, 100 = spento): usa il X % dei thread CPU (arrotondato, almeno uno; con `-t` il X % dei thread indicati) e il X % dei
  work-item delle GPU (a blocchi di 128, almeno uno; ricerca lineare e canguro), **sempre a pieno ritmo e senza pause**. Una versione con riposi proporzionati al lavoro
  fu scartata: i riposi a ondate fanno partire e fermare la ventola; con meno unità al massimo il carico è costante.
- I thread CPU vanno prima su core fisici diversi (uno per core), poi sui secondi thread SMT (`sysinfo_spread_order`: l'ordine dei CPU è riordinato prima
  dell'assegnazione; vale per Linux, dove i fratelli SMT sono in fondo, e per Windows, dove sono vicini).
- Niente taratura a pieno carico e niente profilo con `--limit`; la velocità a regime si ricorda a parte (chiave `|lim=N`) per le stime del tempo, `doctor` la mostra
  con «limite N%». `--retune` con `--limit` è rifiutato. Il pannello, `report.txt` e `status.json` (`limit_percent`) riportano il limite; il limite non è salvato nella campagna.
- Autotest: arrotondamenti di thread e work-item (minimo uno, mai oltre il totale, spento), riordino dei CPU con numerazione Windows e Linux; velocità ricordata separata per limite;
  pannello a ogni larghezza. End-to-end: lineare, canguro e BSGS con `--limit` trovano la chiave con il numero atteso di thread, GPU simulata (CUDA) lineare e canguro, valori non validi rifiutati.
- Limite noto: l'effetto sui watt di una GPU vera non è stato misurato (nei test la GPU è simulata).

## [1.7.5] — 2026-10-22

### Più computer sulla ricerca lineare: `--part`
- `hunt --part Q/T`: i blocchi dell'ordine di visita (la permutazione del seed) si dividono in `T` quote a turno e questo computer scandisce
  le quote `Q` (numerate da 0; anche più d'una e a intervalli: `0,1/4`, `0-86/100`). Le quote di computer diversi non si sovrappongono e insieme
  coprono tutto, senza che i computer comunichino; serve lo stesso intervallo, seed (`-s`) e `--chunk-bits`. Il blocco corto di fondo intervallo,
  se c'è, è contato esattamente (la dimensione della parte è esatta). Solo per la ricerca lineare: il canguro ha `--node` e `merge`.
- La parte è della campagna: salvata in `campaign.conf` (chiave `part`, assente nelle campagne vecchie, che continuano a funzionare), riportata in `report.txt`
  e `status.json`, ricordata dal file di stato (riga `part=`; un checkpoint di un'altra quota, o di nessuna, è rifiutato anche quando le dimensioni coincidono).
  `--part` diverso su una campagna esistente è rifiutato; riprendendo senza `--part` resta quella. Si valida prima di creare la cartella
  (ogni quota deve avere almeno un blocco).
- Avanzamento della *propria* parte; probabilità calcolate sull'intero intervallo con tetto alla parte raggiungibile (`reach` nel modello di stima:
  i tempi oltre la parte sono «n/d»); `where` dice se una chiave cade nella propria parte; a fine parte il programma lo dice (non «campagna completata»).
- Nuovo comando `pzsolve split V1 V2 …`: divide 100 quote in proporzione alle velocità (`split 800M 120M` → `0-86/100` e `87-99/100`), ogni computer almeno una.
- Autotest: sintassi e forma canonica delle quote, copertura esatta senza doppioni di tre divisioni su tre intervalli (blocco corto compreso), posizioni, checkpoint di
  quote diverse, troppe quote per i blocchi, intervallo enorme (spazio virtuale tagliato), modello di stima con tetto, quote proporzionali; fuzz del parser; prove
  end-to-end con due `--part`. Verificato con mutazioni (tre difetti introdotti a mano sono tutti rilevati).

### Correzioni
- **`FOUND.txt` (chiave privata) veniva creato con permessi 0644 invece di 0600**: la funzione di scrittura atomica ignorava il parametro dei permessi da quando
  esiste lo strato di piattaforma (1.2.0), quindi su Linux il file era leggibile da tutti gli utenti del computer. Ora è 0600 (su Windows decide la cartella). Test unitario ed end-to-end.
- Il report di campagna (`report.txt`) ora riporta anche il **seed completo** (riga «Seed»): la guida diceva di cercarlo lì, ma c'era solo la sigla; serve per usare lo stesso seed su un altro computer.
- `kstat` e l'autotest non controllavano l'esito di un `malloc`/`calloc` (ora abortiscono con un messaggio, come il resto del programma).
- **Registro dei punti distinti (`dp.bin`) con il disco pieno**: se la scrittura falliva (disco pieno, quota, errore di I/O) il buffer in memoria andava
  oltre la sua fine (overflow dell'heap, trovato con ASan riunendo un registro grande sotto un limite di dimensione). Ora il buffer cresce (fino a 128 MiB) e si riprova
  al checkpoint successivo; una scrittura interrotta a metà di un record viene riparata (il file è riportato a record interi e completato senza buchi né doppioni);
  oltre il limite i nuovi punti restano solo in RAM con un avviso. Autotest con `/dev/full` e con un limite di dimensione del file che cade dentro un record.
- **Tabella dei punti distinti senza memoria**: se `calloc` falliva nell'ingrandire la tabella il carico saliva fino a bloccare la ricerca nel ciclo di sonda (con il blocco preso); se
  falliva nel diradarla, ogni punto veniva scartato in silenzio dopo poche decine di inserimenti. Ora si ripiega sul diradamento, si avvisa, e l'ultimo posto della tabella non si riempie mai.
- **Sospensione su Linux**: il rilevamento (1.7.2) sottraeva dal tempo della sessione anche le pause che l'orologio monotono non aveva contato (sospensione di Linux, orologio di
  sistema spostato in avanti): `elapsed` poteva diventare negativo, finire in `session.txt`, e le stime restavano «in misura» per tutta la pausa; il primo campione dopo la pausa
  dava una velocità di decine di volte quella vera, poi ricordata. Ora si sottrae solo ciò che l'orologio monotono ha contato (processo congelato, Windows); il caso solo-orologio-di-sistema si segnala e basta.
- **`where` oltre i 128 bit** (puzzle 166 e oltre, blocchi da 2^36): la posizione del blocco veniva troncata, quindi numero di blocco, percentuale e (con `--part`) il verdetto «è nella tua parte»
  erano sbagliati. La posizione è ora a 256 bit. Il numero dei blocchi della parte, per gli spazi virtuali tagliati a 2^126, era anch'esso sbagliato.
- **`hunt -p 256`** (e dal puzzle 230 in su con i blocchi predefiniti) creava la cartella della campagna e poi moriva con «dominio troppo grande» a ogni riavvio: ora il controllo precede la creazione e
  il messaggio dice cosa fare (`--chunk-bits 40` fino al puzzle 232, oppure `-r` con un sotto-intervallo).
- Una GPU con CUDA/OpenCL perdeva il buffer della storia dei salti del canguro (`b_hist`) a ogni sessione (chiusura della GPU); un buffer CUDA la cui copia iniziale falliva perdeva la memoria della scheda; il self-test della GPU
  allocava in base a un conteggio non ancora validato letto da un dispositivo che poteva rispondere a vuoto.
- La velocità a regime ricordata veniva salvata con un numero di thread (meno i core riservati alle GPU OpenCL) diverso da quello della ricerca: non si ritrovava mai. La chiave ora si fissa all'avvio e
  distingue thread scelti da thread predefiniti. Senza lavoro per cinque minuti la velocità ricordata non tiene più in vita la stima; con velocità ignota le probabilità sono «n/d», non 0 %.
- Stima del canguro nei comandi `puzzle` e `puzzles` e regola `auto` fra BSGS e canguro: usavano ancora 1,27·√W (prima della 1.6.0) invece di 1,15·√W; ora una sola costante. La guida della teoria dava c = 1,43·√W.
- Un checkpoint integro ma di un'altra campagna o di un'altra parte non è più descritto come «corrotto» (il messaggio consigliava di cancellarlo): ora dice di che si tratta e che non è stato toccato.
- Variabili `PZ_NO_CUDA`, `PZ_NO_OPENCL`, `PZ_NO_PROFILE`, `PZ_NO_LARGE_PAGES`, `PZ_OPENCL_ALL`, `PZ_KANG_VERBOSE` valgono ora con qualunque valore tranne vuoto e `0` (prima `=0` le attivava).
- Il caricatore OpenCL non pretende più un simbolo (`clGetKernelWorkGroupInfo`) che il programma non chiama mai: un driver senza non disabilita più OpenCL. `bench` oltre 256 thread logici non sfora più gli array.
- `report.txt` ora ha la riga «Seed»; la guida e il README non dicevano più il vero in vari punti (versione, Windows nativo, CUDA, `--mem`, `devices`, opzioni mancanti, messaggi, variabili d'ambiente, `VERSIONAMENTO.md`, `ARCHITETTURA.md`, `tools/README.md`).

### Codice morto e pulizia (nessun cambiamento di comportamento)
- Tolti anche i campi mai letti `run_ctx.resumes`, `run_ctx.hits_total`, `kang_state.dps_total`, `bs_run.giants_at_start`, `rt_dev.cl_plat`, l'opzione memorizzata `gpu_api`, due controlli già fatti da `check_isa_option`, una condizione sempre vera di `build_new_cfg`, il controllo di una lista vuota in `part_parse` e `u128_from_u256_checked`.
- Tolti: `gpu_session_name`, `pz_lib_close`, `pz_ui_block_clear`, `u256_negmod_n`, i campi mai letti `worker_stat.aux`, `kang_state.full_warned`, `tg_scan_cfg.what`,
  i parametri ignorati di `sched_save`, `run_reporter_loop`, `make_summary` e `gpu_*_join`, le costanti OpenCL mai usate, due righe che calcolavano un valore scartato
  (`ui.c`, `u128_to_hex`), un argomento `"" : ""` di `doctor`, una condizione sempre vera in `t_jobq`, una dichiarazione doppia.
- Due funzioni annidate (estensione GNU, non portabile e da evitare con trampolini) sono ora funzioni statiche; un'espressione contorta nel salvataggio della taratura è semplificata.
- Guida e teoria aggiornate: la sezione sulle stime del tempo descriveva ancora la media mobile da 30 s (cambiata nella 1.7.4).

## [1.7.4] — 2026-10-22

### Priorità dei thread che servono le schede (idea dal lavoro sul rilevatore)
- I thread che lanciano i kernel e raccolgono i risultati possono salire sopra i thread di calcolo (`ABOVE_NORMAL` su Windows, `nice -5` su Linux con
  root o `CAP_SYS_NICE`; relativo al valore che il thread aveva). Servono a non far aspettare la scheda quando tutti i core sono occupati.
- La taratura della prima esecuzione (e `bench --together`) lo misura a coppie alternate con tutti i thread CPU e lo tiene solo se vince di almeno il 2 %
  in complesso e in entrambe le coppie (la seconda coppia solo se la prima supera l'1 %). Se vince, tutte le misure che seguono sono fatte con la priorità
  alta; il profilo salvato ricorda `prio=1`.
- Nuova opzione `--prio-gpu auto|yes|no` per `hunt` e `bench --together` (la scelta forzata ha una voce di profilo a parte). `doctor` dice se il sistema lo consente.
- Autotest: verdetto delle misure (casi netto, sotto soglia, coppia contraria, più lento, misura nulla) ed effetto reale sul livello di priorità di un thread, con ripristino.
  Il test `verify` prova opzione, misura nel `bench --together` (canguro e lineare) e `hunt --prio-gpu yes` con la GPU simulata.

### Stima di velocità e tempo rimanente realistica fin dall'inizio
- Il tempo residuo si calcolava da una media mobile di 30 s **iniziata dal primo campione** del primo secondo, quando la GPU non è ancora contata (un lancio
  si conta solo a fine lancio): partiva da una velocità troppo bassa e ci metteva un paio di minuti a recuperare (simulazione coi numeri della tua macchina, CPU 190 M/s e GPU 800 M/s contata dall'8° secondo: velocità usata −81 % nei primi secondi, −41 % a 30 s, −15 % a 60 s, −2 % a 2 minuti, cioè un tempo residuo 1,7 volte troppo lungo a 30 s). Ora non compare finché ogni motore ha
  prodotto qualcosa (riga «in misura», etichetta «[in avvio …]» sulla velocità), e poi usa la media su una finestra scorrevole fino a 2 minuti.
- Parte da una **velocità ricordata**: la velocità *a regime* (da 90 s dopo l'avvio, finestra fino a 10 minuti) raggiunta da una corsa precedente della stessa
  configurazione (macchina, versione, motore, thread, GPU, priorità), salvata nel profilo ogni 5 minuti e alla fine (solo dopo 3,5 minuti di corsa);
  alla prima corsa in assoluto, quella misurata a freddo dalla taratura (che sulla tua macchina è il 19 % sopra quella a regime: per questo la riga
  «stima provvisoria» lo dice). La misura prende il posto del ricordo in 150 s. Se la corsa si ferma del tutto, il ricordo non tiene in vita la stima.
- Il salto di tempo di una sospensione non entra nella finestra. Il BSGS resta con la media mobile (cambia velocità alla fine della tabella).
- Autotest: stimatore su una corsa sintetica (GPU che entra all'8° secondo, macchina che rallenta del 25 % in 3 minuti, sospensione di 10 minuti, anello
  che gira, stallo) e memoria per configurazione (`velocita' ricordata per configurazione`). `doctor` elenca le velocità ricordate.

## [1.7.3] — 2026-10-22

### Più computer sulla stessa campagna canguro
- `hunt --node N`: numero del computer; entra nel flusso casuale delle partenze dei canguri (`epoch = sessione | nodo << 32`), quindi due
  computer con stesso seed, intervallo e chiave pubblica camminano su punti diversi; il nodo 0 è identico a prima (le campagne in corso non cambiano).
- Nuovo comando `merge -c NOME|-p N [-d DIR] -f FILE_O_CARTELLA …`: aggiunge i punti distinti di altri registri `dp.bin` alla campagna
  (tabella e registro), con rilevamento della collisione fra punti domati e selvatici; rifiuta i registri di un'altra campagna e i file illeggibili;
  prende il blocco della campagna (non si unisce in una campagna in esecuzione). Dimostrato con un test deterministico nell'autotest (`unione dei punti
  distinti di due computer`): due registri con metà collisione ciascuno non risolvono da soli, uniti trovano la chiave.
- Il seed deve essere lo stesso (tabella dei salti): due camminate con tabelle diverse non si incontrano. `--dp-bits` può differire, ma
  conviene uguale.

### GPU
- La taratura sceglie la variante **più veloce** invece della prima che supera il predefinito del 3 % (su una GPU NVIDIA da portatile: NK 64 coppie 815,9 M contro
  NK 256 coppie 830,9 M, il 2 % lasciato sul tavolo; lo stesso per il kernel lineare).
- Nuove varianti del kernel del canguro per CUDA con i prodotti del lotto tenuti per un camminatore su **quattro** (`NK 64/128 (prodotti a quattro)`,
  `PZ_GPU_NK=64c`): −16 byte per passo (≈ −8 %) ma 212–220 registri, cioè due blocchi per SM invece di quattro; la taratura le adotta solo se sono più veloci.
  I kernel sono verificati bit per bit anche su OpenCL (pocl) a ogni autotest.
- Provata e scartata: un anello per i tag `prev`/`prev2` (−4 byte per passo) e la sola scrittura nell'ultimo round: `ptxas` introduceva spill nel kernel
  vincente (`NK 64` a coppie). L'autotest ora prova anche lanci che cominciano e finiscono a passi dispari e con numero di passo iniziale diverso da zero.

### Taratura CPU+GPU
- Il confronto parte con un riscaldamento scartato alla configurazione predefinita: la prima misura (la predefinita) era penalizzata dai clock ancora in
  salita e dal branco appena creato (nei tuoi `bench` 891 M contro ~1000 M delle altre).

## [1.7.2] — 2026-10-22

### `pzsolve doctor`
- Nuovo comando: una pagina che controlla programma, sistema operativo, CPU (ogni motore con una chiave nota), RAM, GPU (apertura e confronto
  bit per bit con la CPU, cache L2, GPU integrate, NVIDIA vista solo da OpenCL), cartella di cache con taratura e profilo salvati,
  campagne (ferma o in esecuzione, ultimo salvataggio, sospensioni subite, chiave trovata), spazio su disco e scrittura atomica, coerenza
  degli orologi, schema di alimentazione (Windows) e variabili `PZ_*`. Termina con il numero di errori e di avvisi. `--full` esegue anche
  l'autotest; `-d DIR` indica la cartella delle campagne.

### Protezione da sospensione e ibernazione
- **Salto di tempo rilevato**: il pannello controlla ogni 0,1 s l'orologio monotono e quello di sistema; un vuoto di oltre 15 s (sonno o
  processo congelato) viene tolto dai tempi e dalle velocità, azzera le medie dei lavoratori (niente velocità assurde dopo il risveglio) e forza
  un salvataggio. Tutte le letture del tempo di sessione passano da `run_session_elapsed`.
- **GPU che non risponde dopo il risveglio**: prima un errore di lancio escludeva la scheda per tutta la sessione (si perdeva
  silenziosamente l'80 % della velocità). Ora il contesto viene chiuso e riaperto con tentativi per circa due minuti e mezzo, la scheda ripassa
  lo stesso controllo bit per bit dell'avvio e il lavoro riprende (ricerca lineare: il blocco in corso torna nel pool; canguro: branco nuovo con
  punti di partenza diversi da quelli già percorsi). Limite: 3 errori di fila, 40 in tutto, poi esclusione come prima.
- `hunt --keep-awake` (Windows, `SetThreadExecutionState`) impedisce la sospensione finché la campagna gira.
- Test: il driver CUDA simulato fa fallire un lancio quando compare un file (`PZ_MOCK_FAIL_FILE`); `tools/verify.sh` lo prova su lineare e
  canguro, congela un processo con SIGSTOP per il rilevamento del salto di tempo ed esegue `doctor`. Un errore trovato dal test: dopo la riapertura
  la dimensione del branco restava quella dell'autotest (4 work-item) e la GPU contava passi mai eseguiti; corretto. TSan ha trovato
  una lettura di `started` mentre il lanciatore la scriveva (corretto spostando i campi del thread prima del contesto).

## [1.7.1] — 2026-10-22 (non pubblicata a parte: tutto quanto segue è nella 1.7.2)

Strumenti di misura e messaggi, più una piccola dieta delle istruzioni dei kernel GPU. I motori della CPU e il formato dei file
non cambiano (le campagne in corso si riprendono con il nuovo eseguibile).

### Profilo automatico della macchina
- **Alla prima esecuzione di ogni versione, su una macchina con GPU, `hunt` misura da solo la combinazione migliore** di thread CPU e
  GPU (lo stesso confronto di `bench --together`, circa 2 minuti, per il canguro o per la ricerca lineare), la salva in
  `machine_profile.txt` nella cartella della cache e da allora la usa come predefinita. La chiave del profilo comprende versione,
  processore, core, motore/passo e GPU con driver: se cambia qualcosa si rimisura. `-t`, `--gpu`, `--no-cpu`, `--no-gpu` hanno la
  precedenza; `--retune` rimisura; `--no-tune` o `PZ_NO_PROFILE=1` disattivano. Una configurazione diversa da quella predefinita
  viene adottata solo se batte la predefinita di più del rumore in una conferma a coppie alternate.
- `bench --together --linear` misura la ricerca lineare (prima solo il canguro).

### Branco dimensionato sulla L2 (GPU con cache grande)
- La taratura del canguro prova, sulle GPU con una L2 di almeno 8 MiB, un **branco dimensionato sulla cache**: meno work-item, NK 16/32/64,
  in modo che gli array privati dei camminatori (≈ 128 byte l'uno) stiano in mezza L2. Se il branco sta nella cache, il traffico che
  limita il kernel (≈ 200 byte per passo) resta sul chip. Nuovi kernel NK 16 e 32 (con e senza prodotti a coppie), usati solo da qui.
  Non cambia nulla dove la L2 è piccola (le schede da portatile di fascia bassa hanno 2 MiB: la lista delle varianti è quella di prima) e la variante vince solo
  se misurata più veloce, dopo il confronto bit per bit con la CPU. **Non è mai stata misurata su una scheda con L2 grande**:
  dove funzioni lo dirà la taratura, nel log (`NK 32 (branco nella L2) -> …`).
- Non è un kernel con il branco in memoria condivisa: la memoria condivisa (≈ 100–228 KiB per SM) è sempre più piccola della L2 (la
  RTX 4090 ha 16 MiB di memoria condivisa in tutto e 72 MiB di L2) e non basta per un lotto di inversione abbastanza grande (docs/it/ALGORITMI.md 8.4).

### Strumenti
- `tools/tag_releases.sh`: da un tuo clone crea e pubblica i tag `vX.Y.Z` mancanti sui commit "Release vX.Y.Z" (utile dove il push dei tag
  non è possibile).

### Prestazioni (kernel GPU, da misurare)
- `fe_normalize` (riduzione canonica, 2 volte per passo del canguro) e la negazione condizionale di `y` (rappresentante pari) non
  hanno più rami né la catena a 64 bit: la prima è una catena di riporti PTX con 8 selezioni (≈ 28 istruzioni invece di ≈ 64),
  la seconda un XOR con maschera e una catena di prestiti (≈ 18 invece di ≈ 48 più la divergenza del ramo). Risultati bit per bit
  uguali (stesse funzioni). Con `ptxas` per sm_86 il kernel `kang64b` passa da 6272 a 6160 istruzioni SASS (≈ −112 per passo, ≈ −5 %
  delle ≈ 2200), sempre 128 registri e nessuno spill; anche il kernel lineare ne guadagna un poco. Vale solo se la scheda è limitata dal
  calcolo: se è limitata dalla banda la velocità non cambia (il `bench` della 1.7.1 dice quale dei due tetti sia il più vicino).

### Benchmark
- **`bench --together [-t N] [--gpu LISTA]`**: avvia come in una campagna (puzzle 140) i thread CPU e le GPU e misura la velocità
  totale con 5 numeri di thread CPU (tutti, tre quarti, uno per core fisico, metà, nessuno), poi spegne a turno ogni GPU più lenta
  restituendo alla CPU il core che le serve. Una configurazione migliore della predefinita viene confermata con misure a coppie
  alternate (che annullano la deriva termica) e proposta come `-t N [--gpu L]`. Motivo: nella 1.7.0 la somma dei
  `bench` (1072 M passi/s) superava del 14 % la velocità reale di `hunt` (941 M): CPU, GPU integrata e GPU discreta si dividono
  potenza e raffreddamento e solo la prova insieme dice quale combinazione conviene.
- **Diagnostica delle GPU nel `bench`**: banda di copia in GB/s (con la banda teorica per le NVIDIA), throughput delle istruzioni
  intere a 32 bit (`mad.lo`, `mad.hi`, somma a 3 ingressi) in operazioni/s e per unità e per ciclo, e per le NVIDIA i tetti del
  canguro che ne derivano (≈ 215 byte e ≈ 2200 istruzioni per passo). Sono i due limiti del kernel: dicono quanto margine resta.
  Stampa anche la cache L2 e la memoria condivisa per SM (CUDA e OpenCL) e il rapporto tra il branco del canguro e la L2.
- `bench --gpu-only` (salta le prove della CPU) e `bench --retune` (ignora la scelta salvata delle varianti dei kernel GPU e
  la rimisura). La riga del canguro riporta la memoria usata dai buffer.

### Messaggi
- Le GPU integrate (OpenCL `HOST_UNIFIED_MEMORY`, CUDA `INTEGRATED`) sono segnalate come "RAM di sistema condivisa (limite
  dichiarato dal driver)": i «NN GiB» di una GPU integrata non sono memoria assegnata, sono la quota di RAM che il driver
  lascia indirizzare, mentre una scheda discreta ha davvero la sua VRAM. All'avvio del canguro il log riporta la memoria realmente usata
  (`memoria usata 29.8 MiB della RAM di sistema` / `… su 8.00 GiB`).

### Correzioni
- Nessuna. (Durante lo sviluppo TSan ha trovato una lettura di sessioni GPU già liberate nel riepilogo di `bench --together`,
  corretta prima del rilascio.)

### Test
- Driver CUDA simulato: sonde `pz_diag_bw`/`pz_diag_alu` (con controllo dei limiti delle allocazioni) e attributi del dispositivo
  (integrato, clock e ampiezza del bus della memoria); `tools/verify.sh` esegue `bench --gpu-only` e `bench --together -t 1` sul
  driver simulato. OpenCL su pocl provato a mano. I numeri di banda e di throughput vanno letti sulla propria scheda.

## [1.7.0] — 2026-10-22

### Prestazioni (canguro su GPU)
Punto di partenza, misurato su una GPU NVIDIA da portatile con la 1.6.0: **486 M passi/s**, che con ≈ 400 byte di traffico per
passo sono ≈ 194 GB/s, cioè tutta la banda della scheda: il kernel era limitato dalla memoria. Le varianti "a 1024 thread per SM"
(300 M) e "con prelettura" (uguale) provate nella 1.6.0 lo confermano. La 1.7.0 attacca il traffico e l'inversione:
- **Kernel v2: storia dei salti al posto della distanza.** La distanza a 192 bit non viaggia più da passo a passo (48 byte per passo
  di lettura e scrittura nella memoria privata): ogni passo scrive 16 bit in un buffer di storia (indice del salto e bit "punto
  negato") e `d` si ricostruisce rigiocando la storia, a fine lancio per tutti i camminatori e, per i rari punti distinti, nel
  momento in cui compaiono. Un lancio fa al massimo 64 round.
- **Una sola passata per round**, non due: la passata che muove i camminatori calcola anche i denominatori del round successivo e
  i loro prodotti correnti; questi alternano da prefissi a suffissi (la passata scende, poi sale). Il trucco di Montgomery funziona
  con entrambi. Il camminatore non viene più riletto per formare i denominatori.
- **Stato più piccolo**: etichette anti-ciclo a 32 bit, un solo valore "ultimo DP" al posto del contatore `since` (la fase dei
  controlli di ciclo e il watchdog sono ora legati al numero di passo comune a tutto il branco), 32 parole di stato invece di 36.
  Traffico stimato (per costruzione, non misurato): da ≈ 400 a ≈ 230 byte per passo, limite di banda ≈ 830 M passi/s.
- **Inversione per divstep** (Bernstein–Yang, blocchi di 30 passi) al posto della potenza `p − 2`: ≈ 10 500 istruzioni invece di
  ≈ 55 000. A NK = 128 l'inversione pesava ≈ 430 istruzioni per passo su ≈ 2430 (18 %), ora ≈ 80: ≈ −14 % di istruzioni per passo
  (−25 % a NK = 64). Vale anche per il kernel lineare (un'inversione per finestra).
- **Variante "prodotti a coppie"** (`NK … (prodotti a coppie)`, `PZ_GPU_NK=128b`): i prodotti correnti del lotto si tengono solo per
  un camminatore su due e gli altri si ricalcolano nel passo (mezza moltiplicazione in più per passo, ≈ +6 % di istruzioni):
  togliendo metà delle letture e scritture dei prodotti, ≈ −32 byte su ≈ 230 per passo. Entra nei 128 registri senza spill; una
  versione a blocchi di 4 spillava (344 byte) ed è stata scartata. La taratura sceglie tra le 6 varianti: vince la più veloce sulla
  scheda, quindi non c'è rischio se non guadagna (costa una ventina di secondi alla prima esecuzione).
- PTX con tetto di 128 registri (quattro blocchi da 128 thread per SM): senza il tetto `ptxas` usa 150–220 registri e ne
  entrerebbero solo due; tolte le varianti "con prelettura" e "a 1024 thread per SM" (uguali o molto peggiori).
- Atteso: limite di calcolo ≈ 1,2 G passi/s, di banda ≈ 0,8 G: **da misurare con `bench`**. La
  correttezza è verificata bit per bit (OpenCL su pocl e driver CUDA simulato contro la CPU), la velocità va misurata sulla scheda.

### Semantica del passo (tutti i motori)
- Etichette a 32 bit; i controlli di ciclo cadono ai passi multipli di 64 (gli stessi per tutti i camminatori); il watchdog
  (24 · 2^dp_bits passi senza DP) si valuta solo a quei passi; `since` diventa `lastdp`. I punti distinti già in `dp.bin` restano
  validi (il formato non cambia). CPU scalare, vettoriale (AVX2, AVX-512, IFMA) e GPU restano identiche bit per bit; i test del
  passo girano ora per 75–140 round per far scattare i riavvii del watchdog e verificano i contatori oltre 2^32.

### Documentazione
- `docs/it/ALGORITMI.md` 8.4 (misure sulla scheda di prova, conto del traffico e delle istruzioni) e nuova 8.6: moltiplicazione modulare
  (limbi da 29 bit con somme di colonna a 64 bit: nessun vantaggio su sm_86, 296 istruzioni contro 288; Karatsuba stimato
  inutile) e relazione sul preprint OpenAI "Integer multiplication below n log n" e sui record della comunità: risultati asintotici,
  nessun effetto su prodotti a 256 bit.

### Test
- `t_gpufe`: inversione su 400 000 valori (casuali, piccoli, intorno a `p` e `2^256`, zero) per le varianti portabile e PTX, contro la
  CPU, con canonicità del risultato; 3 mutazioni su 4 rilevate (la quarta, `delta ≥ 0` al posto di `delta > 0`, è una variante
  equivalente dell'algoritmo). Self-test del canguro su GPU: 5 mutazioni del kernel su 5 rilevate.
- `pzsolve kstat` e il self-test su GPU eseguono ora anche prove con la storia dei salti piena e molti cicli (R = 4).

## [1.6.0] — 2026-10-22

### Corretto
- **Canguro: cicli infruttuosi di lunghezza 4, 6, … (bug grave sugli intervalli grandi)**. Camminando sulle classi `{P, −P}` un
  camminatore cade in un ciclo che non contiene punti distinti circa ogni 2,6·10⁵ passi (`≈ 1/(4R²)` con 256 salti; la regola
  sui cicli di 2 non basta). L'unica difesa era il riavvio dopo `24·2^dp_bits` passi senza punto distinto, che con `dp_bits = 45`
  (puzzle 140) non arriva mai: **dopo pochi minuti quasi tutto il branco girava a vuoto**, con la velocità mostrata invariata.
  Ora ogni camminatore prende un checkpoint ogni 64 passi e tiene la minima etichetta vista: tornato sul checkpoint, esce dal
  ciclo dal punto di etichetta minima con un salto di fuga da una tabella separata. L'uscita dipende solo dal ciclo, quindi due
  camminatori che si erano incontrati restano insieme fino al punto distinto (con un riavvio o un'uscita "qualsiasi" la
  collisione andrebbe persa). CPU (tutte le varianti) e GPU, verificate bit per bit tra loro; nuovo self-test sulla curva vera
  con soli 8 salti (un ciclo ogni ~250 passi) e watchdog spento: senza la correzione non trova la chiave, con la correzione sì.

### Prestazioni
- **Canguro: nuova disposizione dei camminatori, circa il 10 % di passi in meno.** Base all'inizio dell'intervallo meno `W/32`,
  un terzo di tame in `[1, W/32)`, due terzi di wild che partono da `−x + v` con `v` pari sparso su tutto l'intervallo, salti pari
  di media `12·√W`: i wild stanno da entrambe le parti dello zero e si incontrano tra loro (è la disposizione del metodo "SOTA"
  di RCKangaroo, studiata e riscritta, senza usarne il codice). Curva vera, 400 prove per riga: **1,21 / 1,20 / 1,11 / 1,08**·√W a
  `W = 2^28 / 2^32 / 2^36 / 2^40` (prima 1,26 / 1,23 / 1,31 / 1,21). Nel simulatore, a parità di condizioni: 1,14–1,23 contro 1,27–1,36.
- `PZ_KANG_MIX=1` (sperimentale): sui core con hyper-threading un thread col passo vettoriale e l'altro con quello scalare.
- **Canguro su GPU: nuove varianti nella taratura.** L'indice del salto del passo successivo è calcolato alla fine del passo,
  quando le etichette del camminatore sono già nei registri (il passaggio in avanti legge 4 byte invece di 28); varianti con
  prelettura del camminatore successivo (per sovrapporre la latenza della memoria privata, che è in memoria video) e, su CUDA,
  con 1024 thread per SM (64 registri). 9 varianti CUDA e 6 OpenCL, scelte misurando sulla scheda; tutte verificate bit per bit
  contro la CPU (pocl e driver CUDA simulato). Il guadagno lo dirà la taratura.

### Compatibilità
- `dp.bin` passa al formato 2 (la base è cambiata). Un `dp.bin` del formato 1 della stessa campagna viene **convertito
  automaticamente** alla prima ripresa (i punti distinti restano validi: cambia solo il modo di scriverne la distanza);
  l'originale resta in `dp.bin.v1`. Non serve cancellare le campagne.

### Bench e diagnostica
- `bench`: canguro CPU per ogni variante del passo con 1 thread, un thread per core fisico e tutti i thread logici, più la
  combinazione mista su hyper-threading. Il programma riconosce i core fisici (Linux e Windows) e li mostra in `devices`.
- `kstat BITS PROVE [THREAD [R [DP]]]`: tabella di `R` salti e `dp-bits` fissi, senza riavvii (per studiare i cicli).
- `tools/ksim.c`: modello dei cicli e della loro uscita, e della disposizione RCKangaroo (`rck=1`).

## [1.5.0] — 2026-10-22

### Corretto
- **Canguro su intervalli grandi**: il contatore dei passi senza punto distinto (che riavvia i camminatori bloccati in un ciclo)
  era a 32 bit e la soglia, 24·2^dp_bits, saturava a 2^32. Con dp_bits ≥ 33 (45 sul puzzle 140) dopo 2^32 passi di un camminatore
  (pochi giorni sulla CPU, qualche settimana sulla GPU) quasi ogni camminatore sarebbe stato riavviato prima del suo punto distinto
  (probabilità di arrivarci ≈ 1/8000): la ricerca sarebbe continuata ma quasi senza poter più rilevare collisioni. Ora contatore e
  soglia sono a 64 bit ovunque (CPU, kernel GPU); i self-test partono con metà dei camminatori oltre 2^32 passi.

### Prestazioni
- **Canguro su GPU riscritto**: l'inversione modulare era condivisa da soli 16 camminatori per thread (≈ 17 moltiplicazioni per passo
  su ≈ 6 del passo vero, ~70 % del tempo) e il kernel usava 255 registri con spill. Ora 64, 128 o 256 camminatori per thread scelti
  da una taratura sul dispositivo, passaggio all'indietro dell'inversione fuso con il passo, 126 registri senza spill e il doppio
  dei thread residenti. Su pocl (OpenCL su CPU) ×2–2,4; su una GPU vera il guadagno atteso è 2–3 volte (la nuova velocità va misurata).
- **Canguro su CPU a corsie SIMD**: 4 (AVX2) o 8 (AVX-512, IFMA) camminatori per istruzione, con tutto lo stato in forma vettoriale,
  lettura della tabella dei salti con trasposizione 8×8 e 4096 camminatori per thread. Con 4 thread: **25,3 → 95 M
  passi/s** con IFMA. La variante è scelta misurando, solo dopo un confronto bit per bit con il passo di riferimento.
- **Ricerca lineare su GPU**: stesso schema "fuso" (memoria privata dimezzata, traffico verso la memoria video diviso per tre) e
  varianti con finestra 512 nella taratura. Filtro dell'hash a 96 bit (ogni candidato è riverificato sulla CPU).
- **BSGS**: finestre dei passi giganti e della tabella a corsie SIMD: giganti 25,8 → ≈ 50 M/s, costruzione ×1,5 (4 thread).
- `bench` misura anche il canguro di ogni GPU e mostra il passo CPU in uso; la misura del canguro CPU esclude la fase di avvio.

### Documentazione
- `docs/it/ALGORITMI.md` sezione 8.4: costo minimo di un passo, dove va il tempo su CPU e GPU, limiti di banda, cosa non conviene
  (batch nel warp), stime per il puzzle 140.

### Test
- `kang_round_check` (selftest e scelta automatica): ogni variante del canguro contro quella di riferimento, con molti punti
  distinti, riavvii, contatori oltre 2^32 e un camminatore sul proprio punto di salto; 8 mutazioni del codice vettoriale
  rilevate su 8. Le finestre BSGS a corsie contro quelle scalari. Il self-test GPU copre tutte le varianti di NK.
- Ripetuta la misura della finestra CPU 512 (nessun guadagno misurabile: resta 256) e dei gather hardware (più lenti: scartati).

## [1.4.0] — 2026-10-21

### Prestazioni
- **Nuovo motore CPU `avx512i` (AVX-512 IFMA)**: la stessa ricerca lineare a corsie di `avx512v`, ma il campo usa 5 limbi da 52 bit
  e le istruzioni `vpmadd52luq/huq` (50 istruzioni per prodotto invece di 81 moltiplicazioni più le addizioni). Misurato
  con 4 thread: **64,9 contro 42,0 M chiavi/s (×1,55)**, anche nell'exe Windows. Viene scelto solo se la CPU dichiara
  IFMA (Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids, Zen 4 e successivi) e se è il più veloce nell'autotune.
- Ogni motore CPU, prima di essere usato (autotune o `--isa`), deve ritrovare una chiave nota: un motore che desse risultati errati
  su una CPU particolare verrebbe escluso invece di far perdere chiavi.

### Aggiunto
- **Puzzle 161–256**: `hunt -p 161` (fino a 256) usa l'indirizzo del puzzle senza bisogno di `-a`, con un avviso: quei puzzle
  esistevano nella transazione del 2015, ma il creatore ne ha ritirato i fondi nel 2017 (blocco 475240) e gli indirizzi sono vuoti.
  `puzzle N` ne mostra indirizzo e stato. Indirizzi presi da due esploratori di blocchi concordanti e verificati contro i 160 già
  noti (`data/README.md`). Il puzzle 256 si ferma all'ordine del gruppo `n − 1`.

### Test
- `tests/t_fev52.c`: l'aritmetica IFMA contro la versione scalare ai limiti di ogni limbo e su valori costruiti attorno ai multipli
  di `p`; 8 mutazioni provate, 7 rilevate e l'ottava dimostrata innocua (e rimossa dal codice: un passaggio di riduzione superfluo).
- Motore IFMA verificato sia direttamente su una CPU con IFMA sia sotto Intel SDE in modalità Tiger Lake
  (`tools/verify.sh` con `PZ_SDE=...`), su Linux e nell'exe Windows; `t_puzzles` ed e2e coprono i puzzle 161–256.

## [1.3.0] — 2026-10-21

### Prestazioni
- **Kernel CUDA più snelli** (misurato sul codice per sm_86): addizione e sottrazione modulari su catene di riporto PTX
  (176 → 88 istruzioni per la coppia), riduzione dopo un prodotto in PTX (moltiplicazione 328 → 288), **quadrato dedicato** con
  36 prodotti invece di 64 (anche per OpenCL). Circa l'11 % di istruzioni in meno per chiave nel kernel lineare, che su una GPU da portatile è
  limitato proprio dal throughput intero; anche il canguro su GPU ne beneficia.
- **Taratura estesa** del kernel CUDA: finestra 128/256 × registri liberi o limitati a 128 (più warp residenti); vince la più
  veloce misurata sul dispositivo. La finestra 64 non viene più provata con CUDA (la più lenta su una GPU da portatile: 438 contro 514 M chiavi/s).
- Le GPU CUDA non tengono più un core della CPU riservato (il loro contesto attende senza consumare CPU): un thread di calcolo in più.

### Corretto (verifica delle opzioni)
- Argomenti senza opzione venivano ignorati: `hunt -p 71 extra` partiva, `hunt 71` dava un errore fuorviante. Ora: errore chiaro
  ("intendevi -p 71?"); lo stesso per `list`, `info`, `where`, e `devices`/`selftest`/`bench`/`version` non accettano argomenti.
- `--gpu` con un indice inesistente veniva accettato in silenzio (si lavorava solo con la CPU); liste come `0,,` o vuote erano
  accettate. Ora sintassi rigorosa e controllo degli indici **prima** di creare la campagna.
- `--table` in una cartella inesistente o non scrivibile: l'errore arrivava solo dopo aver costruito la tabella (ore, per tabelle
  grandi). Ora è verificato subito.
- `--dp-bits` troppo alto (es. 60 sul puzzle 40) rendeva il canguro incapace di trovare la chiave: ora viene limitato al massimo
  sensato per l'intervallo e il numero di canguri, con un avviso.
- `--chunk-bits` sotto 20 rallentava molto la CPU (−33 % con blocchi da 2 chiavi) e le GPU: ora l'intervallo ammesso è 20..40.
- `--new --resume` insieme, `puzzles --open --solved`, `puzzle N --rate` senza valore: messaggi di errore chiari invece di
  comportamenti ambigui.
- `-m bsgs` con una tabella troppo piccola per l'intervallo: avviso che il canguro costerebbe meno.
- Self-test GPU del canguro: ora fallisce anche se i riavvii non coincidono (prima il caso non veniva mai esercitato).

### Test
- `tests/t_gpufe.c`: l'aritmetica dei kernel (variante portabile e variante PTX con il flag di riporto emulato secondo l'ISA PTX)
  contro la CPU su 5 milioni di combinazioni, inclusi i casi rari dei doppi riporti; mutazioni verificate (5 su 5 rilevate).
- e2e: nuovi casi negativi per le opzioni.

## [1.2.0] — 2026-10-21

### Aggiunto
- **Eseguibile nativo per Windows** (`dist/pzsolve.exe`, Windows 10/11 a 64 bit): statico, usa solo DLL di sistema, stessi comandi
  e opzioni di Linux. Nuovo strato `src/platform.h/.c` che raccoglie tutto ciò che dipende dal sistema operativo (file binari con
  offset a 64 bit, rinomina atomica con nuovi tentativi se un lettore tiene aperto il file, blocco della campagna con
  `LockFileEx`, memoria con `VirtualAlloc` e pagine grandi opzionali, console con sequenze ANSI, Ctrl-C e **chiusura della finestra
  con salvataggio**, entropia da `BCryptGenRandom`). Con un doppio clic mostra l'aiuto e attende Invio.
- **Backend CUDA** per le schede NVIDIA, su Windows e Linux: driver API caricata a runtime (`nvcuda.dll` / `libcuda.so.1`, presenti con
  il normale driver grafico, versione ≥ 525), kernel incorporati in PTX: **nessun toolkit CUDA necessario**. Gli stessi kernel
  OpenCL vengono compilati da NVRTC (`tools/gen_ptx.py`, `src/kernels/pz_cuda.h`); il codice GPU (`src/gpu.c`) è ora unico e gira su
  CUDA o OpenCL attraverso `src/gpu_rt.h`. Una scheda vista da entrambi i runtime è usata una volta sola (via CUDA, con OpenCL come
  riserva se il self-test CUDA fallisce). Nuova opzione `--gpu-api auto|cuda|opencl` (anche `PZ_GPU_API`).
- **Driver CUDA simulato per i test** (`tests/mock_cuda/`, `make mockcuda`): stesse API del driver NVIDIA, esegue gli stessi sorgenti
  dei kernel sulla CPU e controlla ogni chiamata; con esso il percorso CUDA è verificato senza GPU (self-test, ricerca lineare,
  canguro, ASan), anche nell'exe Windows (`nvcuda.dll` simulato sotto Wine).
- `make windows`, `make wintest` (test dell'exe sotto Wine), `make winmockcuda`; `tools/verify.sh` verifica anche l'exe Windows, il
  percorso CUDA e che il binario Linux non richieda una glibc più recente della 2.34; `tools/release.sh` pubblica anche l'exe.

### Corretto
- Il binario Linux della 1.1.0 richiedeva **glibc ≥ 2.38** (i nuovi controlli delle opzioni usavano `strtoll`, che lì diventa
  `__isoc23_strtoll`) e non partiva su Ubuntu 22.04 / Debian 12: ora di nuovo glibc ≥ 2.34, con un controllo automatico nel rilascio.
- Il self-test GPU del canguro non metteva mai alla prova il riavvio dei camminatori bloccati (limite troppo alto per i passi
  eseguiti): ora il limite è basso e il test richiede che riavvii e punti distinti avvengano davvero (vale anche per OpenCL).
- GPU: i buffer del canguro creati dal self-test non venivano rilasciati prima dell'uso vero (memoria della GPU sprecata).
- Seed: venivano copiati 8 byte da un campo `long` (4 byte su Windows); ora si usano valori a 64 bit espliciti.
- Documentazione: i codici di uscita erano descritti in modo errato (gli errori escono con 2, non con 1).

### Cambiato
- Gli indici di `--gpu` si riferiscono all'elenco combinato di `devices` (prima i dispositivi CUDA, poi quelli OpenCL).
- Motori `avx2v`/`avx512v` su Windows: le routine interne usano la convenzione di chiamata System V (`sysv_abi`), evitando il
  salvataggio di xmm6–15 a ogni chiamata (≈ +17 % sulla stessa CPU).
- Dimensione dei binari: ≈ 12 MB (i moduli PTX incorporati).

### Noto / limiti
- **Né CUDA né OpenCL sono stati eseguiti su una GPU reale.** Il codice host CUDA è verificato con il driver simulato; la compilazione
  PTX→GPU è del driver NVIDIA ed è controllata bit per bit dal self-test a ogni avvio (dispositivo escluso se non supera).
- La prima esecuzione su una NVIDIA dura circa un minuto in più (compilazione JIT dei kernel da parte del driver, poi in cache).
- L'exe è provato sotto Wine, non su un Windows reale: comportamenti specifici della console di Windows (chiusura della finestra,
  sequenze ANSI) sono implementati secondo la documentazione Microsoft ma non osservati qui.

## [1.1.0] — 2026-10-21

### Aggiunto
- **Database integrato dei 160 puzzle** (`data/puzzles.csv` → `src/puzzles_data.c`): indirizzi, chiavi pubbliche note e chiavi dei puzzle
  risolti, verificati con l'implementazione Python indipendente e a ogni `selftest`. `hunt -p N` imposta da solo intervallo,
  indirizzo e chiave pubblica; nuovi comandi `puzzles` (elenco con filtri) e `puzzle N` (dati + stime di tempo misurate sulla macchina).
- **Campagne riprendibili con un solo comando**: nome stabile `pN`, `hunt -p N` riprende la campagna esistente; blocco `flock` contro
  doppie istanze; comando **`list`** con stato, avanzamento, velocità e tempo residuo di tutte le campagne.
- **Pannello di avanzamento** a più righe aggiornato sul posto (1 Hz, `--ui auto|full|line|log`), con velocità per dispositivo,
  tempi residui al 50/90/100 % (99 % per il canguro), ora prevista e **probabilità di successo a 1 h / 24 h / 7 g / 30 g / 365 g**;
  fuori da un terminale una sola riga al minuto. `status.json` e `report.txt` contengono gli stessi numeri.
- **Modalità BSGS simmetrica con tabella in RAM** (`--mode bsgs`): costo medio ≈ 1,0·√W (meglio del canguro ≈ 1,27·√W), deterministica,
  tabella a indirizzamento aperto da 8 byte per voce, costruzione parallela lock-free, tabella riusabile su SSD (`--table`), scelta
  automatica quando conviene. Solo CPU e RAM.
- **Motori CPU `avx2v` e `avx512v`**: l'aritmetica della curva gira su 4/8 finestre indipendenti per istruzione (campo a 9 limbi da
  29 bit, `src/fev.h`). Su 4 core AVX-512: **23,9 → 41,6 M chiavi/s (×1,74)**; con il resto delle ottimizzazioni la ricerca lineare è ≈ 2,1×
  più veloce della 1.0.0 su quella macchina.
- `fe_add`/`fe_sub` in assembly senza rami (≈ −40 % di tempo per operazione).
- **Taratura automatica del kernel GPU** (finestra 64/128/256) con cache su disco; `selftest` verifica tutte le varianti.
- Test: `t_fev` (AVX2/AVX-512, valori ai limiti, mutazioni verificate), `t_bsgs`, `t_eta`, `t_ui`, `t_puzzles`, `t_fuzz` (file e
  stringhe corrotti), e2e esteso (database, BSGS, blocco, ripresa), verifica sotto ASan/UBSan di tutti i test, BSGS sotto TSan/Valgrind.
- Documentazione: sezioni 9–11 di `docs/it/ALGORITMI.md` (BSGS, aritmetica a corsie, modello dei tempi), `data/README.md` (provenienza dati).

### Corretto (trovato dalla revisione approfondita di questa versione)
- Motori `avx2v`/`avx512v` e scalare: se una finestra conteneva il punto all'infinito (chiave ≡ 0 mod n, o centro uguale al punto di
  passo) il lotto degli hash160 si disallineava e poteva scrivere oltre il buffer; ora il percorso esatto è per finestra e c'è un test.
- Registro dei motori CPU inizializzato con `pthread_once` (prima c'era una corsa tra thread al primo uso).
- GPU: il limite `T_max` ridotto durante la taratura non resta più al valore minimo; la variante vincente viene validata contro la
  CPU **prima** di essere salvata nella cache su disco.
- La chiave privata non viene più scritta in `log.txt` (compare solo in `FOUND.txt`, permessi 0600).
- BSGS: salvataggio della tabella su file temporaneo + `rename` atomico (un'interruzione non lascia più un file troncato con
  intestazione valida), controlli su `T`, `A` e limiti di indice, errori di `pthread_create`, interruzione durante la costruzione.
- Memoria disponibile: ora si tiene conto del limite del cgroup (container) oltre a `/proc/meminfo`.
- `hunt` con opzioni sbagliate (`--isa`, numeri non validi...) non lascia più una cartella di campagna a metà.
- Valori numerici delle opzioni letti in modo rigoroso (`-t 12abc`, `--mem abc` erano accettati e interpretati male).
- Alla ripresa, `-r`, `-k`, `-H`, `-m`, `--bsgs-entries` incoerenti con la campagna esistente vengono rifiutati (prima erano
  ignorati in silenzio); il salvataggio finale dello stato che fallisce (disco pieno) è segnalato e dà codice di uscita 1.
- `list`: una campagna il cui processo è morto senza report finale è `INTERROTTA` (non più `IN CORSO`); colonne dimensionate sul
  contenuto e nessun limite al numero di campagne.
- Fase di costruzione della tabella BSGS: il pannello non mostra più tempi e probabilità riferiti alla tabella come se fossero
  quelli della ricerca (modello `ETA_NONE`, `n/d`), ma il tempo residuo della fase corrente.
- Terminali stretti: i campi vengono omessi per intero invece di essere troncati a metà.
- `SIGHUP` (chiusura del terminale) ferma la campagna salvando lo stato, salvo sotto `nohup`; `TERM=dumb` usa l'uscita a righe.
- Fino a 272 thread di calcolo (prima 48): su macchine con molti core non resta più inutilizzata la parte oltre il 48º.
- `tools/gen_puzzles.py`: controlli più severi sul CSV (alfabeti, duplicati, intestazione) e verifica di sincronia inclusa in `verify.sh`.

### Cambiato
- Il nome predefinito di una campagna per un puzzle è `pN` (prima `pN-<id seed>`); con `-r` o senza puzzle il nome deriva da
  (puzzle, intervallo, obiettivo), così **rilanciare lo stesso comando riprende la campagna**. `--report-interval` predefinito 1 s.
- I nomi di campagna ammettono solo lettere, cifre, `-`, `_`, `.` (non all'inizio): sono nomi di cartella.
- `-p N` con un obiettivo del database verifica che chiave nota → indirizzo siano coerenti e avvisa se l'intervallo indicato esce
  da quello del puzzle (errore se non lo interseca).
- `--mode auto` con chiave pubblica nota sceglie BSGS (se la tabella sta in RAM e non ci sono GPU) invece del canguro.
- Il motore CPU ha ora un `granule` (passo indivisibile della scansione): i motori `*v` coprono multipli di 2048/4096 chiavi.
- `campaign.conf` ha il campo opzionale `bsgs_entries` (le campagne 1.0.x si leggono e riprendono normalmente).

### Documentazione
- `docs/it/ALGORITMI.md`: sezione 8 sui limiti teorici; corretta la valutazione del BSGS (con RAM sufficiente costa 1,0·√W, meno del canguro).

### Noto / limiti
- Prestazioni GPU ancora non misurate su hardware reale. Nessuna versione AVX-512 IFMA (non verificabile qui).
- I puzzle aperti restano computazionalmente enormi: `pzsolve puzzle N` mostra i numeri.

## [1.0.0] — 2026-10-21

Prima versione.

### Aggiunto
- **Ricerca lineare** (solo indirizzo): somma di punti affini con inversione modulare batch e simmetria `C ± i·G`,
  moltiplicazione 4×64 bit MULX/ADX in assembly (fallback C portabile), hash160 vettoriale a 4/8/16 lane
  (SSE2/AVX2/AVX-512) con scelta automatica del motore tramite micro-benchmark.
- **Canguro simmetrico** (chiave pubblica nota): Gaudry–Schost con classi di equivalenza `{P, −P}`; costo medio misurato
  ≈ 1,2–1,3·√W; tabella dei punti distinti in RAM con crescita automatica e log append-only per la ripresa.
- **Ordine di visita unico per campagna**: seed a 256 bit, permutazione di Feistel (round SHA-256) su blocchi di 2^B chiavi,
  scheduler a intervalli con checkpoint atomici (`state.dat` + `.bak`) e ripresa senza lavoro duplicato.
- **Backend GPU OpenCL** (caricato con `dlopen`) per ricerca lineare e canguro, con self-test bit-esatto contro la CPU
  all'avvio e riverifica di ogni chiave candidata sulla CPU.
- **Reportistica**: riga di stato dinamica, `report.txt`, `status.json`, `stats.csv`, `log.txt`, `FOUND.txt` (0600).
- Arresto pulito con Ctrl-C/SIGTERM: stato salvato; un secondo Ctrl-C dopo 0,5 s esce subito, i segnali duplicati ravvicinati
  (es. quelli di `timeout`) sono ignorati.
- Comandi `hunt`, `info`, `where`, `puzzle`, `devices`, `selftest`, `bench`, `kstat`, `version`.
- Test: unitari con vettori da un riferimento Python indipendente, end-to-end (`tests/e2e.sh`), ASan/UBSan/TSan/Valgrind puliti.
- Documentazione in `docs/` e strumenti in `tools/` (riferimento Python, simulatore del canguro, script di rilascio).

### Noto / limiti
- Prestazioni GPU non misurate su hardware reale (solo verifica funzionale su pocl).
- Solo indirizzi P2PKH con chiave compressa; solo Linux (x86-64 consigliato).
