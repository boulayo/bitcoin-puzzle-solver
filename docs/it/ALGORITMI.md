# Algoritmi di pzsolve

Questo documento spiega *cosa* calcola il programma e *perché* è efficiente. Per la struttura del codice vedi
[`ARCHITETTURA.md`](ARCHITETTURA.md), per l'uso [`USO.md`](USO.md).

## 1. Il problema

Per il puzzle *n* la chiave privata `k` sta in `[2^(n-1), 2^n − 1]` (intervallo di ampiezza `W = 2^(n-1)`), l'indirizzo è
`Base58Check(0x00 ‖ RIPEMD160(SHA256(pubkey compressa)))` con `pubkey = k·G` sulla curva secp256k1.

* Se si conosce **solo l'indirizzo** l'unico metodo noto è provare le chiavi: costo medio `W/2` valutazioni di
  `k → hash160(k·G)`.
* Se si conosce la **chiave pubblica** `Q = k·G` (rivelata da una transazione in uscita) il problema diventa un
  *logaritmo discreto in un intervallo*, risolvibile in circa `c·√W` operazioni di gruppo (algoritmi del canguro).

## 2. Ricerca lineare (solo indirizzo)

### 2.1 Aritmetica

* Campo `GF(p)`, `p = 2^256 − 2^32 − 977`, in 4 limbi da 64 bit con riduzione lazy (`2^256 ≡ 2^32 + 977`).
  Moltiplicazione 4×4 in assembly inline con **MULX** e le due catene di riporto **ADCX/ADOX** (ADX); versione C portabile
  per le CPU senza BMI2/ADX (le due sono confrontate in `lib/pzcore/tests/t_fe.c` su 400 000 coppie, bordi inclusi).
* Le chiavi consecutive di un blocco si ottengono **sommando punti affini** invece di rifare una moltiplicazione scalare per
  chiave. Una somma affine richiede un'inversione modulare, che costa come ~270 moltiplicazioni: si ammortizza con il
  **trucco di Montgomery** (un'unica inversione per un vettore di denominatori).
* **Gruppo simmetrico.** Per un centro `C = c·G` e la tabella `i·G` (i = 1…H) i denominatori `x(i·G) − x(C)` valgono sia per
  `C + i·G` sia per `C − i·G` (stessa ascissa, ordinata opposta): un'unica inversione per `2H` chiavi consecutive, più il
  centro successivo `C + 2H·G`. Con `H = 256` (CPU) il costo è ≈ `3,5 M + 1 S` per chiave più 1/512 di inversione.
* Per l'hash serve la parità di `y`: la si ottiene calcolando anche `y` (una moltiplicazione) invece di fare due hash.

### 2.2 Hash160 vettoriale

SHA-256 (un solo blocco: 33 byte + padding) e RIPEMD-160 (un solo blocco: 32 byte + padding) sono scritti una sola volta con
le *vector extensions* di GCC e istanziati a 4 lane (SSE2/NEON), 8 lane (AVX2) e 16 lane (AVX-512). Il motore da usare è
scelto a runtime con un micro-benchmark di 0,25 s (`--isa` lo forza). Per ogni lotto si confronta solo la **prima parola** del
digest con il bersaglio; i rari candidati vengono verificati per intero con l'hash scalare e poi riverificati
ricalcolando `k·G` da zero.

### 2.3 Casi limite

I denominatori nulli (chiavi minuscole, ad es. centro = 2·512) azzerano il prodotto batch: la condizione è rilevata con un
solo controllo per gruppo e il gruppo viene ricalcolato in modo esatto (una moltiplicazione scalare per chiave). I blocchi che
iniziano sotto 2^22 sono scansionati sempre sulla CPU anche in presenza di GPU. Le chiavi oltre il confine di un blocco
non vengono mai assegnate due volte: lo scheduler conosce l'esatta lunghezza di ogni blocco (anche dell'ultimo, più corto,
negli intervalli personalizzati).

## 3. Ordine di visita unico (seed)

L'intervallo è diviso in `M = ⌈W / 2^B⌉` blocchi di `2^B` chiavi (default `B = 36` per gli intervalli grandi, ridotto per i puzzle piccoli). La campagna
visita i blocchi nell'ordine `π(0), π(1), π(2), …` dove `π` è una **permutazione pseudo-casuale di [0, M)** determinata dal seed.

* `π` è una rete di Feistel (8 round) con funzione di round `SHA-256(seed ‖ round ‖ metà)`, su `⌈log2 M⌉` bit; i domini non
  potenza di due si gestiscono con *cycle-walking*. È una **biiezione**: ogni blocco è visitato esattamente una volta, e si
  calcola in O(1) (qualche SHA-256 per blocco, trascurabile rispetto alle 2^32 chiavi di un blocco). L'inversa permette a
  `pzsolve where` di dire *quando* una chiave verrà raggiunta.
* **Perché non un'aritmetica modulare con un primo grande** (`i ↦ a·i + b mod p`): è una permutazione valida e velocissima,
  ma i blocchi successivi risultano in progressione aritmetica (poche osservazioni rivelano il passo, e due campagne con
  moltiplicatori correlati si sovrappongono in modo strutturato). La rete di Feistel è altrettanto economica e statisticamente
  indistinguibile da una permutazione casuale.
* Dentro un blocco la scansione è sequenziale (è la forma più efficiente). Il progresso è tenuto come insieme di
  intervalli completati nello "spazio virtuale" `V = posizione_nel_percorso · 2^B + offset`: i buchi sono al massimo uno per
  richiesta di lavoro in corso, quindi il checkpoint resta di poche righe anche dopo mesi (`state.dat`, con checksum).
* Il seed è a 256 bit (`getrandom` + tempo + pid + host, hashati); `--seed` accetta 64 cifre hex (usate così come sono) o
  un testo qualunque (hashato).

* **Più computer (`--part`, dalla 1.7.5)**. Si dividono le *posizioni* dell'ordine di visita, non l'intervallo: con `T` quote, il computer
  che ha le quote `r₁ < r₂ < … < r_m` scandisce i blocchi alle posizioni `k` con `k mod T ∈ {r₁,…,r_m}`, nel suo ordine locale
  `j ↦ ⌊j/m⌋·T + r_{j mod m}`. Le posizioni dei computer sono disgiunte e `π` è una biiezione, quindi i blocchi (e le chiavi) sono
  disgiunti e coprono tutto, senza comunicazione. Poiché tutti i blocchi sono uguali (tranne al più quello di fondo intervallo), la
  quota è proporzionale al lavoro: a un computer il doppio più veloce basta darne il doppio (`pzsolve split`). A turni, e non a
  fette contigue dell'intervallo, perché così ogni computer segue lo stesso ordine casuale e la copertura cresce in modo uniforme
  su tutto l'intervallo. Il numero di blocchi della parte è `q·m + #{r_i < M mod T}` con `q = ⌊M/T⌋`; la lunghezza esatta in chiavi
  tiene conto del blocco corto: lo si trova con la `π⁻¹` del suo indice.

**Cosa NON fa**: non rende più probabile trovare la chiave. In un intervallo uniforme ogni ordine ha la stessa probabilità di
successo per chiave scansionata. L'ordine casuale evita solo di duplicare il lavoro di chi parte dall'inizio (o dalla
fine) dell'intervallo e rende ogni campagna indipendente dalle altre.

## 4. Il canguro per intervalli (chiave pubblica nota)

### 4.1 Impostazione (dalla 1.6.0)

Questa sezione descrive lo **schema 1**, usato dalle campagne iniziate dalla 1.6.0 alla 1.7.7 (che lo conservano). Le campagne nuove
dalla 1.7.8 usano lo schema 2: stesse classi, stesso passo, ma tre specie di camminatori con residui fissi mod 12 (sezione 8.8), circa il 4 % di
passi in meno.

