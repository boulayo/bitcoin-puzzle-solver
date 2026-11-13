# Da fare: decisioni prese e decisioni aperte

11 novembre 2026. Si riferisce a [PROGETTO_RETE.md](PROGETTO_RETE.md).

Prima le poche cose che aspettano te, poi l'elenco di ciò che è deciso, con il motivo.

---

## 1. Aspettano te

### 1.1 Le tre prove che da qui non posso fare
Servono un computer con la rete di casa vera. Per ognuna basta guardare una riga dell'uscita.

| Prova | Comando | Cosa guardare |
|---|---|---|
| ritrovo sulla rete BitTorrent vera | su due computer: `pzpeer node --network FILE --dir CARTELLA --bt-router router.bittorrent.com:6881 --verbose` | le righe `rendezvous:` e poi `contacts` |
| apertura della porta sul router | lo stesso comando (la chiede da solo) | la riga `upnp:` |
| server Electrum pubblici | `pzpeer run --network FILE --payout INDIRIZZO --electrum electrum.blockstream.info:50001 --debug prova.log` su una rete della catena principale | la riga `chain:` e, in `prova.log`, le righe `electrum` |

### 1.1b Windows e Mac veri
Sulle macchine Windows e macOS della CI i test di pzpeer passano (su Windows anche la firma a due). Manca la prova di una rete intera. **Da fare tu:** avviare `pzpeer.exe run --network FILE --payout INDIRIZZO --debug prova.log` su un Windows vero, insieme a un altro computer, e guardare se parte e trova l'altro.

### 1.2 Revisione esterna della firma a due
La mia revisione non sostituisce quella di specialisti. **Da decidere:** se farla fare prima di usare bitcoin veri. **Consiglio:** sì, prima del #140.

La revisione interna di novembre (quattro revisori indipendenti: firma, sessione e pagamento, rete, risolutore) ha portato alle correzioni
del ramo. Restano questi punti di progetto, da chiudere prima del #140:
- chi attacca può scegliere se essere F o S rifirmando il proprio record: serve un passo di impegno e rivelazione dei nonce;
- un partner può far ripartire la coppia con annullamenti ripetuti per un giorno, e ogni giro costa una commissione al premio;
- avvisi falsi della tabella (record con una x copiata) possono occupare i 64 posti delle sessioni per un giorno;
- i limiti per blocco di indirizzi sono larghi, e il conteggio si azzera con collisioni di FNV;
- la custodia predefinita arriva a 2^28 record (37 GB);
- un saluto fa interrogare i server Electrum prima di qualunque prova;
- gli identificativi delle richieste verso BitTorrent sono prevedibili, e gli indirizzi IPv6 6to4 e Teredo non sono filtrati;
- il tempo con cui P1 fallisce dice quale firma del lotto non andava (al più log2(n+1) bit);
- i valori segreti possono restare nelle riserve di memoria dei numeri grandi di OpenSSL;
- le azioni della CI non sono fissate per hash, e `check_binary.sh` ha una sola lista per tutti i programmi;
- la regola "un punto, una coppia provata" vive in memoria e ricomincia a ogni riavvio della casa;
- con un nodo proprio, un riordino di blocchi che riporta in mempool la prima transazione mentre anche la seconda è lì sembra una spesa
  confermata.

### 1.3 La prova pubblica con due o tre computer
Quando vuoi farla: un puzzle di 60–64 bit creato da noi, con un premio piccolo, e `--debug FILE` su ogni computer. I file di debug non contengono segreti: me li puoi far leggere così come sono.

### 1.4 Aspetti legali
Una rete che divide premi fra sconosciuti: avvertenze, responsabilità, eventuale tassazione. Da chiedere al legale insieme a NOTICE, CONTRIBUTING e licenza commerciale, che sono ancora in sospeso.

### 1.5 Promemoria già aperti
- prova della cifratura su un Windows vero;
- cancellare i rami di prova vecchi su GitHub (`test-122`, `test-123`, `test-1231`, `test-1232`, `test-gpu-*`, `fix-intel-cl`);
- misurare la velocità di pzsolve su una RTX 4090 (ipotesi I10: tutti i tempi dipendono da questo numero).

---

## 2. Deciso

### A. La rete

