# Eseguire il programma su una macchina che non controlli

Una lista di controllo per un computer in affitto (un'istanza nel cloud, una GPU a noleggio, la macchina di un amico), in ordine. Usa la
cifratura della chiave trovata descritta in [USO](USO.md) sezione 11: chi usa la macchina dopo di te non deve poter leggere la chiave.
Ogni comando è dato per Linux; su Windows è `.\pzsolve.exe` in PowerShell, con le stesse opzioni.

## 1. Prima di caricarlo: controlla l'eseguibile

Scarica l'archivio e `SHA256SUMS` dalla pagina [Releases](https://github.com/boulayo/bitcoin-puzzle-solver/releases) e controlla
l'archivio, così che quello che gira sia il programma pubblicato e non una copia modificata da qualcuno. Carica l'archivio, non
l'eseguibile scompattato, e ripeti lo stesso controllo sulla macchina in affitto prima di scompattarlo lì.

```sh
sha256sum -c SHA256SUMS --ignore-missing          # Linux: "pzsolve-1.21.1-linux-x86_64.tar.gz: OK"
```

```powershell
Get-FileHash .\pzsolve-1.21.1-windows-x86_64.zip   # Windows: l'hash deve essere quello della riga "windows" di SHA256SUMS
Get-Content .\SHA256SUMS                           # (lettere maiuscole o minuscole: è lo stesso hash)
```

## 2. A casa: la chiave e la prova generale

Crea la chiave pubblica su un computer di cui ti fidi. Chiede due volte una password (non si vede nulla) e stampa la chiave pubblica
(66 cifre) e il suo identificativo (8 cifre); non scrive nulla su disco. Usa 7 parole casuali o 20 caratteri casuali, estratti con i
dadi o da un programma, mai una parola, un nome o una data: chiunque veda la chiave pubblica può provare password contro di essa
offline. **Una password persa è una chiave persa**: scrivila su carta, in un posto che non sia la macchina in affitto, e tieni la chiave
pubblica dove puoi copiarla.

```sh
pzsolve encrypt-keygen
```

Prova l'intero percorso una volta su un puzzle risolto prima di qualsiasi affitto vero, con la stessa riga di comando di quella vera
tranne il puzzle: il puzzle 40 finisce in pochi secondi. Fallo sullo stesso tipo di sistema dell'affitto se puoi (lo stesso sistema
operativo, la stessa shell per il comando di `--on-found`), e una volta su Windows se l'affitto è una macchina Windows: la richiesta
nascosta della password nella console di Windows non è esercitata dai test di questo progetto (eseguono l'exe sotto Wine e passano la
password da una pipe), quindi provala tu.

```sh
pzsolve hunt -p 40 --encrypt-pubkey CHIAVE_PUBBLICA -c prova --on-found 'curl -s -d "$PZ_FOUND_SEALED" https://example.com/hook'
pzsolve decrypt-key campaigns/prova/FOUND.txt           # la password; deve finire con "check: OK"
pzsolve puzzle 40                                        # la sua chiave è pubblica: deve essere quella stampata da decrypt-key
```

Controlla che il riquadro sullo schermo e `FOUND.txt` mostrino una riga `PZENC1:` e nessuna chiave, che ciò che `--on-found` ha spedito
ti sia arrivato (`log.txt` dice come è finito il comando) e che `decrypt-key` abbia stampato `check: OK`. Poi cancella `campaigns/prova`.

## 3. Sulla macchina in affitto

Avvia la campagna dentro `screen` o `tmux`, così sopravvive alla chiusura della tua connessione (`Ctrl-A D` stacca, `screen -r pzsolve`
riporta dentro; con tmux `tmux new -s pzsolve`, `Ctrl-B D` e `tmux attach -t pzsolve`). Senza nessuno dei due,
`nohup pzsolve hunt ... --ui log > /dev/null &` fa lo stesso, con una riga di log al minuto in `log.txt`.

```sh
screen -S pzsolve
pzsolve hunt -p 71 --encrypt-pubkey CHIAVE_PUBBLICA --keep-awake --on-found 'curl -s -d "$PZ_FOUND_SEALED" https://example.com/hook'
```

