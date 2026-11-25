# Guida a pzpeer: provarlo e usarlo

pzpeer mette in rete più computer sullo stesso puzzle. Ogni computer è una **casa**: fa girare pzsolve, pubblica i suoi punti e custodisce
quelli degli altri. Quando i canguri di due case si incontrano, le due case firmano insieme la transazione che sposta il premio: 45 % a
ciascuna, 10 % all'autore di pzpeer, e nessuno dei due vede la chiave prima che la transazione sia confermata. Su un puzzle senza chiave
pubblica (il 71) la rete fa invece la ricerca lineare condivisa: tiene il registro dei blocchi già scansionati, così che nessuno li rifaccia.

I programmi sono tre, da tenere nella stessa cartella:

| Programma | Cosa fa |
|---|---|
| `pzsolve` | la ricerca, lanciato da pzpeer |
| `pzpeer` | la casa (`run`), il nodo (`node`), la transazione di una chiave trovata (`sweep`) |
| `pzsign` | la firma a due parti del pagamento, lanciato da pzpeer |

Il funzionamento è descritto in [`docs/en/NETWORK.md`](../en/NETWORK.md) (in inglese) e, in italiano, in [PROGETTO_RETE](PROGETTO_RETE.md).
Il programma scrive in inglese: questa guida cita i suoi messaggi così come appaiono.

> I puzzle aperti restano enormi anche in rete. `pzpeer run` ogni 10 minuti scrive quanti nodi vede e il tempo entro cui la rete ha
> metà (e 9 su 10) delle probabilità di trovare la chiave: è quello il numero da guardare.

## 1. Cosa serve

- Linux, macOS o Windows, a 64 bit.
- Per compilare (Linux e macOS): `make`, un compilatore C (gcc o clang), un compilatore C++ per pzsign, `curl` (la prima compilazione di
  pzsign scarica OpenSSL e ne controlla l'impronta), `python3` per i test. Su macOS bastano gli strumenti da riga di comando di Xcode.
- Per Windows: i tre `.exe` si compilano su Linux con MinGW-w64 (`make windows winsigner`) oppure si prendono dagli artefatti della CI
  (`windows-x86_64-exe`, conservati 7 giorni; per Linux `linux-x86_64-programs`).
- Per i test della rete con i pagamenti: Bitcoin Core (`bitcoind`) in modalità regtest, una catena locale dove i blocchi si creano a
  comando. Nessuna moneta vera.

## 2. Compilare

Dalla cartella del repository:

```sh
make && make peer && make signer
```

Il risultato è in `build/`: `pzsolve`, `pzpeer`, `pzsign`. La prima volta `make signer` impiega alcuni minuti (compila OpenSSL), le volte
successive no. Su Windows i programmi vanno in `build/win/` con `make windows winsigner`.

Controllo veloce:

```sh
build/pzpeer version
build/pzsign version
build/pzpeer netcheck 71      # la rete del puzzle 71: indirizzo, intervallo, blocchi e impronta
```

## 3. Provarlo: i test automatici

Vanno eseguiti in quest'ordine, dalla cartella del repository. Ognuno finisce con una riga che dice se è passato.

### 3.1 I test unitari

```sh
make peer-test
```

Ogni test scrive `t_nome: OK` (oppure `t_nome: N FAILED`). Con pzsign compilato, i test della firma usano il pzsign vero. Durano pochi
minuti e non hanno bisogno di rete né di bitcoind.

### 3.2 La catena di prova: bitcoind in regtest

```sh
bitcoind -regtest -daemon -rpcuser=pz -rpcpassword=pz -fallbackfee=0.0002
export PZ_REGTEST=127.0.0.1:18443:pz:pz
```

I test creano da soli il loro portafoglio (`pz`) e i blocchi che servono. Per fermare bitcoind alla fine:
`bitcoin-cli -regtest -rpcuser=pz -rpcpassword=pz stop`.

### 3.3 La sessione di pagamento

```sh
PZSIGN=build/pzsign build/peer/t_session
```

Due case simulate passano per ogni caso della sessione: il pagamento normale, una casa che si ferma a metà e riparte, un partner che non
risponde, che rifiuta, che mente o che prova a prendersi il premio da solo, il premio già preso, la catena letta da server Electrum. Circa
5 minuti; alla fine `t_session: OK`.

### 3.4 La rete intera

```sh
tests/peer/e2e_net.sh
```

Un nodo custode e tre case vere (ognuna col suo pzsolve) su un puzzle di prova di 58 bit con 1 BTC di regtest. Passa quando due case si
incontrano, il premio arriva 45 % a ciascuna e 10 % all'autore, e poi entrambe hanno la chiave in `FOUND.txt`: `E2E NET PASSED`. Se una
casa trova la chiave da sola prima, il test controlla il 90 % a lei e il 10 % all'autore e riprova con un nuovo puzzle. Alcuni minuti.

Le varianti, da provare dopo:

| Variabile | Cosa prova |
|---|---|
| `PZ_E2E_LIGHT=1` | le case senza nodo bitcoind: leggono la catena da due server Electrum e verificano ogni risposta da sole |
| `PZ_E2E_MEET=1` | nessun indirizzo dato, come il primo giorno di una rete: le case si trovano da sole |
| `PZ_E2E_WIN=1` | i programmi Windows sotto Wine (prima `make windows winsigner`) |

Per esempio `PZ_E2E_LIGHT=1 tests/peer/e2e_net.sh`. Se il test fallisce, scrive la cartella con i log delle case e non la cancella.

### 3.5 La ricerca lineare

```sh
tests/peer/e2e_linear.sh
```

Un nodo e due case su due blocchi di 2^34 chiavi, la chiave nel secondo. Passa quando la seconda casa trova la chiave e la prima finisce il
suo blocco e lo rivendica (`E2E LINEAR PASSED`). Non serve bitcoind: la ricerca lineare non paga niente. Circa 7 minuti.

### 3.6 La transazione di una chiave trovata

```sh
tests/peer/e2e_sweep.sh
```

Una chiave con due monete: pzpeer fa e firma la transazione che le sposta, il nodo la accetterebbe, e pzpeer non l'ha mandata (non è nella
mempool). Pochi secondi.