| | Decisione | Motivo |
|---|---|---|
| A1 | **Nessuna firma sul file della rete.** La rete è l'impronta delle righe che la definiscono | È come fanno Bitcoin Core e BitTorrent: i parametri stanno nel programma, e chi li cambia è su un'altra rete. Niente chiavi da custodire, cento righe di codice in meno. La fiducia sta nel programma che si scarica dal tuo repository, come per qualsiasi programma |
| A2 | Nessun server tuo. Un nodo sul NAS, se un giorno lo vorrai, è un partecipante come gli altri | Con Tailscale da solo il nodo è raggiungibile solo dai tuoi dispositivi: per servire la rete gli serve una porta aperta verso Internet |
| A3 | I nodi si trovano attraverso la tabella di BitTorrent | fatto |
| A4 | **Chi scarica il programma lo avvia e partecipa:** `pzpeer run --network FILE --payout INDIRIZZO`. Cartella, programmi, porta (28140), apertura sul router, primi contatti e blockchain hanno un valore predefinito | L'apertura della porta è accesa da sola (`--no-upnp` la spegne): senza, quasi nessuno farebbe da custode. L'indirizzo di pagamento resta l'unica cosa da scrivere: è dove va il premio. Quando i parametri del #140 saranno fissati il file sarà dentro il programma, e non servirà nemmeno `--network` |
| A5 | La revisione della firma a due è fatta per la parte che ho potuto fare. Vedi 1.2 | |
| A6 | Libreria: la firma a due di cb-mpc, di Coinbase (licenza MIT), con due piccole modifiche nostre per Windows | È mantenuta, ha due revisioni esterne (Cure53, 2024) e un premio per chi trova falle. Il suo protocollo (Lindell 2017) è più semplice: una sola delle due parti ha una chiave Paillier, nessuna delle due sceglie parametri che l'altra deve accettare, e non ci sono numeri primi lenti da generare: importazione dei pezzi e firma in meno di un secondo, contro un minuto prima. Usiamo la variante con la prova finale, che non lascia trapelare nulla quando una firma fallisce. Le librerie DKLs23 mature hanno licenze non libere |
| A7 | Nessun nodo Bitcoin proprio: più server Electrum pubblici, ogni risposta verificata. Chi ha un nodo lo usa con `--rpc` | fatto |

**La transazione parte sulla rete pubblica?** Sì, da sola, appena firmata. Per il #140 non è un rischio: la chiave pubblica è già nota a tutti, e la firma non rivela nulla. È diverso per i puzzle senza chiave pubblica nota (vedi C4).

### B. Parametri della rete del #140

| | Decisione | Motivo |
|---|---|---|
| B1 | Seme dei salti: l'hash dell'ultimo blocco Bitcoin al momento in cui pubblichiamo la rete. **Nessuna attesa** | Tutti devono avere lo stesso seme perché due canguri che arrivano sullo stesso punto devono poi fare gli stessi salti: solo così i cammini si fondono e la coincidenza si vede. Preso da un blocco appena uscito, nessuno (nemmeno tu) può aver cominciato a calcolare prima degli altri |
| B2 | Zeri dei punti distinti: provvisoriamente 32, da fissare dopo la prova pubblica | Vedi sotto |
| B3 | Commissione: il doppio della stima per i prossimi blocchi, mai sotto 20 e mai sopra 1000 satoshi per byte | Alta perché la transazione vale molto e deve passare subito; il tetto la tiene sotto lo 0,2 % del premio anche nel caso peggiore |
| B4 | La "moneta minuscola" non ha più una dimensione fissa: cresce con la commissione | Così chi può annullare può farlo a qualunque commissione; prima c'era un tetto di 50 satoshi per byte |

**B2 spiegato.** I canguri non pubblicano ogni punto che toccano, sarebbero troppi: pubblicano solo i punti "distinti", quelli il cui numero comincia con un certo numero di zeri. Con 32 zeri è un punto ogni 4 miliardi di passi.
- **Meno zeri:** più punti da conservare e da spedire (con 30 zeri quattro volte quelli con 32).
- **Più zeri:** ogni canguro spreca in media il lavoro fatto dopo il suo ultimo punto distinto; con tanti canguri lo spreco pesa.
- Il numero giusto dipende da quanti partecipano, e deve essere lo stesso per tutti e per sempre. Per questo lo fisso dopo aver visto quanti partecipano alla prova: indicativamente 30 con 10 000 schede video, 33–34 con 1000.

### C. Migliorie

