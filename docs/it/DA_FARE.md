# Da fare: decisioni prese e decisioni aperte

13 novembre 2026. Si riferisce a [PROGETTO_RETE.md](PROGETTO_RETE.md).

Prima le poche cose che aspettano te, poi l'elenco di ciò che è deciso, con il motivo.

---

## 1. Aspettano te

### 1.1 Le tre prove che da qui non posso fare
Servono un computer con la rete di casa vera. Per ognuna basta guardare una riga dell'uscita.

| Prova | Comando | Cosa guardare |
|---|---|---|
| ritrovo sulla rete BitTorrent vera | su due computer: `pzpeer node --network FILE --dir CARTELLA --bt-router router.bittorrent.com:6881 --verbose` | le righe `rendezvous:` e poi `contacts` |
| apertura della porta sul router | lo stesso comando (la chiede da solo) | la riga `router:` |
| server Electrum pubblici | `pzpeer run --network FILE --payout INDIRIZZO --electrum electrum.blockstream.info:50001 --debug prova.log` su una rete della catena principale | la riga `chain:` e, in `prova.log`, le righe `electrum` |

### 1.1b Windows e Mac veri
Sulle macchine Windows e macOS della CI i test di pzpeer passano (su Windows anche la firma a due). Manca la prova di una rete intera. **Da fare tu:** avviare `pzpeer.exe run --network FILE --payout INDIRIZZO --debug prova.log` su un Windows vero, insieme a un altro computer, e guardare se parte e trova l'altro.

### 1.2 Revisione esterna della firma a due
La mia revisione non sostituisce quella di specialisti. **Da decidere:** se farla fare prima di usare bitcoin veri. **Consiglio:** sì, prima del #140.

La revisione interna di novembre (quattro revisori indipendenti: firma, sessione e pagamento, rete, risolutore) ha portato alle correzioni
del ramo. I punti di progetto che restavano, chiusi il 20 novembre salvo dove è scritto:
- ~~chi attacca può scegliere se essere F o S~~: F ora viene dal punto e dai due tipi di canguro (`pieces_first`), che nessuno sceglie;
  una nuova identità cambia l'ordine delle case, non F. Più semplice dell'impegno e rivelazione, e senza il problema di chi abbandona
  dopo aver visto il numero dell'altro;
- ~~un partner può far ripartire la coppia con annullamenti ripetuti~~: fatto il 14 novembre (3.8);
- ~~avvisi falsi della tabella (record con una x copiata)~~: il custode avvisa solo dopo la prova di chi ha depositato il record (E1);
- ~~il conteggio per blocco si azzera con collisioni di FNV~~: la tabella dei limiti usa un hash con una chiave segreta del nodo;
- ~~la custodia predefinita a 2^28 record~~: fatto il 18 novembre;
- ~~un saluto fa interrogare i server Electrum prima di qualunque prova~~: la catena si interroga solo dopo le prove, e la commissione
  viaggia in un messaggio dopo di esse (`S_FEE`);
- ~~gli identificativi delle richieste verso BitTorrent sono prevedibili, e 6to4 e Teredo non sono filtrati~~: ogni giro ha 4 byte a caso
  nell'identificativo; gli indirizzi 6to4 e Teredo non contano come pubblici;
- ~~il tempo con cui P1 fallisce dice quale firma non andava~~: pzsign controlla tutte le firme prima di fermarsi;
- i valori segreti possono restare nelle riserve di memoria dei numeri grandi di OpenSSL: **accettato**. pzsign vive pochi secondi, non
  lascia file di core e su Linux nessun altro processo lo può leggere; bloccare la sua memoria (mlock) lo farebbe fallire con i messaggi
  grandi della firma, e cambiare l'allocazione dentro cb-mpc vorrebbe una modifica della libreria;
- ~~le azioni della CI non sono fissate per hash, e `check_binary.sh` ha una sola lista~~: fissate per hash; ogni programma ha la sua
  lista di DLL;
- ~~la regola "un punto, una coppia provata" vive in memoria~~: ora è anche nel file `proved` della casa;
- ~~con un nodo proprio, un riordino di blocchi sembra una spesa confermata~~: una transazione tornata nel mempool non conta come spesa.

### 1.3 La prova pubblica con due o tre computer
Quando vuoi farla: un puzzle di 60–64 bit creato da noi, con un premio piccolo, e `--debug FILE` su ogni computer. I file di debug non contengono segreti: me li puoi far leggere così come sono.

### 1.4 Aspetti legali
Una rete che divide premi fra sconosciuti: avvertenze, responsabilità, eventuale tassazione. Da chiedere al legale insieme a NOTICE, CONTRIBUTING e licenza commerciale, che sono ancora in sospeso.

### 1.5 Promemoria già aperti
- prova della cifratura su un Windows vero;
- cancellare i rami di prova vecchi su GitHub (`test-122`, `test-123`, `test-1231`, `test-1232`, `test-gpu-*`, `fix-intel-cl`);
- misurare la velocità di pzsolve su una RTX 4090 (ipotesi I10: tutti i tempi dipendono da questo numero).
- ~~`t_session` con il vero pzsign si è fermato una volta sul Mac arm64 della CI~~ (20 novembre): era la prova, non il programma. Nel
  caso "una vecchia copia della prima transazione" un filo della prova aspettava di vederla nella mempool, e se il blocco arrivava
  prima aspettava per sempre (riprodotto: una volta su 471). Ora la copia prende il posto della transazione nel momento stesso in cui
  è mandata (3.11, punto 5).

---

## 2. Deciso

### A. La rete

