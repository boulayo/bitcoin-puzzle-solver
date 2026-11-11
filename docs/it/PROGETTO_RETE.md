# Rete condivisa per i puzzle Bitcoin: progetto

Versione 10, 11 novembre 2026. Sostituisce le versioni precedenti.
Documenti collegati: [DA_FARE.md](DA_FARE.md) (le decisioni che spettano a te) e [NETWORK.md](../en/NETWORK.md) (la specifica tecnica, in inglese, per chi lavora sul codice).

Come leggere questo documento:
- le sezioni 1 e 2 dicono cosa si vuole ottenere e come funziona;
- la sezione 3 dice cosa è già costruito e provato, e cosa no;
- la sezione 4 elenca le **ipotesi**: le cose che do per vere senza averle potute dimostrare;
- le sezioni 5 e 6 trattano la sicurezza e il passaggio alla rete vera;
- la sezione 7 è la revisione: cosa si può fare meglio;
- la sezione 8 riguarda i puzzle senza chiave pubblica (ricerca lineare).

---

## 1. Lo scopo

Risolvere in tanti i puzzle di cui si conosce la chiave pubblica (il #140 e i successivi multipli di 5), con una regola semplice:

> le due persone i cui calcoli si incontrano si dividono il premio a metà, in automatico, e nessuna delle due può prendersi tutto.

Chi partecipa mette potenza di calcolo e un indirizzo Bitcoin. Il programma fa il resto.

Tre programmi:

| Programma | Cosa fa | Linguaggio |
|---|---|---|
| `pzsolve` | cerca, come oggi. Non sa niente della rete | C |
| `pzpeer` | la rete: pubblica i punti di pzsolve, custodisce quelli degli altri, tratta la coincidenza e il pagamento | C |
| `pzsign` | la firma a due, usata da pzpeer solo al momento del pagamento | Rust |

---

## 2. Come funziona

### 2.1 Il canguro in rete

Il metodo del canguro fa camminare molti "canguri" sulla curva. Ogni tanto un canguro arriva su un **punto distinto** (un punto la cui coordinata ha un certo numero di zeri) e lo annota insieme alla **distanza** percorsa. Quando due canguri di tipo diverso arrivano sullo stesso punto, dalle due distanze si ricava la chiave.

In rete:
- tutti camminano sullo stesso grafo (i salti vengono da un seme pubblico, uguale per tutti);
- ognuno parte da punti segreti, ricavati dal proprio seme;
- di ogni punto distinto si pubblica solo il punto e il tipo. **La distanza resta in casa.**

### 2.2 La coincidenza divide la chiave in due pezzi

Se il punto di Anna e il punto di Bruno coincidono, la chiave del puzzle è la somma di due numeri: uno lo può calcolare solo Anna (dalla sua distanza), l'altro solo Bruno. Nessuno dei due ha la chiave. Chi non è nella coincidenza non ha niente.

### 2.3 La firma a due e il pagamento in due passaggi

Anna e Bruno firmano insieme una transazione senza mai ricostruire la chiave (protocollo CGGMP21). Ognuno calcola da sé cosa sta firmando.

Resta un problema: nell'ultimo scambio uno dei due riceve la firma completa per primo, e potrebbe tenerla da parte mentre prova a rubare. La difesa, senza depositi:

1. **Primo passaggio.** La coppia firma una transazione che rimanda il premio sullo stesso indirizzo del puzzle e dà a ciascuno una moneta minuscola (10 000 satoshi o più, secondo la commissione) su una chiave solo sua. Trattenere questa firma non serve a niente.
2. **Secondo passaggio**, dopo una conferma. La transazione metà e metà spende il premio e anche la moneta minuscola di uno dei due, che chiamo F. L'altro, S, riceve la firma completa per primo e la spedisce.
3. **Se dopo 3 blocchi il premio non si è mosso**, F spende la sua moneta minuscola. La transazione che S teneva da parte non vale più.

Una firma trattenuta vale quindi al massimo mezz'ora.

### 2.4 Cosa vede chi partecipa

Avvia `pzpeer run` con l'indirizzo di pagamento. Il programma crea il seme di casa (una volta), avvia pzsolve, pubblica i punti, e se arriva una coincidenza firma e paga. Alla fine sullo schermo compare l'esito.

---

## 3. Stato: cosa c'è e cosa manca

Tutto il codice è nel ramo `rete-condivisa`. Niente è stato rilasciato.

### 3.1 Costruito e provato

| Parte | Prova |
|---|---|
| pzsolve con "cammino condiviso" (`--jump-seed`, `--start-key`) | prova end-to-end; costo misurato sulla curva: +11,4 % rispetto allo schema normale (3000 prove) |
| canale cifrato fra nodi (Noise XX) | uguale byte per byte a un'implementazione indipendente |
| record dei punti, prova di possesso della distanza | test |
| rete Kademlia: custodi, avvisi, caselle postali | 32 nodi in un processo |
| relay per due case che non accettano connessioni | test |
| pezzi della chiave, firma a due con pzsign | firme verificate con la libreria di Bitcoin Core |
| le tre transazioni (prima, metà e metà, annullamento) | accettate e confermate da Bitcoin Core 28.1 in regtest |
| sessione completa, compreso il partner che trattiene la firma | in memoria e su regtest |
| rete intera: un custode e tre case su una macchina | premio diviso 0,49999366 e 0,49989366 BTC, indirizzo del puzzle vuoto |
| nodo Bitcoin potato, file cookie, nodo non sincronizzato | test |
| primo contatto senza server nostri: i nodi si annunciano e si cercano nella tabella di BitTorrent; apertura della porta del router con `--upnp` | test con nodi bugiardi e router non conformi; tre nodi che si trovano da soli |
| nessun nodo Bitcoin proprio: catena letta da più server Electrum, ogni risposta verificata (server che nascondono, mentono, danno prove false) | test e prova completa con tre case |
| la rete è l'impronta del suo file: nessuna firma, nessuna chiave da custodire | test |
| pzpeer e pzsign per Windows: uno strato unico sopra il sistema operativo, che ha anche accorciato il codice | test unitari e prova completa con tre case sotto Wine |
| avvio senza configurare nulla (cartella, porta, router, primi contatti, blockchain) e registro dettagliato `--debug` | prova completa con tre case |

Controlli generali: tutti i test passano anche con AddressSanitizer, UBSan e ThreadSanitizer; un fuzzer ha provato 26 milioni di ingressi malformati sui lettori di dati senza trovare errori.

### 3.2 La revisione di sicurezza di questa versione

Tre revisioni indipendenti hanno cercato modi di rubare, bloccare o ingannare. Ogni problema reale è stato corretto con un test che fallisce senza la correzione. I più importanti:

| Problema trovato | Gravità | Stato |
|---|---|---|
| nel secondo passaggio un F disonesto poteva ottenere la firma completa senza che S la avesse: la difesa si rovesciava | critica | corretto |
| S poteva proporre commissioni così alte che F non poteva più annullare | alta | corretto |
| gli identificativi delle firme si ripetevano fra sessioni della stessa coppia | alta | corretto |
| chiunque copiava un punto pubblicato poteva far sprecare un minuto di CPU a una casa e bloccarne le sessioni | alta | corretto |
| mancava il limite di firme fallite (10 per rete, 3 per coppia) | alta | aggiunto |
| il file del seme poteva restare leggibile da altri utenti | alta | corretto |
| 8 connessioni lente bloccavano un nodo | alta | corretto |
| un nodo poteva essere mandato a connettersi dentro la rete di casa dell'utente | alta | corretto |
| un attaccante poteva riempire la tabella dei contatti di un nodo (eclissi) | alta | limiti per blocco di indirizzi |
| chiunque poteva riempire il disco dei custodi | alta | quote |
| due case dietro un router non potevano mai aprire una sessione | alta | prove via casella postale e relay |
| con un nodo Bitcoin potato nessuna sessione poteva pagare | alta | corretto |
| con più di 64 monete sull'indirizzo il premio poteva restare fuori | alta | corretto |
| 45 000 monete di polvere sull'indirizzo impedivano ogni sessione | media | il file della rete elenca le monete del premio |
| attese senza fine se il nodo Bitcoin si ferma | media | corretto |

### 3.3 Cosa manca

| Manca | Effetto |
|---|---|
| il file della rete del #140 dentro il programma | oggi il file si dà con `--network`; i parametri del #140 si fissano al rilascio |
| prova del ritrovo sulla rete BitTorrent vera e dell'apertura della porta su un router vero | provati solo contro una tabella e un router finti, vedi 6.2 |
| prova di una rete intera su un Windows vero e su un Mac | sulle macchine Windows e macOS della CI passano i test di pzpeer (su Windows anche la firma a due con `pzsign.exe`); la prova completa con tre case è fatta su Linux e, per i programmi Windows, con l'emulatore Wine |
| ripartenza dei canguri dopo una fusione fra case | un po' di lavoro sprecato |
| sessione che sopravvive a un riavvio di pzpeer | la coppia ricomincia |
| controlli a campione dei cammini | un punto falso occupa spazio, non ruba |
| ripubblicazione che regge a campagna avanzata | traffico troppo alto verso la fine |
| revisione esterna della libreria di firma | la revisione dell'uso che ne facciamo è fatta; quella dell'interno no, vedi ipotesi I4 |
| prova pubblica con persone vere | i numeri di 5 sono stime |

---

## 4. Ipotesi

Sono le cose su cui il progetto si regge e che non ho potuto dimostrare. Se una è falsa, la riga dice cosa succede.

| N. | Ipotesi | Se è falsa |
|---|---|---|
| I1 | Il creatore dei puzzle non sposta i fondi e non conosce in anticipo chi vince | il premio sparisce: nessuna difesa possibile |
| I2 | La chiave pubblica dei puzzle 140, 145, ... 160 è nota (è così oggi) | senza chiave pubblica il canguro non si può usare |
| I3 | Il premio vale meno di circa 7 volte l'elettricità di tutta la campagna | sopra questa soglia a un partner con oltre metà della potenza conviene tentare il furto (5.1) |
| I4 | La libreria cggmp21 0.6.3 non ha difetti nascosti | è il rischio tecnico più grave: un errore può far trapelare un pezzo. Ho controllato il modo in cui la usiamo e provato un partner che bara in ogni messaggio (nessuna firma esce mai); l'interno della libreria resta affidato ai suoi autori. Mitigato dal limite di firme fallite, non eliminato |
| I5 | La vulnerabilità CVE-2025-66017 di cggmp21 non ci riguarda perché non usiamo prefirme | verificato sul codice: pzsign non chiama nessuna funzione di prefirma |
| I6 | Il computer di casa non è compromesso | chi legge il seme o `dp.bin` ha le distanze: perde quella casa |
| I7 | Il proprio nodo Bitcoin dice il vero | un nodo bugiardo può far firmare una transazione su monete inesistenti: perdita di tempo, non del premio |
| I7b | Senza nodo proprio: almeno uno dei server Electrum usati è onesto e raggiungibile, e nessuno spende il lavoro di interi blocchi per ingannare | se tutti mentono o tacciono la sessione aspetta o fallisce: ritardo, non furto. Un blocco falso con lavoro vero costa quanto il premio di un blocco |
| I7c | Nella tabella di BitTorrent nessuno occupa in modo stabile i posti vicini alla chiave della nostra rete | chi entra per la prima volta non trova nessuno o trova solo l'attaccante: ritardo e record nascosti, mai un pagamento sbagliato. Restano i contatti salvati e `--peer` |
| I8 | Gli orologi delle case differiscono di pochi secondi | il relay non fa incontrare le due case; la sessione riprova |
| I9 | Nella rete c'è sempre almeno qualche nodo raggiungibile da fuori | senza, nessuno custodisce i punti e la rete non funziona |
| I10 | Una GPU fa 5–10 miliardi di passi al secondo con pzsolve | i tempi di 5.3 si allungano in proporzione. Misurato: 0,92 miliardi su una A2000 portatile |
| I11 | I numeri dello schema `net-06` (costo +11 %, soglie del furto) valgono anche a 140 bit | il costo è misurato a 40 bit; le soglie vengono dal simulatore |
| I12 | Spazio e traffico per nodo sono quelli stimati (20–30 GB, decine di KB/s) | mai misurati con migliaia di nodi |
| I13 | Chi pubblica il programma è onesto, e chi lo scarica lo prende dal repository ufficiale | un programma malevolo ruba i semi. È la stessa fiducia che si dà a qualsiasi programma, Bitcoin Core compreso |
| I14 | Le fonti di casualità del seme (sistema, CPU, tempi di calcolo) non sono tutte prevedibili insieme | seme prevedibile: perde quella casa |
| I15 | I miner non riscrivono la catena per poche decine di blocchi per rubare | caso estremo, come per qualsiasi pagamento |

---

## 5. Sicurezza

### 5.1 Chi può rubare

- **Chi non è nella coincidenza:** non ha nessuna distanza, quindi niente.
- **Un custode:** vede punti e tipi, non distanze. Può tacere una coincidenza: per questo ogni punto va a 5 custodi.
- **Il partner:** dopo la coincidenza sa abbastanza per cercare il pezzo dell'altro con meno lavoro di una campagna intera. Lo schema `net-06` allarga le zone di partenza perché questo costi il 61 % di una campagna, al prezzo dell'11 % di lavoro in più per tutti.

Con `net-06` tentare il furto conviene solo a chi ha più potenza di tutto il resto della rete messo insieme (oltre il 51 % del totale), finché vale l'ipotesi I3. Chi ha più del 51 % vince comunque più spesso da solo: la rete non gli regala niente.

Per il #140 l'ipotesi I3 vale finché il bitcoin costa meno di circa 1,2 milioni di euro (GPU da 10 miliardi di passi) o 2,4 milioni (GPU da 5 miliardi).

### 5.2 Le difese, una per riga

| Comportamento avversario | Difesa |
|---|---|
| pubblicare punti falsi o copiati | un punto ha effetto solo dopo che il proprietario dimostra di conoscerne la distanza |
| far firmare un'altra transazione | ognuno costruisce la transazione da sé e si confrontano gli hash |
| trattenere la firma | moneta minuscola e annullamento dopo 3 blocchi (2.3) |
| sfruttare un errore del firmatore un bit alla volta | al massimo 10 firme fallite per rete e 3 per coppia, contate su disco |
| riempire dischi, tabelle, caselle | quote per proprietario e per blocco di indirizzi |
| isolare un nodo | al massimo 2 contatti per blocco di indirizzi in ogni cella, 4 in tutto |
| spiare il traffico | canale cifrato fra identità; il relay vede solo dati cifrati |
| file della rete falso | l'impronta del file lega chiavi e record: chi cambia una riga che definisce la rete è su un'altra rete, da solo. Il file ufficiale sta nel programma |
| bloccare il pagamento con monete di polvere | le monete del premio sono elencate nel file della rete |

### 5.3 Tempi e costi (stime, ipotesi I10)

| | 1000 GPU a 5 G | 1000 GPU a 10 G | 10 000 GPU a 5 G | 10 000 GPU a 10 G |
|---|---|---|---|---|
| #140, tempo atteso | 6,4 anni | 3,2 anni | 7,6 mesi | 3,8 mesi |
| #145, tempo atteso | 36 anni | 18 anni | 3,6 anni | 1,8 anni |

L'elettricità totale non dipende dal numero di partecipanti: 12–25 GWh per il #140 (2,5–5 milioni di euro a 0,20 €/kWh), contro un premio di 14 BTC.

---

## 6. Dalla rete di prova alla rete vera

### 6.1 Cosa distingue le due

| | Rete di prova (oggi) | Rete vera |
|---|---|---|
| il file della rete | dato con `--network` | dentro il programma |
| catena | regtest | Bitcoin vero |
| primo contatto | indirizzi dati a mano | rete BitTorrent, nessun server nostro |
| indirizzi privati | ammessi | rifiutati |
| identità dei nodi | costo ridotto | costo pieno (qualche secondo di calcolo) |

Il codice è lo stesso: cambia il file della rete e chi lo firma.

### 6.2 "Scarico il programma e la rete c'è già": si può?

Sì, a due condizioni.

**Trovare i primi nodi.** Una rete senza server ha comunque bisogno di un primo contatto. Tre strade, da usare insieme:

| Strada | Pro | Contro |
|---|---|---|
| nodi d'ingresso nostri: 2 o 3 piccoli server, il cui indirizzo sta nel file firmato | semplice, già funziona | **scartata**: hai deciso di non avere server tuoi |
| la tabella distribuita di BitTorrent (Mainline DHT): i nodi si annunciano sotto l'impronta della rete e si trovano lì | milioni di nodi, sempre accesa, nessun server nostro | **fatta**; qualche rete la blocca; chi mette molti nodi vicino alla nostra chiave può nascondere gli annunci (ritardo, non furto) |
| contatti salvati dalla volta prima | già funziona | non aiuta chi entra per la prima volta |

BitTorrent serve solo a **trovarsi**. I punti non si possono mettere lì: sono troppi (centinaia di miliardi) e quella rete non rileva coincidenze. Per i punti resta la nostra tabella Kademlia, che è fatta apposta.

**Essere raggiungibili.** Quasi tutti i computer di casa stanno dietro un router. Chi non è raggiungibile partecipa comunque (cerca, pubblica, firma, anche attraverso il relay), ma non può fare da custode. Servono quindi alcuni nodi raggiungibili: i nodi d'ingresso, chi ha un server, chi apre la porta. L'apertura automatica della porta (`--upnp`) aumenta il loro numero: è fatta, e resta spenta finché l'utente non la chiede.

Ho valutato anche libp2p (troppo grande come dipendenza) e Nostr (comodo per trovarsi, ma richiede TLS e passa da server altrui). Restano scartati.

### 6.3 I passi, in ordine

1. Prova di una rete intera su un Windows vero e su un Mac.
2. Prova del ritrovo sulla rete BitTorrent vera, dell'apertura della porta su un router vero e di un server Electrum pubblico.
3. Prova pubblica su una rete di test di Bitcoin (signet o testnet) con un puzzle piccolo e volontari.
4. Revisione esterna del firmatore.
5. Parametri del #140: seme dei salti da una fonte pubblica verificabile, bit dei punti distinti, monete del premio, tetto delle commissioni.
6. Rilascio, con pzpeer almeno per Linux e Windows.

---

## 7. Revisione: si può fare meglio?

Ho riguardato l'impianto chiedendomi, per ogni pezzo, se esiste un modo più semplice o più efficiente.

| Pezzo | Alternativa valutata | Conclusione |
|---|---|---|
| dividere la chiave in due con la coincidenza | nessuna migliore: è ciò che rende impossibile il furto a terzi | resta |
| firma a due | scambiarsi i pezzi: chi riceve per primo prende tutto. Un terzo garante: non lo vuoi | resta |
| protocollo di firma CGGMP21 | DKLs23: niente numeri primi da generare (oggi 20–45 secondi), messaggi piccoli (oggi 200 KB) | da valutare, vedi DA_FARE |
| pagamento in due passaggi | deposito dell'utente: scartato. Blocco a tempo: non basta | resta |
| tabella Kademlia per i punti | server centrale: più semplice, e non potrebbe rubare (non ha le distanze), ma può censurare e va mantenuto | resta la tabella; i "nodi grandi" di volontari sono già possibili |
| record firmato da 138 byte | record corto da circa 40 byte: la firma su ogni record non serve più, perché un custode accetta solo record del nodo con cui sta parlando su un canale autenticato, e ogni effetto richiede comunque la prova di possesso | consigliato: 3 volte meno spazio e traffico |
| ripubblicare tutto ogni giorno | mandare a un custode solo ciò che gli manca | necessario prima di una campagna lunga |
| pzsolve | già al minimo: 270 righe più la tabella dello schema | resta |

Nessuna alternativa cambia l'impianto. Le due migliorie che contano sono il record corto e la ripubblicazione leggera.

---

## 8. Puzzle senza chiave pubblica (ricerca lineare)

### 8.1 Cosa non si può fare

Qui si provano direttamente le chiavi private: chi trova la chiave la vede intera. **Dividere il premio in automatico è impossibile.** Si può solo evitare di fare due volte lo stesso lavoro.

### 8.2 Il registro dei blocchi fatti

**Quanto si guadagna.** Senza registro ognuno cerca a caso e le parti si ripetono: in media bisogna provare tante chiavi quante ne contiene tutto l'intervallo. Con un registro comune si prova ogni chiave una volta sola: in media metà intervallo, e mai più dell'intero. Il guadagno è quindi **un fattore 2 in media**, più la certezza di finire. Non di più: la velocità vera viene dal numero di partecipanti.

Esempio sul #71 (stima: 6 miliardi di chiavi al secondo per GPU):

| | 1000 GPU senza registro | 1000 GPU con registro | 10 000 GPU senza | 10 000 GPU con |
|---|---|---|---|---|
| tempo atteso | 6,2 anni | 3,1 anni | 7,5 mesi | 3,7 mesi |
| tempo massimo | nessuno | 6,2 anni | nessuno | 7,5 mesi |

**Come funziona:**
- l'intervallo è diviso in blocchi grandi (2^40–2^44 chiavi: minuti o ore di una GPU);
- ognuno sceglie a caso un blocco non ancora fatto. Non servono prenotazioni: finché restano molti blocchi, due persone scelgono lo stesso molto di rado;
- finito un blocco, lo dichiara nella stessa rete dei punti, con una **prova di lavoro**: le chiavi del blocco il cui hash comincia con un certo numero di zeri (qualche centinaio per blocco). Chi non ha davvero provato tutte le chiavi non le conosce;
- chiunque può ricontrollare un pezzetto a caso di un blocco altrui. Se trova una chiave "con gli zeri" che manca dalla dichiarazione, ha una **prova di frode** che tutti possono verificare: i blocchi di quell'identità tornano da fare.

**Perché mentire non paga.** Dichiarare fatto un blocco non fatto (per tenerselo) richiede le prove di lavoro, cioè farlo davvero.

**Perché condividere conviene.** Chi legge il registro senza pubblicare i propri blocchi vince solo un po' più spesso (circa 1,1 volte la sua quota con il 10 % della potenza). Per togliere anche questo vantaggio: i nodi danno il registro aggiornato solo a chi ha pubblicato lavoro verificato di recente, come fanno le reti di scambio di file. Non è una garanzia matematica, è un incentivo.

**Un pericolo che vale anche per chi cerca da solo.** Quando si spende un puzzle senza chiave pubblica nota, la transazione la rivela. Con intervalli così piccoli chiunque veda la transazione ricava la chiave in secondi e la sostituisce con una sua. Chi trova la chiave deve mandare la transazione direttamente a un pool di mining, non alla rete pubblica.

### 8.3 Come si costruisce

Deciso: si fa, senza transazioni automatiche. La chiave resta a chi la trova, che la spende in modo riservato (vedi sotto).

**In pzsolve, il minimo.** Ogni motore di ricerca (le varianti per CPU e i programmi per le schede video) oggi segnala solo la chiave giusta. Deve segnalare anche le "quasi giuste": le chiavi il cui hash comincia con gli stessi 32 bit del bersaglio, una ogni 4 miliardi circa. Sono la prova di lavoro: chi non ha provato tutte le chiavi di un blocco non le conosce, e chiunque le verifica in un istante. Nient'altro cambia in pzsolve: l'ordine di ricerca, i file, le opzioni restano quelli.

**In pzpeer:**
- la rete divide l'intervallo in blocchi uguali per tutti (indicativamente 2^44 chiavi: poco meno di un'ora di una scheda video veloce);
- pzpeer sceglie a caso un blocco che nella rete non risulta fatto, e avvia pzsolve su quel solo blocco con le opzioni che ha già (`hunt -r`);
- finito il blocco, lo dichiara nella stessa tabella dei punti, con una parte delle chiavi quasi giuste (quelle con 40 bit uguali, una quindicina per blocco); le altre le tiene, per rispondere a chi controlla;
- chi riceve una dichiarazione ne verifica le chiavi prima di crederci;
- ogni tanto un nodo rifà un pezzetto a caso di un blocco altrui: se trova una chiave quasi giusta che il dichiarante non ha, ha una prova di frode che tutti verificano, e i blocchi di quell'identità tornano da fare;
- quando non restano blocchi liberi si ripassano quelli dichiarati da altri, a partire dai meno documentati: una dichiarazione falsa ritarda, non nasconde la chiave per sempre.

