# Da fare: le decisioni che spettano a te

9 novembre 2026. Si riferisce a [PROGETTO_RETE.md](PROGETTO_RETE.md).

Ogni voce dice cosa va deciso, le scelte possibili, cosa consiglio e cosa resta fermo finché non decidi. Sono in ordine di urgenza.

---

## A. Decisioni che bloccano la rete vera

### A1. Chi sono i manutentori
Il file di una rete vale solo se è firmato da una soglia di chiavi scritte dentro pzpeer. Oggi non ce n'è nessuna, quindi girano solo reti di prova.

- **Da decidere:** quante chiavi, di chi, e quante firme servono.
- **Scelte:** tu solo (1 di 1); tu con due chiavi su dispositivi diversi (2 di 2); tu più persone di fiducia (per esempio 2 di 3).
- **Consiglio:** 2 di 3, con almeno una chiave tenuta scollegata da Internet. Con una sola chiave, chi la ruba può pubblicare una rete falsa.
- **Blocca:** tutto ciò che non è prova.

### A2. Nodi d'ingresso: deciso, nessun server tuo
Hai deciso di non avere server tuoi per il primo contatto. La rete funziona lo stesso: i nodi si trovano attraverso la rete BitTorrent (A3), e basta che qualche partecipante sia raggiungibile dall'esterno (A4, o chi ha già un server).

- **Resta da sapere:** finché la rete è piccola, chi entra per primo non trova nessuno. È normale: basta che i primi due o tre partecipanti raggiungibili restino accesi.

### A3. Trovarsi attraverso la rete BitTorrent: fatto
Ogni nodo si annuncia nella tabella distribuita di BitTorrent sotto una chiave ricavata dall'impronta della rete, e lì trova gli altri. Passano solo indirizzi, e ognuno viene poi verificato come qualsiasi altro. Provato qui con una tabella finta e con tre nodi che si trovano senza conoscere alcun indirizzo.

- **Da fare tu:** la prova sulla rete BitTorrent vera, che da qui non raggiungo. Ti do il comando.
- **Limite:** in alcune reti aziendali BitTorrent è bloccato: lì serve indicare a mano un nodo conosciuto (`--peer`). Inoltre chi mette molti nodi vicino alla nostra chiave in quella tabella può nascondere gli annunci: chi entra per la prima volta non trova nessuno, o trova solo lui. È un ritardo, non un furto.

### A4. Apertura automatica della porta del router (UPnP): fatto, spento se non lo chiedi
Con `--upnp` pzpeer chiede al router di casa di far entrare la porta. Provato qui contro un router finto.

- **Da decidere:** se lasciarlo spento finché l'utente non lo chiede (oggi) o accenderlo da solo. Consiglio: spento. Un programma non dovrebbe aprire una porta di casa senza che gli venga detto.
- **Da fare tu:** provarlo sul tuo router con il comando che ti do.
- **È pericoloso avere la porta aperta?** Su quella porta risponde solo pzpeer. Ogni messaggio è cifrato, firmato e limitato in dimensione e frequenza, e il programma è provato con dati casuali e ostili. Il rischio che resta è quello di qualunque programma raggiungibile da Internet: un errore non ancora scoperto. Chi non vuole correrlo partecipa lo stesso senza aprire nulla (cerca, pubblica e firma attraverso il relay); solo non fa da custode.

### A5. Revisione del firmatore: la faccio io, con un limite
Mi hai chiesto di controllare io la libreria della firma a due. Lo faccio: leggo come `signer/` usa cggmp21 e provo un partner che bara in tutti i modi che riesco a costruire.

- **Limite da conoscere:** una mia revisione abbassa il rischio, non lo azzera. La libreria è lunga e matematicamente delicata; i suoi difetti passati sono stati trovati da gruppi specializzati dopo mesi. Con premi di questa grandezza, una revisione esterna prima del #140 resta consigliabile.
- **Da decidere:** se farla comunque più avanti, prima di usare bitcoin veri.

### A6. Quale libreria per la firma
- **Scelte:**
  1. restare su cggmp21 0.6.3 (oggi): provata qui, una vulnerabilità pubblica corretta e una non corretta che riguarda una funzione che non usiamo;
  2. passare a cggmp24 quando esce stabile: stesso protocollo, entrambe corrette;
  3. valutare DKLs23: niente numeri primi (oggi 20–45 secondi per sessione), messaggi molto più piccoli; va verificata la licenza e la maturità delle librerie disponibili.
- **Consiglio:** 1 per le prove; decidere fra 2 e 3 insieme a chi fa la revisione (A5).

