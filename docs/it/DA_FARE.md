# Da fare: decisioni prese e decisioni aperte

11 novembre 2026. Si riferisce a [PROGETTO_RETE.md](PROGETTO_RETE.md).

Prima le poche cose che aspettano te, poi l'elenco di ciò che è deciso, con il motivo.

---

## 1. Aspettano te

### 1.1 Le tre prove che da qui non posso fare
Servono un computer con la rete di casa vera. Per ognuna basta guardare una riga dell'uscita.

| Prova | Comando | Cosa guardare |
|---|---|---|
| ritrovo sulla rete BitTorrent vera | su due computer: `pzpeer node --network FILE --seed-hex SEME --dir CARTELLA --bt-router router.bittorrent.com:6881 --verbose` | le righe `rendezvous:` e poi `contacts` |
| apertura della porta sul router | lo stesso comando (la chiede da solo) | la riga `upnp:` |
| server Electrum pubblici | `pzpeer run --network FILE --payout INDIRIZZO --electrum electrum.blockstream.info:50001 --debug prova.log` su una rete della catena principale | la riga `chain:` e, in `prova.log`, le righe `electrum` |

### 1.1b Windows e Mac veri
Sulle macchine Windows e macOS della CI i test di pzpeer passano (su Windows anche la firma a due). Manca la prova di una rete intera. **Da fare tu:** avviare `pzpeer.exe run --network FILE --payout INDIRIZZO --debug prova.log` su un Windows vero, insieme a un altro computer, e guardare se parte e trova l'altro.

### 1.2 Revisione esterna della firma a due
La mia revisione non sostituisce quella di specialisti. **Da decidere:** se farla fare prima di usare bitcoin veri. **Consiglio:** sì, prima del #140.

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
| A6 | Libreria: cggmp21 0.6.3. Passaggio a cggmp24 quando avrà un rilascio stabile | È l'unica libreria aperta, mantenuta e con licenza libera che dà anche il nome di chi bara. Il successore cggmp24 è ancora in versione alfa. Le librerie DKLs23 mature hanno licenze non libere. I protocolli a due più semplici hanno una storia di fughe a ogni interruzione |
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
| D2 | pzsign già compilato, nello stesso archivio di pzsolve e pzpeer | È in Rust e contiene la libreria GMP (licenza LGPL): chiunque può ricompilarlo dai sorgenti, che è ciò che quella licenza chiede. Per chi riceve una licenza commerciale va scritto nella licenza (domanda per il legale) |
| D3 | Tutto resta nel ramo `rete-condivisa`. Nel repository niente italiano fuori da `docs/it` | controllato: non ce n'è |
| D4 | Prova pubblica fatta da te, con il registro dettagliato `--debug FILE` | il registro c'è |