| | Decisione | Stato |
|---|---|---|
| C1 | Record corto (senza firma su ogni record, un terzo dello spazio): **sì, ma dopo la prima prova** | È un cambio di formato grosso; la prima prova serve a vedere che il resto regge. Va fatto prima del #140 |
| C2 | Ripubblicazione leggera: sì | da fare insieme a C1, prima del #140 |
| C3 | Ricerca lineare condivisa: sì. Nessuna transazione automatica, la chiave resta a chi la trova | da fare. Richiede che ogni motore di ricerca di pzsolve (CPU e schede video) riporti le chiavi che servono da prova di lavoro: senza quella prova chiunque potrebbe dichiarare fatti dei blocchi mai cercati |
| C4 | Chiave trovata senza chiave pubblica nota: avviso sempre; invio riservato a un pool di mining **solo se l'utente lo chiede** | da fare |

### D. Rilascio

| | Decisione | Stato |
|---|---|---|
| D1 | Linux, Windows e macOS | Linux: fatto. Windows: `pzpeer.exe` e `pzsign.exe` passano i test sulla macchina Windows della CI, e la prova completa con tre case passa con l'emulatore Wine. macOS: compilazione e test passano sulla CI. Manca una rete intera provata su un Windows vero e su un Mac |
| D2 | pzsign già compilato, nello stesso archivio di pzsolve e pzpeer | È in C++ e contiene cb-mpc (MIT), una copia di libsecp256k1 (MIT) e OpenSSL (Apache-2.0): licenze compatibili con la nostra, nessun obbligo di ricompilazione come quello che dava la libreria GMP |
| D3 | Tutto resta nel ramo `rete-condivisa`. Nel repository niente italiano fuori da `docs/it` | controllato: non ce n'è |
| D4 | Prova pubblica fatta da te, con il registro dettagliato `--debug FILE` | il registro c'è |

## 3. Dalla revisione completa (novembre 2026)

Otto revisori indipendenti, uno per area, più gli analizzatori statici. Quello che è stato corretto è nel `CHANGELOG.md` e nei
commit; qui resta ciò che è aperto. I rilievi che chiedono una tua decisione sono nel rapporto della revisione.

### 3.1 Sicurezza, da fare prima di una rete con denaro vero

| | Rilievo | Rimedio proposto |
|---|---|---|
| E1 | Un avviso con un punto copiato costa alla vittima circa 50 connessioni, e riempie la sua cassetta finché non arriva una prova | Il custode chiede a chi ha depositato il record una prova di possesso legata al custode, prima di avvisare (circa 30 righe) |
| E2 | Le cassette di una casa che non ascolta stanno sui 5 nodi più vicini a un nome pubblico: chi mette nodi lì le controlla | Più cassette, scelte anche con un segreto condiviso fra le due case; connessioni tenute aperte al posto delle interrogazioni |
| E3 | Una coppia arrivata dopo può sostituire la prima transazione di una coppia arrivata prima | Prima transazione sempre alla commissione massima |
| E4 | Chi fa fallire le firme blocca un punto (3 fallimenti) senza pagare nulla | I fallimenti si dimenticano col tempo; un tentativo costa qualcosa a chi lo chiede |
| E5 | Un server Electrum lento allunga ogni lettura della catena | Un limite di tempo per server e per operazione |
| E6 | Il partner può imporre fino a 1000 sat/vB e fermarsi dopo la prima transazione | Un tetto più basso, o la commissione presa dal blocco |
| E7 | Il generatore del seme accredita 256 bit al tempo anche con un orologio senza rumore | Togliere i rami che dipendono dai dati nella parte cronometrata, oppure stimare sulla differenza di due ripetizioni |
| E8 | pzsign: 126 MB per un solo messaggio dentro il limite di dimensione | Un limite agli elementi per fase |
| E9 | Windows: la cartella predefinita `pznet-NOME` prende i permessi della cartella madre (il seme di un nodo non passa più dalla riga di comando: sta in `node.seed`) | Permessi espliciti |
| E10 | La mappatura UPnP "senza fine" non viene mai tolta | Toglierla all'uscita |
| E11 | Una casa (`pzpeer run`) chiede da sola al router di aprire la porta, con UPnP (decisione A4). Bitcoin Core ha tolto UPnP nella 29.0 e dalla 30.0 chiede da solo al router, ma con PCP o NAT-PMP: un messaggio binario corto verso il router, senza la ricerca sulla rete di casa e senza l'XML di UPnP | **Da decidere.** Consiglio: come Bitcoin Core 30, apertura accesa di default, chiesta prima con PCP e NAT-PMP e poi con UPnP per i router che parlano solo quello. Costo: circa 150 righe, compresa la ricerca del router su Linux, Windows e macOS |

