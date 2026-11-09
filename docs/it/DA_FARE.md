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

### A2. Nodi d'ingresso
- **Da decidere:** se affittare 2 o 3 piccoli server (pochi euro al mese l'uno) che fanno da primo contatto e da custodi.
- **Consiglio:** sì, su fornitori e paesi diversi. Senza, la rete dipende dal fatto che qualche partecipante sia raggiungibile.
- **Blocca:** la prova pubblica.

### A3. Trovarsi attraverso la rete BitTorrent
- **Da decidere:** se scrivere la ricerca dei nodi sulla tabella distribuita di BitTorrent (circa 600 righe in pzpeer, nessuna libreria esterna).
- **Pro:** chi scarica il programma trova la rete anche se i nostri server sono spenti.
- **Contro:** codice in più da mantenere; in alcune reti aziendali è bloccata.
- **Consiglio:** sì, insieme ad A2.

### A4. Apertura automatica della porta del router (UPnP)
- **Da decidere:** se scriverla. Va provata su router veri, che io non ho: servirebbero prove tue o di volontari.
- **Consiglio:** sì, ma dopo A2 e A3. Più case raggiungibili vuol dire più custodi.

### A5. Revisione esterna del firmatore
È il rischio tecnico più grave (ipotesi I4 del progetto): un errore nella firma a due può far perdere il premio.

- **Da decidere:** se pagare una revisione di `signer/` e dell'uso che fa di cggmp21 prima di usare bitcoin veri.
- **Consiglio:** sì prima del #140. Per le prove su reti di test non serve.

### A6. Quale libreria per la firma
- **Scelte:**
  1. restare su cggmp21 0.6.3 (oggi): provata qui, una vulnerabilità pubblica corretta e una non corretta che riguarda una funzione che non usiamo;
  2. passare a cggmp24 quando esce stabile: stesso protocollo, entrambe corrette;
  3. valutare DKLs23: niente numeri primi (oggi 20–45 secondi per sessione), messaggi molto più piccoli; va verificata la licenza e la maturità delle librerie disponibili.
- **Consiglio:** 1 per le prove; decidere fra 2 e 3 insieme a chi fa la revisione (A5).

### A7. Serve un nodo Bitcoin proprio?
Oggi sì (anche potato, bastano pochi GB).

- **Da decidere:** se accettarlo per la prima versione o aspettare la verifica leggera (senza nodo, con più server Electrum e prove verificate; il disegno è in NETWORK.md 8.2).
- **Consiglio:** prima versione con il nodo; verifica leggera dopo.

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