### 3.7 Se qualcosa non va

- `--debug FILE` su `pzpeer run` o `pzpeer node` scrive ogni messaggio, passo e risposta della catena con l'ora, e nessun segreto: è il
  primo file da leggere.
- L'uscita di pzsolve è in `pzsolve.out` della cartella della casa, quella di pzsign in `pzsign.out`.
- `--verbose` scrive su stderr cosa fa il nodo.

## 4. Una prova a mano su due computer

Prima di usarlo davvero conviene vedere due case lavorare insieme. Questa prova usa la ricerca lineare su un puzzle già risolto (il 38:
2^37 chiavi, 8 blocchi di 2^34), quindi non serve una catena e non si rischia niente. Con una CPU recente un blocco dura pochi minuti.

Su entrambi i computer, nella cartella dei programmi, un file `prova.txt` uguale (192.168.1.10 è l'indirizzo locale del primo computer):

```
net=prova38
puzzle=0
address=1HBtApAFA9B2YZw3G2YKSMCtb3dVnjuNe2
range=2000000000:3fffffffff
blockbits=34
chain=main
entry=192.168.1.10:28140
```

`pzpeer netcheck prova.txt` deve dare su tutti e due la stessa impronta (`fingerprint`): è quella che fa di due file la stessa rete. Poi su
ciascun computer:

```sh
./pzpeer run --network prova.txt --no-bt --allow-private
```

`--no-bt` tiene la prova fuori dalla tabella pubblica di BitTorrent, `--allow-private` accetta gli indirizzi della rete locale. Le righe da
aspettarsi:

```
home on the linear network prova38 (1HBtApAFA9B2YZw3G2YKSMCtb3dVnjuNe2, blocks of 2^34 keys, 8 of them): 1 contact, ...
block 5: pzsolve scans it, its output in pznet-prova38/pzsolve.out
block 5 scanned and claimed with 4 proof keys: 2 custodians hold the claim
```

Una casa salta i blocchi che un'altra ha già rivendicato. La chiave del puzzle 38 sta nel blocco 0: la casa che lo
prende scrive `THE KEY OF THE PUZZLE IS IN ...FOUND.txt` e si ferma. Un blocco con poche chiavi di prova può finire con `not claimed`: è
normale (circa una volta su dieci con blocchi di 2^34), la casa non lo rifà e un'altra può rifarlo. Ctrl-C ferma una casa; ripartita,
riprende il blocco che aveva in corso. Alla fine si può cancellare la cartella `pznet-prova38`.

Per provare lo stesso su un computer solo: due cartelle e due porte, `--dir a --listen 28150` e `--dir b --listen 28151`, con
`entry=127.0.0.1:28150` nel file e, dopo `--`, le opzioni di pzsolve (`-- -t 2` per due thread ciascuna).

## 5. Usarlo davvero

### 5.1 Una casa su un puzzle con chiave pubblica

```sh
./pzpeer run                  # il primo puzzle aperto con chiave pubblica nota (oggi il 140)
./pzpeer run --puzzle 135     # un altro
```

Alla prima partenza mostra i termini del pagamento e chiede di scrivere `yes`, poi chiede l'indirizzo di pagamento della casa (qualsiasi
indirizzo standard: `1...`, `3...`, `bc1q...`, `bc1p...`). Entrambi restano nella cartella, quindi le volte dopo non li chiede più. Per un
servizio senza terminale: la prima volta `--payout INDIRIZZO --accept-terms`.