**Quando la chiave viene trovata.** pzsolve avvisa di non spenderla con un portafoglio, e `pzpeer sweep` prepara e firma la transazione senza inviarla: la si consegna a un pool di mining che accetta transazioni riservate. Questa parte c'è già.

**Stato:** fatti l'avviso, `pzpeer sweep`, la modifica ai motori (`hunt --near`, provata su tutti i motori CPU e sul percorso delle schede video con il driver simulato, mai su una scheda vera) e la dichiarazione di un blocco con la sua verifica. Da fare: il file di una rete lineare, il registro delle dichiarazioni nella tabella, la casa che cerca blocco per blocco, i controlli a campione.

---

## 9. Parole usate

| Parola | Significato |
|---|---|
| casa | il computer di un partecipante, con il suo seme |
| punto distinto | un punto del cammino con una coordinata che ha molti zeri |
| distanza | quanto ha camminato il canguro fino a quel punto: il segreto di casa |
| coincidenza | due case sullo stesso punto, con tipi diversi |
| pezzo | la parte di chiave che ogni casa ricava dalla coincidenza |
| custode | un nodo che conserva punti altrui e avvisa delle coincidenze |
| file della rete | il file che descrive una rete: puzzle, intervallo, seme dei salti. La sua impronta è la rete |
| F e S | nel secondo passaggio: F può annullare, S riceve la firma per primo |