| | Decisione | Motivo |
|---|---|---|
| A1 | **Nessuna firma sul file della rete.** La rete è l'impronta delle righe che la definiscono | È come fanno Bitcoin Core e BitTorrent: i parametri stanno nel programma, e chi li cambia è su un'altra rete. Niente chiavi da custodire, cento righe di codice in meno. La fiducia sta nel programma che si scarica dal tuo repository, come per qualsiasi programma |
| A2 | Nessun server tuo. Un nodo sul NAS, se un giorno lo vorrai, è un partecipante come gli altri | Con Tailscale da solo il nodo è raggiungibile solo dai tuoi dispositivi: per servire la rete gli serve una porta aperta verso Internet |
| A3 | I nodi si trovano attraverso la tabella di BitTorrent | fatto |
| A4 | **Chi scarica il programma lo avvia e partecipa:** `pzpeer run --network FILE`; la prima volta accetta le condizioni e scrive l'indirizzo di pagamento, che resta nella cartella. Cartella, programmi, porta (28140), apertura sul router, primi contatti e blockchain hanno un valore predefinito | L'apertura della porta è accesa da sola (`--no-portmap` la spegne): senza, quasi nessuno farebbe da custode. L'indirizzo di pagamento resta l'unica cosa da scrivere: è dove va il premio. Dal 18 novembre le reti dei puzzle sono dentro il programma: `pzpeer run` basta (A11) |
| A5 | La revisione della firma a due è fatta per la parte che ho potuto fare. Vedi 1.2 | |
| A6 | Libreria: la firma a due di cb-mpc, di Coinbase (licenza MIT), con due piccole modifiche nostre per Windows | È mantenuta, ha due revisioni esterne (Cure53, 2024) e un premio per chi trova falle. Il suo protocollo (Lindell 2017) è più semplice: una sola delle due parti ha una chiave Paillier, nessuna delle due sceglie parametri che l'altra deve accettare, e non ci sono numeri primi lenti da generare: importazione dei pezzi e firma in meno di un secondo, contro un minuto prima. Usiamo la variante con la prova finale, che non lascia trapelare nulla quando una firma fallisce. Le librerie DKLs23 mature hanno licenze non libere |
| A7 | Nessun nodo Bitcoin proprio: più server Electrum pubblici, ogni risposta verificata. Chi ha un nodo lo usa con `--rpc` | fatto |
| A8 | **Quota dell'autore: 10%**, all'indirizzo delle donazioni (`bc1qfw2y5xzvsy6w70rwqfq6n65tahk906678c7tvz`). Due case: 45% a ciascuna. Scritta nel programma per versione delle condizioni, fuori dal file della rete e dall'impronta: nessuno la imposta, e una versione nuova può cambiare l'indirizzo o abbassarla senza una rete nuova. Una coppia paga secondo la più vecchia delle due versioni | fatto. Alzarla in seguito non converrebbe: chi resta alla versione vecchia pagherebbe meno |
| A9 | Chi trova la chiave da solo: la casa manda subito le monete, 90% al suo indirizzo e 10% all'autore | fatto. Chi ferma o modifica il programma può evitarlo |
| A10 | Prima partenza: le condizioni sullo schermo da accettare (`--accept-terms` per un servizio), l'indirizzo chiesto e ricordato, una riga delle condizioni a ogni avvio, e nella riga della rete la parte del premio attesa in media | fatto |
| A11 | Le reti di tutti i puzzle con chiave pubblica nota dentro il programma, il file solo per le prove e per un puzzle nuovo | fatto il 18 novembre: `pzpeer run --puzzle N`, e senza opzioni il primo puzzle ancora aperto (oggi il #140) |

**La transazione parte sulla rete pubblica?** Sì, da sola, appena firmata. Per il #140 non è un rischio: la chiave pubblica è già nota a tutti, e la firma non rivela nulla. È diverso per i puzzle senza chiave pubblica nota (vedi C4).

### B. Parametri della rete del #140

| | Decisione | Motivo |
|---|---|---|
| B1 | Seme dei salti: SHA256("pznet1/jumps" \|\| chiave pubblica del puzzle). Nessun blocco, nessuna data | Tutti devono avere lo stesso seme perché due canguri che arrivano sullo stesso punto devono poi fare gli stessi salti. La chiave pubblica l'ha fissata nel 2015 chi ha creato i puzzle: nessuno l'ha scelta, quindi nessuno può averne preso una che gli conviene. Conoscerlo prima vuol dire solo poter camminare prima: lavoro vero, che conta come quello degli altri (deciso il 18 novembre al posto del blocco) |
| B2 | Zeri dei punti distinti: nel programma il minimo è max(8, (N − 1)/2 − 44), 25 per il #140; ogni casa aggiunge fino a 8 bit secondo la rete (C2b) | Vedi sotto |
| B3 | Commissione: il doppio della stima per i prossimi blocchi, mai sotto 20 e mai sopra 1000 satoshi per byte | Alta perché la transazione vale molto e deve passare subito; il tetto la tiene sotto lo 0,2 % del premio anche nel caso peggiore |
| B4 | La "moneta minuscola" non ha più una dimensione fissa: cresce con la commissione | Così chi può annullare può farlo a qualunque commissione; prima c'era un tetto di 50 satoshi per byte |

**B2 spiegato.** I canguri non pubblicano ogni punto che toccano, sarebbero troppi: pubblicano solo i punti "distinti", quelli il cui numero comincia con un certo numero di zeri. Con 32 zeri è un punto ogni 4 miliardi di passi.
- **Meno zeri:** più punti da conservare e da spedire (con 30 zeri quattro volte quelli con 32).
- **Più zeri:** ogni canguro spreca in media il lavoro fatto dopo il suo ultimo punto distinto; con tanti canguri lo spreco pesa.
- Il numero giusto dipende da quanti partecipano. Per questo ogni casa lo sceglie da sola (C2b), fra il minimo della rete e 8 bit in più: abbastanza pochi perché il canguro che arriva secondo su un punto raggiunga presto il suo punto distinto, abbastanza perché i custodi possano tenere i punti. Il minimo scritto nel programma è quello di una rete di un milione di schede video.

### C. Migliorie

| | Decisione | Stato |
|---|---|---|
| C1 | Record corto (senza firma su ogni record, un terzo dello spazio): sì | fatto il 18 novembre: 66 byte (34 in una STORE, dove il proprietario è chi la manda; 38 nell'archivio, col numero del proprietario), con i bit di zeri del punto |
| C2 | Ripubblicazione leggera: sì | fatto il 18 novembre: un punto va solo ai custodi che non aveva all'ultima ripubblicazione; senza nodi arrivati o andati non parte niente |
| C2b | Bit di zeri adattivi: sì | fatto il 18 novembre: la casa li sceglie dalla grandezza della rete (al minimo quelli del file della rete, al massimo 8 di più) e li cambia quando la rete diventa 4 volte più grande o più piccola |
| C3 | Ricerca lineare condivisa: sì. Nessuna transazione automatica, la chiave resta a chi la trova | **fatto il 20 novembre:** la rete di ogni puzzle senza chiave pubblica è nel programma (`pzpeer run --puzzle 71`, blocchi di 2^44 chiavi); la casa sceglie a caso un blocco che nessuno ha dichiarato, lo fa cercare a pzsolve con le chiavi quasi giuste, e pubblica la dichiarazione con le prove ai 5 custodi del blocco; quando tutti i blocchi guardati sono dichiarati, ripassa quelli dichiarati. Provata con un nodo e due case (`tests/peer/e2e_linear.sh`). Mancano i controlli a campione (servono un protocollo loro): senza, una dichiarazione falsa ritarda la chiave fino al secondo passaggio |
| C4 | Chiave trovata senza chiave pubblica nota: avviso sempre; invio riservato a un pool di mining **solo se l'utente lo chiede** | **fatto il 20 novembre:** `pzpeer sweep --private` manda la transazione firmata a MARA Slipstream e a nient'altro, con il `curl` del sistema (`--pool URL` per un altro pool). L'indirizzo del servizio non è documentato da MARA: è quello che usano la sua pagina e i programmi di altri. Provato con un finto pool; mai con quello vero. La quota dell'autore non c'è, per scelta: la chiave resta a chi la trova |

### D. Rilascio

| | Decisione | Stato |
|---|---|---|
| D1 | Linux, Windows e macOS | Linux: fatto. Windows: `pzpeer.exe` e `pzsign.exe` passano i test sulla macchina Windows della CI, e la prova completa con tre case passa con l'emulatore Wine. macOS: compilazione e test passano sulla CI su Apple silicon e su Intel, pzsign e la firma a due compresi (dal 20 novembre anche su Intel, sotto Rosetta). Manca una rete intera provata su un Windows vero e su un Mac |
| D2 | pzsign già compilato, nello stesso archivio di pzsolve e pzpeer | È in C++ e contiene cb-mpc (MIT), una copia di libsecp256k1 (MIT) e OpenSSL (Apache-2.0): licenze compatibili con la nostra, nessun obbligo di ricompilazione come quello che dava la libreria GMP |
| D3 | Tutto resta nel ramo `rete-condivisa`. Nel repository niente italiano fuori da `docs/it` | controllato: non ce n'è |
| D4 | Prova pubblica fatta da te, con il registro dettagliato `--debug FILE` | il registro c'è |

## 3. Dalla revisione completa (novembre 2026)

Otto revisori indipendenti, uno per area, più gli analizzatori statici. Quello che è stato corretto è nel `CHANGELOG.md` e nei
commit; qui resta ciò che è aperto. I rilievi che chiedono una tua decisione sono nel rapporto della revisione.

### 3.1 Sicurezza, da fare prima di una rete con denaro vero

| | Rilievo | Stato |
|---|---|---|
| E1 | Un avviso con un punto copiato costava alla vittima circa 50 connessioni, e riempiva la sua cassetta | **Fatto (20 novembre):** il custode risponde `COINCIDENCE` e aspetta la prova di chi ha depositato il record (`PROVE`, legata al custode); senza prova non avvisa nessuno |
| E2 | Le cassette di una casa che non ascolta stanno sui 5 nodi più vicini a un nome pubblico: chi mette nodi lì le controlla | **Fatto (20 novembre):** la risposta a `PROVE` porta le coppie al proprietario direttamente dal custode del suo punto; la ripubblicazione a tutti i custodi (al più una settimana) porta ogni coincidenza anche con le cassette in mano a un attaccante. Le cassette restano una scorciatoia |
| E3 | Una coppia arrivata dopo può sostituire la prima transazione di una coppia arrivata prima | **Lasciato com'è, per scelta:** la commissione massima sulla prima sposterebbe solo la gara sulla seconda (anche l'altra coppia sa spendere l'uscita del premio), costerebbe troppo ai premi piccoli, e un programma modificato può sempre pagare di più. Fra due coppie oneste vince chi conferma prima |
| E4 | Chi fa fallire le firme blocca un punto (3 fallimenti) senza pagare nulla | **Deciso (20 novembre): resta così.** Il pezzo in gioco è quello del punto: dimenticare i suoi fallimenti riaprirebbe la strada a un difetto della firma che perda un bit a ogni fallimento. Chi blocca il punto non guadagna niente, perché l'altra metà del premio è la sua |
| E5 | Un server Electrum lento allungava ogni lettura della catena | **Fatto:** oltre al limite di una chiamata, 60 secondi per server per tutta un'operazione |
| E6 | Il partner può imporre fino a 1000 sat/vB e fermarsi dopo la prima transazione | **Fatto (14 novembre):** una commissione oltre 4 volte la propria è rifiutata, e al massimo 5 pagamenti non riusciti per punto |
| E7 | Il generatore del seme accreditava bit al tempo anche con un orologio senza rumore | **Fatto:** prima delle misure, lo stesso lavoro (stessi indirizzi, nessun salto che dipende dai dati) viene cronometrato 64 volte; se i tempi sono tutti uguali non c'è rumore, e la sorgente è rifiutata. Misurare sempre lo stesso lavoro, provato, varia troppo poco sulle macchine della CI |
| E8 | pzsign: 126 MB per un solo messaggio dentro il limite di dimensione | **Accettato** (come K5): solo memoria, una firma alla volta, il processo finisce subito dopo |
| E9 | Windows: la cartella della casa prendeva i permessi della cartella madre | **Fatto:** la cartella nasce per il solo proprietario (e il sistema), senza ereditare nulla |
| E10 | La mappatura UPnP "senza fine" non veniva mai tolta | **Fatto:** all'uscita la casa chiede al router di chiudere la porta (PCP, NAT-PMP o UPnP) |
| E11 | Apertura della porta come Bitcoin Core 30 | Fatto prima (PCP e NAT-PMP, poi UPnP). Da provare con un router vero |

### 3.2 Sprechi

| | Rilievo | Rimedio proposto |
|---|---|---|
| F1 | A ogni arresto il canguro butta il percorso di ogni camminatore dall'ultimo punto distinto (fino a K·2^dp passi) | Fatto il 18 novembre: `herd.bin` nella campagna, anche per le schede video |
| F2 | Pubblicare un record costa 7 connessioni; un riavvio ripubblica tutto | Fatto il 18 novembre: una ricerca per zona all'ora invece che per punto, e dopo un riavvio i punti che `own.bin` ha già non ripartono. Le connessioni tenute aperte no: legano i thread di chi risponde, ed è un modo di bloccare un nodo |
| F3 | Ogni sessione rilegge tutte le monete non spese; in attesa, 4n+1 richieste ogni 15 secondi | Seguire le uscite e non le transazioni; `scripthash.subscribe` |
| F4 | Un processore con AVX2 ma senza ADX (Intel Haswell) cade sul motore di base: 4,0 contro 8,8 M chiavi/s | **Fatto il 26 novembre** (3.12): i motori a corsie AVX2 senza `-madx` |
| F5 | Motori nuovi misurati: hash con SHA-NI nel motore a corsie AVX2 (+9 %), curva a 4 corsie da 52 bit con AVX-IFMA (+30 %, +45 % con SHA-NI) | Aggiungerli |
| F6 | La scelta del motore CPU si rifà a ogni avvio (2 secondi); la memoria della scheda è contata due volte nel limite dei lanci | Ricordare la scelta; contare una volta |
| F7 | Un core intero riservato per ogni scheda OpenCL | `clFlush`, attesa, poi `clFinish` |
| F8 | La tabella del canguro è una potenza di due dentro `--mem`; voci da 16 byte | Voci da 8 byte, tabella di qualsiasi misura |
| F9 | pzpeer teneva 4,5 MB per 64 sessioni possibili | **Fatto**: ogni sessione tiene solo lo spazio di un saluto (32 KB in tutto) |
| F10 | Ogni giro di una ricerca lanciava tre domande e aspettava la più lenta: un nodo spento fermava il giro per tutto il tempo limite | **Fatto**: tre domande sempre in volo. Con nodi spenti e rifiuti lenti come su Windows lo scambio tramite cassette passa da oltre 30 a 23 secondi |

### 3.3 Semplificazioni proposte (circa 1000 righe in meno)

Fatte G3, G6, G7 e G8: 92 righe in meno nel codice nostro e 7418 nella copia di cb-mpc. Le altre sono state valutate e lasciate, con il motivo.

| | Proposta | Righe |
|---|---|---|
| G1 | Un solo strato di sistema per `src` e `src/peer` | **Valutato, non fatto (20 novembre):** le funzioni in comune sono una dozzina, ma con regole diverse: in pzpeer ogni file e socket non passa ai processi figli, i file segreti rifiutano i collegamenti e le risposte sono vero o falso; lo strato di pzsolve legge anche le sue variabili d'ambiente. Unirli porterebbe in pzpeer le dipendenze di pzsolve, o cambierebbe come pzsolve apre i suoi file |
| G2 | Una tabella delle opzioni, e una dei campi `chiave=valore` dei file | **Valutato, non fatto:** il file di rete ha già la sua tabella (le chiavi e un controllo per chiave), e in pzpeer ogni opzione è una riga; una tabella generica aggiungerebbe il codice che la legge senza togliere righe |
| G3 | Le funzioni per l'esadecimale stanno in 9 posti: una coppia in pzcore | **Fatto (20 novembre):** `pz_hex_digit`, `pz_hex_encode` e `pz_unhex` in pzcore (u256.h), usate da pzsolve e da pzpeer; via le sette copie |
| G4 | I nove `eng_*.c` e i sei `h160_*.c` cambiano solo per le definizioni: un file per famiglia compilato più volte | **Valutato, non fatto:** ogni file è già una dozzina di righe di definizioni attorno a un'unica implementazione inclusa; compilare un file più volte sposterebbe quelle righe nel Makefile, e i nomi dei file non direbbero più quale motore c'è |
| G5 | `rt.c`: tredici scelte fra CUDA e OpenCL diventano una tabella di funzioni | **Valutato, non fatto:** le scelte sono righe brevi e si leggono dove servono; una tabella di funzioni aggiunge un passaggio in più per seguire il codice e toglie poche righe |
| G6 | Tre riempimenti di SHA-256 in pzcore (hash.c, kdf.c): uno | **Fatto (20 novembre):** `pz_sha256_tail` in hash.c fa il riempimento per `pz_sha256` e per l'HMAC; via `sha_final` e la struttura di appoggio di kdf.c |
| G7 | La copia di cb-mpc tiene 7400 righe che pzsign non usa | **Fatto (20 novembre):** tolti 16 sorgenti e 46 intestazioni, 7418 righe; nessun sorgente usato cambia, e le prove della firma passano |
| G8 | Togliere i numeri di sequenza dei record (con C1) | fatto il 18 novembre, con C1 |

### 3.4 Documenti da allineare

**Fatto (20 novembre)**, tutto l'elenco qui sotto, controllato sul codice. Due cose erano da correggere nel codice e non nei documenti: le
stime di tempo di una rete ora contano 1,18·√W (`KANG_C_NET` in pzsolve, `C_MEAN` in pzpeer), il costo misurato del cammino di net-06;
le chiavi di prova della ricerca lineare sono 16 in media in blocchi di 2^34-2^37 o 2^42-2^45 chiavi, non "da 40 a 48 bit".

- `PROGETTO_RETE.md`: UPnP è acceso se non lo si spegne (6.2 dice il contrario); nessuno firma il file della rete (6.1, 6.2); i
  fallimenti di firma sono 3 per punto proprio e non per coppia (3.2, 5.2); le chiavi di prova della ricerca lineare sono 16 e i
  blocchi da 40 a 48 bit (8.2); "mezz'ora al massimo" (2.3) vale solo con blocchi e server puntuali.
- `ALGORITHMS.md`: 2.2 (NEON ha una sua implementazione), 4.3 (due camminatori che si uniscono su un ciclo di 2), 4.4 (lo sfoltimento
  "una corsa su cinque" vale solo con `--mem` potenza di due), 4.7 e `USAGE.md` 9.2 (quanto si perde a un riavvio), 277-280 (cosa
  riporta la scheda nella ricerca lineare).
- `NETWORK.md`: la custodia (55 GB per casa contro i 37 scritti), l'indice (dal 19 al 25 %, non 1/8), la stima 1,06 mostrata nelle
  campagne di rete contro l'1,18 misurato.
- Commenti: `chain_spv.c:3`, `chain_spv.h:6`, `dht_priv.h:10`, `net.h:5`, `gpu.c:178`, `hash160_batch.h`.

### 3.6 Il lavoro nel tempo

Una casa che entra dopo mesi confronta ogni suo punto con quelli di tutte le case dal primo giorno (NETWORK.md 7.3). Restano aperti:

| | Rilievo | Rimedio proposto |
|---|---|---|
| H1 | Chi lascia la rete per sempre porta via la sua metà di ogni chiave: una coincidenza con i suoi punti non la paga nessuno | Da decidere. Oggi chi vuole lasciare il posto a qualcuno gli dà la cartella della casa (`home.seed` e la campagna). Un comando "passa la casa" lo renderebbe esplicito |
| H2 | Un camminatore che incontra il punto di una casa sparita, o una fusione con un'altra casa, rifà un percorso già fatto | Farlo ripartire da un punto nuovo (pzpeer lo dice a pzsolve). È la "ripartenza dei camminatori" già in programma |
| H3 | La ripubblicazione quotidiana di mesi di punti pesa molto | Fatto il 18 novembre con la ripubblicazione leggera (C2) |

### 3.7 Dall'analisi del 13 novembre

Quattro revisori (rete, pagamento, demone e sistema, pzsolve) più gli analizzatori statici. Corretti con prova: il tetto ai proprietari
di un custode, il saluto lungo che teneva un posto di sessione, la blockchain interrogata prima del canale (il saluto arrivava oltre i 2
secondi), il partner preso anche durante l'attesa al relay, l'ordine dei primi contatti, la ripubblicazione dopo ogni tabella vuota, gli
avvisi propri prima di quelli da custode, la mediana delle commissioni, la cartella con un collegamento, `nohup`, la password di `--rpc`;
in pzsolve `merge` che non cancellava nulla al ritrovamento cifrato, `near.txt` lasciato, il seed nel log, i collegamenti nella cartella
della campagna, la cifratura fallita che chiudeva la campagna. Restano aperti:

| | Rilievo | Rimedio proposto |
|---|---|---|
| J1 | 16 blocchi di indirizzi tenevano occupati i 32 posti d'attesa di un relay (e 4 blocchi gli 8 relay attivi) | **Fatto:** 256 posti d'attesa (sono solo connessioni aperte, senza thread) e 32 relay attivi; con 2 per blocco ne servono 128 e 16 |
| J2 | Pochi blocchi di indirizzi potevano tenere piena la coda degli avvisi da custode con lavori lenti | **Fatto:** al massimo 8 avvisi in coda per i record di un blocco |
| J3 | 256 monetine di polvere mandate all'indirizzo del puzzle fra la prima e la seconda transazione nascondono la seconda alla catena leggera: niente denaro perso, ma il pagamento risulta fallito | **Aperto.** Ogni voce della storia va scaricata per sapere se spende: un tetto serve sempre, e chi manda la polvere lo supera pagando. Con un nodo proprio il problema non c'è |
| J4 | Un pzpeer ucciso (memoria finita, crash) lasciava pzsolve acceso | **Fatto, meglio del rimedio proposto:** pzpeer dà a pzsolve un canale in ingresso che non scrive mai (`--stop-with-input`); quando pzpeer finisce, anche ucciso, il canale si chiude e pzsolve salva lo stato e si ferma, come a un Ctrl-C. Provato con `kill -9`: fermo dopo 2 secondi |
| J5 | Il "chiudi alla nascita" dei descrittori arrivava un attimo dopo la loro creazione | **Fatto:** `SOCK_CLOEXEC`, `accept4`, `pipe2` e `fopen "e"` su Linux, `POSIX_SPAWN_CLOEXEC_DEFAULT` su macOS |
| J6 | La cartella della casa è controllata, poi i file si riaprono per nome | **Accettato:** la cartella è dell'utente e nessun altro la può aprire, quindi nessun altro può scambiare i file nel frattempo; `openat` non esiste su Windows |
| J7 | Su macOS `fsync` non arriva fino al disco: un'interruzione di corrente subito dopo il primo avvio poteva perdere il seme nuovo | **Fatto**: `F_FULLFSYNC` per il seme, lo stato delle sessioni e la custodia |
| J8 | Il controllo della monetina minuscola di `t_session` falliva a volte (i pagamenti erano giusti) | **Fatto**: quando la metà dispari dava un satoshi in meno a F, la differenza senza segno del test diventava enorme |

### 3.8 Dalla revisione della firma a due (14 novembre)

Due revisori: la crittografia (pzsign e cb-mpc) e il protocollo fra le due case. Nessuno dei due ha trovato un modo per rubare la chiave
o un pezzo, per prendersi più della metà o per far firmare altro. La copia di cb-mpc (5 ottobre) contiene già le due correzioni
dell'avviso di sicurezza GHSA-h3cp-c5xm-jjj8 (versioni 0.1.1 e 0.2.2). Corretti con prova: lo svuotamento del premio a cicli (al massimo
5 pagamenti non riusciti per punto, e una commissione del partner oltre 4 volte la propria è rifiutata), il conteggio gratuito dei
fallimenti quando S trattiene le firme (pzsign ora lo distingue, 0x84), la monetina di S che restava bloccata (ora c'è solo quella di
F, e la seconda transazione divide tutto a metà), le firme controllate prima di essere mandate, i posti delle coppie che hanno finito i
tentativi. Restano aperti:

| | Rilievo | Rimedio proposto |
|---|---|---|
| K1 | Chi attacca sceglieva se essere F o S cambiando identità (2 tentativi in media) | **Fatto:** F viene dal punto e dai due tipi (`pieces_first`); nessuna delle due case lo può scegliere |
| K2 | Una copia vecchia della prima transazione, minata al posto di quella nuova, fermava la sessione per un giorno | **Fatto:** quando le monete della prima sono spese in un blocco e passano altri 2 blocchi senza di essa, la sessione si ferma e riprova dopo 10 minuti dalle monete di allora (un pagamento non riuscito, al massimo 5) |
| K3 | Un partner che manda messaggi rovinati fa contare un fallimento alla casa onesta: con i punti di una fusione può arrivare a 10 e la casa non firma più su quella rete | **Fatto (20 novembre):** il conteggio della rete riparte 30 giorni dopo il suo primo fallimento (la riga del file ne tiene la data); quello di un punto proprio resta per sempre (E4). Un disturbatore ferma una casa sulla rete al massimo per un mese |
| K4 | Le prove di cb-mpc sulla chiave Paillier e sulla cifratura del pezzo hanno un margine di 2^-50 per tentativo, non 2^-128 | Nessuno: con i limiti dei fallimenti resta vicino a 3·2^-50. Scritto in NETWORK.md 7.6 |
| K5 | Un messaggio costruito ad arte fa riservare alla libreria circa 120 MB prima di essere rifiutato | Accettato (solo memoria, una firma alla volta); oppure una modifica locale di cb-mpc |

### 3.9 Scambio dei pezzi, stima del tempo, passo (14 novembre)

Fatti: lo scambio dei pezzi dopo 6 conferme del pagamento (la chiave in `FOUND.txt` e sullo schermo, cifrata se pzsolve cifra; `paid`
nella cartella per riprendere dopo un riavvio), una riga sullo schermo per ogni passaggio, la stima della rete ogni 10 minuti, e
`hunt --step` per i bit ignoti in mezzo alla chiave. Decisioni prese: scelta a) per le monete delle catene nate da Bitcoin (chi riceve
il pezzo per primo può tenersi il suo e prenderle da solo: al #140 circa 300 euro, contro metà premio intatta); 6 conferme, non 100.

| | Cosa | Stato |
|---|---|---|
| L1 | La stima del tempo nella ricerca lineare condivisa: dai blocchi rivendicati nella rete, come per i punti del canguro (NETWORK.md 11) | **Fatto:** ogni 10 minuti la riga con i blocchi dichiarati dalla casa e quelli della rete (le dichiarazioni tenute × nodi / 5) |
| L2 | La stima la vede solo una casa che accetta connessioni (solo quella custodisce punti) | Si potrebbe chiedere il conteggio a qualche custode; per ora no |
| L3 | Bit ignoti in più gruppi separati: `--step` copre un solo blocco | Più campagne, una per ogni valore del gruppo più piccolo |
| L4 | BSGS può riportare la chiave giusta poco oltre la fine dell'intervallo (anche senza passo) | Innocuo: la chiave è sempre quella giusta |
| L5 | Con la sola scheda video, vicino all'ordine del gruppo, un lancio il cui centro è il punto all'infinito ferma il dispositivo (c'era già prima) | Ricontrollare quel lancio sul processore, come le altre anomalie |

La verifica del 14 novembre (quattro revisori: lo scambio dei pezzi, il passo, l'usabilità, memoria e thread) ha trovato un caso grave,
corretto con un test: con una seconda coincidenza fra le stesse due case e i server Electrum, che nascondono le monete spese nel mempool,
un partner disonesto poteva farsi dare il pezzo prima del pagamento vero e prendersi il premio. Ora il pagamento conta solo se la seconda
transazione della coppia è in un blocco, il pezzo parte dopo 6 sue conferme provate, e la catena leggera guarda anche il mempool.

### 3.10 Quota dell'autore e prima partenza (14 novembre)

Fatti A8, A9 e A10 (sopra), con i test: le tre uscite della seconda transazione e le versioni delle condizioni (`t_payout`), la sessione
intera con il 45% a testa e un saluto con condizioni sconosciute (`t_session`), la prova con tre case su regtest che controlla il 10%
all'indirizzo dell'autore anche quando una casa trova la chiave da sola.

| | Cosa | Stato |
|---|---|---|
| N1 | Se pzsolve cifra la chiave (`--encrypt-pubkey`), la casa non la legge e non manda le monete: lo fa chi la decifra, e la quota è sulla fiducia | Si potrebbe far passare la chiave da pzsolve a pzpeer prima di cifrarla |
| N2 | Due case con un programma modificato, nella stessa coppia, possono saltare la quota | Inevitabile: nessun controllo a distanza è possibile su un programma che gira a casa d'altri |
| N3 | La parte attesa del premio, come la stima del tempo, la vede solo una casa che accetta connessioni | Come L2 |
| N4 | Nella prova con tre case le coincidenze fra case diverse erano dieci volte più rare del previsto | **Causa trovata, corretto.** Con 14 bit di zeri ogni casa trovava circa 900 punti al secondo e ne pubblicava qualche centinaio: la rete ne vedeva solo dal 16 al 36%, e gli incontri crescono col quadrato dei punti visti. Il cammino è giusto: due pzsolve affiancati, confrontando tutti i punti, si incontrano quanto previsto. La prova ora usa 16 bit (la rete vede tutti i punti, la coppia paga al primo tentativo), e la casa avvisa quando ha punti in attesa di pubblicazione |
| N5 | Quanti punti al secondo può pubblicare una casa | Qualche centinaio su questa macchina di prova. Sulla rete del #140, con 30 bit di zeri, basta per decine di schede video in una stessa casa; oltre, la casa pubblica in ritardo (e lo dice). Dal 18 novembre una casa non fa più una ricerca per punto (F2): nella prova con tre case la rete vede tutti i punti anche con 14 bit di zeri. E con i bit di zeri adattivi (C2b) una rete grande ha molti meno punti per casa |

### 3.11 Dalla revisione del 20 novembre (quattro revisori), corretto il 25 novembre

Nessun modo di rubare il premio. Tutto quello che segue è corretto, con una prova per ciascun punto dove si poteva.

| | Cosa | Com'è ora |
|---|---|---|
| 1 | Ricerca lineare: con blocchi da 2^44 si potevano dichiarare solo i primi 2^20 blocchi (1 su 64 per il puzzle 71) | `block_range` e `block_count` in `block.c`, un solo calcolo; rifiutati solo i blocchi oltre la fine dell'intervallo |
| 2 | La risposta di un custode alla prova di un punto veniva sempre scartata | corretta; da quella risposta si prendono solo le coppie dei punti provati, al più 7 per punto |
| 3 | Con la catena leggera, server che non mostrano monete facevano credere preso il premio | una risposta vuota dei server non prova niente: la sessione finisce e una successiva richiede |
| 4 | Una riga vecchia dei fallimenti, già al limite, non riceveva mai la data | scritta anche quando la firma viene rifiutata |
| 5 | La prova bloccata sul Mac (vedi 1.5) | la copia nell'invio stesso; il filo dell'altra coppia finisce con il caso |
| 6 | pzsolve: un avvio fallito faceva buttare il branco salvato | `herd.bin` non dipende più dal numero di sessione, e viene cancellato quando il primo camminatore riparte |
| 7 | `--part` dopo `--` faceva dichiarare mezzi blocchi | rifiutato |
| 8 | Una dichiarazione falsa costava un quarto di blocco | metà; nel secondo giro si rifà per primo il blocco con meno chiavi di prova, mai uno della casa stessa |
| 9 | Una dichiarazione si copiava con identità nuove senza cercare | un altro proprietario è tenuto solo con una chiave di prova che manca alle dichiarazioni già tenute |
| 10 | Un nodo della ricerca lineare accettava record del canguro (5 GB di disco gratis) | rifiutati, con gli avvisi e le prove; niente `store.bin` |
| 11 | 6to4 e Teredo: ogni indirizzo era un blocco a sé (limiti aggirati) | ogni indirizzo non locale conta col suo /24 (IPv4) o /32 (IPv6): 6to4 e Teredo come gli altri |
| 12 | Un custode ostile fermava la pubblicazione per un'attesa a record | una sola prova per custode, con tutti i suoi record |
| 13 | Avvisi falsi con un record vero intasavano il filo di lavoro della vittima | al più 8 lavori per blocco di indirizzi, anche nei nuovi tentativi; i lavori a turno, una sola ricerca dell'altro proprietario |
| 14 | Due copie piantate nascondevano il vero proprietario | il custode tiene tutti gli "altri" del punto (fino a 7) e li dà nella risposta a `PROVE` |
| 15 | Una casa riavviata prima del blocco della seconda transazione dimenticava il pagamento | la seconda transazione è salvata appena firmata, e ogni sessione comincia scambiandola: chi non l'ha la prende dall'altro |
| 16 | S mandava la seconda transazione una volta sola, e dopo l'annullamento aspettava 144 blocchi | mandata di nuovo a ogni blocco; S smette di aspettare quando F annulla |
| 17 | Un pzsign che non parte faceva contare tentativi di pagamento | non conta, la sessione dice perché e avvisa il partner; all'avvio si prova `pzsign version` |
| 18 | Lo scambio dei pezzi non si chiudeva se le due case avevano già speso le loro uscite | conta anche l'uscita dell'autore |
| 19 | La monetina di F restava sulla sua chiave nei fallimenti | F la manda al suo indirizzo alla sessione dopo, quando nessuna seconda transazione la può più prendere |
| 20 | pzpeer aspettava pzsolve senza limite | un minuto, poi l'interruzione, poi la chiusura forzata |
| 21 | La ripubblicazione dei record bloccava il ciclo della casa | nessun blocco tenuto durante una connessione: una pubblicazione non aspetta più la ripubblicazione |
| 22 | Ricerca lineare: registro della casa, slot illeggibili, ripubblicazione, scelta del blocco | registro della casa nel formato dei custodi (`own_claims.bin`), slot rotti tagliati, ripubblicazione due minuti dopo l'avvio e poi ogni giorno, una per giro |
| 23 | pzsolve: `--node` nel suggerimento di ripresa | aggiunto. `--dp-bits` diverso su una campagna di rete: **lasciato**, perché è pzpeer stesso a cambiarlo (con `dpbits.txt`) |
| 24 | Seme: il controllo dei 64 campioni non separava il rumore | ogni campione misura lo stesso lavoro; una macchina senza rumore vede la fonte rifiutata, e il seme della casa si fa allora con un TPM o su un altro computer |
| 25 | Pubblicazione lenta (una connessione alla volta) | 16 custodi alla volta, e la ripubblicazione in ordine di chiave (a 1000 nodi un pacchetto va a circa 9 custodi invece di 695) |
| 26 | Apertura del registro quadratica | un solo ordinamento |
| 27 | La variante del canguro scelta da un solo campione | tre campioni alternati per forma |
| 28 | Le attese chiedevano la catena a ogni giro | una volta per blocco; l'esame dell'indirizzo solo quando decide qualcosa |

Sul punto 8: con la soglia a metà, un blocco onesto su cento circa non si può dichiarare. La casa che l'ha cercato lo tiene per sé e
non lo rifà, ma altre case lo possono riprendere: il tempo atteso per la chiave cresce di meno dell'1%, la fine del primo giro un po'
di più (quei blocchi vengono ripresi più volte).

### 3.12 Dalla revisione del 26 novembre (quattro revisori)

Nessun modo trovato, nel protocollo, di rubare il premio o di sapere un pezzo; il punto P1 è un furto possibile attraverso i server
Electrum, e resta da decidere (sotto). Tutto il resto è corretto, con una prova che fallisce senza la correzione dove si poteva.

| | Cosa | Com'è ora |
|---|---|---|
| P2 | F cercava le sue monetine senza `S_ALIVE`: con un nodo lento S si arrendeva a ogni sessione | la ricerca manda gli `S_ALIVE` |
| P3 | Catena leggera: un minuto di nuovo per ogni moneta; 256 monete di polvere bloccavano la casa per ore | un minuto per server per operazione, intestazioni comprese; chi annuncia blocchi che non mostra resta fuori dieci minuti |
| P4 | Solo le prime 256 voci della storia: la polvere nascondeva una spesa | tutta la storia dopo la moneta, con una memoria di ciò che ogni transazione spende |
| P5 | Il pagamento di monete arrivate dopo teneva l'altezza di quello prima: monete bloccate | la sua seconda transazione, reinviata o vista nulla |
| P6 | Lo scambio dei pezzi con tutte le uscite già spese si chiudeva solo nella stessa sessione | anche in una sessione dopo |
| P8 | Il pagamento da solo non aspettava una seconda transazione firmata e non ancora in un blocco | la aspetta |
| P10 | Una transazione qualsiasi nel mempool (anche inventata da un server) bloccava lo scambio per sempre | conta solo una spesa firmata con la chiave dell'indirizzo |
| P11 | Una seconda transazione tenuta si reinviava solo dentro una sessione | la casa la reinvia ogni dieci minuti |
| P7, P9, P12-P17 | Domande ripetute alla catena, un'interruzione dello scambio, messaggi al partner con indirizzi e percorsi, tariffe in sat/kvB, file ereditabili, buffer di stdio con le distanze | corretti |
| R1 | 7 o 8 copie piantate nascondevano ancora una coincidenza | tutti gli altri proprietari (fino a 8, i più nuovi prima); un record provato è tenuto oltre il limite (fino a 16) |
| R2 | Un'ondata di avvisi buttava fuori quello vero prima del suo lavoro | un avviso col lavoro in coda resta; un solo lavoro per avviso |
| R3 | Un nodo che inoltrava i byte prendeva il posto di un contatto buono | l'indirizzo cambia solo dopo un fallimento del vecchio |
| R4 | Il test del relay contava 30 nodi su 32 (il fallimento visto su macOS) | conta tutti i nodi |
| R5 | Chiunque leggeva la cassetta di chiunque | `MBOX_GET` solo al proprietario |
| R7 | Due pzpeer sulla stessa cartella si rovinavano i file | `DIR/lock`: il secondo si ferma |
| R6, R8-R17 | 256 file aperti su macOS, orologio messo indietro, `peers.txt` scritto da due, relay su una rete lineare, codice morto, "cannot listen" per ogni errore | corretti |
| Win | Cartella fatta da una shell elevata: l'utente restava fuori | diritti col SID dell'utente, anche per una cartella fatta a mano |
| S1 | `--part` senza `-s`: quote sovrapposte e blocchi che nessuno cerca | rifiutato: serve lo stesso `-s` su tutti i computer |
| S2 | Windows: la cartella di una campagna sotto `C:\` era leggibile da tutti | lista di accesso dell'utente e del sistema |
| S4 | Una campagna cifrata lasciava al ritrovamento il suo avanzamento (con il seed degli altri computer porta alla chiave) | rapporti senza avanzamento, `stats.csv` cancellato |
| S6 | Canguro: una chiave non salvata non si ritrovava più | il punto che risolve va anche in `dp.bin` |
| S3, S5, S7-S9, S11-S14 | Seed stampato, `near.txt` aperto su Windows, checkpoint lento preso per sospensione, `dp.bin` e rinomine non sincronizzati, contatore statico, doppioni, commenti, `merge` senza log | corretti |
| M1 | I motori a corsie AVX2 volevano ADX: Haswell girava 2,5 volte più lento | compilati senza ADX (stessa velocità altrove) |
| M2 | La verifica rapida di un motore metteva una chiave sola: un motore rotto a metà passava | una chiave in ogni finestra, ai bordi e al centro (pochi ms) |
| M3-M11 | GPU e OpenCL: lanci con gruppi di 1, un centro all'infinito preso per errore, quasi-colpi doppi, compilazioni ripetute, scritture non controllate; autotune senza motori buoni; SHA-256 su macOS 11 arm64 | corretti |

**Da decidere.**
- **P1, catena leggera e scambio dei pezzi.** ~~Da decidere~~: **fatto il 26 novembre con la strada (c)**, vedi 3.13. Dopo il pagamento una casa dà il suo pezzo quando i server dicono che all'indirizzo del
  puzzle non resta niente. Un partner che controlla tutte le risposte Electrum di quella casa (rete locale, DNS, o un file di rete con
  i suoi server) può nascondere la moneta grande, far pagare una moneta esca, e dopo lo scambio prendersi il premio vero da solo. Strade:
  (a) mettere nel programma le monete di ogni puzzle (txid, uscita, importo, verificati da due fonti) e dare il pezzo solo quando
  ciascuna è provata spesa; (b) con la catena leggera non dare mai il pezzo, solo con un nodo proprio (`--rpc`): le case incassano
  comunque la loro quota, perdono solo la chiave dopo; (c) tutte e due. Proposta: (c).
- Contatore dei passi del canguro a 64 bit (finisce dopo 1,8·10^19 passi di una campagna, decenni): lasciato.
- Il registro delle dichiarazioni inserisce in ordine (lento verso i 2 milioni); l'apertura del deposito ricalcola un SHA-256 per voce
  (1-2 minuti a deposito pieno): da cambiare con il formato dei file.
- Un solo corpo per il passo del canguro: no, costa l'1,2 % di istruzioni in AVX2. L'inversione a lotti di BSGS in comune con gli altri
  motori: ~~da misurare~~ fatta il 26 novembre per BSGS con IFMA (10 % in meno per finestra, 3.14); per gli altri motori resta da
  decidere (0,4 % su AVX-512 senza IFMA).

### 3.13 Le migliorie del 26 novembre

| | Cosa | Com'è ora |
|---|---|---|
| P1 | Con la catena leggera, server che nascondono la moneta del premio facevano dare il pezzo della chiave | le monete del creatore dei puzzle 140-160 sono nel programma (`data/prizes.csv`, due esploratori concordi); il pezzo si dà solo quando ognuna è spesa con la prova; senza monete note, con la catena leggera mai (con `--rpc` sì); prima dei pezzi ogni casa dice se dà il suo, e chi non lo dà non riceve quello dell'altra |
| M1 | Il rilascio conteneva solo pzsolve | gli archivi hanno anche pzpeer e pzsign, con le licenze di libsecp256k1, cb-mpc e OpenSSL; il rilascio li compila e li prova su tutte e cinque le piattaforme |
| M2 | Sanitizer e test in regtest solo a mano | due lavori nuovi nella CI: i test di pzpeer sotto ASAN, UBSAN e TSAN (pzsign compreso) e bitcoind in regtest con sessione, rete intera, catena leggera, sweep e ricerca lineare |
| M3 | Nessun controllo, in produzione, di un motore che perde chiavi | ricerca lineare: le quasi giuste del motore CPU e di ogni GPU contate contro le chiavi cercate, a finestre di 2^38 chiavi; chi ne trova troppo poche viene escluso e le sue chiavi tornano da cercare. Da decidere: conservare i conti tra una sessione e l'altra (`state.dat`), un conto per core, un punto distinto ricontrollato ogni tanto per il canguro |
| M4 | `--part`: niente mostrava che i computer avessero lo stesso seed e gli stessi blocchi | l'identificativo della divisione (8 cifre) a ogni avvio, in `report.txt` e in `where`; `--seed-file` dà il seed senza scriverlo nella riga di comando |
| M5 | Registro delle dichiarazioni lento verso i 2 milioni; deposito pieno aperto in 2 minuti | una tabella per indice del blocco (1 µs a dichiarazione); il deposito si apre in circa 50 s, con un indice che nessuno può intasare; nessun file cambia formato |
| W | `pzsign.exe selftest` andava in crash ogni tanto su Windows (lo ha trovato la prova del pacchetto) | due corse fra le due parti del self-test, che sono due thread di un processo (nell'uso vero ogni parte è un processo): corrette, e la CI prova pzsign sotto TSAN |

### 3.14 Dalla terza revisione del 26 novembre: bug corretti e codice tolto

Chiesto: correggere i 18 bug, togliere tutto ciò che non costa niente, seme semplificato, custode passivo, tetto a vita delle firme
fallite. Il punto 24 di 3.11, la riga P1 di 3.13 e la voce E7 sono superati da questa sezione.

| | Cosa | Com'è ora |
|---|---|---|
| X1 | Chiave trovata da soli senza un nodo: `pzpeer sweep` e il pagamento da soli non vedevano le monete più vecchie del checkpoint; al massimo spazzavano un deposito recente, mostrando la chiave e lasciando il premio a chi arriva primo | superato da 3.15: la catena leggera parte dal blocco 338688 e prova ogni moneta di ogni puzzle; lo sweep le prende tutte o non fa nessuna transazione |
| X6 | Una casa con nodo e una senza costruivano prime transazioni diverse ai puzzle 155 e 160 (vecchi depositi di estranei) | superato da 3.15: le due case si scambiano le monete che vedono (`S_COINS`) e usano quelle comuni |
| X2-X4 | Scambio dei pezzi: 100 000 satoshi di polvere lo bloccavano per sempre; con tutte le uscite spese e monete rimaste non si pagava più; una seconda transazione ridata dal partner era rifiutata dopo un cambio di indirizzo di incasso | conta solo ciò che un pagamento può prendere; una sessione dopo paga il resto; la seconda transazione si riconosce dalle firme |
| X5 | Firme fallite: ogni punto nuovo di un cammino dava al partner altri tre tentativi sullo stesso segreto, e il conto per rete ripartiva ogni 30 giorni | tetto a vita di 64 per casa e rete; al tetto la casa non firma più e dice di spostare `home.seed` |
| X7-X18 | Avvisi senza prova, cassette con coppie non provate, copie piantate su otto record, proprietari di un custode riempiti in 7 ore, ricerca fermata da quattro nomi in un blocco di indirizzi, spesa non vista in transazioni con più di 16 voci, `PROVE` senza risposta ritentato dopo una settimana, canarino senza salvataggio, file lasciati da una campagna cifrata | corretti (CHANGELOG 1.24.0) |
| RB | Custode attivo: ricordava le coppie avvisate, teneva i record in attesa di prova, mandava avvisi suoi | passivo: dice al proprietario che altri hanno la stessa x, e basta; il proprietario prova e avvisa. Due limiti di frequenza invece di quattro, due tipi di lavoro invece di tre |
| SB1 | Generatore del seme: sei fonti, prove di salute, regola dei 128 bit, memoria bloccata (1024 righe) | il generatore del sistema letto due volte, con quello della CPU e 256 letture del contatore mescolati con HKDF (281 righe); rifiuta solo se il generatore del sistema fallisce |
| RA, PA, SA, MA | Regole e codice doppi | tolti: l'elenco è nel CHANGELOG (Changed e Removed). Fra questi il motore CPU `avx512`, il modulo GPU `kang4g`, `HOME/proved`, i limiti per proprietario e per identità |
| S | Campagna cifrata che trova la chiave | restano solo `FOUND.txt` e `campaign.conf` senza seme: tutto il resto della cartella è cancellato, anche un file messo lì a mano |

**Cambi di comportamento da sapere.** `-s auto` è rifiutato. Dopo un ritrovamento cifrato lo stato di uscita di `--on-found` è solo sul
terminale. Un `PROVE` per un record che il custode non ha preso chiude la connessione. La regola dello scambio dei pezzi passa per
ciò che resta all'indirizzo (le monete nominate non ancora viste spese contano come rimaste).

**Resta aperto.**
- ~~Una seconda transazione con una tariffa troppo bassa non si può sostituire~~: fatto, 3.15.
- ~~`data/prizes.csv` è una fotografia da rifare quando il checkpoint va avanti~~: non più, 3.15. Le righe di un puzzle risolto vanno
  tolte.
- La cancellazione dei file al ritrovamento cifrato è provata qui su Linux; su macOS e Windows la prova solo la CI.
- `tools/gen_ptx.py --check` vuole NVRTC, che qui e nella CI non c'è: i 25 moduli sono stati rifatti quando è stato tolto `kang4g`,
  il controllo va ripetuto su un computer con CUDA. Due espressioni ternarie da piegare alla prossima rigenerazione.
- Non fatti, perché costano un po' di sicurezza o di velocità e non erano chiesti: `POSSESS` (RB3), RB9, e le liste PB, SB3-13, MB
  della revisione.
- La strada vera con i server Electrum pubblici resta provata solo con server finti: da qui non si raggiungono.
- Una ricerca nella rete non ricorda i contatti che non rispondono se non sono nella sua tabella: un nodo che ne nomina di morti li fa
  richiamare a ogni ricerca. Su Linux e macOS un rifiuto non costa niente, su Windows due secondi (lo ha mostrato la CI: il test della
  rete ci passava a fatica, ora il nodo che li nominava si ferma a fine prova). Il costo di una ricerca resta limitato (4 nodi per
  blocco di indirizzi, 10 secondi nella prova con nodi ostili); una memoria breve dei rifiuti lo ridurrebbe. In 3.15 un fallimento conta
  solo all'indirizzo della tabella, e un nodo nominato a un indirizzo falso si cerca anche a quello vero.

### 3.15 Dalla quarta revisione del 26 novembre: tutto corretto, e le monete senza tabelle da aggiornare

Chiesto: un deposito di qualsiasi importo a un indirizzo di un puzzle non deve mai fermare niente perché checkpoint o tabelle non sono
aggiornati; correggere tutti i bug; un programma pronto e resistente; due case che trovano una coincidenza si dividono il premio in
modo sicuro.

| | Cosa | Com'è ora |
|---|---|---|
| Monete | Senza nodo le monete più vecchie del checkpoint (blocco 969696) non si potevano provare: serviva una tabella da rifare a ogni spostamento del checkpoint, e lo sweep rifiutava un indirizzo senza monete nominate | la catena leggera parte dal blocco 338688, prima di qualsiasi moneta di qualsiasi puzzle (gennaio 2015): ogni moneta, vecchia o nuova, si prova come le altre, e il checkpoint non si sposta mai più. La prima partenza scarica circa 51 MB di intestazioni (minuti), poi aggiunge solo i blocchi nuovi. La tabella tiene solo le tre monete del creatore per puzzle (231), che sono storia |
| Coincidenza | Due case con catene sfasate di un blocco, o un deposito confermato durante la sessione, costruivano prime transazioni diverse | `S_COINS`: ognuna manda le monete che vede, e si usano quelle comuni; una moneta vista da una sola va a una sessione dopo |
| N1 | Un avviso che aspettava più di un giorno era rifiutato per sempre: la coincidenza persa fino al riavvio | scaduto si dimentica, e la coppia torna nuova |
| N4 | La propria prova spedita una volta e persa non si rispediva | l'avviso è fatto solo quando qualcuno la prende, e la si rimanda ogni ora per tutto il giorno |
| N2 | Con un punto valido si svuotavano le cassette di una casa | si butta la coppia più vecchia del punto che ne ha di più: un'inondazione butta le sue |
| N3 | Un custode pieno faceva pagare 256 gettoni per ogni record di una casa nuova, ogni volta | 256 solo se il proprietario viene preso, se no uno |
| N5, N6 | Un indirizzo falso nascondeva un nodo; le coppie di una risposta a PROVE contate in tutto | si prova anche l'indirizzo vero; 8 per ogni record; un fallimento conta solo all'indirizzo della tabella |
| M1 | Un partner ostile faceva pagare commissione e monetina a ogni punto nuovo, senza tetto per la casa | 10 pagamenti non riusciti in tutta la vita della casa su una rete (poi serve un seme nuovo); la monetina si misura sulla tariffa più bassa |
| M2 | Monete rimaste contate a 20 sat/vB ma non pagabili alla tariffa della sessione: pezzi fermi | contano solo quelle che un pagamento a quella tariffa prende |
| M3 | Sweep col nodo: con la scansione fallita mandava solo le monete nominate | rifiuta su ogni catena |
| M4, M5 | Seconda transazione minata mentre la casa era via presa per nulla; polvere che bloccava il recupero delle monetine | si chiude dalle monete spese; la polvere si lascia |
| M6 | Sweep di 16 ingressi con il consiglio di mandare il resto dopo (il resto lo prende chiunque legge la chiave pubblica) | tutte le monete che valgono il loro ingresso in una transazione, fino a 675 |
| RBF | Una seconda transazione con tariffa troppo bassa aspettava per sempre | le due case la rifirmano alla tariffa nuova, dalle stesse monete alle stesse uscite: qualunque delle due venga minata, la divisione è la stessa |
| S1, S2 | Campagna cifrata uccisa fra il ritrovamento e la cancellazione: seme e stato restavano per sempre; su Windows la chiusura della console arrivava prima della cancellazione | il `hunt` o `merge` dopo cancella; su Windows si aspetta la cancellazione |
| S3-S8 | `-s` rifiutato dopo il ritrovamento cifrato, `where` con un ordine falso, chiave non salvata detta FOUND, due salvataggi, copie del seme in memoria | corretti |
| E1-E3 | Commento della tabella dei moduli, avviso di `PZ_GPU_NK`, strumento di ricerca del modulo tolto | corretti, lo strumento tolto |

**Resta aperto.**
- La prima sincronizzazione dal blocco 338688 è provata qui solo con server finti (da qui i server pubblici non si raggiungono); con
  quelli veri la prova `tests/peer/mainnet.sh` (`pzpeer netcheck N --chain`), nel flusso `mainnet.yml` di GitHub. ~~`pzpeer sweep`
  riscarica le intestazioni a ogni esecuzione~~: le tiene in `pzchain`.
- Con un solo server lento e centinaia di monete, uno sweep leggero può esaurire il minuto del server e rifiutare (con `--rpc` no).
- ~~Dopo il pagamento, una moneta rimasta che nessun pagamento prende va a chi la spazza per primo con la chiave~~: con 3.16 nessuna
  casa conosce la chiave, e una moneta rimasta resta sotto la chiave comune per una sessione dopo.
- Un avviso fatto che rimanda ancora la sua prova è il primo a uscire quando la tabella degli avvisi (1024) è piena.
- Una macchina persa fra il ritrovamento cifrato e la cancellazione tiene seme e stato finché un `hunt` o `merge` non ci gira.
- La preferenza di `S_SECOND` per la seconda transazione rifirmata e la strada -2 della catena leggera sono provate solo leggendo il
  codice.

### 3.16 Semplificazioni del 27 novembre

Fatte tutte quelle senza perdita, più queste con una piccola perdita: M1, E1, S1 con E6, S2, N12, M4, S15. Non fatte: S3 ed E10 (scelta
del 27 novembre), E2 (serve prima una misura su una scheda vera), N14, N15, M2, S16, S17.

| | Cosa | Com'è ora |
|---|---|---|
| M1 | Lo scambio dei pezzi della chiave dopo il pagamento | tolto: il premio si divide come prima, 45/45/10, con la firma a due, e nessuna casa conosce la chiave. Era l'unico passo dopo il pagamento che poteva costare soldi a una casa. Le monete arrivate dopo all'indirizzo le paga una sessione successiva della coppia, a ogni partenza della casa |
| E1 | La forma "tags once" del canguro GPU | tolta: 19 moduli invece di 25, 243 KB in meno nell'eseguibile, l'1,5 % in meno sulla A2000 |
| S1, E6 | Le tabelle BSGS della 1.13.0 e prima | rifiutate con il consiglio di cancellarle: si rifanno |
| S2 | La velocità ricordata per la stima dei tempi | tolta: la stima compare dopo 3 secondi, provvisoria per un minuto |
| N12 | La rotazione delle chiavi dentro una connessione | tolta; prologo `pznet6` |
| M4 | Una firma di pzsign alla volta | le firme possono sovrapporsi: il conto della rete ne limita il numero |
| S15 | `stats.csv` | tolto |
| Senza perdita | Codice morto o doppio in rete, pagamenti, pzsolve e motori; un solo generatore casuale del sistema; strumenti di ricerca abbandonati; test più veloci | circa 1500 righe di codice e 2,5 MB di PTX in meno; `t_session` da 7 minuti e mezzo a 3, `t_record` da 10 a 3 secondi |

**Resta aperto.**
- E2: la durata fissa dei lanci GPU (250 righe in meno) aspetta una misura su una scheda vera.
- Le misure che decidono sul motore `shani`, sui greggi GPU in L2 (una RTX 40) e sulle finestre CUDA da 128 e NK 256.
- La prova `e2e_linear` in regtest dura da 8 a 34 minuti nella CI, secondo dove cade la chiave, con un limite di 40.
- Dopo M1 un pagamento si riconosce dalla monetina di F spesa insieme al premio. Se un'altra coppia prende il premio mentre la seconda
  transazione di questa aspetta, e F recupera la sua monetina, la casa scrive "paid" anche se non lo è stato. Nessun soldo si muove
  diversamente, e il premio è già andato: è sbagliato solo il messaggio.

### 3.5 Librerie

- OpenSSL 3.6.5 è uscita (13 CVE, nessuna tocca ciò che pzsign usa), ma cb-mpc accetta solo la 3.6.4: va scritta la procedura di
  aggiornamento (versione e impronta in `signer/openssl.sh`, il controllo di versione nella copia, i test).
- libsecp256k1 0.8.0 è uscita (nessuna correzione di sicurezza; verifica fino all'11 % più veloce).
- cb-mpc: a luglio e agosto 2026 gli autori hanno corretto quattro volte la firma a due. A ogni aggiornamento va riconfrontato
  `import_piece` di pzsign con il loro `dkg`.
- ~~Un archivio che contiene pzpeer o pzsign deve portare le licenze di cb-mpc, libsecp256k1 e OpenSSL~~: fatto il 26 novembre
  (`licenses/` negli archivi; quella di OpenSSL è `signer/openssl-LICENSE.txt`, da riprendere a ogni nuova versione).
- Le protezioni del compilatore (canarini, FORTIFY) vengono solo dalla distribuzione: su Windows `pzpeer.exe` non le ha. Vanno
  scritte nel Makefile per pzpeer e pzsign.