* `--encrypt-pubkey CHIAVE_PUBBLICA`: la chiave del passo 2. Viene salvata nella campagna, quindi una ripresa non la richiede, e una chiave
  diversa è rifiutata. Il log dice `the key will be encrypted to the public key with id XXXXXXXX`: confronta l'identificativo con quello
  stampato da `encrypt-keygen`.
* `--on-found COMANDO` ti spedisce la riga cifrata nel momento in cui esiste, anche se la macchina ti viene tolta un minuto dopo. La riga
  si può spedire ovunque; `PZ_FOUND_SEALED` la contiene (su Windows `%PZ_FOUND_SEALED%`, eseguito da `cmd.exe`). Un `curl` a un webhook
  come sopra, oppure una mail: `--on-found 'printf "%s\n" "$PZ_FOUND_SEALED" | mail -s "pzsolve" me@example.com'`. Non viene salvato:
  dallo a ogni avvio. Non metterci una password o un token: gli altri utenti della macchina possono leggere la riga di comando di un processo.
* `--keep-awake` impedisce la sospensione su Windows e macOS; su Linux avvia invece con `systemd-inhibit pzsolve hunt ...`.
* `--limit 50` se la macchina è condivisa: metà dei thread CPU e dei work-item GPU, a piena velocità, senza pause.

Il primo avvio di una versione su una macchina con GPU la misura prima di cercare: le varianti dei kernel GPU e la lunghezza dei loro
lanci (circa un minuto per kernel su un portatile; un driver NVIDIA compila anche i kernel una volta, circa un minuto), poi la
combinazione migliore di thread CPU e GPU (circa 2 minuti). Il risultato è salvato nella cartella della cache (`~/.cache/pzsolve`, su
Windows `%LOCALAPPDATA%\pzsolve`) e l'avvio successivo è immediato.

Avanzamento: `pzsolve list` dalla stessa cartella (stato, avanzamento, velocità, tempo residuo); `campaigns/p71/report.txt` è il
rapporto leggibile e `status.json` lo stesso per i programmi, entrambi riscritti ogni 5 secondi. Ferma con Ctrl-C (o `kill PID`): lo
stato viene salvato. Riprendi con lo stesso comando, `--on-found` compreso. Più macchine sullo stesso puzzle: lo stesso seed su tutte e
una quota ciascuna, `pzsolve hunt -p 71 -s SEED --part 0/2 ...` sulla prima e `--part 1/2` sulla seconda (USO sezione 8.2; `pzsolve split`
divide le quote fra macchine di velocità diverse).

## 4. Cosa può e cosa non può vedere il proprietario della macchina

* Protetto: chi legge, dopo di te, i file della campagna, lo schermo o il suo scorrimento, il log, un backup, un'immagine del disco o un
  disco restituito: trova la riga cifrata, che si apre solo con la tua password.
* Non protetto: un amministratore (root, il fornitore di una macchina virtuale) che legge la memoria del processo nel momento in cui la
  chiave è trovata, o che sostituisce l'eseguibile o la chiave pubblica nella riga di comando (l'identificativo nel log te lo dice dopo,
  non prima).
* In chiaro per natura: l'indirizzo, il puzzle, l'intervallo, il seed, l'avanzamento e l'ora del ritrovamento. USO sezione 11.5 ha il
  modello di minaccia completo.

## 5. Dopo

Porta a casa `FOUND.txt`, o la riga che hai ricevuto. La riga da sola basta, e resta valida per sempre.

```sh
pzsolve decrypt-key FOUND.txt                            # la password; stampa la chiave, il suo WIF e i due indirizzi
```

Deve stampare `check: OK` con l'indirizzo che cercavi: una riga la può fare chiunque conosca la chiave pubblica, quindi solo il
controllo dell'indirizzo prova che la chiave è quella giusta. Sposta i fondi con un wallet che importa il WIF. Quando l'affitto finisce,
cancella la cartella della campagna sulla macchina in affitto (`rm -rf campaigns/p71`): contiene l'intervallo, il bersaglio, il seed, la
chiave pubblica e l'avanzamento della ricerca, mai la chiave, ma non c'è motivo di lasciarli lì.
