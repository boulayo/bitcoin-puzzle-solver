# Database dei puzzle

`puzzles.csv` contiene i 160 puzzle del "1000 BTC puzzle": numero `n`, indirizzo P2PKH, chiave pubblica compressa (se è mai comparsa
sulla blockchain) e chiave privata (solo per i puzzle risolti, che sono pubblici). Da questo file `tools/gen_puzzles.py` genera
`src/puzzles_data.c`, compilato dentro l'eseguibile.

## Provenienza (data di acquisizione: 2026-10-21)

* Elenco con indirizzi e chiavi: <https://btcpuzzle.info/puzzle> (dati strutturati della pagina) e
  <https://privatekeys.pw/puzzles/bitcoin-puzzle-tx> (pagina HTML). Le due fonti **concordano su tutti i 160 indirizzi e su tutte le 83
  chiavi dei puzzle risolti**.
* Le chiavi pubbliche note vengono dalla seconda fonte e sono state **ricalcolate** (non fidate): per i puzzle risolti sono `k·G`;
  per i 5 puzzle aperti che hanno la chiave pubblica (140, 145, 150, 155, 160) ne viene verificato l'hash160 contro l'indirizzo.

## Verifiche eseguite (a ogni rigenerazione e a ogni `pzsolve selftest`)

* checksum Base58Check e tipo (P2PKH mainnet) di ogni indirizzo;
* la chiave di un puzzle risolto sta nel suo intervallo `[2^(n-1), 2^n-1]`, `k·G` ha l'hash160 dell'indirizzo e coincide con la chiave
  pubblica indicata;
* la chiave pubblica di un puzzle aperto è un punto della curva e il suo hash160 (forma compressa) coincide con l'indirizzo.

`tools/gen_puzzles.py` usa l'implementazione Python indipendente `tools/ref.py` (nessun codice in comune con il programma C);
`puzzles_verify()` (C) ripete i controlli a ogni `selftest`. `tools/gen_puzzles.py --check` fallisce se `src/puzzles_data.c` non è
allineato al CSV.

Controllo sulla blockchain (2026-10-21, API pubblica di blockstream.info): **tutti i 77 indirizzi dei puzzle aperti risultavano
ancora finanziati**, per un totale di circa 903 BTC.

## Avvertenze

* Lo stato "risolto / aperto" è quello della data di acquisizione: un puzzle può essere stato risolto dopo. Prima di una campagna lunga
  controlla l'indirizzo con un esploratore di blocchi. Il programma non accede alla rete.
* Una chiave pubblica può comparire sulla blockchain in futuro (quando qualcuno sposta i fondi): se ne conosci una nuova, passala con
  `-k`. Il programma sceglie allora da solo l'algoritmo adatto.
* Non c'è nessun segreto nel file: i puzzle risolti hanno chiave pubblica da anni; i puzzle aperti hanno solo indirizzo (e, per cinque,
  chiave pubblica), cioè dati pubblici.

## Puzzle 161–256 (`puzzles_withdrawn.csv`, acquisiti il 2026-10-21)

La transazione originale `08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15` (gennaio 2015, blocco 339085) ha
256 uscite P2PKH; l'uscita `i` vale `(i+1)·0,001 BTC`. Il file contiene gli indirizzi delle uscite 161–256, presi dalle API
pubbliche di **blockstream.info** e **mempool.space**, che concordano byte per byte su tutte le 256 uscite (script e importo).
Verifiche:

* le uscite 1–160 coincidono, nell'ordine, con i 160 indirizzi di `puzzles.csv` (ottenuti da altre fonti): l'uscita `n` è il
  puzzle `n`;
* l'importo di ogni uscita è `n` millesimi di BTC;
* tutte le uscite 161–256 sono state spese da **un'unica** transazione nel blocco 475240 (luglio 2017): sono i fondi che il
  creatore ha ritirato per aumentare i premi dei puzzle 1–160;
* Base58Check di ogni indirizzo e nessun duplicato con `puzzles.csv` (`tools/gen_puzzles.py`, `puzzles_verify()`).

Di questi puzzle non si conoscono chiavi pubbliche né private. Restano nel database solo perché `hunt -p 161` funzioni senza `-a`.

## Aggiornare il database

```sh
$EDITOR data/puzzles.csv                 # n,address,pubkey,privkey  (pubkey/privkey possono essere vuoti)
python3 -I tools/gen_puzzles.py          # verifica ogni riga e riscrive src/puzzles_data.c
make test                                # include la verifica in C
```