Sia `S = W/32` (o meno, se l'intervallo comincia sotto `W/32`), `base = lo − S` e `Q' = Q − base·G`. La chiave è
`k = base + x` con `x ∈ [S, S + W)`: l'intervallo sta tutto da una parte del punto `0` attorno a cui le classi si ripiegano.

**Classi di equivalenza.** `P` e `−P` hanno la stessa ascissa; si lavora sulle classi `{P, −P}` e si sceglie come
rappresentante il punto con `y` pari. Il camminatore (canguro) porta con sé:

* il punto canonico `P = e·G` e il suo esponente *esatto* come intero con segno 192 bit:
  * canguro **tame** (un terzo): parte da `e = t`, `t ∈ [1, W/32)`, ed `e = d`;
  * canguro **wild** (due terzi): parte da `e = −x + v`, `v` pari in `[0, W + W/16)` (cioè dal punto `v·G − Q'`), ed
    `e = σ·x + d`, con `σ = ±1` (cambia segno quando la canonicalizzazione inverte il punto).

Passo: `j = x(P) mod 256`, `P ← canon(P + S_j)`, `d ← d + s_j`; se `y` è dispari si nega il punto (`d ← −d`, `σ ← −σ`).
I salti `s_j` (256, **pari**, uniformi in `[2, 24·√W]`, media `12·√W`) sono pseudo-casuali dal seed. Poiché il rappresentante
ha un segno casuale, l'esponente fa una passeggiata simmetrica (nessuna deriva): ogni camminatore esplora i dintorni del
punto di partenza.

**Punti distinti (DP).** Quando `x.n[1]` ha `dp_bits` zeri bassi si registra `(ascissa, tipo, d)` in una tabella; due
registrazioni con la **stessa classe ma tipo diverso** rivelano `x` (tipo ∈ {tame, wild σ=+1, wild σ=−1}):

| coppia                         | equazione dalla stessa classe | soluzione            |
|--------------------------------|-------------------------------|----------------------|
| tame `d_t`, wild(+1) `d_w`     | `d_t = x + d_w`               | `x = d_t − d_w`      |
| tame `d_t`, wild(−1) `d_w`     | `d_t = −x + d_w`              | `x = d_w − d_t`      |
| wild(+1) `d₁`, wild(−1) `d₂`   | `x + d₁ = −x + d₂`            | `x = (d₂ − d₁)/2`    |

Con salti e partenze pari, `d₂ − d₁` è sempre pari: ogni incontro wild(+1)–wild(−1) dà la soluzione.
Le coppie dello stesso tipo non danno informazione (stessa traiettoria) e il canguro viene riavviato in un punto casuale.
Ogni soluzione candidata è verificata (`(base+x)·G` contro indirizzo/pubkey) prima di essere accettata: i falsi positivi
della tabella (tag a 62 bit) non possono produrre risultati sbagliati.

### 4.2 Perché costa meno

Nel canguro classico (tame/wild con salti solo positivi) il wild deve "raggiungere" la traccia del tame:
`≈ 2·√W` passi. Con le classi i punti vivono in uno spazio grosso modo dimezzato e le collisioni sono un *problema del
compleanno*.

* **Fino alla 1.5.0** si usava il Gaudry–Schost "centrato": `base` al centro dell'intervallo, tame uniformi su tutto
  l'intervallo, wild in una finestra `±W/8` attorno a `x`; le collisioni utili erano quasi solo tame–wild (bipartite).
  Misurato: **≈ 1,26·√W** (teoria di Galbraith–Ruprai ≈ 1,36·√W).
* **Dalla 1.6.0** la disposizione è quella del metodo "SOTA" di RCKangaroo (RetiredCoder, 2024; studiato, codice non usato):
  i wild partono da `−x + v` con `v` sparso su tutto l'intervallo, quindi i loro esponenti stanno da **entrambe le parti**
  dello zero e le classi si ripiegano su sé stesse: due wild in posizioni simmetriche (`−x + v₁ = −(−x + v₂)`, cioè
  `v₁ + v₂ = 2x`) collidono e danno `x`. Ogni wild può incontrare quasi tutti gli altri wild (compleanno *non* bipartito,
  più efficiente), mentre i tame coprono la zona vicina allo zero, dove i wild ripiegati sono più radi. Misurato: **≈ 1,1–1,2·√W**
  (sezione 4.5), circa il **10 % di passi in meno**, coerente con il K = 1,15 dichiarato da RCKangaroo.

### 4.3 Cicli infruttuosi e deriva

Camminando sulle classi `{P, −P}` un camminatore può tornare su un punto già visitato senza mai incontrare un DP: da lì in
poi ripete gli stessi punti per sempre (ciclo *infruttuoso*). Con `R` salti nella tabella (Bos–Kleinjung–Lenstra, *On the use
of the negation map in the Pollard rho method*, 2010):

* **Cicli di lunghezza 2** (`P → P' → P`): probabilità `1/(2R)` per passo. Quando il punto corrente coincide con quello di
  due passi prima si usa il salto successivo `(j+1) mod R`: la regola dipende solo dalla storia `(P_{n−1}, P_n)`, quindi due
  camminatori che si fondono continuano a fondersi.
* **Cicli di lunghezza 4, 6, …**: anche eliminando quelli di 2 restano con probabilità `≈ (R−1)/(4R³) ≈ 1/(4R²)` per passo,
  cioè **uno ogni ≈ 2,6·10⁵ passi** con `R = 256` (misurato: 3·10⁵ nel modello di `tools/ksim.c`, quasi tutti di lunghezza 4).
  **Fino alla 1.5.0 l'unica difesa era il riavvio dopo `24·2^dp_bits` passi senza DP**: va bene per gli intervalli piccoli, ma
  con `dp_bits = 45` (puzzle 140) quella soglia non viene mai raggiunta, e ogni camminatore restava intrappolato dopo pochi
  minuti (GPU: ~1700 passi/s per camminatore). La velocità mostrata restava alta, ma da lì in poi la ricerca non poteva più
  trovare collisioni. Nel modello, con pochi salti per rendere frequenti i cicli e la soglia disattivata, il 38–95 % delle
  ricerche non termina.
* **Rilevazione e uscita deterministica (dalla 1.5.1)**: ogni `64` passi il camminatore memorizza l'etichetta del punto
  corrente (*checkpoint*, 64 bit di `x`) e da lì tiene la minima etichetta vista. Se torna sul checkpoint è in un ciclo, e la
  minima è quella del ciclo intero; continua a girare finché arriva al punto con l'etichetta minima e da lì fa un **salto di
  fuga**, preso da una tabella separata di 16 salti (stessa distribuzione di quelli normali). Il punto di uscita dipende solo
  dal ciclo, non da dove o quando il camminatore ci è entrato: due camminatori che si erano fusi escono nello stesso modo e
  restano fusi, quindi la collisione si vede al DP successivo (con il riavvio, o con un'uscita dal punto di rilevazione, le
  tracce si separerebbero a ogni ciclo, migliaia di volte tra un DP e l'altro). Il salto di fuga non fa parte della tabella
  normale, quindi la camminata non può ricadere nello stesso ciclo con una combinazione di salti normali. Se il ciclo non si
  chiude entro un periodo (lunghezza ≥ 32: probabilità trascurabile) la rilevazione ricomincia; il riavvio dopo
  `24·2^dp_bits` passi resta come ultima difesa.
  Costo: qualche confronto per passo e in media ≈ 40 passi sprecati per ciclo (0,015 %). Verifiche: nel modello, con 16 salti
  e la soglia disattivata, ogni ricerca termina (costo 2,0·√W, contro il 38 % di ricerche bloccate senza uscita); sulla curva
  vera il self-test risolve un intervallo di 2^30 chiavi con soli 8 salti (un ciclo ogni ~250 passi), DP ogni 128 passi e
  nessun riavvio, che senza l'uscita dai cicli non termina.
* **Contatore dei passi senza DP a 64 bit** dalla 1.5.0: fino alla 1.4.0 era a 32 bit e la soglia si fermava a 2^32, così con
  `dp_bits ≥ 33` quasi ogni camminatore sarebbe stato riavviato prima del suo punto distinto.
* **Nessuna deriva da regolare**: la passeggiata sulle classi è simmetrica; la lunghezza media dei salti (`12·√W`) è
  quella di RCKangaroo, e nel simulatore l'esito è quasi indifferente tra `0,5·√W` e `40·√W` (1,16–1,21).

### 4.4 Parametri (scelti automaticamente)

| Parametro | Valore | Motivazione |
|-----------|--------|-------------|
| base | inizio − `W/32` | l'intervallo da una parte dello zero, lontano dal ripiegamento |
| zona tame | `[1, W/32)`, un terzo dei camminatori | come RCKangaroo; nel simulatore 1/3 è meglio di 1/5 e 1/2 |
| zona wild | `−x + v`, `v` pari in `[0, W + W/16)` | i wild coprono tutto l'intervallo da entrambe le parti dello zero |
| salti | 256 pari, uniformi in `[2, 24·√W]` (+16 di fuga dai cicli) | passeggiata simmetrica, salti pari per le collisioni wild–wild |
| camminatori per thread CPU | `clamp(√W/(256·thread), 16, 4096)`, multiplo di 16 | l'inversione batch è condivisa da tutto il branco del thread; 4096 è l'ottimo misurato (sez. 8.4) |
| camminatori GPU | `NK` per work-item (64/128/256, tarato) × 512 work-item per SM (CUDA) | al più `√W/256` in tutto, così ognuno cammina abbastanza |
| `dp_bits` | `max(⌊log2(0,01·√W/K)⌋, bit richiesti dalla RAM, 2)` | più RAM → meno overhead |
| tabella DP | 32 byte/voce, cresce fino al limite `--mem` (default 40 % della RAM libera, max 64 GiB); se il limite viene raggiunto la densità dei punti memorizzati viene dimezzata (si tengono solo i DP con un bit in più a zero) invece di fermarsi | un solo mutex: i DP sono rari |

### 4.5 Misure (curva reale, `pzsolve kstat BITS PROVE THREAD`)

Costo medio in unità di `√W` (4 thread, chiave uniforme nell'intervallo, `dp-bits` automatici):

| W | prove | media 1.6.0 | mediana | 90° percentile | media fino alla 1.5.0 (300 prove) |
|---|-------|-------------|---------|----------------|-----------------------------------|
| 2^28 | 400 | **1.21**·√W | 1.13·√W | 2.13·√W | 1.26·√W |
| 2^32 | 400 | **1.20**·√W | 1.10·√W | 2.06·√W | 1.23·√W |
| 2^36 | 400 | **1.11**·√W | 1.01·√W | 2.02·√W | 1.31·√W |
| 2^40 | 400 | **1.08**·√W | 1.00·√W | 1.89·√W | 1.21·√W |
| 2^44 | 400 | **1.16**·√W | 1.12·√W | 1.99·√W | 1.31·√W |

Media complessiva ≈ **1,15·√W** (errore standard per riga ≈ 0,03, complessivo ≈ 0,015), contro 1,26 della disposizione
precedente, ≈ 2·√W del canguro classico e ≈ 1,36·√W della stima teorica Galbraith–Ruprai.

La distribuzione ha coda lunga (è di tipo "compleanno": circa `1 − exp(−(n/c)²)` con `c ≈ 1,30·√W`): mediana ≈ 1,08·√W,
90° percentile ≈ 2,0·√W. Il programma mostra la probabilità di successo stimata e i tempi per il 50 % e il 90 %.

### 4.6 Cose valutate e scartate

* **Endomorfismo GLV** (`λ·P`, classi da 6): riduce il canguro/rho sull'intero gruppo, ma nell'intervallo `λ·e mod n` esce
  dall'intervallo piccolo, quindi non crea collisioni utili.
* **BSGS con tabella in RAM**: con `T` voci in tabella (le ascisse di `t·G`, `t < T`, valgono anche per `−t`) ogni passo
  gigante copre `2T+1` valori: costo medio `T + W/(4T)`, minimo `1,0·√W` per `T = √W/2` (caso peggiore `1,41·√W`). **Con RAM
  sufficiente batte quindi il canguro (1,15·√W)**. Dalla 1.1.0 è implementato (`--mode bsgs`, sezione 9); la tabella
  occupa `≈ 6,2·√W` byte (25 GiB a `W = 2^64`, 98 GiB a `2^68`), per cui è usabile solo per intervalli medio-piccoli.
* **Canguro a 4 animali** (≈ 1,715·√W) e Gaudry–Schost senza simmetria (≈ 1,66·√W): superati dalla variante simmetrica.
* **Parametri della variante centrata (fino alla 1.5.0, `tools/ksim.c`, 8000 prove per punto, W = 2^28, errore ≈ ±0,007)**: griglia di 45
  combinazioni di zona wild `B ∈ {1/16…1/4}·W`, frazione tame `{0,4; 0,5; 0,6}` e salto medio `{0,3; 0,5; 0,8}`, più 4 ampiezze della
  zona tame. Il punto in uso (`A = W/2`, `B = W/8`, metà tame, salto 0,5) è il migliore (1,292·√W nel modello); l'ottimo è piatto e
  le scelte lontane costano fino al 7 % (tame su `0,75·W`: +17 %). Nessun margine *in quella famiglia*: il guadagno è
  venuto cambiando disposizione (sezione 4.2). Nel modello, 3000–4000 prove per punto (errore ≈ ±0,01), stesso numero di
  camminatori e `dp`: centrata 1,27–1,36·√W, nuova disposizione 1,14–1,23·√W su `W = 2^26 … 2^34` e 64–2048 camminatori
  (`ksim rck=1 tf=0.3333 jabs=12`).
* **Terza specie di camminatori su `2x`** (wild con esponente `±2x + d`, partenza da `2Q`, ogni collisione con coefficienti diversi dà
  `(c₁ − c₂)·x = d₂ − d₁`): 27 combinazioni di quote e ampiezze, la migliore costa 1,365·√W, cioè il 6 % **in più**; più camminatori
  su `2x` peggiorano (fino a 1,78). I loro punti cadono spesso fuori dalla zona dei tame e le coppie in più non compensano.
* **Distribuzione delle chiavi risolte**: la posizione relativa nell'intervallo delle 82 chiavi note è compatibile con l'uniforme
  (Kolmogorov–Smirnov D = 0,074, soglia al 5 % 0,15; media 0,51; bit a 1 nel 49,3 %): nessuna parte dell'intervallo è più probabile.

### 4.7 Ripresa

Ogni DP nuovo è accodato a `dp.bin` (32 byte + intestazione con impronta di intervallo e chiave). Alla ripresa la tabella è
ricostruita e i camminatori ripartono da punti nuovi (flusso casuale diverso per sessione); le tracce vecchie
restano utili perché ogni loro DP successivo è già in tabella. Un file di una campagna diversa viene riconosciuto e rifiutato.
Dalla 1.7.8 le campagne nuove usano lo schema 2 e il formato 3 del registro (`"PZDP3"`, sezione 8.8); quelle con un registro in formato 1 o 2
restano sullo schema 1. Dalla 1.6.0 la base è cambiata (sezione 4.1): un `dp.bin` della 1.5.0 o precedenti viene convertito alla prima ripresa
(i punti restano gli stessi; le distanze dei wild sono riscritte rispetto alla nuova base, `d' = d − σ·(c − b)`), con un
self-test che lo verifica sulla curva.

## 5. GPU (CUDA e OpenCL)

* I kernel sono scritti **una volta sola** in OpenCL C 1.2 con campo a 8 limbi da 32 bit. Per OpenCL il driver li compila dai sorgenti
  incorporati; per CUDA `tools/gen_ptx.py` li compila con NVRTC (tramite `src/kernels/pz_cuda.h`, che traduce il dialetto) in PTX per
  `compute_52`, incorporato nell'eseguibile e compilato dal driver NVIDIA per la scheda presente (risultato in cache). Entrambi i runtime
  sono caricati dinamicamente (`dlopen` / `LoadLibrary`): nessuna dipendenza in compilazione né SDK a runtime.
* Con CUDA: 128 thread per blocco (`__launch_bounds__(128)`), griglia arrotondata per eccesso e work-item in più che escono subito
  (ogni kernel riceve il numero di work-item come ultimo argomento); hash160 e inversione modulare compilati una volta sola come
  funzioni vere (PTX più piccolo e meno registri: 128 per il kernel lineare su sm_86, nessuno spill significativo); il contesto attende
  la fine dei kernel senza attesa attiva, così il core resta ai thread di calcolo della CPU.
* **Aritmetica CUDA (dalla 1.3.0).** Misurato sul codice sm_86 (Ampere): per chiave il kernel lineare esegue ≈ 4200 istruzioni
  intere, metà nell'hash160 (≈ 2100, già vicino all'ottimo) e metà nel campo. A ≈ 513 M chiavi/s la scheda di prova (una GPU NVIDIA da portatile) è al limite
  del suo throughput intero (20 SM × 64 operazioni per ciclo): più thread non servono, conta il numero di istruzioni. Per questo
  addizione, sottrazione e la riduzione dopo un prodotto usano catene di riporto PTX (`add.cc`/`addc`, `sub.cc`/`subc`,
  `mad.lo.cc`/`madc.hi.cc` per la moltiplicazione per 977): add+sub 176 → 88 istruzioni, moltiplicazione 328 → 288, quadrato
  dedicato (36 prodotti invece di 64) 328 → ≈ 232. Il prodotto 8×8 resta in C perché Ampere non ha `madc.hi` nativo (la versione
  tutta PTX misurava 368). Stima: ≈ 11 % di istruzioni in meno per chiave. La logica delle catene è verificata con un'emulazione
  esatta della semantica PTX (`tests/t_gpufe.c`, 5 milioni di combinazioni, casi limite dei doppi riporti; test delle mutazioni);
  sul dispositivo la conferma il self-test.
* **Varianti del kernel lineare (CUDA)**: finestra 128, 256 o 512 × registri liberi (≈ 160) o limitati a 128 (`__launch_bounds__(128, 4)`:
  più warp residenti). La taratura le misura tutte e sei sul dispositivo e tiene la più veloce.
* **Kernel lineare con inversione "fusa" (1.5.0)**: il passaggio all'indietro del trucco di Montgomery ricava ogni inverso e lo usa
  subito per i punti `C ± i·G`, ricalcolando il denominatore dalla tabella invece di tenerlo in un array. La memoria privata per
  work-item si dimezza (finestra 256: 17 → 8,7 KB) e il traffico verso la memoria video scende da ≈ 96 a ≈ 32 byte per chiave
  (con ≈ 600 M chiavi/s erano ≈ 57 GB/s su ≈ 190). Il filtro sull'hash usa le prime tre parole (96 bit, ogni candidato è comunque
  riverificato sulla CPU): non dipendono dagli ultimi passi di RIPEMD-160, che il compilatore elimina (≈ 0,4 % di istruzioni).
* **Lineare**: ogni work-item scansiona `ngroups` gruppi consecutivi dal proprio centro `P0 + gid·D` (calcolato sul
  dispositivo con una piccola moltiplicazione double-and-add); gruppi da `2·HALF` chiavi, tabella `i·G` in memoria constant, hash
  160 completo sul dispositivo, hit raccolti con un contatore atomico. La durata di un lancio è regolata a ~0,35 s.
  `HALF` è una costante di compilazione del kernel: **64, 128 o 256**; alla prima esecuzione su una GPU reale il programma le prova tutte e
  tre (qualche secondo ciascuna), sceglie la più veloce (serve un vantaggio > 3 % su 128) e memorizza la scelta in
  `$XDG_CACHE_HOME/pzsolve/gpu_tune.txt` (chiave: nome, versione del driver, unità di calcolo, versione del programma).
  `PZ_GPU_HALF=N` la impone, `PZ_GPU_TUNE=0` disattiva la taratura; `pzsolve selftest` verifica tutte le varianti.
* **Canguro (riscritto nella 1.5.0, kernel v2 nella 1.7.0)**: fino alla 1.4.0 ogni work-item gestiva **16** camminatori: un'inversione (≈ 270 moltiplicazioni)
  ogni 16 passi, cioè ≈ 17 moltiplicazioni per passo contro le ≈ 6 del passo stesso, e il kernel usava 255 registri con spill.
  Ora `NK` = 64, 128 o 256 camminatori per work-item (taratura sul dispositivo, `PZ_GPU_NK` per imporlo), stesso schema "fuso"
  del lineare (nessun array dei denominatori), **128 registri senza spill** e quindi 512 work-item residenti per SM. Stato in
  memoria globale (128 byte per camminatore: x, y, distanza a 192 bit, tipo, segno, etichette a 32 bit anti-ciclo, ultimo DP);
  i DP escono in un buffer e vengono inseriti nella stessa tabella della CPU. I riavvii sono gestiti dall'host.
  **Kernel v2 (1.7.0)**, costruito attorno al traffico di memoria (sez. 8.4): (a) la distanza non viaggia da passo a passo: ogni
  passo aggiunge 16 bit a un buffer di *storia* (indice del salto, bit "negato") e `d` si ricostruisce rigiocando la storia, a fine
  lancio per tutti e, per i rari punti distinti, al momento; (b) una sola passata per round invece di due: la passata che muove i
  camminatori calcola anche i denominatori del round dopo e i loro prodotti correnti, che alternano da prefissi a suffissi (la
  passata scende, poi sale, ...): il trucco di Montgomery funziona con entrambi e ogni passata legge e sovrascrive i prodotti
  lasciati dalla precedente; (c) l'inversione è per divstep (sez. 8.4); (d) un lancio fa al massimo 64 round (profondità della storia),
  a partire dal numero di passo comune a tutto il branco.
* **Verifica**: all'avvio ogni dispositivo esegue (a) operazioni di campo, hash160 e somme di punti confrontate con la CPU,
  (b) una ricerca lineare con chiave piantata, (c) per il canguro **lo stesso gruppo di camminatori eseguito su GPU e su CPU,
  con confronto bit-a-bit di stati, DP e riavvii** (con un limite di passi basso, così i riavvii avvengono davvero). Un dispositivo
  che non passa viene escluso; una scheda NVIDIA che fallisce via CUDA viene riprovata via OpenCL. Ogni chiave trovata viene
  comunque riverificata sulla CPU.
* Prestazioni: vedi il README; i kernel vanno rimisurati con `bench` sulla propria scheda. Verifica funzionale: OpenCL su pocl (OpenCL su CPU); CUDA con un driver simulato che
  esegue gli stessi sorgenti dei kernel dietro le API del driver (`tests/mock_cuda/`), mentre la traduzione PTX→GPU del driver vero è
  coperta dal self-test all'avvio. `ptxas` accetta tutti i moduli per sm_86.

## 6. Quanto tempo serve (ordini di grandezza)

Ricerca lineare, **caso medio** (metà intervallo), a velocità costante:

| puzzle | chiavi (media) | 100 M/s (CPU) | 1 G/s | 10 G/s |
|--------|----------------|---------------|-------|--------|
| 50 | 2.81e+14 | 32.6 giorni | 3.26 giorni | 7.82 ore |
| 60 | 2.88e+17 | 91.3 anni | 9.13 anni | 334 giorni |
| 66 | 1.84e+19 | 5.8e+03 anni | 585 anni | 58.5 anni |
| 70 | 2.95e+20 | 9.4e+04 anni | 9.4e+03 anni | 935 anni |
| 71 | 5.90e+20 | 1.9e+05 anni | 1.9e+04 anni | 1.9e+03 anni |
| 75 | 9.44e+21 | 3.0e+06 anni | 3.0e+05 anni | 3.0e+04 anni |
| 80 | 3.02e+23 | 9.6e+07 anni | 9.6e+06 anni | 9.6e+05 anni |
| 90 | 3.09e+26 | 9.8e+10 anni | 9.8e+09 anni | 9.8e+08 anni |
| 100 | 3.17e+29 | 1.0e+14 anni | 1.0e+13 anni | 1.0e+12 anni |
| 130 | 3.40e+38 | 1.1e+23 anni | 1.1e+22 anni | 1.1e+21 anni |
| 135 | 1.09e+40 | 3.5e+24 anni | 3.5e+23 anni | 3.5e+22 anni |
| 160 | 3.65e+47 | 1.2e+32 anni | 1.2e+31 anni | 1.2e+30 anni |


Canguro, **caso medio** (≈ 1,15·√W passi dalla 1.6.0; dalla 1.7.8 ≈ 1,10·√W, cioè circa il 4 % in meno di quanto in tabella):

| puzzle | passi (media) | 100 M/s (CPU) | 1 G/s | 10 G/s |
|--------|---------------|---------------|-------|--------|
| 60 | 9.64e+08 | 9.64 s | 964 ms | 96.4 ms |
| 70 | 3.09e+10 | 309 s | 30.9 s | 3.09 s |
| 80 | 9.87e+11 | 2.74 ore | 987 s | 98.7 s |
| 90 | 3.16e+13 | 3.66 giorni | 8.78 ore | 3.16e+03 s |
| 100 | 1.01e+15 | 117 giorni | 11.7 giorni | 1.17 giorni |
| 110 | 3.24e+16 | 10.3 anni | 1.03 anni | 37.4 giorni |
| 120 | 1.04e+18 | 328 anni | 32.8 anni | 3.28 anni |
| 130 | 3.31e+19 | 1.0e+04 anni | 1.0e+03 anni | 105 anni |
| 135 | 1.87e+20 | 5.9e+04 anni | 5.9e+03 anni | 594 anni |
| 140 | 1.06e+21 | 3.4e+05 anni | 3.4e+04 anni | 3.4e+03 anni |
| 150 | 3.39e+22 | 1.1e+07 anni | 1.1e+06 anni | 1.1e+05 anni |
| 160 | 1.09e+24 | 3.4e+08 anni | 3.4e+07 anni | 3.4e+06 anni |

I puzzle senza chiave pubblica sono fuori portata per l'hardware di un singolo computer da `n ≳ 70`; quelli con chiave
pubblica nota lo sono da `n ≳ 110–120`. Il programma resta corretto per qualunque `n ≤ 190` (canguro) / `n ≤ 255` (lineare),
ma il tempo cresce come `2^(n/2)` e `2^n`.

## 7. Riferimenti

* S. Galbraith, R. Ruprai, *Using Equivalence Classes to Accelerate Solving the Discrete Logarithm Problem in a Short
  Interval* (PKC 2010): <https://eprint.iacr.org/2010/615> — costo ≈ 1,36·√N con classi di equivalenza.
* S. Galbraith, J. Pollard, R. Ruprai, *Computing Discrete Logarithms in an Interval* (Math. Comp. 82, 2013):
  <https://eprint.iacr.org/2010/617> — canguro a 4 animali ≈ 1,715·√N, Gaudry–Schost migliorato ≈ 1,661·√N.
* S. Galbraith, M. Holmes, *A non-uniform birthday problem with applications to discrete logarithms*:
  <https://eprint.iacr.org/2010/616>.
* P. van Oorschot, M. Wiener, *Parallel collision search with cryptanalytic applications* (J. Cryptology 1999): punti distinti.
* P. Gaudry, E. Schost, *A low-memory parallel version of Matsuo, Chao, and Tsujii's algorithm* (ANTS 2004).
* Il simulatore astratto usato per tarare i parametri è in `tools/ksim.c`.

## 8. Limiti teorici: quanto si può ancora migliorare?

Questa sezione separa ciò che è **noto in letteratura** da ciò che è **un mio ragionamento** (indicato come tale).

### 8.1 Il fattore davanti a √W (costante `q`)

| Algoritmo | Memoria | Costo medio in unità di √W | Origine |
|-----------|---------|----------------------------|---------|
| canguro classico (van Oorschot–Wiener) | piccola | 2 | letteratura |
| 4 canguri / Gaudry–Schost migliorato | piccola | 1,715 / 1,661 | Galbraith–Pollard–Ruprai |
| Gaudry–Schost con classi `±` | piccola | ≈ 1,36 (teorico) | Galbraith–Ruprai |
| **questa implementazione** | piccola (tabella DP) | **schema 3 (1.7.9): 1,06 misurato da 2^32 a 2^44, ≈ 1,02 sui puzzle aperti (modello, 8.10)**; schema 2 (1.7.8): ≈ 1,045 con camminate corte (8.8); schema 1: 1,08–1,21, media 1,15 (misurato, 1.6.0; prima 1,26), ≈ 1,09 con tanti camminatori (modello, 8.7) | `pzsolve kstat`, `ksim` |
| BSGS con `±`, `T = √W/2` | `≈ 4·√W` byte | **1,0** (caso peggiore 1,41) | calcolo diretto (sez. 4.6) |
| limite inferiore per conteggio | qualsiasi | **≥ ≈ 0,67**, ottimistico: con valori interi l'ottimo del modello è 0,8165 (8.7) | mio ragionamento, sotto |

**Sì, `q < 1` è possibile in linea teorica? Solo oltre un certo punto.** Con memoria sufficiente il BSGS con `±` arriva a
`q = 1,0`; alternando passi piccoli e giganti con una disposizione ordinata delle tessere la stima sale a `≈ 0,94`
(non verificata). Il minimo teorico è sconosciuto: un argomento di conteggio dà solo `q ≳ 0,67`.

*Argomento di conteggio (mio, nello spirito di Shoup).* Nel modello generico ogni elemento ottenuto ha esponente `a + b·x`
con `x` incognito. L'unica informazione su `x` viene da due elementi che coincidono (o sono opposti): una coppia con
`b₁ ≠ ±b₂` fissa al più 2 valori di `x`, una coppia con `|b₁| = |b₂|` al più 1, due elementi con `b = 0` (tame) nessuno.
Con `n` elementi ci sono al più `n²/2` coppie × 2 valori ⇒ la probabilità di aver trovato `x` dopo `n` operazioni è
`≤ n²/W`, e quindi `E[costo] ≥ ∫₀^{√W} (1 − n²/W) dn = (2/3)·√W ≈ 0,67·√W`. È un limite per eccesso di ottimismo (coppie con
`|b₁ − b₂| > 1` quasi mai danno un `x` intero nell'intervallo), non una costruzione: nessun algoritmo noto lo raggiunge.

**Dove si usa.** BSGS con `q = 1,0` richiede `T = √W/2` voci (≈ 12 byte l'una): per il puzzle 135 sarebbero `2^66` voci,
fuori da qualunque macchina, e per i puzzle aperti con chiave pubblica (140–160) `2^69…2^79`. Il guadagno (≈ 20 % di tempo rispetto
al canguro, in più un costo *deterministico* senza la coda lunga del canguro) esiste per `W ≲ 2^68` con 128 GB di RAM (un budget tipico di una workstation) ed è
implementato in 1.1.0 (sezione 9). Con poca memoria il minimo noto resta ≈ 1,2–1,3.

### 8.2 L'esponente

* **Una sola istanza, gruppo "scatola nera": `Ω(√W)`** (Nechaev, Shoup). Il conteggio sopra lo mostra per gli intervalli:
  nessun algoritmo che usi solo l'operazione di gruppo, per quanto complesso, può scendere sotto `√W`. Cambiano solo le costanti.
* **Sfruttare la struttura di secp256k1**: per l'ECDLP su campi primi non è noto alcun algoritmo sub-esponenziale (il calcolo
  degli indici, p. es. con i polinomi di sommazione di Semaev, non è competitivo). Le simmetrie della curva (`±P`, e
  l'endomorfismo `λ` di ordine 3 perché `j = 0`) danno fattori costanti: fino a `√6` sul gruppo intero, ma sugli intervalli corti
  resta utile solo `±P` (il mio ragionamento: `λ·e mod n` non cade nell'intervallo).
* **Molte istanze con la stessa ampiezza (precalcolo)**: Bernstein–Lange portano il costo per istanza a `≈ 1,93·W^{1/3}`
  moltiplicazioni dopo un precalcolo di `≈ 1,21·W^{2/3}` e una tabella di `W^{1/3}` voci (misure sperimentali); nel modello
  generico il compromesso `S·T² = Ω̃(ε·N)` (Corrigan-Gibbs–Kogan) è stretto. Il precalcolo ripaga soltanto con `≳ W^{1/6}` istanze
  della stessa ampiezza (≈ `2^22` per il puzzle 135): non è il nostro caso (poche istanze, ampiezze diverse).
* **Più bersagli simultanei**, stessa ampiezza `W`, `m` istanze: con una tabella tame condivisa il costo totale cresce come
  `√(m·W)`, cioè `√(W/m)` a istanza (conteggio dei prodotti `n_T·n_W` come sopra). Per i puzzle aperti, di ampiezze molto diverse, il guadagno è piccolo.
* **Senza chiave pubblica** il problema è un'inversione di `hash160(k·G)`: classicamente `Θ(W)` (costo medio `W/2`), nessun
  esponente migliore; i compromessi tempo–memoria di tipo Hellman costano un precalcolo `≥ W`, utili solo con molti bersagli.
* **Quantistico (solo teoria, oggi non esistono macchine adatte)**: l'algoritmo di Shor risolve l'ECDLP in tempo polinomiale
  in `log n` (Roetteler–Naehrig–Svore–Lauter: `9n + 2⌈log₂ n⌉ + 10` qubit logici, cioè 2330 per `n = 256`); per i logaritmi *brevi*
  (esponente di pochi bit, come nell'intervallo) Ekerå–Håstad riducono ulteriormente la dimensione del circuito. Senza
  chiave pubblica, Grover ridurrebbe `W/2` a `≈ (π/4)·√W` chiamate dell'oracolo (`q ≈ 0,785`), ma ogni chiamata contiene una
  moltiplicazione scalare e due hash reversibili e l'algoritmo si parallelizza male.
* **Debolezze nella generazione delle chiavi** sarebbero un attacco di natura diversa (non un logaritmo discreto). Non ho
  informazioni verificabili in merito e non le considero.

### 8.3 Sintesi

| Obiettivo | Possibile? |
|-----------|------------|
| `q < 1` con poca memoria | non noto; il meglio noto è ≈ 1,15–1,36 (qui ≈ 1,02 con lo schema 3 a grande scala, 8.10) |
| `q ≈ 1,0` con molta RAM | sì (BSGS con `±`, implementato in 1.1.0): `W ≲ 2^68` con 128 GB; meglio del canguro fino a `W ≈ 2^70` |
| esponente `< 1/2` per una singola istanza, classico | no nel modello generico; nessun attacco specifico noto su secp256k1 |
| esponente `1/3` per istanza | sì con precalcolo `W^{2/3}` e molte istanze della stessa ampiezza |
| tempo polinomiale | solo con un computer quantistico fault-tolerant |

Fonti aggiuntive: Bernstein–Lange, *Computing small discrete logarithms faster* (<https://eprint.iacr.org/2012/458>);
Corrigan-Gibbs–Kogan, *The Discrete-Logarithm Problem with Preprocessing* (<https://eprint.iacr.org/2017/1113>);
Roetteler et al., *Quantum resource estimates for computing elliptic curve discrete logarithms* (<https://eprint.iacr.org/2017/598>);
Ekerå–Håstad, *Quantum algorithms for computing short discrete logarithms and factoring RSA integers* (<https://eprint.iacr.org/2017/077>).

### 8.4 Costo di un passo e colli di bottiglia dell'hardware (1.5.0)

Fissato l'algoritmo (costante ≈ 1,15 dalla 1.6.0 e esponente 1/2, sezioni 8.1–8.2), resta il costo di **un passo** e quanti passi al secondo
l'hardware riesce davvero a fare.

**Il minimo aritmetico.** Un passo è una somma affine `P + S_j`: pendenza `λ = (y − y_j)/(x − x_j)`, `x₃ = λ² − x − x_j`,
`y₃ = λ(x − x₃) − y`, cioè 2M + 1S più un'inversione. Con il trucco di Montgomery su un lotto di `n` camminatori l'inversione costa
3M a elemento più `≈ 270/n` M (una sola inversione vera per lotto). Totale ≈ **5M + 1S + 270/n M**, più due normalizzazioni
(la scelta del salto e la rappresentante `±` richiedono `x` e `y` canonici). Le coordinate proiettive non aiutano: per scegliere
il salto serve `x` canonico, cioè di nuovo un'inversione. Non conosco modi per fare meno operazioni di campo per passo.

**CPU (AVX-512 IFMA).** Misurato: un prodotto di campo costa 2,7 ns per corsia (throughput, 8 corsie per
istruzione), un quadrato 2,5, una normalizzazione 0,9. Il minimo sopra vale ≈ 17 ns per passo di sola aritmetica, ≈ 28 ns con
normalizzazioni e sottrazioni. Il passo misurato è passato da 52 a **40 ns** con: contabilità tutta vettoriale (distanze a 192 bit
con riporti vettoriali, segni, etichette, contatori: 10 → 1 ns), lettura della tabella dei salti con un caricamento da 64 byte per
corsia e trasposizione 8×8 nei registri (i *gather* hardware erano più lenti e la costruzione per elementi causava stalli di
store-forwarding: −8 ns), passaggio all'indietro fuso, negazione diretta `p − y`, 4096 camminatori per thread (inversione su 512
passi per corsia; oltre non migliora). Restano ≈ 12 ns sopra il minimo: carichi/scritture dello stato (≈ 200 byte per passo in
L2) e la catena di prodotti del lotto. Spezzarla in due catene indipendenti non ha dato guadagni misurabili (l'esecuzione fuori
ordine già sovrappone le latenze). **Il canguro CPU è ora limitato dall'unità IFMA**, cioè dal calcolo.

**Due vettori alla volta (1.7.7).** Nella seconda metà del passo un vettore è una catena di circa mille istruzioni dipendenti (prodotto,
quadrato, sottrazioni, prodotto, normalizzazioni, scelta del rappresentante `±`), più lunga della finestra di esecuzione fuori ordine
(352 posti nel Tiger Lake): il vettore seguente non può cominciare finché questo non è quasi finito e le unità aspettano le latenze. Il
modello di `llvm-mca` (icelake-client) dà ≈ 550 cicli per vettore contro ≈ 260 di sola capacità di calcolo, e il passo misurato (40 ns contro
≈ 28 ns di aritmetica) è coerente con questa lettura, ma resta **un'ipotesi: non ho una CPU con IFMA su cui misurare**. Perciò c'è una seconda
forma del passo, `kang_round_avx512i2` (`KV_PAIR` in `kang_vec_impl.inc`), che esegue ogni fase della seconda metà del passo per due vettori
affiancati (due prodotti indipendenti uno accanto all'altro riempiono i vuoti), con gli stessi calcoli e gli stessi eventi nello stesso ordine.
Il programma cronometra le due forme al primo uso con **tutte le CPU logiche occupate** (con il thread gemello nello stesso core i vuoti sono
già riempiti e il vantaggio svanisce) e usa quella a coppie solo se vince di almeno il 3 %; `PZ_KANG_ROUND=avx512i` o `avx512i2` la forzano e
`bench` le elenca. Sul percorso AVX-512 senza IFMA e su AVX2 la stessa idea non ha dato nulla (9,7–10,7 contro 10,2–10,6 M passi/s: i registri
vettoriali non bastano per tenere due catene e il compilatore aggiunge spostamenti), quindi lì non c'è. Correttezza: con l'emulazione delle
due istruzioni IFMA (`tools/ifma_emul.h`, AVX-512F) sia il passo a coppie sia quello a corsie sia il motore `avx512i` risultano identici al
riferimento scalare; `tools/verify.sh` lo ripete a ogni rilascio su qualunque CPU con AVX-512.

**GPU.** *Misura della 1.6.0 su una GPU NVIDIA da portatile: 486 M passi/s con il kernel v1* (taratura: NK 64/128/256 → 490/487/476 M; le
varianti con prelettura sono uguali, quelle a 1024 thread per SM molto peggiori, 300 M: il kernel non è limitato
dall'occupazione ma dal traffico degli array privati, che con 512 thread per SM sono ≈ 11 MB e stanno in memoria video). Con
≈ 400 byte per passo il conto dà 486 M × 400 B ≈ 194 GB/s, cioè **tutta la banda della scheda**: il kernel v1 era limitato dalla
memoria al 100 %. Di qui il v2 (sez. 5): per passo si leggono e scrivono x e y (128 B), i prodotti del lotto (64 B), i tag e i flag
(≈ 40 B) e si aggiunge 1 B di storia: ≈ **230 byte** (stima per costruzione, non misurata), quindi limite di banda ≈ 830 M passi/s.
Conto delle istruzioni (sm_86, da `ptxas`, parti misurate separatamente): moltiplicazione ≈ 250, quadrato ≈ 200, sottrazione ≈ 40,
normalizzazione ≈ 30; il passo vale ≈ 2000 istruzioni più la quota d'inversione, che con la potenza `p − 2` (≈ 55 000
istruzioni) era ≈ 430 per passo a NK = 128 (un quinto del totale) e col divstep (≈ 10 500) è ≈ 80. Al throughput intero osservato sul
kernel lineare (≈ 2,5·10¹² istruzioni/s) il limite di calcolo passa da ≈ 1,0 a ≈ 1,2 G passi/s: **dopo il v2 il kernel dovrebbe
essere limitato ancora dalla banda, ma vicino al punto di incrocio**. Stime: vanno confermate con `bench` sulla scheda.
* **Misura della 1.7.0 sulla stessa scheda: 810,7 M passi/s** (NK 64, prodotti a coppie), ×1,67 sul v1; il conto per costruzione
  (≈ 200 byte per passo con le coppie, ≈ 830 M passi/s di limite a 192 GB/s) era giusto entro il 3 %. Con la banda realmente
  ottenibile (≈ 85 % del picco) e ≈ 2200 istruzioni per passo (a NK = 64: 5 prodotti ≈ 1250, quadrato ≈ 200, sottrazioni ≈ 320,
  inversione ≈ 160, mezzo prodotto in più ≈ 125, resto ≈ 160), il kernel è a ≈ 1,8·10¹² operazioni intere al secondo, cioè
  verosimilmente **vicino a entrambi i limiti della scheda** (picco intero ≈ 20 SM × 64 × clock ≈ 1,5–2,2·10¹² a seconda del clock
  sotto carico; il kernel lineare, ≈ 3300 istruzioni per chiave a 663 M chiavi/s, ne fa ≈ 2,2·10¹²). Margine residuo su questa scheda
  stimato in ≤ 15–20 %; per saperlo con certezza servono la banda ottenibile e il clock sotto carico (`nvidia-smi`).
* **Come misurare i due tetti (1.7.1).** `bench` stampa per ogni GPU la banda di copia (stesso schema di traffico dello stato:
  lettura e scrittura coalescenti) e il throughput di `mad.lo`, `mad.hi` e somma a 3 ingressi su 8 catene indipendenti per thread,
  con griglia multipla di ogni numero di thread residenti (nessuna ultima ondata parziale). Dividendo la banda per ≈ 215 byte per
  passo e il ritmo di `mad.lo` per ≈ 2200 istruzioni si hanno i tetti del modello; il canguro misurato, diviso per il più basso, dice
  quanto margine resta. Per questa scheda ci si aspetta una banda di copia di ≈ 85–90 % del picco (≈ 165–175 GB/s su 192) e un
  `mad.lo` vicino a 64 per SM per ciclo al clock reale: se il canguro è già al 90 % del più basso dei due, il passo successivo non è
  più "togliere traffico" o "togliere istruzioni" da soli, ma farlo con entrambi insieme (un kernel con il branco in memoria
  condivisa toglierebbe traffico aggiungendo qualche istruzione per l'inversione cooperativa nel blocco).
* **Perché il branco in memoria condivisa o in L2 non conviene su questa scheda (1.7.1).** Idea (stile RCKangaroo): tenere lo stato
  dei camminatori sul chip e togliere il traffico di memoria (resterebbero le ≈ 2200 istruzioni per passo, tetto ≈ 1 G passi/s).
  Capacità: ogni camminatore occupa ≈ 130 byte (x, y, prodotti, tag, flag); la memoria condivisa è ≈ 100 KiB per SM, cioè ≈ 800
  camminatori per SM, ≈ 1,5 per thread con 512 thread residenti; la L2 di una GA107 è di 2 MiB, ≈ 15 000 camminatori in tutto
  (il branco attuale ne ha 850 000 e 85 MiB). L'inversione a lotto, per ammortizzarsi, vuole almeno ≈ 48 camminatori per
  inversione (a 10 500 istruzioni l'una); con lotti su più thread l'inversione, che è una catena seriale di divstep, la eseguirebbe un
  solo thread per blocco: costo relativo ≈ 153/W (W = camminatori del blocco), cioè ≈ 60 % con W = 256 e ≈ 15 % con W = 1000, contro il
  ≈ 7 % di oggi. In breve: la riduzione del traffico c'è solo se il branco sta nella cache, e per starci servono poche migliaia di
  camminatori per SM, troppo pochi per nascondere le latenze e per ammortizzare l'inversione. Funziona su GPU con L2 molto grande
  (RTX 4090: 72 MiB, RTX 4070: 36 MiB) dove un branco di qualche centinaio di migliaia di camminatori sta in L2; su una scheda con 2 MiB
  no. `bench` stampa ora la L2 e il rapporto con il branco per verificarlo su ogni scheda. Dalla 1.7.1 la taratura prova perciò, solo dove la L2
  è di almeno 8 MiB, un branco ridotto per stare in mezza L2 (meno work-item, NK 16/32/64); la memoria condivisa non serve a questo
  (è più piccola della L2 su tutte le schede) e un kernel con inversione cooperativa nel blocco resta non realizzato. Il traffico minimo di un passo in streaming è
  x e y letti e scritti (128 byte) più prodotti e metadati: ≈ 200 byte, di cui solo ≈ 4 (la copia `prev2` scritta a ogni passo) sono
  eliminabili, e non in modo sicuro (nel percorso di uscita anticipata serve il valore dell'ultimo passo).
* *Batch nel warp con gli shuffle* (tenere i camminatori nei registri e fare il trucco di Montgomery tra i 32 thread di un warp):
  scartato in teoria. In SIMT l'inversione del lotto la eseguono tutti i 32 thread, quindi ogni thread la ammortizza solo sui
  pochi camminatori che tiene nei registri (2–4): ≈ 76 moltiplicazioni per passo contro ≈ 11. Il lotto per thread in memoria
  privata è la scelta giusta per le GPU.
* *Stato più compatto*: fatto nel v2 (etichette a 32 bit, un solo ultimo-DP per camminatore, distanza fuori dal ciclo caldo).
* *Prodotti del lotto solo ogni 2 o 4 camminatori* (gli intermedi si ricalcolano dai denominatori nel blocco): con blocchi di 2
  si tolgono 32 B su ≈ 230 al prezzo di 0,5 moltiplicazioni per passo (≈ +6 % di istruzioni) e il kernel resta a 128 registri
  senza spill: **realizzato come variante** ("prodotti a coppie", `PZ_GPU_NK=128b`) e lasciato alla taratura. Con blocchi di 4
  (−48 B, +0,75 moltiplicazioni) `ptxas` spilla 344 byte: scartato. Senza una scheda su cui misurare non so se guadagna.

**BSGS.** Dopo la vettorizzazione delle finestre (sez. 9.2) un passo gigante costa ≈ 25 ns di aritmetica e ≈ 55 ns di lettura
casuale della tabella (thread singolo, tabella di 64 MB). Per tabelle grandi decide la latenza della RAM: pagine grandi (TLB) e
prefetch servono; un filtro in cache non aiuta quando la tabella stessa è di decine di GB, e un ordinamento dei sondaggi per
indirizzo darebbe località solo con lotti paragonabili alla tabella. Il BSGS comunque non si applica ai puzzle aperti (8.1).

**Ordini di grandezza per il puzzle 140** (1,06·10²¹ passi in media, mediana ≈ 0,99·10²¹): a 700 M passi/s (ordine di grandezza di un portatile
con GPU limitata dalla banda, CPU IFMA e GPU integrata) sono ≈ 48 000 anni in media; la probabilità di riuscire in un anno è
≈ `(n/c)² ≈ 3·10⁻¹⁰` (modello della sezione 11). Ogni raddoppio di velocità dimezza il tempo, ma l'ordine di grandezza resta.

### 8.5 Confronto con lo stato dell'arte: il puzzle 135 e RCKangaroo (1.6.0)

Il puzzle 135 (`W = 2^134`) è stato risolto; si dice con circa 300 GPU per 5 mesi (dato riportato, non verificato qui). Conti:

* passi attesi con `K = 1,15`: `1,15·2^67 ≈ 1,7·10²⁰`;
* 300 RTX 4090 con il kernel più veloce pubblico (RCKangaroo: 8 G passi/s dichiarati nelle prime versioni, 14,5 nelle ultime)
  per 1,3·10⁷ s: `3,1–5,7·10¹⁹` passi, cioè **il 18–33 % dello sforzo atteso**;
* nel modello della sezione 11 la probabilità di finire entro quella frazione è ≈ 3–8 %: è andata bene, oppure le risorse
  erano più grandi di quanto riportato. In ogni caso i conti tornano solo con GPU da 8–14 G passi/s.

Cosa ne segue per questo programma:

1. **Costante**: 1,26 fino alla 1.5.0 contro 1,15 di RCKangaroo. Studiato il loro metodo e rimodellato nel simulatore, la
   differenza era nella disposizione dei camminatori; adottata (sezione 4.2): ora ≈ 1,1–1,2.
2. **Cicli infruttuosi** (sezione 4.3): RCKangaroo li gestisce esplicitamente (cicli di 2 con una seconda tabella di salti, cicli
   più lunghi ricostruiti in un kernel a parte). Qui mancava la gestione dei cicli di 4 e oltre, e con `dp_bits` grandi i
   camminatori si bloccavano: corretto nella 1.6.0. **Era il difetto più grave**: senza la correzione una campagna sul 140 non
   avrebbe mai trovato la chiave.
3. **Velocità del passo su GPU** (testo scritto per il v1; il v2 della 1.7.0 ne riprende i punti principali): è il divario più grande. RCKangaroo fa ≈ 14,5 G passi/s su una 4090 (128 SM a 2,5 GHz). In
   proporzione alla capacità di calcolo intero, su una scheda da portatile di fascia bassa (20 SM, ≈ 1,4 GHz) sarebbero ≈ 1–1,3 G passi/s; il kernel v1 faceva
   0,21 (p140) e 0,49 G/s misurati dopo la 1.6.0. Le differenze principali del loro kernel: stato di 24 camminatori per thread tenuto nella cache L2
   (su una 4090, 72 MB), nessuna distanza a 192 bit nel ciclo (si scrive solo l'indice del salto, 2 byte, e le distanze si
   ricostruiscono in un secondo kernel), un'inversione veloce (algoritmo di tipo GCD binario invece della potenza `p − 2`) e
   aritmetica a 64 bit in assembly PTX. ≈ 70–230 byte per passo, quasi tutti in L2, contro ≈ 330–400 byte qui in memoria video.
   Una scheda da portatile di fascia bassa ha solo 2 MB di L2, quindi su quella di prova il guadagno possibile è più vicino a 3× che a 5×.

### 8.6 Moltiplicazione modulare: cosa si è provato (1.7.0)

Domanda: esistono algoritmi di moltiplicazione modulare a 256 bit più efficienti di quello usato? Misure statiche (istruzioni SASS
per sm_86 prodotte da `ptxas`, includono ≈ 30 istruzioni di caricamento/scrittura del banco di prova), confronto a parità di
funzione:

| operazione | 8 limbi da 32 bit (in uso) | 9 limbi da 29 bit, somme di colonna a 64 bit |
|---|---|---|
| moltiplicazione | 288 | 296 (368 forzando `mad.wide` con asm) |
| quadrato | 232 | 256 (296) |
| sottrazione | 72 | 80 |
| normalizzazione canonica | 64 | 168 |

* **Limbi da 29 bit con accumulo a 64 bit** (la rappresentazione della CPU AVX2/`fev.h`, portata sulla GPU): non conviene. Il
  conteggio indica che la somma di colonna non si riduce a una `IMAD.WIDE` per prodotto (non ho un disassemblatore per guardare il
  codice generato); forzandola con `mad.wide.u32` il codice peggiora. Il prodotto a 32 bit
  con catena di riporti costa già ≈ 3,9 istruzioni per prodotto di limbi. Scartato.
* **Karatsuba a un livello: provato (1.7.7).** 3 prodotti 4×4 invece di 1 da 8×8 (48 prodotti di limbi invece di 64), nelle due forme
  (additiva, con somme di 129 bit corrette con maschere; sottrattiva, con `|a1 − a0|·|b1 − b0|` e un segno), verificate sul computer
  con 2 milioni di prodotti (`tools/sass/kara_check.c`) e contate con NVRTC 12.0, `ptxas` e `nvdisasm` per sm_86
  (`tools/sass_count.py`, kernel in `tools/sass/`). Il banco di prova costa 96 istruzioni di caricamento e scrittura, tolte dalle cifre:

  | prodotto 8×8 limbi, senza riduzione | istruzioni |
  |---|---|
  | scolastico (in uso) | 184 |
  | Karatsuba additivo | 248 (+35 %) |
  | Karatsuba sottrattivo | 256 (+39 %) |
  | per colonne, una `IMAD.WIDE` e due somme per prodotto | 232 (+26 %) |
  | per righe, una catena di riporti per riga | 176 (−4 %) |

  Il codice generato per il prodotto scolastico ha 64 `IMAD.WIDE.U32` (più 24 che calcolano indirizzi nel banco di prova), 86 somme
  `IADD3`/`IADD3.X` e 13 `IMAD.X`: ≈ 2,9 istruzioni per prodotto di limbi. Karatsuba risparmia 16 prodotti (≈ 48 istruzioni) ma le somme
  dei semi-operandi, le correzioni del riporto, le due sottrazioni di `z0` e `z2` e la ricomposizione costano più di quanto tolgono. **Perde.**
  Il prodotto per righe (un solo `IADD3.X` per prodotto, con il limbo della riga precedente come addendo a 64 bit) toglie il 4 % del prodotto
  nudo ma non cambia la moltiplicazione completa con la riduzione (328 istruzioni, uguali): ne restano 58 `MOV` che azzerano la metà alta
  dell'addendo, perché `IMAD.WIDE.U32` vuole una coppia di registri. Lo stesso schema nel quadrato dà −15 % (232 invece di 256 istruzioni
  del banco, 136 contro 160 al netto), cioè ≈ −1 % del passo (c'è un solo quadrato per passo): sotto il 2 % chiesto, non adottato.
  **CPU con IFMA**: Karatsuba non ci sta nemmeno in linea di principio, perché i moltiplicatori leggono solo 52 bit per operando e la somma di due
  limbi di 52 bit ne ha 53; con limbi da 51 bit i 256 bit non stanno in 5 limbi (ne servirebbero 6: 36 prodotti invece di 25).
* **Numero di moltiplicazioni per passo**: 5 prodotti e 1 quadrato (2 per l'inverso del lotto, 1 per la pendenza, 1 per `y`, 1 per
  il prodotto corrente; il quadrato per `x`). Non conosco forme che ne richiedano meno per un passo affine con inversione a lotti:
  la coordinata `y` serve per scegliere il rappresentante `±` e per il passo dopo.
* **Inversione**: qui il guadagno c'era, ed è realizzato (divstep, ≈ 5× meno istruzioni dell'esponenziazione).
* **Notizie recenti.** Il 23 settembre 2026 OpenAI ha pubblicato il preprint "Integer multiplication below n log n" e una comunità
  guidata da D. Colkitt (repository CrocSwap/integer-mult-bounds, con contributi di molte persone e agenti, fra cui Rohan Arun)
  ne migliora un risultato *condizionale*: la complessità `O(n (log n)^(1−κ))` con κ ≈ 0,0023 (altri candidati fino a 0,003).
  È un risultato **asintotico**: il repository stesso dice che i costrutti sono "astronomicamente grandi", non materializzati, e che
  "non si dichiara alcun miglioramento pratico di velocità". Per numeri di 256 bit (8 limbi) non ha alcuna rilevanza: lì vince il
  prodotto scolastico. Sono risultati dello stesso filone gli schemi per moltiplicare *matrici* trovati da ricerca automatica
  (AlphaTensor, AlphaEvolve: 48 prodotti per matrici complesse 4×4), anch'essi inutilizzabili per un prodotto scalare modulare.
  (Il testo del post su X non ho potuto leggerlo, il sito risponde 402 agli accessi automatici: quanto sopra viene dal repository.)

L'unica leva rimasta sul lato calcolo è quindi la quota di moltiplicazioni nel passo; quella grossa è il traffico di memoria (8.4).

### 8.7 Quanto si può scendere ancora: il conteggio delle coppie con valori interi (1.7.7)

La sezione 8.1 dava un limite per conteggio di `0,67·√W`, ottenuto supponendo che **ogni** coppia di elementi certifichi due valori di `x`.
Rifatto con più cura (`tools/bounds.py` ripete i conti):

* Gli elementi sono classi `{E, −E}` con `E = b·x + a` (`b = 0` tame, `b = 1` wild, `b = 2, 3, …` camminatori su `2x`, `3x`, …). Due elementi
  collidono in due modi, `E₁ = E₂` e `E₁ = −E₂`, e danno `x = (a₂ − a₁)/(b₁ − b₂)` e `x = −(a₁ + a₂)/(b₁ + b₂)`: ciascuno è un intero solo con
  probabilità `1/|differenza|`, a meno di scegliere gli scostamenti `a` perché lo sia.
* Tame–wild: due valori, sempre interi. Wild–wild: uno, intero se gli scostamenti sono pari (il trucco «distanza pari» del metodo di
  sezione 4.2). Tame–tame: nessuno. Con coefficienti 2 o più le coppie valgono in media `1/2`, `1 + 1/3`, … e **non c'è congruenza che le
  renda tutte intere**: un tame con scostamenti pari vedrebbe solo `x` pari.
* Con frazione tame `α` e il resto wild: copertura `c·n²/W` con `c = 2α(1 − α) + (1 − α)²/2`, massima per `α = 1/3` con `c = 2/3`: **la
  frazione che il programma usa dal 1.6.0** (non è una scelta fortuita: è l'ottimo esatto). Aggiungendo specie con coefficiente 2…5 l'ottimo
  numerico resta `(1/3, 2/3, 0, …)`; senza il trucco della distanza pari scende a `c = 0,572` con un 5 % di specie `2x` (coerente con la
  prova di 4.6: la terza specie costava il 6 % in più).
* Due modi di avere la copertura. **Strutturata** (insiemi senza sovrapposizioni, come nel BSGS): `E = (2/3)/√c`, cioè **`0,8165·√W`** per
  `c = 2/3` (0,882 senza il trucco). **Casuale** (coppie indipendenti, Poisson): `E = (√π/2)/√c`, cioè **`1,0854·√W`** (con `c = 1/2`, solo
  tame–wild, dà `√(π/2) = 1,2533`, il valore del canguro classico con `±`).
* Il `0,67` di 8.1 è il caso in cui ogni coppia di specie diverse valga sempre 2, con infinite specie: `c → 1`. Con valori interi quella
  situazione non esiste, e l'ottimo del modello è `0,8165`. Non è un limite inferiore dimostrato per ogni algoritmo (resta `Ω(√W)` con costante
  ignota), ma è il meglio che questa famiglia di costruzioni (tame e wild `±1`, anche con più specie) può dare.

**Dove siamo.** `tools/ksim.c` (modello astratto, `rck=1 tf=0.3333 jabs=12`, `W = 2^34`, 1500 prove per punto, errore ≈ ±0,02; non conta il costo di
piazzare i camminatori, che per `K ≪ √W` è trascurabile) misura il costo medio in unità di `√W` al variare dei camminatori `K` e dei bit dei punti distinti:

| camminatori `K` | `dp = 0` (ogni punto è distinto) | `dp = 2` | `dp = 4` |
|---|---|---|---|
| 64 | 1,166 | 1,229 | 1,248 |
| 256 | 1,129 | 1,148 | 1,194 |
| 1024 | 1,108 | 1,150 | 1,248 |
| 4096 | 1,098 | 1,198 | 1,452 |

Con `dp = 0` il costo scende verso **1,085 al crescere di `K`**: il caso casuale ideale è davvero il limite di questo metodo. Il sovraccosto dei punti
distinti è ≈ `1–2 · K·2^dp / √W` (un'intesa trovata va riconosciuta solo al punto distinto seguente, e nel frattempo tutti i `K` camminatori
avanzano di `2^dp` passi). Alle scale dei puzzle aperti il rapporto è minuscolo: `K ≈ 2^20`, `dp ≈ 40` (la tabella deve stare in RAM), `√W = 2^69,5`
danno `2^−9,5` ≈ 0,1 %. **Il costo atteso alle scale reali è quindi ≈ 1,09, non 1,15** (*corretto in 8.10: vale solo con camminate corte o con
moltissimi camminatori; con una macchina sul puzzle 135 e i salti di `12·√W` le camminate restano locali e il costo sale a ≈ 1,35*): il 1,15 di 4.5 viene da misure con 64–2048 camminatori su
intervalli piccoli, dove il sovraccosto e la lunghezza delle camminate pesano di più. Il resto del divario con 1,085 viene dalla sovrapposizione
non uniforme delle zone (per la disuguaglianza di Jensen una copertura non uniforme costa di più). Rispetto alla copertura strutturata (0,8165)
mancano ancora il 33 %, che non si recupera senza memoria `∝ √W`.

**Perché la copertura strutturata non c'è.** Richiede di tenere in memoria gli elementi per confrontarli con quelli nuovi, cioè memoria
proporzionale a `√W`: per `W = 2^139` sono `2^69` voci. Con la memoria che c'è si può solo seguire camminate casuali e registrare punti
distinti, quindi la copertura è casuale e il valore da battere è 1,085. Dove la memoria basta (`W ≲ 2^68`) c'è il BSGS (sezione 9): tabella
prima, passi giganti poi, costo `1,0·√W` con `√W/2` voci. Calcolo nuovo: **intercalare** le due fasi, conservando anche i giganti, porta
a `0,9428·√W` (nel modello a coppie con `±`) ma solo con memoria totale `M ≳ 1,41·√W`; con `M = √W` è 0,958, con `M ≈ 0,7·√W` è peggiore del BSGS
semplice (1,046 contro 1,000). Il guadagno massimo è quindi il 5,7 % a patto di avere più del doppio della memoria del BSGS ottimo;
non realizzato: la modalità `bsgs` è comunque inutilizzabile per i puzzle aperti.

**Cosa resta.** Tra 1,15 e 1,085 c'è al più un 6 % da recuperare con le camminate casuali (meno fusioni inutili, punti distinti migliori).
L'affermazione «nessuna congruenza rende intere tutte le coppie» vale coppia per coppia, ma non impedisce di scegliere *per quali* `x` ogni
coppia è intera: la sezione 8.8 lo fa e scende a 1,045 (schema 2, dalla 1.7.8). Il limite assoluto per qualunque algoritmo generico è `Ω(√W)`.


### 8.8 Sotto 1,085: specie con residui fissi (schema 2, dalla 1.7.8)

In 8.7 una coppia di camminatori con coefficienti `b ≠ b'` dava un `x` intero solo con probabilità `1/|b − b'|`, e nessuna congruenza
rendeva intere tutte le coppie insieme. Vero coppia per coppia; ma si può **scegliere per quali `x` ogni coppia è intera** e far sì che,
per ogni `x`, ce ne siano abbastanza. È lo schema delle campagne nuove dalla 1.7.8.

* Ogni camminatore ha `E = b·x + u` con `u ≡ r (mod 12)` e **tutti i salti multipli di 12**: il residuo `r` non cambia mai (il cambio di
  rappresentante `E → −E` porta `(b, r)` in `(−b, −r)`, cioè nella stessa specie vista dall'altra parte). La partenza `u` è uniforme nella
  zona della specie e arrotondata in basso al residuo.
* Due specie `(b, r)` e `(b', r')` possono cadere sulla stessa classe solo se `r + (b − b')·x ≡ r' (mod 12)` (oppure la condizione analoga
  per `E = −E'`). Quando la condizione vale, gli esponenti dei due gruppi stanno sullo stesso reticolo di passo 12 e si incontrano 12 volte
  più spesso; quando non vale non si incontrano mai. Il fattore della coppia passa da `1/|b − b'|` a `12/|b − b'|` sui residui di `x` buoni
  e a 0 sugli altri: con le specie giuste ogni residuo di `x` riceve più coppie utili di prima.
* `tools/kspecies.py` calcola la copertura esatta (geometria delle zone, i due modi di collidere, la penalità di Jensen) e cerca le specie:
  prima enumera le assegnazioni di classi di residui alle specie, poi ottimizza le quote. Il migliore per `b ∈ {0, 1, 3}` (camminatori
  domati, selvatici su `Q`, selvatici su `3Q`):

| specie | residui mod 12 | zona di `u` (in unità di `W`) | quota |
|---|---|---|---|
| domati, `E = u` | 0, 3, 6, 9 (quote 4,2 %, 9,4 %, 4,2 %, 11,6 %) | `[−0,023, 0,023]` | 29,4 % |
| selvatici su `Q`, `E = x + u` | 3, 7, 11 | `[−1,005, 0,005]` | 34,9 % |
| selvatici su `3Q`, `E = 3x + u` | 9 | `[−3,005, 0,005]` | 35,8 % |

  Costo nel modello di copertura casuale: **1,045·√W**, contro 1,085 dell'ottimo senza residui (la disposizione della 1.6.0). Altre famiglie
  (`b ∈ {0, 1, 3, 5}` mod 60, `{0, 1, 3, 9}` mod 36) danno lo stesso valore nel modello ideale senza geometria (0,991); coefficienti 2, 5, 9
  non aiutano. Il valore 0,991 è il pavimento di questa idea; le zone che si sovrappongono in modo non uniforme portano a 1,045.
* **Modello delle camminate** (`tools/ksim.c design=tools/kang_schema2.txt jabs=12`, salti in `[2/3, 4/3]` di `12·√W` arrotondati a multipli di
  12; `W = 2^36`, `dp = 0`, 5000 prove per punto, errore ≈ ±0,008):

| camminatori `K` | schema 1 (1.6.0–1.7.7) | schema 2 | differenza |
|---|---|---|---|
| 256 | 1,127 | 1,091 | −3,2 % |
| 1024 | 1,127 | 1,053 | −6,6 % |
| 4096 | 1,092 | **1,044** | −4,4 % |

  Con molti camminatori e il ritardo dei punti distinti trascurabile (il regime dei puzzle aperti, 8.7, *ma vedi la correzione di 8.10*) lo schema 2 arriva al suo valore
  teorico 1,045 come lo schema 1 arrivava al suo 1,085–1,09: **circa il 4 % di passi in meno**. Con pochi camminatori e punti distinti fitti
  il vantaggio si riduce (`K = 256`, `dp = 2`, `W = 2^32`: 1,127 contro 1,161) e con 64 camminatori si inverte (`W = 2^28`, `dp = 2`: 1,276
  contro 1,214): con pochi camminatori per specie la copertura di ogni residuo di `x` è troppo irregolare. Per questo una campagna nuova usa lo
  schema 2 solo con almeno 256 camminatori (sempre, sui puzzle aperti).
* **Sulla curva vera** (`pzsolve kstat`, 1 thread, quindi `K ≈ √W/256` e `dp` automatici; `PZ_KANG_SCHEMA=1|2` sceglie lo schema):

| W | camminatori | prove | schema 1 | schema 2 |
|---|---|---|---|---|
| 2^28 | 64 | 4000 | 1,222 | 1,307 (peggio: sotto 256 camminatori si usa lo schema 1) |
| 2^32 | 256 | 3000 | 1,151 | 1,134 |
| 2^36 | 1024 | 2000 | 1,170 | 1,095 |
| 2^40 | 4096 | 600 | 1,146 | 1,099 |
| 2^44 | 16384 (4 thread) | 400 | 1,188 | 1,071 |

  Errore standard ≈ 0,01 per le righe con 2000–4000 prove, 0,02–0,03 per le altre. Da `2^32` a `2^44` la media scende da **1,164 a 1,100·√W**
  (−5,5 %), coerente con il modello; il programma riproduce il modello anche nei dettagli (`W = 2^32`, 256 camminatori, `dp = 2`: 1,134 sulla
  curva, 1,127 nel modello). Il vantaggio cresce con i camminatori; alle scale dei puzzle aperti il valore atteso è quello del modello, ≈ 1,045 (*con camminate corte: vedi 8.10*).

* **Costo per passo: nessuno.** Il passo di CPU e GPU è lo stesso; cambiano solo la tabella dei salti (calcolata una volta), le partenze
  (`u·G + b·Q'`, con `3·Q'` calcolato una volta) e la soluzione: una coppia con codici di tipo `k₁, k₂` dà `(c₁ − c₂)·x = d₂ − d₁` con
  `c ∈ {0, +1, −1, +3, −3}`, divisione esatta per `|c₁ − c₂| ∈ {1, 2, 3, 4, 6}`. Il passo scrive ancora il codice a 2 bit di sempre (domato o
  selvatico col suo segno) e l'host lo trasforma in quello a 3 bit della specie, per cui il kernel GPU non è cambiato. L'etichetta dell'ascissa
  nella tabella scende da 62 a 61 bit (falsi positivi comunque verificati).
* **Compatibilità.** Lo schema è deciso alla creazione della campagna: un `dp.bin` in formato 1 o 2 (campagne della 1.7.7 e precedenti) la
  tiene sullo schema 1, che resta identico; una campagna nuova usa lo schema 2 e il formato 3 del registro (`"PZDP3"`, base = inizio,
  impronta con `"PZDPLOG3"`). I punti dei due schemi non si incontrano: `merge` rifiuta un registro dell'altro schema spiegando il perché, e per
  aggiungere un computer a una campagna dello schema 1 lo si avvia con `PZ_KANG_SCHEMA=1`.

**Cosa resta.** Fra 1,045 e la copertura strutturata 0,8165 c'è ancora il 22 %, che senza memoria `∝ √W` non sembra recuperabile con
camminate casuali; il pavimento dell'idea dei residui (0,991, senza perdite di geometria) indica che resterebbero al più il 5 % da zone migliori.


### 8.9 Più RAM o un precalcolo: cosa si può comprare (analisi, dopo la 1.7.8)

Domanda: con un algoritmo più complesso, più RAM o un precalcolo tenuto in memoria, si può abbassare di molto il fattore davanti a √W?
Risposta breve: **per una sola chiave no**. La memoria abbassa il fattore solo quando è dell'ordine di √W, e un precalcolo rende veloce la ricerca
successiva solo spostando il costo, accresciuto, nel precalcolo stesso.

**1. Il limite per una chiave.** Nel modello generico ogni algoritmo, con qualunque quantità di memoria, deve calcolare `Ω(√W)` elementi
(Shoup, Nechaev; 8.1–8.2). La memoria può cambiare solo la costante:

| metodo | fattore medio | memoria |
|---|---|---|
| canguro, schema 3 (8.10) | ≈ 1,02 (modello, puzzle aperti) | la tabella dei punti distinti: trascurabile |
| BSGS con `±`, `T = √W/2` | 1,0 | `√W/2` voci |
| BSGS intercalato (8.7) | 0,943 | `≳ 1,41·√W` voci |
| copertura strutturata, tame e wild `±1` (8.7) | 0,8165 | `∝ √W` |
| conteggio delle coppie (8.1) | ≥ 0,67 | qualsiasi |

Anche con memoria illimitata il guadagno massimo noto è quindi dal 10 al 20 % circa, e richiede `√W` voci. Per il puzzle 135 sono `2^66` voci, cioè
`≈ 2·10^21` byte con 32 byte a voce. Con memoria `M ≪ √W` il guadagno è dell'ordine di `M/√W` (mio ragionamento: una tabella di `M` punti fissi
aggiunge `n·M` coppie utili alle `n²` delle camminate). Con 64 GiB, `M ≈ 2^31` e `n ≈ 2^67`: `2^−36`, cioè nulla.

**2. Precalcolo (Bernstein–Lange).** Si fanno partire molte camminate «domate» che non dipendono dalla chiave, ognuna fino a un punto
distinto, e si tengono in una tabella i `T` punti distinti raggiunti dal maggior numero di camminate. Poi, per una chiave, basta camminare da
`Q + r·G` fino a un punto distinto e sperare che sia in tabella. La tabella vale per **ogni** intervallo della stessa ampiezza (basta
traslare la chiave pubblica). Misure nel modello astratto (`tools/blsim.c`, canguro senza classi `±`, 200–400 chiavi per riga):

| W | T | camminate di precalcolo | precalcolo | ricerca online | prodotto |
|---|---|---|---|---|---|
| 2^32 | 2^10 | `T`, corte | 0,48·√(W·T) | 5,33·√(W/T) | 2,57·W |
| 2^36 | 2^12 | `T`, corte | 0,49·√(W·T) | 5,19·√(W/T) | 2,57·W |
| 2^40 | 2^14 | `T`, corte | 0,50·√(W·T) | 4,89·√(W/T) | 2,46·W |
| 2^32 | 2^10 | `16·T` | 16,5·√(W·T) | 2,13·√(W/T) | 35·W |
| 2^40 | 2^14 | `16·T` | 16,0·√(W·T) | 2,07·√(W/T) | 33·W |

  (Bernstein–Lange, con una scelta più accurata delle camminate, riportano 1,21·√(W·T) e 1,93·√(W/T): prodotto 2,34·W.) Le leggi di scala sono
  quelle della teoria: la ricerca diventa `√T` volte più veloce, ma il precalcolo costa `√T` volte più di un canguro intero, e **precalcolo ×
  ricerca ≥ ≈ 2,3·W** a ogni taglia. Per una sola chiave il totale minimo è `2·√(2,3·W) ≈ 3,0·√W`, il triplo del canguro (1,02–1,06).
  Con classi `±` (`sym=1`) il prodotto misurato è peggiore (≈ 4,5·W) perché le camminate sono passeggiate senza deriva; non l'ho ottimizzato
  oltre, la conclusione non cambia. Il limite teorico generico per gli algoritmi con precalcolo (Corrigan-Gibbs–Kogan) è `S·T_online² ≳ W` con
  `S` bit di tabella, e i precalcoli noti costano tutti `≈ √(W·S)`.

**Cosa significherebbe per il puzzle 135** (W = 2^134, una macchina da ≈ 1,03 G passi/s, 32 byte per voce):

| strategia | memoria | precalcolo | ricerca |
|---|---|---|---|
| canguro, schema 2 | trascurabile | nessuno | **≈ 4 700 anni** |
| precalcolo Bernstein–Lange | 16 GiB (`T = 2^29`) | 1,3·10^8 anni (5·10^7 con camminate corte) | 138 giorni (358) |
| precalcolo Bernstein–Lange | 64 GiB (`T = 2^31`) | 2,6·10^8 anni (1·10^8) | 69 giorni (179) |
| BSGS | 64 GiB | tabella in pochi minuti | 7,8·10^13 anni |

Il precalcolo costa da 10 000 a 50 000 volte il canguro. Una tabella preparata da altri non cambierebbe nulla: qualcuno avrebbe dovuto pagarla.

**3. Quando il precalcolo conviene.** Con `m` chiavi della stessa ampiezza (o più piccola) che condividono una tabella, il totale è
`P + m·S` con `P·S ≈ 2,3·W`. Il minimo è `2·√(2,3·m·W)`, cioè `≈ 3,0·√(W/m)` a chiave, e batte il canguro (≈ 1,02·√W a chiave) da **circa 9 chiavi in su**.
I puzzle aperti con chiave pubblica sono 6 (135, 140, …, 160), di ampiezze da `2^134` a `2^159`. La più piccola domina il costo e le altre
non la aiutano. Lo stesso vale per i bersagli multipli senza tabella (8.2): con ampiezze così diverse il guadagno è trascurabile.

**4. Altri modi di usare la RAM per andare più veloci.**
* *Passo più economico*: ogni punto nuovo richiede almeno un'addizione sulla curva. Le tabelle aiutano solo i punti di partenza (pettine di
  `ec_mul_g`, già usato) e la condivisione dell'inversione (già fatta su 4096 camminatori per thread CPU e su quelli di un work-item GPU, sez. 8.4).
* *Più memoria per i punti distinti*: abbassa `dp_bits` e quindi il ritardo con cui si riconosce una collisione. Alle scale reali quel ritardo
  vale già ≈ 0,1 % (8.7).
* *Punti «gratis»*: con una sola inversione si ottengono sia `P + J` sia `P − J`, come nei passi giganti del BSGS (sez. 9). In una camminata il
  secondo punto non sta su nessuna traccia: serve solo se è esso stesso un punto distinto, cioè una volta su `2^dp_bits`. Non conviene.

**Conclusione.** Con le risorse di un computer (o di molti) il fattore che conta è quello del canguro, ≈ 1,02 a grande scala con lo schema 3 (8.10). La RAM non lo
abbassa in modo apprezzabile e il precalcolo lo alza. Le leve rimaste sono quelle del passo (velocità per passo, sez. 8.4–8.6) e, per pochi punti
percentuali, la geometria delle specie (8.8).


### 8.10 Camminate locali, salti lunghi e zone a fasce (schema 3, dalla 1.7.9)

**Il difetto trovato.** Con le classi `±` la camminata non ha deriva: ogni passo sposta `|E|` di `±s` (s = salto), quindi dopo `n` passi un
camminatore ha esplorato solo una regione larga ≈ `√n·s` attorno alla sua partenza. I modelli di 8.7 e 8.8 erano misurati con camminate corte
(il simulatore con `dp = 0` faceva ripartire un camminatore a ogni ciclo di 2 passi, cioè ogni ≈ 500 passi), dove questo non si vede. Con
camminate lunghe due camminatori si possono incontrare solo se le loro regioni si sovrappongono, e il numero di coppie domato–selvatico che si
sovrappongono vale circa

`P ≈ (s / W^¾) · K^1,5`, con `K` camminatori e `n ≈ √W/K` passi ciascuno.

Con `P` piccolo la copertura dipende da poche coppie, che si incontrano molte volte sprecando gli incontri in più. Per il puzzle 135
(`W = 2^134`) con una GPU e la CPU di un solo computer (`K ≈ 2^19,4`) e il salto di `12·√W` degli schemi 1 e 2: **`P ≈ 0,6`**. RCKangaroo non ha il problema
perché gira con decine di milioni di camminatori su molte GPU (`P` cresce come `K^1,5`).

**Misure.** Simulatore (`tools/ksim.c`, che ora conta fusioni e ripartenze; `rsmin=N` fa ripartire un camminatore al primo punto distinto
dopo `N` passi) e curva vera (`PZ_TEST_KTHREAD` e `PZ_TEST_JUMP` impongono camminatori per thread e salto medio, solo per le prove):

| caso | `P` | costo |
|---|---|---|
| curva vera, `W = 2^40`, 64 camminatori, `dp = 7`, schema 2, salto `1·√W` | 0,5 | **1,548** |
| la stessa con salto `64·√W` | 32 | 1,299 |
| modello, gli stessi due casi | 0,5 / 32 | 1,556 / 1,270 |
| modello, `W = 2^40`, 256 camminatori, `dp = 4`, salti `4, 16, 64·√W` | 16 / 64 / 256 | 1,169 / 1,168 / 1,089 |
| modello, 1024 camminatori, salto `64·√W` | 2048 | 1,095 |
| **modello vicino al caso reale**: `W = 2^48`, 4096 camminatori, `dp = 3`, schema 1 attuale | 0,6 | **1,344** |
| lo stesso, schema 2 attuale | 0,6 | **1,380** |
| lo stesso, schema 2 con salti `W^¾/16` | 16384 | 1,049 |
| lo stesso, **schema 3** | 16384 | **1,018** |

  Nell'ultimo gruppo (300 prove per riga, errore ≈ ±0,035) `P` e il rapporto fra distanza dei punti distinti e lunghezza delle camminate sono quelli
  del puzzle 135 su una macchina: **con gli schemi 1 e 2 il costo vero sarebbe ≈ 1,35–1,38·√W, non 1,10–1,15**. Far ripartire i camminatori
  aiuta quando `P` è piccolo (64 camminatori, `dp = 7`, salto `64·√W`: da 1,270 a 1,125, 1,106 e 1,063 ripartendo dopo 4096, 1024 e 128 passi), ma
  con `P` di qualche centinaio non serve: non è nel programma (sulla GPU ogni ripartenza è un calcolo sulla CPU).

**Schema 3.**
* **Salto medio `W^¾/16`** (multiplo di 12 come nello schema 2): `P ≈ K^1,5/16`, cioè 256 con 256 camminatori e ≈ 2^25 sul puzzle 135, mentre la
  regione esplorata resta `√W/(16·√K)` di `W`, molto più piccola delle zone di partenza. Sugli intervalli dei test (`W ≤ 2^44`) il salto resta fra
  16 e 64·√W, quasi quello di prima.
* **Zone a fasce**: le stesse specie e gli stessi residui dello schema 2, con ogni zona divisa in fasce (domati: 2 da `0,02·W`; selvatici su `Q`: da
  `0,1·W`; su `3Q`: da `0,2·W`) e una quota per fascia, ottimizzate nel modello esatto di copertura (`tools/kbands.py`; nel
  programma `src/kang_spec3.inc`, generato da `tools/kang_schema3.txt` con `tools/kspec_table.py`). La densità dei selvatici su `Q` si concentra
  vicino ai due estremi della zona, quella su `3Q` a metà. Modello di copertura: 1,0455 → 1,0344 (1,0304 con fasce più fini, non adottate: nelle
  camminate nessuna differenza). Modello delle camminate, `W = 2^36`, 4096 camminatori, `dp = 0`, 5000 prove, due semi: **1,015 e 1,021 contro 1,044 e
  1,053** dello schema 2. Con 64 camminatori le fasce non aiutano (1,063 contro 1,054).
* Ogni camminatore ha in modo fisso il suo gruppo (coefficiente e residuo, come nello schema 2) e a ogni partenza estrae la fascia secondo le quote: con
  51 fasce e pochi camminatori, una fascia fissa per camminatore lasciava scoperti alcuni residui e certe chiavi senza soluzione (trovato
  dall'autotest su `W = 2^24` con 32 camminatori).
* Una quarta specie non aiuta: nel modello ideale, con `{0, 1, 2, 3}` mod 60 e `{0, 1, 3, 5}` mod 120, l'ottimizzatore le dà quota zero e resta a
  0,9908, il valore di tre specie. Il conto teorico con quattro specie (0,957) richiede una distribuzione uniforme dei residui che quelle famiglie non permettono.

**Sulla curva vera** (`kstat`, `K ≈ √W/256`, dove `P` è già grande anche con i salti vecchi: qui conta solo l'effetto delle fasce):

| W | camminatori | prove | schema 1 | schema 2 | schema 3 |
|---|---|---|---|---|---|
| 2^32 | 256 | 3000 | 1,151 | 1,134 | **1,105** |
| 2^36 | 1024 | 2000 | 1,170 | 1,095 | **1,056** |
| 2^40 | 4096 | 600 | 1,146 | 1,099 | **1,026** |
| 2^44 | 16384 (8192 per lo schema 3: 2 thread) | 400 | 1,188 | 1,071 | **1,051** |

  Media da `2^32` a `2^44`: **1,060** contro 1,100 (schema 2) e 1,164 (schema 1); è la costante delle stime di tempo (`KANG_C_MEAN`). Alle scale dei
  puzzle aperti il modello dà ≈ 1,02.

**Compatibilità.** Lo schema 3 scrive il registro in formato `PZDP4` (impronta `PZDPLOG4`). Le campagne degli schemi 1 e 2
restano come sono, perché cambiare i salti renderebbe inutili i punti già trovati. Se il loro `P` è sotto 64 il programma lo dice all'avvio e
consiglia una campagna nuova: il lavoro di qualche settimana è una frazione trascurabile dello sforzo atteso sui puzzle aperti, mentre lo schema 3
costa circa il 25 % in meno.

## 9. BSGS simmetrico con tabella in RAM (`--mode bsgs`, dalla 1.1.0)

### 9.1 Algoritmo

Chiave incognita `k ∈ [lo, hi]`, `W = hi − lo + 1`, bersaglio `P = k·G`. Si pone `c = lo + ⌊(W−1)/2⌋`, `d = k − c`, `|d| ≤ H = ⌈W/2⌉`.

* **Baby step.** Le ascisse di `b·G` per `b = 1…T` vanno in una tabella hash (`b` e `−b` hanno la stessa ascissa: una voce
  copre due punti).
* **Giant step.** Con `S = 2T + 1`, per `a = −A…A` (`A = ⌈H/S⌉`) si cerca nella tabella l'ascissa di
  `Q_a = (P − c·G) − a·S·G`. Un riscontro con indice `b` significa `Q_a = ±b·G`, quindi `k = c + a·S ± b` (si provano i due segni e
  si verifica `k·G = P`).

Ogni `d ∈ [−H, H]` si scrive in modo unico come `a·S + b` con `|b| ≤ T` e `|a| ≤ A`: la ricerca è **esaustiva e
deterministica** (nessuna probabilità di fallire come nel canguro). Con i giganti visitati nell'ordine casuale del seed il
costo medio è `T + W/(4T)` operazioni di gruppo (metà dei giganti in media), minimo `1,0·√W` con `T = √W/2`; nel caso
peggiore `1,5·√W`. Il caso `b = 0` (la chiave cade esattamente su un passo gigante: `Q_a` è il punto all'infinito) è gestito da un
percorso esatto lento, attivato anche da ogni denominatore nullo nell'inversione batch.

### 9.2 Come è implementato

* **Passi giganti a gruppi**: come nella ricerca lineare, un'inversione batch (trucco di Montgomery) serve una finestra di 1024
  giganti `C ± i·U` con `U = −S·G`; qui servono solo le ascisse, quindi si risparmia anche la moltiplicazione per `y`
  (≈ 2,5M + 1S per passo, contro 3,5M + 1S della ricerca lineare).
* **Tabella**: indirizzamento aperto con sondaggio lineare, **8 byte per voce** (24 bit di etichetta presi da `x.n[1]`, 40 bit
  di indice `b`), fattore di carico 0,65 (≈ 12,3 byte per baby step), indice di cella `⌊x.n[0]·N/2^64⌋` (qualunque numero di
  celle), inserimento *lock-free* con `compare_exchange` e **idempotente** (riesecuzione sicura), pagine grandi trasparenti
  (`madvise(MADV_HUGEPAGE)`) per ridurre i TLB miss. Un'etichetta che coincide per caso (probabilità `~2^-24` per cella
  controllata) è un falso positivo scartato dalla verifica `k·G = P` e contato nelle statistiche.
* **Costruzione parallela**: blocchi di 2^22 baby step presi da un contatore atomico; ogni thread usa la stessa finestra a
  inversione batch. Misurato su 4 core: ≈ 16–24 M voci/s.
* **Finestre a corsie SIMD (1.5.0)**: 4 (AVX2) o 8 (AVX-512, IFMA) finestre consecutive per istruzione, con l'aritmetica di
  `fev.h`/`fev52.h`, sia per i giganti sia per la costruzione della tabella. All'avvio ogni variante deve dare esattamente le
  ascisse della routine scalare (due chiamate, così è coperto anche l'avanzamento dei centri), altrimenti non viene usata;
  `PZ_BSGS_SCALAR=1` le disattiva. Qui (4 thread, tabella 2^22): giganti da 25,8 a ≈ 50 M/s; ora il limite è la lettura casuale
  della tabella.
* **Ricerca**: i giganti sono i "chiavi" 1…Ng di uno scheduler identico a quello lineare (stessa permutazione dal seed,
  stessi checkpoint/ripresa); `prefetch` software a distanza 16 nasconde la latenza della RAM.
* **Persistenza (`--table FILE`)**: la tabella dipende solo da `G` e da `T`, **non dal bersaglio né dall'intervallo**:
  si costruisce una volta, si salva (intestazione di 4 KiB con SHA-256, `T`, ascissa di `T·G`, somma di controllo del corpo; scrittura
  su `.tmp` con intestazione per ultima + `rename`) e si rilegge per qualunque campagna successiva (controlli: dimensione, somma di
  controllo, numero di voci, 256 baby step ricalcolati a campione). Per `T` fissato il costo di una nuova chiave scende a
  `W/(4T)` giganti.
* **Selezione automatica**: con chiave pubblica nota e intervallo ≥ 2^20 il programma sceglie `bsgs` se la tabella ottima (o la
  massima che sta nel 70 % della RAM libera, o in `--mem`) costa meno del canguro (`KANG_C_MEAN`: 1,10·√W dalla 1.7.8, prima 1,15) e **non ci sono GPU** (o il lavoro
  è di pochi secondi, o la tabella esiste già su disco); altrimenti usa il canguro, che sfrutta le GPU. `T` è fissato alla
  creazione della campagna e salvato in `campaign.conf` (`bsgs_entries`).
* Solo CPU e RAM: le GPU non partecipano (il canguro resta la scelta per sfruttarle).

### 9.3 Quando conviene

| `W` | `T` ottimo | RAM della tabella | costo BSGS | canguro (1,15·√W; con lo schema 2 il 4 % in meno) |
|-----|-----------|-------------------|------------|-------------------|
| 2^50 | 2^24 | 0,19 GiB | 3,4e7 | 3,9e7 |
| 2^60 | 2^29 | 6,2 GiB | 1,1e9 | 1,2e9 |
| 2^64 | 2^31 | 25 GiB | 4,3e9 | 4,9e9 |
| 2^68 | 2^33 | 98 GiB | 1,7e10 | 2,0e10 |
| 2^70 | 2^34 | 197 GiB | 3,4e10 | 4,0e10 |

Con 128 GB di RAM (un budget tipico di una workstation; tabella ≤ 90 GiB) il BSGS ottimale entra fino a `W ≈ 2^67,7`; con una tabella più piccola di `√W/2` costa
`T + W/(4T)`, che resta inferiore al canguro per `T ≥ 0,29·√W`, cioè fino a `W ≈ 2^69` (con la costante 1,26 del canguro
fino alla 1.5.0 erano `0,24·√W` e `2^70`).

**Onestà sull'applicabilità.** I puzzle aperti con chiave pubblica nota (140, 145, 150, 155, 160) hanno `W = 2^139…2^159`:
`T` ottimo sarebbe `2^69,5…2^79,5`, impensabile. Per loro il BSGS non può aiutare e resta il canguro (comunque ≈ 2^70 passi per il 140). Il
BSGS è utile per intervalli con chiave pubblica nota fino a `W ≈ 2^70` e come "risolutore universale" con una tabella
precalcolata riusabile.

## 10. Aritmetica a corsie SIMD (`avx2v`, `avx512v`, `avx512i`)

La ricerca lineare spende circa il 75 % del tempo nell'aritmetica della curva (moltiplicazioni di campo) e il resto negli hash.
Le moltiplicazioni scalari 4×64 bit (MULX/ADX) costano ≈ 37 cicli ciascuna su una CPU x86-64 recente. I motori `*v` eseguono invece la
stessa aritmetica su **4 (AVX2) o 8 (AVX-512) finestre indipendenti** in parallelo, una per corsia:

* **Rappresentazione** (`lib/pzcore/include/pzcore/fev.h`): 9 limbi da 29 bit per elemento, "struttura di array" (il limbo `i` di tutte le corsie è un
  registro). `vpmuludq` (32×32→64) fa 4 o 8 prodotti per istruzione e una colonna di 9 prodotti da ≤ 2^60 sta in 64 bit.
* **Riduzione**: `2^261 ≡ 32·(2^32 + 977) = 0x7A20 + 256·2^29 (mod p)`: le colonne alte si propagano in limbi da 29 bit e si
  ripiegano con due prodotti piccoli ciascuna.
* **Limiti dei limbi** (documentati in `fev.h`, ricavati a mano e verificati): risultati "stretti" ≤ 2^29 + 2^23;
  operandi "larghi" (somma di due stretti) ≤ 2^30 + 2^24 sono accettati da `fev_mul`/`fev_sqr`; la sottrazione aggiunge un multiplo di
  `p` con limbi più grandi del sottraendo (`FEV_K1`, `FEV_K2`, verificati multipli di `p` dai test).
* **Finestre e ordine**: il round `g` copre `N` finestre consecutive di 512 chiavi a partire da `start + g·N·512` (la corsia `l`
  prende la finestra `g·N + l`): dopo qualunque numero di round le chiavi coperte sono un **prefisso** del blocco richiesto, come
  pretende lo scheduler. I denominatori nulli (centro che coincide col passo) fanno ricadere l'intero round su un percorso lento
  esatto, esercitato dai test.
* **Misure** (CPU x86-64 con AVX-512 senza IFMA, 4 thread): motore scalare `avx512` 23,9 M chiavi/s → `avx512v`
  **41,6 M chiavi/s (×1,74)**. Per corsia, `fev_mul` costa ≈ 19 cicli (AVX-512) o ≈ 28 (AVX2) contro 37 della scalare, `fev_sqr`
  ≈ 11 / 23 contro 34; l'unità vettoriale è il collo di bottiglia (`vpmuludq` zmm: 1 per ciclo su questa CPU). Con AVX2 il guadagno
  è modesto (≈ ×1,15) e l'autotune sceglie il motore migliore per la CPU in uso.

### 10.0 Canguro a corsie (1.5.0, `src/kang_vec_impl.inc`)

Lo stesso passo del canguro scalare (`kang_isa.inc`) su 4 o 8 camminatori per istruzione: il camminatore `c·N + l` sta nella
corsia `l` del vettore `c`, e tutto il suo stato resta in forma vettoriale tra un round e l'altro (coordinate, distanza a 192 bit
come tre corsie a 64 bit con riporti vettoriali, segno, etichette anti-ciclo, contatore). Solo i punti distinti e i riavvii, rari,
passano per la corsia singola, e vengono riordinati per indice come li produce il codice scalare. Le regole sono identiche (scelta
del salto, fuga dai cicli di 2, `y` pari, DP, riavvii): `kang_round_check` esegue variante e riferimento sugli stessi branchi
(molti DP e riavvii, metà dei contatori oltre 2^32, un camminatore forzato sul proprio punto di salto, che manda il round nel
ripiego scalare) e confronta tutto bit per bit; il selftest lo fa per ogni variante e la scelta automatica esclude chi non
coincide. Test delle mutazioni: 8 errori introdotti nel codice vettoriale, 8 rilevati.

Velocità (1 thread, passi/s): portabile 4,4 M, BMI2/ADX (la versione fino alla 1.4.0) 6,4 M, AVX2 a corsie
8,3 M, AVX-512 a corsie 12,7 M, **AVX-512 IFMA a corsie 24,9 M (×3,9)**. Con 4 thread: 25,3 → 95 M passi/s.

### 10.1 Variante AVX-512 IFMA (`avx512i`, `lib/pzcore/include/pzcore/fev52.h`)

`vpmadd52luq` / `vpmadd52huq` sommano a un accumulatore a 64 bit la metà bassa / alta (52 bit ciascuna) del prodotto di due
operandi da 52 bit. Con **5 limbi da 52 bit** un prodotto completo costa 25 + 25 di queste istruzioni, contro 81 `vpmuludq` più le
addizioni della rappresentazione a 29 bit. Il motore (`engine_vec_impl.inc`) è lo stesso: cambia solo l'intestazione del campo.

* **Vincolo**: le istruzioni leggono solo i 52 bit bassi degli operandi, quindi ogni valore che può finire in un prodotto deve
  avere limbi esatti. Tutte le operazioni restituiscono la forma "stretta": limbi 0..3 < 2^52, limbo 4 ≤ 2^49 (valore < 2^257);
  anche l'addizione fa la propagazione dei riporti (non esiste una forma "larga").
* **Prodotto**: 10 colonne da < 2^57. La metà alta (peso `2^260 ≡ R = 16·(2^32+977) = 0x1000003D10`) si porta a limbi esatti
  (il suo valore è < `a·b/2^260 < 2^254`, quindi il limbo alto resta < 2^46) e si ripiega con 10 istruzioni IFMA; i bit oltre
  `2^256` (limbo 4 sopra il bit 48, più la parte alta dell'ultimo ripiegamento, < 2^36) si moltiplicano per `c = 2^32+977` con
  altre 2; segue **una sola** propagazione dei riporti. Il quadrato calcola i 10 prodotti incrociati una volta e raddoppia le
  colonne (raddoppiare un operando supererebbe i 52 bit): 30 istruzioni IFMA invece di 50.
* **Sottrazione**: si aggiunge `4p` (o `8p` per `a − b − c`) scritto con limbi che dominano il sottraendo (`FEV_K1`, `FEV_K2`).
* **Normalizzazione**: un solo ripiegamento lascia il limbo 4 ≤ 2^48, cioè `r < 2^256 + 2^208`; poi `r ≥ p ⇔ r + c ≥ 2^256`
  e `r − p < p` (il secondo ripiegamento della versione a 29 bit qui è superfluo: lo ha mostrato il test delle mutazioni).
* **Verifica**: `lib/pzcore/tests/t_fev52.c` (valori casuali e ai limiti di ogni limbo, multipli di `p` ± piccoli, catene lunghe, inversione;
  8 mutazioni del codice, tutte rilevate tranne quella dimostrata innocua), `t_engine` e `selftest` sul motore completo. Eseguiti
  sia direttamente su una CPU con IFMA (`PZ_ASSUME_IFMA=1` salta il controllo
  della CPUID, **solo per i test**) sia sotto **Intel SDE** in modalità Tiger Lake, che emula anche la
  CPUID. Il programma sceglie `avx512i` solo se la CPUID dichiara IFMA, e ogni motore prima dell'uso deve ritrovare una chiave
  nota (verifica rapida, pochi millisecondi).
* **Misure** (4 thread, x86-64): `avx512v` 42,0 → `avx512i` **64,9 M chiavi/s (×1,55)**; 1 thread 10,7 → 16,2.
  Con IFMA la curva costa ≈ 21 ns per chiave e i due hash ≈ 39 ns: ora **il collo di bottiglia della CPU è l'hash** (≈ 65 %).
* **SHA-NI, provata e scartata**: un blocco SHA-256 con le istruzioni SHA-NI costa ≈ 63 ns sulla stessa CPU, mentre SHA-256 +
  RIPEMD-160 sulle 16 corsie AVX-512 costano ≈ 39 ns per chiave in tutto. Non conviene.

## 11. Modello di stima del tempo (`src/eta.c`)

Le stime sono calcolate da un ritmo *realistico fin dall'inizio* (dalla 1.7.4, `rate_est` in `eta.c`): nessuna stima finché ogni
motore ha prodotto qualcosa (una GPU si conta solo a fine lancio), poi la media su una finestra scorrevole fino a 2 minuti; all'inizio
è mescolata, con peso crescente fino a 150 s, a una velocità *ricordata* (quella a regime di una corsa precedente della stessa
configurazione, che sui portatili è più bassa di quella misurata a freddo dalla taratura). Il BSGS, che cambia velocità alla fine
della costruzione della tabella, resta con una media mobile di ≈ 30 s. Mostrano il tempo *ancora necessario* e la probabilità di
successo nelle prossime 1 h / 24 h / 7 g / 30 g / 365 g:

* **Lineare e BSGS** (la chiave è uniforme sulle unità non ancora controllate): `P(entro e unità) = e / residue`; tempo per
  la probabilità `q`: `q·residue / velocità`.
  Con `--part` il computer può scandire solo la sua parte (`reach` unità ancora da fare) mentre la chiave è uniforme su tutte
  le `residue` unità non ancora da lui controllate: `P(entro e) = min(e, reach) / residue`, e le probabilità `q` oltre `reach/residue`
  non sono raggiungibili da solo (tempo «n/d»).
* **Canguro**: costo `n` con `P(trovata entro n) = 1 − exp(−(n/c)²)`, `c = k/0,8862·√W` con `k` il costo medio dello schema della campagna: 1,10 per lo schema 2 (dalla 1.7.8, sezione 8.8, `c ≈ 1,24·√W`), 1,15 per lo schema 1 (dalla 1.6.0, sezione 4.5, `c ≈ 1,30·√W`; `c = 1,43·√W` prima). Dato che dopo `n₀` passi non
  è ancora stata trovata: `P(entro n₀+e | non trovata) = 1 − exp(−e(2n₀+e)/c²)` e `e_q = c²·L/(n_q + n₀)` con `L = −ln(1−q)`
  (forma senza cancellazioni numeriche).

Queste probabilità sono *condizionate* all'ipotesi che la chiave sia nell'intervallo e che il programma sia corretto; non dicono
nulla sul fatto che i fondi siano ancora presenti (controlla l'indirizzo con un esploratore della blockchain).