### A7. Nodo Bitcoin proprio: deciso, non serve
Fatto. Senza `--rpc`, pzpeer legge la catena da più server Electrum pubblici e non si fida di nessuno: ogni blocco deve avere il suo lavoro, ogni moneta la sua prova. Un server bugiardo può solo far perdere tempo. Chi ha un nodo proprio lo usa con `--rpc` ed è più protetto.

- **Da decidere:** quali server Electrum mettere nel file della rete (ne servono almeno 3 o 4, di gestori diversi, che accettino connessioni senza cifratura), e quale blocco usare come punto di partenza (`checkpoint=`).
- **Da fare tu:** una prova con un server pubblico vero, che da qui non posso raggiungere. Ti do il comando.

---

## B. Parametri della rete del #140

### B1. Seme dei salti
Deve essere pubblico e non scelto da nessuno. **Consiglio:** l'hash di un blocco Bitcoin di un'altezza futura annunciata prima.

### B2. Zeri dei punti distinti
Meno zeri: più punti da conservare. Più zeri: più lavoro sprecato. **Consiglio:** 30 con 10 000 GPU attese, 33 con 1000 (stime da misurare nella prova pubblica).

### B3. Tetto delle commissioni e monete del premio
Vanno nel file firmato. **Consiglio:** tetto a 50 satoshi per byte (oltre, chi può annullare non potrebbe più farlo).

### B4. Dimensione della moneta minuscola
Oggi 10 000 satoshi. Va bene finché le commissioni restano sotto il tetto di B3.

---

## C. Migliorie da approvare

### C1. Record corto
Togliere la firma da ogni record (da 138 a circa 40 byte): un terzo dello spazio e del traffico. Cambia il formato, quindi va fatto prima della prova pubblica. **Consiglio:** sì.

### C2. Ripubblicazione leggera
Oggi ogni casa rimanda tutti i suoi punti ogni giorno: a campagna avanzata è troppo. **Consiglio:** sì, necessario prima del #140.

### C3. Ricerca lineare condivisa (puzzle 71 e simili)
Il disegno è nella sezione 8 del progetto. Il guadagno è un fattore 2 in media, e il premio non si può dividere.

- **Da decidere:** se farla, e se accetti la modifica ai motori di ricerca di pzsolve che richiede (riportare le chiavi che servono da prova di lavoro).
- **Consiglio:** dopo la rete del canguro. È utile ma rende meno, e tocca il cuore di pzsolve.

### C4. Avviso a chi trova una chiave senza chiave pubblica nota
Chi spende un puzzle come il #71 sulla rete pubblica rischia di farsi sostituire la transazione. **Da decidere:** se far dire a pzsolve, quando trova una chiave così, di mandare la transazione direttamente a un pool di mining. **Consiglio:** sì, è una riga di testo e protegge chi vince.

---

## D. Rilascio

### D1. Sistemi
pzpeer oggi gira solo su Linux. **Da decidere:** l'ordine. **Consiglio:** Windows subito dopo Linux (è dove stanno le GPU), macOS dopo.

### D2. Come distribuire pzsign
È in Rust e include la libreria GMP (licenza LGPL). **Da decidere:** se distribuire il binario già compilato insieme a pzsolve. Chi riceve una licenza commerciale deve poter ricollegare GMP: va scritto nella licenza (domanda per il legale).

### D3. Versione e ramo
**Da decidere:** quando unire `rete-condivisa` a `main` e con che numero (1.24.0 o 2.0.0). **Consiglio:** unire solo la parte di pzsolve (il cammino condiviso) come 1.24.0, e tenere pzpeer nel ramo fino alla prova pubblica.

### D4. Prova pubblica
**Da decidere:** su quale rete di test di Bitcoin (signet o testnet), con quale puzzle (consiglio 60–70 bit, risolvibile in giorni da pochi volontari) e con quale premio simbolico.

### D5. Aspetti legali
Una rete che divide premi fra sconosciuti: avvertenze, responsabilità, eventuale tassazione dei premi. Da chiedere al legale insieme a NOTICE, CONTRIBUTING e licenza commerciale, che sono ancora in sospeso.

---

## E. Promemoria già aperti

- prova della cifratura su un Windows vero;
- cancellare i rami di prova vecchi su GitHub (`test-122`, `test-123`, `test-1231`, `test-1232`, `test-gpu-*`, `fix-intel-cl`);
- misurare la velocità di pzsolve su una RTX 4090 (ipotesi I10: tutti i tempi dipendono da questo numero).