### 3.2 Sprechi

| | Rilievo | Rimedio proposto |
|---|---|---|
| F1 | A ogni arresto il canguro butta il percorso di ogni camminatore dall'ultimo punto distinto (fino a K·2^dp passi) | Salvare il branco |
| F2 | Pubblicare un record costa 7 connessioni; un riavvio ripubblica tutto | Connessioni tenute aperte, salto di ciò che `own.bin` ha già (con C1 e C2) |
| F3 | Ogni sessione rilegge tutte le monete non spese; in attesa, 4n+1 richieste ogni 15 secondi | Seguire le uscite e non le transazioni; `scripthash.subscribe` |
| F4 | Un processore con AVX2 ma senza ADX (Intel Haswell) cade sul motore di base: 4,0 contro 8,8 M chiavi/s | Compilare i motori a corsie senza `-madx` |
| F5 | Motori nuovi misurati: hash con SHA-NI nel motore a corsie AVX2 (+9 %), curva a 4 corsie da 52 bit con AVX-IFMA (+30 %, +45 % con SHA-NI) | Aggiungerli |
| F6 | La scelta del motore CPU si rifà a ogni avvio (2 secondi); la memoria della scheda è contata due volte nel limite dei lanci | Ricordare la scelta; contare una volta |
| F7 | Un core intero riservato per ogni scheda OpenCL | `clFlush`, attesa, poi `clFinish` |
| F8 | La tabella del canguro è una potenza di due dentro `--mem`; voci da 16 byte | Voci da 8 byte, tabella di qualsiasi misura |
| F9 | pzpeer teneva 4,5 MB per 64 sessioni possibili | **Fatto**: ogni sessione tiene solo lo spazio di un saluto (32 KB in tutto) |
| F10 | Ogni giro di una ricerca lanciava tre domande e aspettava la più lenta: un nodo spento fermava il giro per tutto il tempo limite | **Fatto**: tre domande sempre in volo. Con nodi spenti e rifiuti lenti come su Windows lo scambio tramite cassette passa da oltre 30 a 23 secondi |

### 3.3 Semplificazioni proposte (circa 1000 righe in meno)

| | Proposta | Righe |
|---|---|---|
| G1 | Un solo strato di sistema per `src` e `src/peer` | −250 |
| G2 | Una tabella delle opzioni, e una dei campi `chiave=valore` dei file | −200 |
| G3 | Le funzioni per l'esadecimale stanno in 9 posti: una coppia in pzcore | −70 |
| G4 | I nove `eng_*.c` e i sei `h160_*.c` cambiano solo per le definizioni: un file per famiglia compilato più volte | −150 |
| G5 | `rt.c`: tredici scelte fra CUDA e OpenCL diventano una tabella di funzioni | −40 |
| G6 | Tre riempimenti di SHA-256 in pzcore (hash.c, kdf.c): uno | −40 |
| G7 | La copia di cb-mpc tiene 7400 righe che pzsign non usa | −7400 (nessun cambio ai sorgenti usati) |
| G8 | Togliere i numeri di sequenza dei record (con C1) | da valutare |

### 3.4 Documenti da allineare

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
| H3 | La ripubblicazione quotidiana di mesi di punti pesa molto | La ripubblicazione leggera (C2, F2) |

### 3.5 Librerie

- OpenSSL 3.6.5 è uscita (13 CVE, nessuna tocca ciò che pzsign usa), ma cb-mpc accetta solo la 3.6.4: va scritta la procedura di
  aggiornamento (versione e impronta in `signer/openssl.sh`, il controllo di versione nella copia, i test).
- libsecp256k1 0.8.0 è uscita (nessuna correzione di sicurezza; verifica fino all'11 % più veloce).
- cb-mpc: a luglio e agosto 2026 gli autori hanno corretto quattro volte la firma a due. A ogni aggiornamento va riconfrontato
  `import_piece` di pzsign con il loro `dkg`.
- Un archivio che contiene pzpeer o pzsign deve portare le licenze di cb-mpc, libsecp256k1 e OpenSSL (`tools/package_release.sh`
  copia solo quella del progetto).
- Le protezioni del compilatore (canarini, FORTIFY) vengono solo dalla distribuzione: su Windows `pzpeer.exe` non le ha. Vanno
  scritte nel Makefile per pzpeer e pzsign.
