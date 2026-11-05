> **Documentazione italiana.** Questa cartella (`docs/it/`) contiene il README e la guida all'uso in italiano. Il programma scrive i suoi messaggi in inglese,
> e la guida li cita così come appaiono. Per tutto il resto (algoritmi, architettura, rilascio, storia delle versioni) c'è solo la documentazione inglese e fa fede quella.

# pzsolve

[![CI](https://github.com/boulayo/bitcoin-puzzle-solver/actions/workflows/ci.yml/badge.svg)](https://github.com/boulayo/bitcoin-puzzle-solver/actions/workflows/ci.yml)

pzsolve è un risolutore per il **Bitcoin Puzzle**, la serie di 160 indirizzi in cui la chiave privata *n* sta nell'intervallo
`[2^(n-1), 2^n - 1]`, scritto in C. Riunisce i tre metodi sensati per questo problema (scansione lineare dell'intervallo, baby-step
giant-step con tabella in RAM e canguro di Pollard), sceglie quello adatto a ciò che si conosce del bersaglio e lo esegue sulla CPU con
codice SIMD (AVX2, AVX-512, AVX-512 IFMA, le estensioni SHA) e sulle GPU con CUDA e OpenCL.

Sceglie da solo il motore CPU più veloce e, su una macchina con una GPU utilizzabile, la prima volta misura la combinazione migliore di thread
CPU e GPU. Ogni motore e ogni GPU sono confrontati bit per bit con un riferimento prima dell'uso, e ogni chiave candidata è riverificata. Le
campagne riprendono dopo qualsiasi interruzione e si possono dividere fra più computer.

Software libero: se ti è utile puoi [sostenere il progetto](#sostieni-il-progetto).

> **Onestà sui numeri.** Il programma è corretto e veloce, ma i puzzle ancora aperti restano computazionalmente enormi.
> `pzsolve puzzle 71` mostra quanto tempo servirebbe *su questa macchina*: il puzzle 71 richiede in media circa 6·10^20 chiavi,
> migliaia di anni anche a miliardi di chiavi al secondo. Nessun ordine di ricerca aumenta la probabilità di successo per chiave
> scansionata: l'ordine della campagna serve solo a non sovrapporsi a chi scansiona in sequenza e a rendere la campagna
> riproducibile e riprendibile.

## Cosa fa

| Cosa si conosce | Algoritmo | Costo medio |
|---|---|---|
| solo l'**indirizzo** | scansione lineare: inversione batch con simmetria ±, aritmetica della curva SIMD, hash160 SIMD/GPU | ½ · 2^(n-1) chiavi |
| anche la **chiave pubblica**, tabella in RAM (senza GPU) | **BSGS simmetrico con tabella in RAM** (riusabile da SSD) | **≈ 1,0 · √W** passi (W = 2^(n-1)), deterministico |
| anche la **chiave pubblica**, tabella troppo grande o con GPU | canguro di Pollard simmetrico | ≈ 1,06 · √W passi misurati, ≈ 1,04 attesi a scala di puzzle (modello) |

`--mode auto` (il predefinito) sceglie da solo. Un "passo" è un salto del canguro o un passo gigante del BSGS; una "chiave" è una chiave
candidata della scansione lineare. Gli algoritmi sono spiegati in [`docs/en/ALGORITHMS.md`](../en/ALGORITHMS.md) (in inglese).

* **Tutti i puzzle sono già dentro.** `pzsolve puzzles` elenca i puzzle 1-160 e `pzsolve hunt -p N` avvia una campagna: intervallo,
  indirizzo e chiave pubblica vengono da un database verificato con un riferimento Python indipendente. Ci sono anche i puzzle svuotati
  161-256, che si raggiungono con `pzsolve puzzle N` e `hunt -p N`. Lo stesso comando **riprende** la campagna. `pzsolve list` mostra avanzamento, velocità e tempo residuo.
* **Un ordine di visita per campagna.** Un seed casuale a 256 bit (riusabile con `--seed`) guida una permutazione pseudo-casuale
  biiettiva dei blocchi: nessun blocco è saltato o ripetuto e dentro il blocco si scansiona in sequenza.
* **Motori CPU.** Inversione batch di Montgomery con la simmetria ±, aritmetica della curva su corsie SIMD (AVX2, AVX-512, AVX-512 IFMA) e
  hash160 SIMD, con lo stadio SHA-256 sulle istruzioni SHA dove la CPU le ha.
* **GPU: CUDA e OpenCL.** CUDA usa il driver installato con il normale driver NVIDIA (nessun toolkit necessario); OpenCL copre AMD e
  Intel.
* **Più computer.** Ricerca lineare: `--part Q/T` (stesso seed) divide il lavoro senza comunicazione e `pzsolve split` lo ripartisce in
  proporzione alle velocità. Canguro: `--node N` e `pzsolve merge` uniscono i punti distinti.
* **Usare solo una parte della macchina.** `--limit X` usa il X % dei thread CPU e dei work-item GPU a pieno ritmo, senza pause.
* **Campagne persistenti.** Checkpoint atomici, ripresa dopo Ctrl-C, crash, riavvio o sospensione, blocco contro due processi sulla
  stessa campagna. `--keep-awake` impedisce la sospensione su Windows e macOS.
* **Taratura automatica.** `--retune` misura di nuovo la macchina, `--no-tune` la salta; `bench --together`, `bench --variants` (ogni
  variante dei kernel GPU, misurata a turni) e `doctor`: vedi [USO](USO.md) 7.4-7.6.
* **Computer in affitto.** `--encrypt-pubkey` (o `--encrypt-password`) cifra la chiave a una chiave pubblica nel momento in cui viene trovata: lo schermo, `FOUND.txt` e il log
  contengono solo una riga `PZENC1:...`, che si può spedire ovunque (`--on-found` esegue un tuo comando con essa), e `pzsolve decrypt-key` la apre a casa con la password
  data a `pzsolve encrypt-keygen`. Protegge ciò che resta dopo di te, non una macchina il cui amministratore legge la memoria mentre il programma gira:
  [USO](USO.md) sezione 11.
* **Pannello e file.** Un pannello aggiornato ogni secondo mostra velocità per dispositivo, tempo residuo e probabilità di successo. La
  cartella della campagna contiene `report.txt`, `status.json`, `stats.csv`, `log.txt` e, a chiave trovata, `FOUND.txt` (permessi 0600).

## Avvio rapido

Scarica l'archivio per la tua piattaforma dalla pagina [Releases](https://github.com/boulayo/bitcoin-puzzle-solver/releases), decomprimilo e controllalo con `SHA256SUMS`. Non
serve installare nulla; per le GPU NVIDIA basta il normale driver grafico (versione 525 o successiva).

```sh
./pzsolve devices          # hardware rilevato
./pzsolve selftest         # controlli di correttezza, GPU comprese
./pzsolve puzzle 71        # intervallo e tempo stimato su questa macchina
./pzsolve hunt -p 71       # avvia; Ctrl-C ferma, lo stesso comando riprende
./pzsolve list             # avanzamento di tutte le campagne
./pzsolve hunt -p 40       # prova su un puzzle risolto: trova la chiave in pochi secondi
./pzsolve encrypt-keygen   # computer in affitto: la chiave pubblica per hunt --encrypt-pubkey; decrypt-key apre il risultato
```

Su Windows usa `.\pzsolve.exe` in PowerShell. Le campagne stanno in una cartella `campaigns` dove lanci il comando. Il primo avvio con
una GPU NVIDIA dura circa un minuto in più, perché il driver compila i kernel.

Per un bersaglio che non è nel database usa `-a INDIRIZZO` e/o `-k CHIAVE_PUBBLICA` (esadecimale compressa o non compressa) e,
volendo, `-r INIZIO:FINE` (esadecimale). Più computer sulla ricerca lineare usano lo stesso seed e intervallo, con una quota diversa:

```sh
./pzsolve hunt -p 71 -s <seed da 64 cifre esadecimali> --part 0/2     # computer A
./pzsolve hunt -p 71 -s <seed da 64 cifre esadecimali> --part 1/2     # computer B
./pzsolve hunt -p 71 --limit 50                                       # usa metà di questa macchina
```

`pzsolve hunt --help` elenca tutte le opzioni; la guida completa è [`docs/it/USO.md`](USO.md).

## Piattaforme supportate

| Piattaforma | Codice CPU | GPU |
|---|---|---|
| Linux x86-64, Windows x86-64 (10/11, exe statico) | portabile, AVX2, estensioni SHA, AVX-512, AVX-512 IFMA (scelto a runtime) | NVIDIA con CUDA, le altre con OpenCL |
| Linux arm64 | codice portabile, più motori NEON con lo SHA-256 hardware della CPU | quello che offrono i driver CUDA/OpenCL |
| macOS arm64 (Apple silicon) | codice portabile, più motori NEON con lo SHA-256 hardware | nessuna: solo CPU |
| macOS Intel | solo codice portabile | nessuna: solo CPU |

Su arm64 solo lo stadio dell'hash della ricerca lineare usa NEON e le istruzioni SHA-256 della CPU; l'aritmetica della curva, il canguro
e il BSGS usano codice portabile, molto più lento dei motori SIMD x86-64. Queste build servono per provare il programma, eseguire i test o
cercare puzzle piccoli. Il binario Linux x86-64 richiede glibc 2.34 o successiva (Ubuntu 22.04+,
Debian 12+).

Su macOS un file scaricato con il browser è messo in quarantena e il binario non è notarizzato: esegui una volta
`xattr -dr com.apple.quarantine <cartella>`, oppure compila dai sorgenti. Su Windows SmartScreen può chiedere «Ulteriori informazioni»
e «Esegui comunque».

## Confronto

Costo in passi, in unità di √W, con W la taglia dell'intervallo. Il confronto riguarda solo i passi: non dice nulla sulla velocità
pura, che dipende da hardware e qualità dei kernel. Esegui `pzsolve bench` per misurare la tua e condividi nelle Issues i confronti
con altri strumenti.

| Metodo | Costo medio | Stato |
|---|---|---|
| canguro classico di Pollard (van Oorschot-Wiener) | ≈ 2·√W | letteratura |
| canguro simmetrico con classi ± (Galbraith-Ruprai) | ≈ 1,36·√W | teoria |
| RCKangaroo | 1,15·√W | valore pubblicato dall'autore |
| pzsolve | 1,06·√W | misurato, W = 2^32 … 2^44, curva reale |
| pzsolve, a scala di puzzle | ≈ 1,04·√W | modellato, non misurato |
| BSGS con ± e tabella di √W/2 voci | 1,0·√W | serve una tabella che stia in RAM: non fattibile per i puzzle aperti |

Le tabelle complete e il modello sono in [`docs/en/ALGORITHMS.md`](../en/ALGORITHMS.md) (sezioni 8.8 e 8.10, in inglese) e, per le misure che li sostengono,
in [`docs/en/RESEARCH.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RESEARCH.md) (stesse sezioni, in inglese).

## Limiti e cosa aspettarsi

| Puzzle | Noto | Lavoro medio | Velocità ipotizzata | Tempo |
|---|---|---|---|---|
| 71 | solo indirizzo | 5,9·10^20 chiavi | 1 G chiavi/s | 50 % in 18.700 anni |
| 71 | solo indirizzo | 5,9·10^20 chiavi | 10 G chiavi/s | 50 % in 1.870 anni |
| 140 | chiave pubblica | 8,8·10^20 passi | 1 G passi/s | mediana 26.300 anni |
| 160 | chiave pubblica | 9,1·10^23 passi | 1 G passi/s | mediana 27 milioni di anni |

* **Serve collaborazione.** Le velocità si sommano fra computer, ma i puzzle aperti sono molti ordini di grandezza oltre una singola
  macchina. Il puzzle 135, ora risolto, ha richiesto secondo quanto riportato circa 300 GPU per 5 mesi (dato riportato, non verificato qui).
* **È uno strumento di ricerca e didattico.** Nessuna garanzia di trovare qualcosa. Lo stato «risolto / aperto» è quello della data di
  creazione del database: prima di una campagna lunga controlla con un esploratore della blockchain che i fondi siano ancora lì.
* Sono supportati solo indirizzi di una chiave pubblica **compressa**: P2PKH (`1...`) e SegWit nativo P2WPKH (`bc1q...`).
* Il canguro è probabilistico e il suo costo ha una coda lunga. La velocità delle GPU è stata misurata su una scheda NVIDIA (CUDA) e su
  una GPU Intel integrata (OpenCL); sul canguro la GPU è limitata dalla banda di memoria, e le schede AMD non sono provate per la velocità.

## Cosa è stato misurato, e su cosa

Le velocità citate nella documentazione vengono da due macchine del manutentore, e diversi motori non hanno nessun numero. Questa tabella
dice, per ogni motore, da dove viene il suo numero e con quale versione; `pzsolve bench` misura la tua macchina.

| Motore | Macchina | Valore | Versione |
|---|---|---|---|
| ricerca lineare, CPU | Core i7-11850H (8 core, 16 thread, AVX-512 IFMA), `bench`, `avx512i` | 157,9 M chiavi/s con 16 thread, 30,3 M con uno; 133 M con 15 thread accanto alla GPU in una campagna | 1.15.0 ([RESEARCH](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RESEARCH.md) 10.5) |
| ricerca lineare, GPU, CUDA | RTX A2000 Laptop (20 SM, 4 GB), `bench --variants`, finestra 512 con il limite di registri | 888 M chiavi/s (mediana di tre turni) | 1.20.0 (RESEARCH 5) |
| ricerca lineare, GPU, OpenCL | Intel UHD dello stesso portatile (Xe-LP, 32 EU) | 17,5 M chiavi/s in `bench`, 16 M in una campagna | 1.15.0 (RESEARCH 10.5) |
| canguro, CPU | lo stesso i7-11850H, corsie IFMA | 261 M passi/s con 16 thread, 48,0 M con uno | 1.15.0 (RESEARCH 10.5) |
| canguro, GPU, CUDA | la stessa RTX A2000 Laptop, `NK` 64 quads con i tag scritti una volta (la scelta del tuner) | 922 M passi/s | 1.20.0 (RESEARCH 5) |
| canguro, GPU, OpenCL | la stessa Intel UHD | 56 M passi/s | 1.15.0 (RESEARCH 10.5) |
| BSGS | un thread di uno Xeon a 2,8 GHz sotto KVM, corsie AVX-512 | 36 ns di aritmetica per passo gigante più da 16 a 45 ns di ricerche in tabella (tabella in L2, o da 768 MiB); nessuna velocità dell'intera macchina | 1.14.0 (RESEARCH 8.11) |
| GPU AMD (OpenCL) | nessuna | non misurato: solo conteggi di istruzioni (clang) | 1.15.0 |
| Linux arm64, Apple silicon (motori NEON) | le macchine della CI | non misurato come velocità: solo rispetto al motore portabile sulla stessa macchina (circa il 55-60 % più veloce su Linux arm64) | 1.8.1 |
| macOS (Intel e Apple silicon) | nessuna | non misurato: la CI compila e prova i binari, la velocità non è nota | - |

I valori delle GPU sono picchi di `bench` su un portatile: in una campagna CPU e GPU condividono un solo budget di potenza e dopo qualche
minuto la macchina si assesta più in basso (RESEARCH 10.5: da 940 a 960 M chiavi/s per CPU e GPU insieme dopo due minuti, da 880 a 900
dopo dieci). Le modifiche alla CPU dalla 1.17.0 alla 1.19.2 (da +1 a +4 % ciascuna, un thread, altre macchine) non sono state rimisurate
su quel portatile.

## Compilazione dai sorgenti

Servono `gcc` 9 o successivo (o `clang`) e `make`. Per compilare non serve nessun SDK per GPU.

```sh
make            # costruisce build/pzsolve
make test       # test unitari + autotest integrato
make e2e        # test end-to-end su puzzle piccoli (circa 5 minuti)
make windows    # build/win/pzsolve.exe, cross-compilato con MinGW-w64
```

Il binario è portabile e sceglie il motore a run time (AVX2, AVX-512, IFMA, NEON), e i kernel GPU li compila il driver per la scheda: compilare
sulla propria macchina non lo rende più veloce. `make ARCH_NATIVE=1` (`-march=native`) ha dato la stessa velocità, entro il rumore di
`bench`, su una macchina AVX-512 IFMA, e il risultato può non funzionare su altre CPU.

Le build per macOS e arm64 sono compilate e provate dalla CI a ogni push; la verifica completa è descritta in
[CONTRIBUTING.md](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/CONTRIBUTING.md) (in inglese).

## Documentazione

* [`docs/it/USO.md`](USO.md): comandi, tutte le opzioni, cartella della campagna, GPU, più computer, problemi comuni, Windows.
* [`docs/it/AFFITTO.md`](AFFITTO.md): la lista di controllo per un computer in affitto, dal checksum dell'eseguibile alla decifratura della chiave a casa.
* In inglese: [`docs/en/ALGORITHMS.md`](../en/ALGORITHMS.md) (algoritmi, tempi, limiti teorici), [`docs/en/RESEARCH.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RESEARCH.md) (diario di ricerca: cosa è stato provato e misurato), [`docs/en/ARCHITECTURE.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/ARCHITECTURE.md) (sorgente), [`docs/en/RELEASING.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RELEASING.md) (rilascio), [`CHANGELOG.md`](../../CHANGELOG.md) e [`tools/README.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/tools/README.md).

## Uso responsabile

pzsolve è pensato per la transazione pubblica del Bitcoin Puzzle e per altre chiavi che hai diritto di cercare. Non usarlo contro
portafogli che non sono tuoi. L'autore non si assume responsabilità sull'uso del programma.

## Sostieni il progetto

pzsolve è libero e sviluppato nel tempo libero. Se ti ha fatto risparmiare tempo o ti ha insegnato qualcosa, una donazione è gradita e
mai dovuta.

| **Bitcoin (on-chain)** | **Bitcoin (indirizzo Lightning)** |
|:---:|:---:|
| <img src="../img/donate-bitcoin.svg" width="190" alt="Codice QR dell'indirizzo Bitcoin"> | <img src="../img/donate-lightning.svg" width="190" alt="Codice QR dell'indirizzo Lightning"> |
| `bc1qfw2y5xzvsy6w70rwqfq6n65tahk906678c7tvz` | `trivialalarm90@cake.cash` |

Inquadra un codice con il wallet, oppure copia il testo. L'unico posto ufficiale per questi indirizzi è il README del repository
ufficiale: diffida delle copie altrove.

## Licenza

pzsolve è software libero, rilasciato con licenza **GNU Affero General Public License, versione 3 o successiva**
(`AGPL-3.0-or-later`, vedi [LICENSE](../../LICENSE)). Puoi usarlo, studiarlo, modificarlo e condividerlo. Se distribuisci una versione
modificata, o la offri agli utenti attraverso una rete, devi dare loro il codice sorgente completo corrispondente con la stessa licenza.

**Termini aggiuntivi (sezione 7 dell'AGPL).** Oltre all'AGPL, chi distribuisce pzsolve, modificato o no, deve conservare l'avviso di
copyright, la licenza e il file [NOTICE](../../NOTICE), conservare l'avviso che stampa `pzsolve version`, indicare che una versione
modificata è diversa dall'originale e non presentarla come l'originale. I termini sono nel file [NOTICE](../../NOTICE) (in inglese).

**Licenza commerciale.** Se vuoi usare pzsolve in un prodotto o servizio proprietario senza gli obblighi dell'AGPL, è disponibile
una licenza commerciale a pagamento. Apri una issue intitolata «Commercial license» (senza dettagli riservati) oppure scrivi a leonardo@boulayo.it. I contributi sono benvenuti alle condizioni di
[CONTRIBUTING.md](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/CONTRIBUTING.md).

Copyright (C) 2026 Leonardo Boulay.