Da solo, pzpeer:
- crea la cartella `pznet-p140` (un'altra con `--dir`);
- usa pzsolve e pzsign che stanno accanto a lui (altrimenti `--pzsolve PERCORSO`, `--pzsign PERCORSO`);
- chiede al router di aprire la porta 28140 (`--no-portmap` per non farlo, `--listen 0` per non ricevere connessioni);
- trova gli altri nodi nella tabella di BitTorrent;
- legge la catena da server Electrum pubblici e verifica ogni risposta. Con un bitcoind proprio è meglio:
  `--rpc 127.0.0.1:8332 --rpc-cookie ~/.bitcoin/.cookie`.

Le opzioni dopo `--` vanno a pzsolve, per esempio `./pzpeer run -- -t 8` (otto thread) o `-- --limit 50` (metà della macchina). pzpeer
rifiuta quelle che imposta lui (il puzzle, l'intervallo, il seed, i bit dei punti).

Le righe da aspettarsi: `home ... on network p140: N contacts, listening`, poi `pzsolve started`, e ogni 10 minuti
`network: about N nodes, ... points in all, ... jumps/s (this home ...); half the chance within ..., 9 in 10 within ...`.

**Quando due case si incontrano** non c'è niente da fare: compaiono righe `session ...: a coincidence with home ...: the payout starts`,
poi `session ...: ended:` con l'esito. Il premio va dritto dall'indirizzo del puzzle agli indirizzi delle due case e dell'autore; dopo la
conferma le due case si scambiano i pezzi della chiave, che finisce in `FOUND.txt` della cartella. Se la casa si ferma a metà, alla
ripartenza riprende da dove era (il file `paid`). **Una casa che trova la chiave da sola** manda subito il 90 % al suo indirizzo e il 10 %
all'autore.

Ctrl-C ferma la casa; ripartita, riprende la campagna di pzsolve.

### 5.2 La ricerca lineare del puzzle 71

```sh
./pzpeer run --puzzle 71
```

Nessun termine, nessun indirizzo di pagamento, nessuna catena: una chiave trovata resta a chi la trova. La casa prende a caso un blocco di
2^44 chiavi che nessuno ha rivendicato, lo fa scansionare a pzsolve (da qualche ora con una GPU a qualche giorno con la sola CPU), lo
rivendica e passa al successivo. Le sue rivendicazioni restano in `own_claims.bin`; il blocco in corso è nel file `block` e riparte dopo
un riavvio. Ogni 10 minuti una riga con i blocchi fatti da questa casa e quelli che la rete conosce.

### 5.3 Se trovi la chiave (ricerca lineare)

La casa si ferma e la chiave è in `FOUND.txt` della campagna (`campaigns/b<blocco>`). **Non spenderla con un portafoglio**: la transazione
mostra la chiave pubblica e con una chiave così corta chiunque trova in pochi secondi quella privata e manda le monete altrove prima di te.
Invece:

```sh
echo CHIAVE_IN_64_CIFRE_ESADECIMALI | ./pzpeer sweep --to TUO_INDIRIZZO --private
```

(la chiave con gli zeri davanti fino a 64 cifre). pzpeer fa e firma la transazione e la dà solo a Slipstream di MARA, un pool che mina le
transazioni senza mostrarle (`--pool URL` per un altro). Senza `--private` la stampa soltanto, da dare a mano a un pool. La chiave del
puzzle 38 (`000...0022382facd0`, ormai vuoto) serve a provare il comando senza rischi: risponde `this key has no confirmed coin`.

### 5.4 Un nodo sempre acceso

Un nodo senza pzsolve custodisce i punti degli altri e inoltra le sessioni: aiuta la rete anche su una macchina piccola.

```sh
./pzpeer node                 # oppure --puzzle N, --network FILE
```

- Meglio su una macchina senza dati personali (un piccolo server in affitto, un mini computer): la porta è aperta a tutti.
- Su Linux [`tools/pzpeer-node.service`](../../tools/pzpeer-node.service) è il servizio systemd già protetto, con il suo utente.
- La porta TCP 28140 deve essere raggiungibile: su un server in affitto lo è già, a casa va aperta sul router verso quella macchina, e
  allora `--no-portmap`.
- `--store-gb N` è lo spazio per i punti degli altri (5 GB se non indicato); il loro indice occupa dal 30 al 60 % di N in RAM.

### 5.5 Partire con il computer

Prima una partenza a mano (termini e indirizzo), poi, con i percorsi completi e `--dir`:

- Linux e macOS: `crontab -e` e una riga
  `@reboot /PERCORSO/pzpeer run --dir /PERCORSO/casa >> /PERCORSO/casa.log 2>&1`;
- Windows: `schtasks /create /tn pzpeer /sc onlogon /tr "C:\PERCORSO\pzpeer.exe run --dir C:\PERCORSO\casa"`.

### 5.6 Cosa tenere al sicuro

- **`home.seed`**: è l'identità della casa e la chiave di tutti i suoi punti e rivendicazioni. Tienine una copia fuori dal computer. pzpeer
  rifiuta un seed leggibile da altri utenti o una cartella della casa che altri possono aprire.
- `payout`: l'indirizzo di pagamento. Controllalo dopo la prima partenza (`payout address of this home: ...`).
- `FOUND.txt` e `paid`, se ci sono: non cancellarli.

I file della cartella: `home.seed`, `start.key`, `terms`, `payout`, `failed-signings`, `campaigns/`, `pzsolve.out`, `pzsign.out`, `paid`,
`FOUND.txt`, `node/` (contatti e punti custoditi); nella ricerca lineare `block` e `own_claims.bin`.

## 6. Messaggi frequenti

| Messaggio | Cosa fare |
|---|---|
| `the terms of the payout are not accepted yet` | avviare `pzpeer run` una volta in un terminale, o aggiungere `--accept-terms` |
| `--payout ADDRESS is missing` | dare `--payout INDIRIZZO` (senza terminale non può chiederlo) |
| `network: no contact, looking for the others` | aspettare qualche minuto; se resta, controllare la porta 28140 o dare `--peer HOST:PORTA` di un nodo noto |
| `... does not run (pzsign version): --pzsign gives the signer` | pzsign manca o non parte: metterlo accanto a pzpeer o dare `--pzsign PERCORSO` |
| `no seed for ...: ... make it on another computer` | la macchina non ha TPM e il suo orologio mostra troppo poco rumore: avviare la casa su un altro computer e copiare il suo `home.seed` (permessi 0600) nella cartella |
| `points of this home are not published yet` | la casa trova punti più in fretta di quanto li pubblica: la rete regola da sola i bit dei punti |
| `pzsolve ended with status ...` | leggere `pzsolve.out`; la casa resta come custode |
