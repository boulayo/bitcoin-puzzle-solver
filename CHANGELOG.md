# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows [Semantic Versioning](https://semver.org/).
Each release is tagged `vX.Y.Z` (see [`docs/en/RELEASING.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RELEASING.md)).

## [1.24.0] — unreleased

### Added
- pzpeer: the shared linear search (docs/en/NETWORK.md 11), for a puzzle whose public key is not known. Its network is in the program
  (`pzpeer run --puzzle 71`: its address, blocks of 2^44 keys), or a file gives `address=` and `blockbits=`. The home picks a block
  nobody claimed, has `pzsolve hunt --near` scan it, and publishes the claim of the block with its proof keys to the 5 custodians of the
  block, which keep it and answer who asks (`CLAIM`, `CLAIM_GET`); when every block it looks at is claimed, it scans a claimed one again.
  A key found stays with the home. `pzpeer node` keeps the claims of such a network too. Proof keys of 32 bits for blocks of 2^34 to
  2^37 keys, of 40 bits for blocks of 2^42 to 2^45.
- `pzpeer sweep --private`: the signed transaction goes to MARA's Slipstream, a mining pool that does not show it in the public mempool,
  through the `curl` of the system; `--pool URL` names another one. Without them it is printed and not sent, as before.
- pzsolve `hunt --stop-with-input`: the end of standard input stops the campaign as a Ctrl-C does. `pzpeer run` starts pzsolve with a
  pipe it never writes, so a pzpeer that is killed or crashes no longer leaves a pzsolve searching on alone (its campaign taken, at the
  next start, by a search nobody reads).
- pzpeer: the network of every puzzle with a known public key is in the program: `pzpeer run --puzzle N`, and with neither `--puzzle`
  nor `--network` the first puzzle still open (today 140). Its jump seed is `SHA256("pznet1/jumps" || public key)`, its least zero
  bits `max(8, (N - 1) / 2 - 44)`, its chain the main one; `pzpeer netcheck 140` prints it. A network file stays for tests and for
  puzzles that are not in the table.
- The kangaroo keeps its walkers across a stop: when a run stops, every walker's distance, sign and group go to `herd.bin` in the
  campaign folder (the GPU walkers read back from the device), and the next run puts each one back at its point and walks on. Before,
  a stop threw away every walker's steps since its last distinguished point, `K·2^dp` steps on average, most of the work with millions
  of walkers and many zero bits. A walker goes back to a slot of its own group, so every herd keeps its mix of species; the next run may
  have more or fewer walkers, and other `--dp-bits`. Only the next session of the same computer takes the file (not a copy of the folder,
  nor a run after a crash); it is deleted once the key is found. `status.json` tells the walkers of a kangaroo run.
- `hunt --step STEP`: only the keys `S, S + STEP, S + 2·STEP, ...` of the range, for a key whose unknown bits are one block in the middle.
  Every method runs at its full speed: the kangaroo and BSGS on a transformed public key, the linear engines and kernels moving by
  `STEP·G`. A campaign with a step needs 1.24.0 (`requires=` in its campaign.conf).
- pzpeer: after the payout has 6 confirmations and nothing of value is left at the puzzle address, the two homes give each other their
  pieces, and each has the key of the puzzle, in `FOUND.txt` and on the terminal (encrypted when pzsolve encrypts its key). Every step
  that matters gets a short line on the terminal, and every 10 minutes a line of the network with its rate and the time the key needs.
- pzpeer: 10% of a prize goes to the author of pzpeer, the rest half each to the two homes (45% each); a home whose pzsolve finds the
  key on its own sends the coins at once, 90% to its payout address and 10% to the author. The share and its address are in the code, by
  version of the terms, outside the network file and its fingerprint; a pair pays by the older version of its two homes. The first start
  shows the terms and asks to accept them (`--accept-terms` for a service), then asks the payout address, which the folder keeps
  (`--payout` is no longer needed after it); the line of the network says what the home gets of the prize on average.
- pzpeer: the short record. A record is 66 bytes (type, zero bits, x, owner) and carries no signature and no sequence number: a STORE
  sends 34 bytes a record and its owner is the sender, whose identity the handshake proved, and an alert still waits for the proofs of
  possession. A custodian's entry is 38 bytes, with the owner's number in a file of owners beside the store. A record carries its own
  zero bits (at least those of the network). The custody by default is 5 GB (2^27 records). A new protocol (`pznet2`): homes of the
  earlier test builds do not talk to these.
- pzpeer: the zero bits of a home's points follow the network. The network file gives the least; a home takes the fewer of what the
  delay allows (once two walks meet, the second walker must reach a distinguished point: `2^dp = E / (32·N·K)`, with N the nodes of the
  network and K the walkers of its pzsolve, from `status.json`) and what the room of the custodians wants (`2^dp = 10·E / (N·S)`), at
  most 8 bits more than the network's, which is also the most a custodian takes. It decides from three lookups and changes them when
  they move by 2 or more: pzsolve saves its walkers and resumes its campaign with the new `--dp-bits`, and `dpbits.txt` remembers from
  which point of `dp.bin` each value holds. The time estimate weighs each record by its zero bits. pzpeer refuses the options after
  `--` that it gives pzsolve itself.
- pzpeer: publishing without a lookup per record. A lookup of the 20 nodes closest to a target is kept for an hour, and the custodians
  of a key near it come from it when it shows that they are the 5 closest of all; a test network of 31 nodes takes 2000 records in
  0.5 s instead of 1.4, and the homes of `tests/peer/e2e_net.sh` keep up with pzsolve at 14 zero bits. `own.bin` is a plain journal,
  and after a restart the records it has are not sent again. The daily republish sends a record only to the custodians it did not have
  at the last republish (`own.cust`), so with no node come or gone it sends nothing; a custodian that refused or did not answer gets the
  records again the next time, the first republish comes two minutes after a start, and once a week every record goes to every
  custodian (a custodian that lost its store, an alert lost on the way). Only whole lookups are kept. A record is named without its zero
  bits wherever it counts (the hello, the name of a pair, a proof), so two custodians with other zero bits for one point cannot keep a
  pair from paying.
- pzpeer: a home that finds points faster than it publishes them (tens of graphics cards at 30 zero bits) says how many wait; the
  network sees them late, and the coincidences, which grow with the square of the points seen, come later.
- `hunt --jump-seed HEX --start-key FILE`: a shared walk, the kangaroo of the shared network (docs/en/NETWORK.md). The jumps come from the
  public seed, the walkers follow the schema net-06, and every start draws from a generator of its own seeded with the secret start key,
  which is never written in the campaign files or in the reports. The distinguished points are exactly those of `--dp-bits`, and `dp.bin`
  keeps every point, also those the RAM limit leaves out of the table, for pzpeer to publish. pzsolve knows nothing else of the network.
- pzpeer, the daemon of the shared network, and pzsign, its two-party signer (docs/en/NETWORK.md): a home publishes the points of its
  pzsolve, keeps the records of others in a Kademlia table, and for every coincidence of a tame and a wild point of two homes runs with
  the other home a two-party ECDSA signing (the two-party ECDSA of Coinbase's cb-mpc, in signer/vendor, which pzsign builds for Linux,
  Windows and macOS) and the two-step payout. A network is its file: the fingerprint of the lines
  that define it binds every key and record, and nobody signs it. Homes behind a router reach each other through a relay; the node of the home can be pruned and
  is reached with its cookie file (`--rpc-cookie`).
- pzpeer needs no Bitcoin node of one's own: without `--rpc` it reads the chain from several Electrum servers (`--electrum HOST:PORT`
  and the `electrum=` lines of the network file) and trusts none. Block headers count only with their proof of work from the
  `checkpoint=` of the network file, a coin only with its transaction and Merkle proof, a spend only when proven in a block: a lying server can
  delay a payout, never misdirect it. A node of one's own stays the stronger choice.
- pzpeer finds the other nodes with no server of the network: each node announces its port in the Mainline DHT of BitTorrent under
  a key made from the network's fingerprint and looks the others up there (`--bt-router`, `--no-bt`). Only addresses pass through it,
  each dialled and checked as any other. A node listens on port 28140 and asks the home router to let it in, as Bitcoin Core does: by PCP or
  NAT-PMP, and by UPnP for the routers that only speak that (`--no-portmap`, `--listen 0`).
- `pzpeer run --network FILE --payout ADDRESS` is all a home needs: the folder, the programs, the port, the first contacts and the
  chain have defaults. `--debug FILE` logs every message, step and answer of the chain, with no secret, for reading afterwards.
- The fee of the payout is twice the estimate of the chain, 20 to 1000 sat/vB, and the tiny coins grow with it, so that the party
  that can cancel always can.
- A key found for an address whose public key was not known (a linear search) comes with a warning: spending it through the public
  mempool shows the public key, from which a key this short is found in seconds. `pzpeer sweep --to ADDRESS` makes and signs the
  transaction that moves its coins and sends nothing, for the finder to give to a mining pool in private.
- `hunt --near` (linear search) writes the near misses of the scan to `near.txt`: the keys whose hash160 has four bytes of the
  target's, one in 2^32. The CPU engines and the GPU kernels report them to one function of the host, which checks each and takes
  the key of the campaign from among them; they are the proof of work of the shared linear search to come (docs/en/NETWORK.md). The
  file is for the user alone (mode 0600), and the key itself never goes there, only to `FOUND.txt`.
- pzpeer and pzsign for Windows: `make windows winsigner` cross-compiles `pzpeer.exe` and `pzsign.exe` with MinGW-w64 beside
  `pzsolve.exe`. What pzpeer asks of the operating system is in one place (`src/peer/sys.h`), with an implementation for Linux and
  macOS and one for Windows. Under Wine the unit tests pass and three homes pay a coincidence (`PZ_E2E_WIN=1 tests/peer/e2e_net.sh`);
  the CI runs the unit tests on a real Windows machine, the two-party signing with `pzsign.exe` among them, and builds and tests
  pzpeer on macOS. A whole network has not been run on those systems yet.
- New campaign seeds come from the generator of several sources of pzcore (OS, CPU generator, timing jitter, TPM), mixed with HKDF.

### Changed

- pzsolve: a new campaign with `--part` needs `-s`, the same on every computer of the split. Without it each computer drew a seed of its
  own, and the shares overlapped and left blocks that none of them scanned (about a quarter of the range with two computers), each one
  ending "scanned in full". `merge` writes what it merged, and a key it finds, to the log of the campaign.
- The AVX2 lane engines (linear `avx2v`, the AVX2 lanes of the kangaroo and of BSGS) and the AVX2 hash of pzcore are built without ADX and
  need AVX2 and BMI2 only: a Haswell, which has no ADX, runs them instead of the portable code (2.3 to 2.6 times the keys per second of
  the linear search and 1.8 times the steps of the kangaroo, on the test machine). pzcore: `pz_cpu_has_avx2` is AVX2 and BMI2, and the
  new `pz_cpu_has_adx` adds ADX, which the MULX/ADX code (`avx2`, `shani`, the BMI2/ADX kangaroo step and BSGS windows) and AVX-512
  require. Elsewhere the speed is unchanged.
- pzpeer: a publish, a lookup and the light chain do less work: the mailbox code without its dead branch, no second check of the answer
  types that `exchange` checks, the height of the light chain asked of the servers every 10 seconds at most for all the sessions (before,
  each session every 15 seconds), a wait that got no answer asks again once a minute. The fee rates of a refused session are given in
  sat/vB. The fuzzer of hostile input also takes records, claims of blocks and network files.
- Simpler code: pzsolve reads the name of a mode in one function and checks `-m` when the options are read, sets the address of a campaign
  once, counts the work of a session (`run_session_units`) and the kangaroo's workers in the run; the two linear engine templates share
  `engine_common.inc`, the three lane templates take their conversions from `fev_chain.inc`, and `selftest` checks the hash lanes with
  the quick check. pzpeer: one function gives a task of the home its chain (`home_chain`), one checks a payout script
  (`btc_script_standard`), the hook that let a test hold back the second transaction is gone, and `main` makes, checks and locks the
  folder before the home starts.
- pzsolve: a stop logs `stopping: saving the state and the walkers (Ctrl-C again quits without them)`, and the end of the input of
  `--stop-with-input` logs why. The kangaroo CPU step is chosen from three interleaved samples of each form (one sample could pick the
  slower form for a whole run after a busy moment).
- pzpeer: one function writes a file whole or not at all (`sys_new_file`), one stops a child (`sys_proc_stop`), one reads the optional
  lines of a network file (`netfile_opt`), one tells the owner (`home_say`); `pzcore` keeps the only hex functions and SHA-256 padding.
- pzpeer: a publish stores to 16 custodians at a time, and the republish sends `own.bin` in the order of the keys, 65,536 records at a
  time: a batch of 255 records reaches about 9 custodians at 1000 nodes instead of about 695. The jobs of a node take turns by kind,
  oldest first (before, its own alerts always came first). PROVE carries up to 8 records, and a session starts with `S_SECOND`: the
  prologue is `pznet5`.
- Faster scalar engines: the batch inversion multiplies in two interleaved chains (a quarter less time with BMI2/ADX, a tenth
  without), and the straight-line addition and subtraction are used on every x86-64 build, not only with BMI2/ADX. Measured on one
  thread: kangaroo step BMI2/ADX 9.4 to 10.9 M steps/s, portable 7.2 to 8.2 M; linear baseline 4.62 to 4.74 M keys/s, avx512
  10.2 to 10.6 M. The lane engines (avx2v, avx512v, avx512i) invert in lanes and are unchanged.

### Security

- pzpeer: the answer to PROVE gives every other owner of the x, up to 8, the newest first, and a custodian keeps a record that its owner
  proves also past the 8 owners of its x, up to 16: copies planted before the real record, which their makers cannot prove, can no
  longer hide a coincidence (before, 7 copies at a custodian made the next owner of the point hear of the copies only, and 8 kept the
  owner's record out); the owner takes from that answer only the pairs of the records it proved, at most 8 each (before, any 64 pairs of
  any of its records). The alert jobs of one address block hold 8 places in the queue, retries included, and the other
  owner is looked up once a job (before, one address kept a real alert behind up to 1024 forged ones). 6to4, Teredo and every other
  address that is not local count by their /24 or /32 block (before, one IPv4 behind 6to4 had 2^80 blocks). A node of a linear search
  takes no records, ALERT, PROVE or POSSESS, and has no `store.bin` (before, anyone could fill its 5 GB).
- pzpeer: an alert whose job waits in the queue is no longer pushed out of the table of alerts by a flood (before, two address blocks
  whose forged alerts made slow jobs pushed every real alert out before its turn); an alert has one job at a time. A known contact moves
  to a new address only after its old one failed (before, a node that named it at an address of its own, passing the bytes on, took its
  place in the table and could cut it out later). A mailbox answers `MBOX_GET` only to its owner (before, anyone read the points of
  anyone's coincidences). A node of a linear search relays nothing.
- pzpeer: one pzpeer a folder: `run` and `node` hold `DIR/lock` while they run, and a second one on the folder stops at once (before,
  two wrote over each other's records, journals and sessions). On Windows the folder gets an access list of the user, by the user's own
  identity, and the system: one made from an elevated shell no longer shuts out the same user without elevation, and a folder made by
  hand is made private too.
- pzpeer, the light chain: each Electrum server has one minute for all its calls of an operation, its headers included (before, a minute
  again for every coin it did not list: a server answering each call just in time, with 256 coins of dust sent to the puzzle address,
  held every chain call of the home for hours, and every payout through servers failed); a server that announces blocks it cannot show
  is left alone for ten minutes. A spend is looked for in the whole history after the coin, within that minute, and what each
  transaction spends is kept from one look to the next (before, the oldest 256 entries: dust sent between a coin and its spend hid the
  spend, and one lying server kept a spent coin in the first transaction). A spend waiting in the mempool counts only with an input
  signed by the key of the address, in a transaction a server gives (before, any transaction a server listed there, a deposit or an id
  it made up, held the exchange of the pieces for ever).
- pzpeer: a home whose pzsolve finds the key on its own also waits for a second transaction it signed with a partner that is in no block
  yet (before, its own transaction could replace that payout in the mempools, and with it the partner's share). What a session tells
  the partner when it stops no longer says where this home's node or pzsign is.
- pzsolve: an encrypting campaign that finds its key no longer says how far its search got: its last `report.txt`, `status.json` and line
  of the log carry no progress, and `stats.csv` is deleted with the state (every other computer of a `--part` split holds the seed, and
  with the progress that is where the key is). On Windows `near.txt` is deleted too (it was still open); a file that cannot be deleted is
  said. A campaign with `--part` no longer prints its seed at every start.
- pzsolve on Windows: the folder of a campaign is its owner's alone, as on Linux and macOS: an access list of the user and the system that
  takes nothing from the folder above (a folder made under `C:\` let every user of the machine read the key and the seed and change
  `campaign.conf`). A folder that another user made is refused; the folder must be on NTFS.
- pzpeer: the failed signings of a network count for 30 days from the first of them, then start again; those of an own point
  count for ever, as before. A partner that spoils its messages could stop an honest home on a network for ever; now for a month at most.
- pzpeer, before a network with real coins (docs/en/NETWORK.md; the prologue is `pznet5`, so earlier versions do not talk to this one):
  - F, the party whose tiny coin goes into the second transaction, comes from the point and the two types of the pair, which the walks
    fixed: a home could choose its part with a new identity (about two tries).
  - A custodian alerts a coincidence only once the owner that stored the record proved it (`COINCIDENCE`, then `PROVE`, a proof for that
    custodian): a copy of another home's point alerts nobody. The answer gives the owner the pairs at once, from a custodian of its own
    point, so mailboxes that nodes placed near their key keep from a home cannot hide its coincidences for more than a week.
  - The fee rate comes after the proofs (`S_FEE`), and the chain is asked only then: a stranger's hello made a home ask its chain.
  - Each operation of the light chain has 60 seconds per server in all; the limits by address block live in a table keyed with a secret
    (FNV let chosen blocks push each other out, back to a full share); at most 8 queued custodian alerts for the records of one block;
    256 waiting places and 32 relays at a relay; the transaction IDs of the BitTorrent client carry a secret of the round; 6to4 and
    Teredo addresses are not public; descriptors are closed on exec from their birth (Linux) and a child gets only its own (macOS);
    the folder of a home on Windows is for its owner alone; the router is asked to close the port at the end.
  - A pair waits no longer for a first transaction whose coins were spent by another (an old copy of it with other signatures): two
    blocks after, it gives up and tries again. The rule "one point, one proved pair" is kept on disk (`proved`). With an own node, a
    first transaction that a reorganisation put back in the mempool is no spend in a block. pzsign checks every signature of a batch
    before it stops on a bad one, so the time does not tell which one it was.
- The jitter source of the seed generator times the same work in every sample (the same addresses, no branch on the data), so its
  health tests and its live estimate measure noise only; the walk grows until that work shows noise, and a machine whose timing is too
  quiet has the source refused ("too little noise"), its home seed then made with a TPM or on another computer. Before, the walk varied by its work too, and a
  noiseless machine behind a coarse timer passed.
- pzpeer, the shared linear search: a claim needs half the average number of proof keys (a false claim costs half a block, not a
  quarter); a custodian keeps another owner's claim of a block only when it brings a proof key the claims it holds lack, so a claim
  copied under a new identity, which costs no scan, is not kept; when every block looked at is claimed, the one whose claims show the
  fewest proof keys is scanned again, never one of the home itself; `--part` after `--` is refused (half a block would be claimed).
- pzpeer: a light chain whose servers show no coin at the puzzle address no longer makes the home take the prize for gone and stop its
  search: an empty answer of servers proves nothing (they may hide the coins), and a later session asks again.
- CI: the actions are named by the hash of their commit; `check_binary.sh` checks each Windows program against the DLLs it needs.
- `dp.bin` of a kangaroo campaign, whose distances lead to the key, is made for the user alone (mode 0600), as `FOUND.txt`
  already was.
- The two-party payout, after a review of the signing (docs/en/NETWORK.md 7.5): a payout counts from the signing of its first
  transaction until it pays, at most 5 on one own point, and a partner's fee rate more than 4 times the home's own is refused: a
  partner could make the pair try for ever, each try paying a fee and a tiny coin from the prize. Only F gets a tiny coin, and the
  second transaction splits the prize and that coin half each (S's coin was never spent again). P2 that gave all its messages and
  only lacks P1's signatures ends with a frame of its own (0x84) and counts no failed signing: a partner that kept them could use up the
  home's count for free. pzsign verifies the signatures before it sends them. A pair out of tries gives its slot back.
- The fixes of 1.23.4 (below), and with them: an encrypting campaign that finds the key deletes `near.txt` too, whose last lines are
  keys next to the key; `near.txt` is never opened through a link; the folder of a home or node of pzpeer must not be a link.
- pzpeer: a custodian keeps the owners of at most one record in 256 it may hold, and 1024 more: identities cost little, and their table
  grew in memory without a bound. A hello longer than any real one is refused before it takes a place among the sessions (it kept one
  until a restart). The password of `--rpc` is blanked in the list of processes (`--rpc-cookie` stays the better way). The fee rate of
  the Electrum servers is their lower median, so that one server of two cannot raise it.

### Fixed

- pzpeer, the session: F reads the coins of its tiny coins while `S_ALIVE` goes to the partner (with a node, a scan of the whole set: S gave
  up as soon as F's node was four minutes slower than its own, at every session of the pair). A payout of coins that came to the address
  after the payout keeps its own second transaction (before, the record kept the height of the payout before, and the exchange neither
  sent the new second again nor saw it void: the coins stayed under the joint key and the pieces never came). The exchange of the pieces
  settles also in a later session when every output of the second is spent. A count of failed signings taken off that cannot be written
  no longer stops the exchange. A chain that does not answer is no longer read as a payout that went elsewhere. A second transaction kept
  in `HOME/paid` and in no block yet is sent again every ten minutes by the home itself while its pair has no session.
- pzpeer: the files of a home, the cookie of bitcoind, `headers.bin`, `peers.txt` and `/dev/urandom` are not inherited by the programs
  pzpeer starts; `dp.bin` and the cookie are read without a buffer of stdio (the distances and the password only in buffers that are
  wiped). The files it may have open are raised to 4096 where the system allows (macOS gives 256, fewer than the sockets of a busy
  relay). The maintenance thread sleeps on the clock that only goes forward (a clock set back stalled it as long). Two bootstraps at once
  no longer write `peers.txt` together. A start that fails says why (the port in use, the threads), instead of "cannot listen" for every
  cause.
- pzsolve, the kangaroo: a key whose `FOUND.txt` could not be written, or that could not be encrypted, is found again by the same command:
  the point that solved it goes to `dp.bin` too (before, its walker was saved on that point and never reported it again). `dp.bin`
  reaches the disk at every checkpoint (it was only flushed, while `herd.bin` was synced), and on Linux and macOS a durable rename syncs
  its folder too. A checkpoint or report that keeps the disk busy more than 15 seconds is no longer taken for a suspension of the system.
- The quick check of a CPU engine planted one key, on one side of the centre of one window: an engine that missed the keys above the
  centre, or every lane of a vector but one, passed it. It now plants a key in every window and at every position where a window takes
  its own branch, a few milliseconds per engine. An autotune in which no engine passes stops and says so (before, it used the portable
  one anyway).
- GPU, linear search: a block whose first centre is the point at infinity was taken for an error of the device (a recovery, the device
  given up after four): the CPU scans it again; a block scanned again on the CPU reports its near misses once (before, twice); a recovery
  builds the linear program once. GPU, kangaroo: a restart the device does not take is a device error; a distinguished point of a walker
  outside the herd is dropped, not inserted without its species. OpenCL: launches are rounded up to a multiple of 64 work-items (a block
  at the end of the range or the herd of a small range left the driver work-groups of 1 or n); the runtime is loaded once whichever
  thread asks first; a device whose compiler refused the inline assembly builds without it from then on. macOS arm64: the SHA-256
  instructions are no longer probed with a sysctl that macOS 11 lacks: every arm64 Mac has them.
- t_dht: the relays done are counted on every node that listens. A channel relayed by the eviction node or the late node, as the slot
  picked them, failed the test (seen once on macOS).
- pzpeer: the owner takes the answer to PROVE (an MBOX, not the type of the request + 1; before, it refused it and counted the custodian
  as failed, so it never heard of its pairs from a custodian of its point). One PROVE goes to each custodian with every record it said
  COINCIDENCE for, at most 8 (before, a custodian that said so for every record and then kept silent held the home 10 s a record). A
  publish no longer waits for a republish under way: no lock is held across a connection. `pzpeer node` on a linear network shows the
  claims it holds.
- pzpeer, the shared linear search: a block whose index needs more than 64 − blockbits bits could not be claimed (with blocks of 2^44
  keys, 63 of the 64 million blocks of puzzle 71): the blocks are those of the range, `block_range` in one place. The home keeps the
  blocks it scanned in `own_claims.bin`, a file of claims as the custodians keep (a write cut by a crash hid every claim after it; one
  in `claims.bin` stopped the node from starting: it is cut there now), sends its claims again from two minutes after a start, one a
  turn of its loop (before, only after a day without a stop, all at once), keeps a block with too few proof keys as scanned without
  claiming it, and no longer saves the block it was picking when it is stopped. `claims.bin` opens with one sort (it was quadratic).
- pzpeer, the session: the second transaction is kept on disk as soon as it is signed, and every session of a pair starts with what
  each side has of it (`S_SECOND`): a home stopped before its block, or one that lost its record, is still paid and gets its key (before,
  the two homes failed at every try). The transactions of a payout and the cancel go to the chain again at every block; S stops waiting
  for the second transaction once F's cancel is in a block (before, 144 blocks); a wait asks the chain once a block. A pzsign that does
  not start counts no try of the payout, and the session says why and tells the partner (before, an empty reason); `pzpeer run` checks
  `pzsign version` at its start. The exchange of the pieces settles when both homes spent their payout already (the output of the
  author shows the second transaction in a block). F's tiny coins left by earlier sessions of a pair go to its payout address once no
  second transaction can take them. A line of `failed-signings` without its date that is at the limit gets its date. pzpeer stops
  pzsolve within a minute, then interrupts it, then kills it (before, it could wait for ever). With Electrum servers each server has its
  own minute for its calls of an operation (a slow first one used it for all).
- pzsolve: the kangaroo no longer throws away its saved walkers when a start ends before they move (no compute device, an interrupted
  GPU self-test or kernel build, a `dp.bin` it cannot use): `herd.bin` is taken by any later session of the same `--node`, and removed
  once its walkers walk, so a run that does not stop well still leaves none. The line that says how to resume a kangaroo gives its
  `--node`, and a resume with another one is told. `--near` is refused outside the linear search, and given late it warns that the
  keys already scanned have no near misses; the state is taken before `near.txt` is synced. The signal handler and the
  `--stop-with-input` thread no longer reach the run once it is freed.
- The test of the session no longer hangs one time in about 500 (the case of an old copy of the first transaction waited for a
  moment that a fast block could skip); a case that runs more than 5 minutes fails with its name.
- The time estimates of a campaign of the shared network (pzsolve, and the network line of pzpeer) take the measured cost of its
  walk, 1.18·√W (net-06), not the 1.06·√W of a campaign alone: they were about a tenth short.
- `--near`: the near misses are on the disk before the blocks they prove are recorded as scanned (a power cut left blocks marked
  as scanned with their near misses missing).
- pzpeer: a session asks its chain and its fee rate before it opens the channel, so that its hello reaches the partner within the 2
  seconds a new connection is given (with Electrum servers it came later, and the session failed). A side waiting at a relay takes the
  partner's own connection if it comes meanwhile. The contacts of `peers.txt` are tried before the addresses found in the table of
  BitTorrent, which could push them out. The records go again at the first contacts after none only when one went to no other node
  meanwhile (not after every outage). The own alerts of a node come before those it sends as a custodian, and an alert that finds the
  queue full is tried again with the next record of the pair instead of an hour later. Under `nohup` the terminal closing no longer
  stops pzpeer. On macOS a new seed, the state of a session and the custody reach the disk itself (`F_FULLFSYNC`).

## [1.23.4] — 2026-11-14

Fixes from a second review of the program, each case tried as an attacker would. Campaigns of 1.23.3 resume as they are.

### Security
- `merge` of an encrypting campaign that found the key cleared nothing: the seed and `dp.bin` stayed, and the key came back in two
  seconds. `merge` now ends as `hunt` does: the seed goes from `campaign.conf`, the state and `dp.bin` are deleted, the folder must be
  its owner's alone, and the command fails when `FOUND.txt` cannot be written.
- Encryption added to a campaign on resume left the seed in `log.txt` (the line of `--part`): those lines are taken out of the log.
- The files of a campaign (the lock, the log, `stats.csv`, `dp.bin`, the temporary files) are never opened through a link: a folder
  copied from elsewhere could hold one that points at another file of the user, which the lock emptied. The log and `stats.csv` are
  made for the user alone.

### Fixed
- A key that could not be encrypted at the find (the system gave no random numbers) left a `FOUND.txt` saying "not saved" and wiped
  the campaign, whose next run then said "found" without searching. Now nothing is written and the state stays as it was, so that a
  new search finds the key again.
- For the kangaroo, a key that could not be written to `FOUND.txt` is no longer said to be found again by the same command: a new
  session walks another way.

## [1.23.3] — 2026-11-13

Fixes from a complete review of the program. Campaigns of 1.23.2 resume as they are.

### Security
- An encrypting campaign (`--encrypt-pubkey`) left behind what gave the plain key back: the state said where the search stood at the
  find and the seed gave the order of the blocks, so whoever read the folder afterwards found the key again in a tenth of a second.
  At the find the seed is now replaced with zeros in `campaign.conf` and left out of `FOUND.txt` and of the log, and `state.dat`,
  its copy and `dp.bin` are deleted. A copy of the folder made before the find still holds the seed (USAGE 11.5).
- The folder of a campaign is its owner's alone (mode 0700), and a folder that belongs to another user is refused: other users of the
  machine could read the state of a search, or plant a folder with links and a configuration of theirs.
- The scratch folders of the kangaroo self-tests (`pzkang*` in the temporary folder) are cleaned up only when they are real folders of
  this user, and links are never followed: another user could plant one whose cleanup deleted this user's files.
- Windows: the libraries of the GPU drivers (`nvcuda.dll`, `OpenCL.dll`) are loaded from the system folder only, and `powercfg` is run
  by its full path. On a machine without that driver Windows went on to the current folder, so a campaign folder holding a file of
  that name (a share, a downloaded archive) ran it inside pzsolve. On macOS the OpenCL loader of Homebrew is opened by its full path
  for the same reason. `PZ_CUDA_LIB=PATH` names a CUDA driver to load by its path (the tests give their simulated driver this way).

### Fixed
- A key found while the disk was full was lost: `FOUND.txt` could not be written, yet the state recorded the block as searched, and the
  next start ended "completed without the key". A campaign now keeps two blocks of the disk for `FOUND.txt` (`reserve.bin`) and gives
  them back at the find; if the file still cannot be written, the state is left as it was before the find, so that the same command
  finds the key again, and the exit status is 2.
- Linear search on a GPU: the size of a launch could stop adapting to the device (it was compared with a claim that the scheduler had
  rounded down to 1024 keys).

### Tests
- `tests/e2e.sh`: links planted in the scratch folders, a find on a full disk, the mode of the campaign folder and `reserve.bin`, and
  what an encrypting campaign leaves after a find. `tools/verify.sh`: a `nvcuda.dll` beside the Windows exe is not loaded.

## [1.23.2] — 2026-11-08

### Added
- When a key is found, a few lines under the box congratulate the finder and give the donation addresses of the README (Bitcoin and
  Lightning), with a pointer to the README of the official repository to check them against. They are not shown when the key could not
  be encrypted and was lost.

## [1.23.1] — 2026-11-07

### Changed
- `report.txt` and `status.json` are rewritten once a minute, together with the checkpoint, instead of every 5 seconds: the panel on
  the screen still updates every second, and the files are still written 2 s after the start, after a system suspension and at the end.
  A campaign folder kept in a synced folder (OneDrive, Dropbox, Google Drive) or on a network disk is no longer uploaded every few seconds.

## [1.23.0] — 2026-11-05

### Removed
- Eight options of `hunt` that had no use in a normal campaign. Each one is now refused with exit status 2 and a message that says what
  to use instead (USAGE.md 3):
  - `-H`/`--hash160`: give the address with `-a`.
  - `--new` and `--resume`: `hunt` creates the campaign or resumes it.
  - `--report-interval` and `--checkpoint-interval`: the panel is updated every second, the state is saved every 60 seconds.
  - `--gpu-api`, `--no-tune`: the environment variables `PZ_GPU_API` and `PZ_NO_PROFILE`, as before.
  - `--isa`, `--prio-gpu`: the new environment variables `PZ_ISA` and `PZ_PRIO_GPU`, same values. `bench --together --prio-gpu` stays.
### Changed
- `help hunt` lists the options most campaigns need first and the others under "ADVANCED".
- A test build reports its own version, `X.Y.Z-test.COMMIT`: a build from a commit that is not the release tag no longer shares the saved
  GPU tuning and machine profile with the release (RELEASING.md).
- `tools/verify.sh` runs its sections in parallel (`-j N`, default the number of cores): about 15 minutes instead of 50 on four cores.

## [1.22.0] — 2026-11-05

### Added
- `campaign.conf` gets `requires=X.Y.Z` when the campaign encrypts the key: a version older than that refuses to resume it instead of
  writing the key in plain. Versions 1.21.0 and 1.21.1 do not know the line and still ignore it (ALGORITHMS.md 12.5).
- `docs/en/RENTED.md` and `docs/it/AFFITTO.md`: the checklist for a computer you do not control, from the checksum of the binary to
  `decrypt-key` at home. The README has a table of what was measured, on which machine and in which version.
- `tools/check_docs.sh` keeps the English and Italian guides in step (sections, options, variables, links); run by `verify.sh` and the CI.
### Changed
- The release workflow refuses to publish unless the CI has passed on the exact commit; the macOS Intel binary is built on the Apple
  Silicon runner and self-tested under Rosetta (the Intel runners of GitHub are scarce and being retired).
- Sources: `platform.c` is `platform_posix.c` and `platform_win.c`; `main.c` keeps `main`, the dispatch and the help texts, the commands
  are in `cmd_*.c`; the interleaved form of the linear kernel left `pz_linear.cl` for `tools/research/`. No change of behaviour (output
  byte-identical to 1.21.1 on every command).
- Tests: the timing checks of `tests/e2e.sh` and `tools/verify.sh` wait for a state with a deadline instead of sleeping a fixed time,
  so a loaded machine no longer gives false alarms.
- CHANGELOG: the entries from 1.19.0 on say what changed in a few lines and point to RESEARCH.md for the measurements.

## [1.21.1] — 2026-11-04

### Fixed
- A file size limit (`ulimit -f`) no longer kills the program: the write past the limit fails like a full disk, is logged, and the session
  is saved. The two write signals are ignored for every command, `merge` included.
- The box printed when the key is found no longer says "saved in" when `FOUND.txt` could not be written: it says that the key was not
  saved and must be copied from the screen (plain and encrypted).
- Ctrl-C during the one-time tuning of the GPU kernels stops it at once (the default variant is kept and nothing is saved) instead of
  being ignored until the measurements end.
- Documentation and help texts: the Italian guide describes the tuner of 1.20.0; the exit status of `merge`, the log lines of the machine
  profile, the sizes in ARCHITECTURE.md, the default of `--part` and the memory rule of `merge --mem` are corrected; `FOUND.txt` has no
  0600 permissions on Windows.

### Changed
- Source and tests, no change of behaviour: every file says in its first line what it holds, its parts are in reading order with a section
  comment each, names say what they hold or do (`deep` to `full` in `doctor`, `load_chunk` to `load_block`), five unused functions and two
  macros are gone, and the steps of the tests have names.
- Internal reorganization, no change of behaviour: `gpu_tune.c` is split into the tuner, the benchmark (`gpu_bench.c`) and the self-test of
  the choice (`gpu_tune_selftest.c`), and the encoding of a GPU kernel variant lives in `gpu_variant.c`, so that a new variant is one table
  row. The output of every command, the files, the codes of the variants and the saved choices are the same as in 1.21.0 (checked on `help`,
  `version`, `selftest`, `bench --variants`, `decrypt-key` and a harness over 1152 device configurations); the kernels and the PTX modules
  are untouched. ARCHITECTURE.md section 1.

## [1.21.0] — 2026-11-01

### Added
- Encrypting the key that is found, for computers that someone else controls: `hunt` and `merge` take `--encrypt-pubkey HEX` (66 digits
  compressed, 130 uncompressed) or `--encrypt-password` (typed hidden, twice; the public key is made from it), and the box on the screen,
  `FOUND.txt` and `log.txt` then carry only a line `PZENC1:...` (ECIES on secp256k1, Base58Check), safe to send anywhere, and never the key
  nor its public key. An invalid key stops the command before anything is created; the program never falls back to showing the key.
  USAGE section 11, ALGORITHMS section 12.
- The public key is stored in `campaign.conf` (`encrypt_pubkey`, written only when it is used: the files of other campaigns and of older
  versions are unaffected); a resumed campaign uses it, a different key is refused, a campaign started without one can be given one on
  resume. Core dumps are turned off (Linux, macOS; on Linux the process is also non-dumpable), the key is wiped as soon as the line is
  made, and the password buffers and the scrypt memory are locked in RAM when the system allows it.
- `--on-found COMMAND` runs a command once, with `/bin/sh -c` (Windows: `cmd.exe /c`), after `FOUND.txt` is written, in a thread of its
  own (at most 120 seconds; the search does not wait for it), with `PZ_FOUND_SEALED`, `PZ_FOUND_ADDRESS`, `PZ_FOUND_PUZZLE`,
  `PZ_FOUND_CAMPAIGN` and `PZ_FOUND_FILE` in its environment, never the key. It is not stored in the campaign: give it at every start.
- New commands `encrypt-keygen [--random]` (the public key from a password, or a random key pair; a warning under 12 characters) and
  `decrypt-key [--raw-key] [--address A] [FILE | -]` (finds every `PZENC1:` token in a file or a pasted text, says "wrong password" by key
  id before trying, prints the key, its WIF and its two addresses, and checks the key against the `address:` line or `--address`: exit
  status 1 on a mismatch or a wrong secret, 2 when the input is not usable). The password is read with the echo off, never from an argument.
- pzcore: `seal.h` (HMAC-SHA256, HKDF-SHA256, PBKDF2, scrypt, a constant-time comparison, a wipe the compiler cannot remove, the seal and
  its opening) and `pz_ec_mul`, tested with the vectors of RFC 4231, 5869 and 7914 and an independent Python generator. Tests `t_kdf`,
  `t_seal`, `t_encrypt`, the encrypted key in `selftest`, the end-to-end cases of the three modes and `tests/pty_seal.py` for the hidden
  prompt; `PZ_TEST_SEAL_LOG2N` (tests only, with a warning) lowers the scrypt cost.

### Changed
- When the key is encrypted, `FOUND.txt` no longer carries the public key of the key found (nor the private key): it has the date, campaign,
  puzzle, address, seed, version, the key id and the encrypted line. Without `--encrypt-pubkey` or `--encrypt-password` everything is as before.

## [1.20.1] — 2026-11-01

### Added
- Legal notice: `pzsolve version` also prints the copyright, the license and the warranty disclaimer, and the main help ends with them.
  The new `NOTICE` file adds two terms under section 7 of the AGPL (whoever conveys pzsolve keeps the notices and marks a modified version
  as different from the original); the README states them, and the pzcore folder has its own `NOTICE` and `LICENSE`.

### Changed
- pzcore carries the copyright and the license of the project in its own files: the `SPDX-FileCopyrightText: 2026 Leonardo Boulay` line
  next to every `SPDX-License-Identifier`, also in the generated test files, and `lib/pzcore/LICENSE`.
- Documentation: the README (English and Italian) mentions `bench --variants` and says that `make ARCH_NATIVE=1` does not make the program
  faster: the engine is chosen at run time and the GPU kernels are compiled by the driver (measured on an AVX-512 IFMA machine, the same
  speed within the noise of `bench`).

## [1.20.0] — 2026-11-01

### Added
- `bench --variants [--turns N]`: every variant of the linear and kangaroo kernels of each GPU, measured fairly in one run (3 s of warm-up,
  then three turns: in order, reversed, in order), one table per kernel with the code that `PZ_GPU_HALF` or `PZ_GPU_NK` takes, the rate of
  each turn, the median, the tuner's saved choice and what its rule would pick, then the rate of the variant in use at launches of 0.15 to
  1.4 s. Nothing is saved unless `--retune` is given too. `bench` also prints the launch length of each kernel. USAGE section 7.5.
### Changed
- Panel: with more than four GPUs, or when their names do not fit the terminal, the speed line gives their sum with the least and the
  greatest (`GPU x8: 7.21 G (870.12 M to 912.33 M each)`) instead of a rate per card; `report.txt` and `status.json` list every worker.
- GPU tuning: 3 s of warm-up, the variants measured in turns (in order, reversed, in order, so that the drift of a laptop clock weighs the
  same on all), the median of each, the default kept unless another is more than 1 % faster, and among the variants within 1 % the largest
  window (the most walkers per work-item for the kangaroo), since on the RTX A2000 windows 256 and 512 measure the same in `bench` and 512
  is faster in a campaign. The log gives the code to force the choice. About a minute per kernel once, then saved. ALGORITHMS section 5.
- GPU launches: their length is tuned per device (the shortest of 0.15, 0.35, 0.7 and 1.4 s within 1 % of the best) instead of the fixed
  0.35 s of 1.19.x, and saved in `gpu_tune.txt` next to the variant (a line of 1.19.x without it means 0.35 s). The speed of a GPU on the
  dashboard and in `bench --together` is taken between the ends of its launches, so that long launches do not make it swing. Measured on
  simulated devices only. ALGORITHMS section 5.
- GPU kangaroo: a second form of the metadata of a step, "tags once" (`PZ_GPU_NK` with `m` after the letter: `128m`, `128bm`, `128cm`), as
  variants the tuner measures next to the kernels of 1.19.2, which stay the default (every module byte for byte the PTX of 1.19.2). On the
  RTX A2000 Laptop (`bench --variants`, medians of three turns) `NK` 64 quads with the tags once gives 922 M steps/s against 896, the
  tuner's pick (+1.5 % over 1.19.2's best), while `NK` 128 quads loses (743 against 908). Six more PTX modules, 243 KB more in the
  executable. The byte audit of the step and the measurements: RESEARCH.md section 5.
- CUDA linear search: three forms were built and timed on the RTX A2000 Laptop with `bench --variants` and none is kept: two that move hash
  work onto the FMA pipe (863.6 against 875.0 M keys/s and 808.2, window 512 with the register cap) and one with the hash of a pair of
  keys and the curve arithmetic of the next in one instruction stream (851 against 888; it stays in the source under `PZ_INTERLEAVE`, no
  module uses it). The CUDA linear kernels are those of 1.19.2. The issue model, the floor of the hash160 filter and the counts: RESEARCH.md section 5.
- Documentation: RESEARCH.md 8.14 searches once more for a lower constant of the kangaroo (the literature from Pollard 2000 to 2026 and
  ten ideas priced in the exact pair model and the walk model): none reaches 1 % under the constraints of a campaign, and the program is
  unchanged. New scripts in `tools/research` (`ksched.py`, `kprecomp.py`, `klambda.py`, `kideal.py`, the option `cheap=1` of `kwalk.c`).

## [1.19.2] — 2026-10-31

### Changed
- CPU, AVX-512 IFMA lanes: four masks that did nothing are gone and a product with an added term starts from that term. Measured on one
  thread (Sapphire Rapids class Xeon): kangaroo lanes +1.8 %, pairs +1.3 %, linear search +0.9 %. Same results bit for bit. RESEARCH.md
  10.8 also records the attempt to schedule the IFMA curve step by hand, none of it faster on every processor model.
- CUDA linear search: the energy model per key from the SASS (`tools/research/sass_energy.py`) and the measurement on an RTX A2000 Laptop
  of the three changes it suggested: one cost 2.8 % and the others did not win it back, so the CUDA linear kernels are those of 1.19.0.
  RESEARCH.md section 5.
- `tools/gen_ptx.py` compiles every module in a process of its own (in one process the output of the largest module changed between runs,
  and `--check` failed at random). The callseq comments of all the modules are renumbered; no instruction changes.
- GPU kernels, GPU tuning and the choice of CPU threads and GPUs are those of 1.19.1; the tuning is saved per version, so the first run of
  this version measures the GPU variants again.

### Added
- GPU kangaroo: a *block herd* kernel variant (`kang4g`, "NK 4 block herd"), forced with `PZ_GPU_NK=4g`: 4 walkers per work-item in
  registers for the whole launch and one field inversion per block of 128 work-items and round, so a step touches the device memory only
  for its 2-byte history. Bit-exact against the CPU; slower than `NK` 128 quads on the A2000, as the model expected, so no tuner measures
  it. ALGORITHMS section 5, RESEARCH.md section 5.

## [1.19.1] — 2026-10-30

### Changed
- Linear search: the default size of the blocks is as large as the range allows, with at least 16 blocks for each share of `--part` and at
  most 2^36 keys (a range of 2^36 keys now gets blocks of 2^32, was 2^24): a GPU launch cannot cross a block, and with blocks of 2^24 keys
  an RTX A2000 Laptop ran 647 against 827 M keys/s. The value depends on the range and the number of shares only; existing campaigns
  keep the value in `campaign.conf`; BSGS and the kangaroo are not changed. **Upgrade note.** The computers of a `--part` split must have
  the same block size: if one of them runs a version before 1.19.1, give `--chunk-bits` explicitly on all of them.

## [1.19.0] — 2026-10-30

### Changed
- CPU linear search, lane engines: the hash filter has fewer operations on the two vector ports (the majority function of SHA-256 is one
  `vpternlogd` with AVX-512, the RIPEMD-160 round constants are broadcast operands from a table). Measured on one thread: avx512i +2.0 %
  (275 to 269 instructions per key), avx512v +1.2 %; AVX2 and SSE2 within 0.1 %. Same digests, same candidates.
- Documentation: RESEARCH.md 10.7 records what was measured and dropped (the step of the next points interleaved with the hash of the last
  batch, the compiler variants for the engine object) and why the second thread of a core adds 9 to 12 % on the laptop of 10.5.

## [1.18.0] — 2026-10-30

### Changed
- CUDA linear search: the additions of SHA-256 and RIPEMD-160, the carries out of the field addition chains and the normalization of x
  are `IMAD` with a multiplier of 1 read from the constant bank, so they run on the FMA pipe, which was idle three quarters of the time,
  and the ALU pipe that bounds the kernel keeps only the rotations, the logic and the carry chains: per key on sm_86 2249 to 1743
  ALU-pipe instructions and 515 to 1385 FMA-pipe ones (3155 in all, was 2791), modelled time per key -20 to -23 % (RESEARCH.md section 5
  has the model, checked against the 793 M keys/s of the A2000, the budget per phase, and why the borrow of a subtraction cannot be
  taken that way: `ptxas` carries the inverse of the PTX borrow, which the first test build read wrongly on the card). Counted with
  ptxas; measured on an RTX A2000 Laptop: 793 to 882 M keys/s in `bench` (+11 %), self-tests passing. The register-capped variants no
  longer spill; the kangaroo kernels are unchanged; the OpenCL kernels compile to the same code as before.

## [1.17.0] — 2026-10-30

### Changed
- AVX-512 IFMA lanes (linear search and kangaroo): the subtractions that follow a product (`x3 = λ² − x − x'`, `y3 = λ·t − y`) go into the
  product as a term added before its reduction, so their carry pass, a stage of the dependency chain of every point, is gone. Measured on
  one thread: linear search avx512i +2 % (the curve part alone +7 %), kangaroo avx512i +3 to +4 %, avx512i2 unchanged; 280 to 275
  instructions per key (kangaroo lane step 143.2 to 141.1). The 29-bit engines are unchanged.
- Kangaroo: where RAM does not set `dp-bits`, the automatic value is one bit lower (the overhead term goes from 0.01 to 0.005 of `√W`):
  the delay of the last distinguished point, about `0.95·K·2^dp-bits` steps (measured in the walk model and on the real curve, RESEARCH.md
  8.13), falls from 0.7 % to 0.35 % of the expected steps on average, for twice the table and twice the `dp.bin`. CPU herds and GPU herds
  up to 2^20 walkers on machines with 32 GB and more are affected; with 2^22 walkers memory sets `dp-bits` below 128 GB and nothing changes.
  The jumps, the walks and the format of `dp.bin` are unchanged: campaigns resume as they are, and the points already recorded stay valid.
- `pzsolve kstat` prints the standard error of the mean.
- The build refuses an incompatible pointer type (`-Werror=incompatible-pointer-types`): GCC only warns about it by default, and a call
  with the address of a pointer instead of the pointer reads the wrong memory.
- Documentation: RESEARCH.md 10.6 has the cycle model of the lane engines (ports, the AVX-512 clock, where the cycles of a key go) and the
  changes that were measured and dropped; 8.13 records the last percent of the kangaroo constant: finer start bands gave 0.8 ± 0.3 % on the
  real curve and 0.23 % in the exact model of the puzzles, below the bar for a change of the walk, and the finding that a resume draws new
  starts, so the bands and shares are not part of what a resume needs (`tools/research/kang_schema3f.txt` keeps the layout).

## [1.16.0] — 2026-10-30

### Changed
- The first-run measurement of CPU threads and GPUs also tries one CPU thread fewer per CUDA device, and leaves out a GPU whose loss is not
  measurable (a configuration without it within 1 % of the best, confirmed in alternating pairs like a faster one): on a laptop such a
  device costs more shared power than it returns. Measured on an i7-11850H with an RTX A2000 and an Intel UHD: the integrated GPU gave
  16 M keys/s and cost the other GPU 90 M; without it the linear search went from 865 to 948 M keys/s. `--retune` redoes the choice.
- Documentation: RESEARCH.md 10.5 models the linear search on that laptop against the owner's measurements: port floors of Tiger Lake
  (112 cycles per key against 158 measured), why SHA-NI does not pay there, the issue ceiling of the A2000 (the kernel at 73 % of its
  ALU pipe), power sharing between CPU and GPU; what the options do.

## [1.15.1] — 2026-10-29

### Fixed
- OpenCL on GPUs other than AMD (Intel, NVIDIA through OpenCL): the add-with-carry and subtract-with-borrow helpers of the field
  arithmetic went through a 64-bit sum, which the Intel Xe-LP compiler got wrong in a chain of eight: the start-up self-test reported
  "operation 3 differs from the CPU" and the device was excluded. They use 32-bit operations only now. Reported on an Intel UHD (Tiger Lake).

## [1.15.0] — 2026-10-29

### Changed
- CPU kangaroo: x and y of a step are made canonical by the fast reduction (the full one only for a vector with a lane that needs it); the
  jump coordinates are stored as `K1 − c`, so the subtractions of the step become additions; on AVX2 the jump entries are read as rows and
  transposed. Measured on one thread against 1.14.0: avx512i2 +7.2 %, avx2v +5.3 %, avx512i +4.0 %, avx512v +2.7 %. The jumps, the walk
  and `dp.bin` are unchanged.
- BSGS with AVX-512 IFMA: the x of a giant step is made canonical by its top limb, about 5 % faster.
- Kangaroo walker starts about twice as fast: herd starts are made affine 64 at a time, and every `k·G` uses signed 8-bit comb windows (at
  most 32 additions instead of 64; table 264 KB, was 61 KB). A GPU herd of 2^22 walkers needs about 26 s of CPU time to start instead of 52.
- GPU kernels: a subtraction from a normalized value skips its second correction (CUDA kangaroo step 1167 to 1132 instructions, linear
  search -1 % per key on sm_86; one spill of `kang128b` is gone).
- OpenCL on AMD GPUs (gfx9 to gfx12): the field product in inline assembly on `v_mad_u64_u32` with its carry out (multiplication -22 %,
  kangaroo step loop -11 %, linear loop -17 % in instructions). A build or a start-up self-test that fails with it falls back to the C form.
- GPU figures are instruction counts (ptxas, clang for AMD); not yet measured on a card.
- Documentation: RESEARCH.md 10.4 and section 5 log what was measured, including the tables that do not pay (the jump table in L1 or in
  shared memory, a larger linear window, precomputed hash words, per-jump constants).

## [1.14.0] — 2026-10-29

### Changed
- **Upgrade note.** BSGS tables are saved as `PZBSGS03`, which 1.13.0 and earlier cannot read. `PZBSGS02` and `PZBSGS01` tables are still
  read and used as they are.
- BSGS: the baby-step table keeps a 32-bit tag in 4-byte cells (16 per cache line, 70 % in use): 5.7 bytes per baby step instead of 11.4, so
  twice the baby steps fit in the same RAM. When a tag matches, the baby index is found again by a small search, a few milliseconds about once
  per 3·10^8 giant steps. Measured: 0.78 of the time per giant step at equal table size, and about half the time per key when RAM limits the
  table (2^51 keys with `--mem 0.05`: 3.5 s to 1.2 s). With 128 GB BSGS now pays off up to about 2^71 keys instead of 2^69.
- Faster field inversion on the CPU: divsteps in variable time instead of an addition chain (about 4 times faster), and the SIMD lanes are
  inverted together through one scalar inversion. Herd start about twice as fast; a walker start needs one inversion instead of two.
- Faster lane arithmetic: the canonical reduction needs two carry passes instead of four; subtractions that only feed a product skip their
  carry pass; on AVX2 the column sums of a product are no longer reordered and spilled by the compiler.
- Faster lane kangaroo: x3 and x − x3 come from the denominator with lazy carries, and restarted walkers are found a vector at a time.
  Measured on one thread against 1.13.0 (interleaved runs): about +38 % on AVX2 lanes, +18 % on AVX-512 lanes, +10 % with IFMA.
- Faster CPU linear search: x and the parity of y are made canonical by a fast path; the filter compares hash160 word 2, which needs four
  fewer RIPEMD-160 steps; the lane engines hash eight batches at a time; with IFMA the batch inversion runs as two chains of products; on
  AVX-512 the field products are inlined; on AVX2 the byte swap and the rotations by 8 are one byte shuffle; the scalar engines form
  x(C)+x(i·G) once per pair. Measured on one thread against 1.13.0: avx2v about +25 %, avx512v about +20 %, avx512i +12 %, avx2 +10 %,
  avx512 +9 %. BSGS giant steps on lanes: +4 to +13 % from the same arithmetic.
- GPU (CUDA): one `IMAD.WIDE` per limb product in the field multiplication and square (multiplication 235 to 121 instructions); the modular
  inversion does its divsteps in variable time (about 14,600 instead of 22,000 instructions per warp); the jump table is read with 16-byte
  loads; RIPEMD-160 byte swaps are one `PRMT`; x3 on one pair of borrow chains. Instructions per kangaroo step on sm_86: 1868 to 1167
  (-38 %); per key of the linear search: about -18 %. Counted with ptxas, not yet measured on a card; the tuner picks the fastest variant.
- GPU linear kernel: filter on hash160 word 2, and the keys that pass are hashed again in full on the device, so a hit matches all 160 bits.
- OpenCL on AMD GPUs (and pocl): field arithmetic on add-with-carry chains, 40 to 49 % fewer instructions per kangaroo step (static counts
  for gfx906, gfx1030, gfx1100). NVIDIA through OpenCL and Intel keep the portable code.
- `bench`: kangaroo ceilings computed from about 1300 instructions per GPU step instead of 2200.
- Documentation: the expected cost of the kangaroo at the scale of the open puzzles is about 1.04·√W (was 1.02, from a run of 300 trials);
  RESEARCH.md 8.12 splits the cost from the counting bound 0.67 to the measured 1.06 and lists the ideas measured and dropped (longer jumps,
  other jump distributions, finer bands, other species); 8.11, 10.2 and 10.3 and section 5 log every measurement of this release.
- Research tools: `tools/research/kwalk.c` (the walk of schema 3 with the split of its cost) and `tools/research/kcover.py`.

## [1.13.0] — 2026-10-29

### Changed
- **Upgrade note.** Every command refuses the options it does not use (for example `list -t 7`) instead of ignoring them; `--mem` must be
  at least 0.001 (GiB); `merge` exits with 2, not 1, when a file cannot be merged. Scripts that relied on the old behaviour must be adjusted.
- The kangaroo table of distinguished points uses 16 bytes per entry instead of 32: it keeps the index of the point in `dp.bin` and reads the
  distance back when two kinds of walkers meet. Where memory sets `dp_bits` it is one bit lower, which halves the delay before a collision is
  seen. Modelled with 2^22 kangaroos on puzzle 140: 1.10 to 1.08 times the square root of the range with 32 GB, 1.15 to 1.10 with 16 GB,
  1.23 to 1.15 with 8 GB; 1 % or less with 2^20 kangaroos or 64 GB and more. Measured on 2^40 and 2^44 keys with a small `--mem`: 8 to 9 %
  fewer steps. `dp.bin` is unchanged; points that are in no log keep a full copy, so a solve is never lost.
- CPU threads go to distinct physical cores first for every thread count (`-t N`, the machine profile, `bench`), not only with `--limit`.
  This matters where the two threads of a core are numbered next to each other (Windows, Intel hybrid CPUs).
- Kangaroo on CPUs with SMT: the mixed layout (vector step on the first thread of each core, BMI2/ADX step on the other) is measured at
  start-up and used when it is at least 3 % faster (+14 % on the CI runner). `PZ_KANG_MIX=0` turns it off, `=1` forces it.
- BSGS: the table is built in blocks sized to the table and the threads, so small tables use every thread (`bench`: about 3 times faster on 4
  cores). The table is the same.
- GPU kangaroo: the tuner also tries wider herds (4 and 8 times the work-items per compute unit with OpenCL, at most 128 per unit on Intel
  GPUs; 512 threads per SM on CUDA cards with more than about 80 SMs); a launch runs up to 256 rounds when the device memory allows and reads
  its two counters in one transfer; the replay at the end of a launch loads the jump history 8 entries at a time. Correctness checked on the
  simulated CUDA driver and on pocl; the gain on real cards is not measured yet.
- CUDA linear search: the register-capped variants get a grid of 4 blocks per SM, the occupancy they were built for.
- Terminal output:
  - the kangaroo start-up is one line with grouped digits (the internals with `-v`);
  - GPU tuning prints one line per GPU and kernel, with the rates of every variant in `log.txt`;
  - the jump table id is shown with any `--node`, 0 included;
  - a completed campaign is shown as completed, and resuming it prints one line and exits with 1 instead of running a session;
  - usage errors show the usage line of the command;
  - the main help has one line per command, and `hunt --help` documents every option and the complete rule of `-m auto`;
  - `info` and finished campaigns show no rows of zeros; chance windows stop at the first 100 %; a range size is written `2^k` only when
    it is exact;
  - tables of `bench --together` are never wrapped;
  - a GPU runtime switched off by `PZ_NO_CUDA` or `PZ_NO_OPENCL` is reported as disabled.
- The thinning of the table of distinguished points is an information, not a warning: it is expected in about one run in five where memory
  sets `dp_bits`.
- Documentation: shorter guide (the option list is `pzsolve hunt --help`), a glossary, the GPU memory of the kangaroo (40 MiB plus 256 bytes
  per kangaroo), `--mem` in GiB; release archives ship only the user documents.

### Fixed
- A failed opening of `dp.bin` for a reason other than a missing file (permissions, disk error) recreated it empty.
- A thread count in `machine_profile.txt` beyond the worker limit stopped the kangaroo with "too many workers".
- `doctor` on an empty campaigns folder, and `--mem` below 32 bytes, did undefined operations.
- `log.txt` got two "final state" lines in log mode and none with the panel; log records written to a file or a pipe were split; `--ui line`
  and `info` ignored the terminal width; the panel stopped at 110 columns instead of 128.
- The first-run measurement wrote its kangaroo set-up into the log of the new campaign.
- `split` and `puzzle --rate` accepted speeds with blanks, a sign or hexadecimal digits.
- Messages: a foreign `dp.bin` is "left as it is", a log with another jump table means "another seed", GPUs are always named `GPU[n]`;
  `FOUND.txt` writes the key with `0x`.

## [1.12.0] — 2026-10-29

### Removed
- `--limit 100` is refused (no `--limit` means no limit, as before). Scripts that passed 100 must drop the option.

### Changed
- **Upgrade note.** The kangaroo log `dp.bin` now starts with `PZDP5` and records the jump table: versions before 1.12.0 cannot read a log written
  by 1.12.0. Logs written by 1.9.1 to 1.11.0 (`PZDP4`) are still read, appended to and merged.
- Faster hash160 stage: the filter on the first word of the hash no longer computes the unused part of RIPEMD-160, the SHA-256 round constants
  are read as broadcasts from memory and the byte swap is one shuffle on AVX-512. Instructions per key: -7.4 % (AVX-512), -3.6 % (AVX2); time
  measured in interleaved runs: about -4 % on both.
- Faster lane kangaroo: the second pass of a step reads only the parts of the jump it needs. Time per step: -4.5 % (AVX2 lanes), -3.8 %
  (AVX-512 lanes); about -2 % of instructions on the IFMA variants.
- The kangaroo picks its distinguished-point density with a margin of 1.3 instead of 4 when memory decides it, now that the table is thinned in
  place: the expected cost on machines where memory sets `dp_bits` drops (modelled, puzzle 140 with 2^22 kangaroos: 1.19 to 1.10 times
  the square root of the range at 32 GB, 1.33 to 1.15 at 16 GB), and `dp.bin` grows 2 to 4 times.
- `merge` refuses a log written with another jump table (another seed or another setup) instead of merging points that can never meet.
- Terminal output reviewed: fewer lines at start-up (one line per GPU, the machine profile only when it changes something, the jump table id
  only with `--node`) and at the end (the path of `FOUND.txt` once, no time left after the end); one format for numbers, percentages,
  durations and labels; output wraps at the width of the terminal (at most 128 columns); every command has a short help
  (`pzsolve help CMD`, `CMD --help`); errors say what to do next; `where` says whether the key was already scanned; `info` and `list` show
  the time left at the last speed for a stopped campaign and hide it for a finished one. Labels of `report.txt` changed slightly (the keys of
  `status.json` did not; its ETAs are null when beyond the model).
- The documentation describes what the program does now; history and discarded variants are in `docs/en/RESEARCH.md`; the Italian guide has
  the same sections as the English one.

### Fixed
- Windows: a `dp.bin` larger than 2 GiB could not be cut back after a torn write (resume failed) or, above 4 GiB, was cut to the wrong size.
- The kangaroo now warns when it has done 5 times the expected effort without a collision (wrong public key or range) instead of showing
  "100 %" and "<0.1 s" forever.
- BSGS: the speed shown after the table was built was the speed of the baby steps, and so were the time left and `list`.
- Custom targets were shown as "puzzle #0"; the BSGS notice about GPUs was printed without any GPU; the panel cut words at 80 columns;
  `help CMD` printed the main help; `split` refused decimal speeds such as `1.5G`.
- A big batch of points from a GPU could write the kangaroo log while holding the table lock.
- A fatal error could leave a torn line at the end of `log.txt` when another thread was logging at the same moment.
- The CI check of the puzzle database failed where Python's hashlib lacks RIPEMD-160 (`tools/ref.py` has its own now).

## [1.11.0] — 2026-10-28

### Added
- Exit status of `hunt`: 0 key found, 1 search finished without the key, 130 interrupted (Ctrl-C, SIGTERM, SIGHUP), 2 error. A failed final
  state save is now 2.
- A fatal error is also written to the `log.txt` of the campaign, so after a crash the log says why the run stopped.
- `tests/t_env.c` and the sanitizer flag `float-cast-overflow` in the sanitizer builds.

### Changed
- BSGS: the baby-step table is made of buckets of 8 cells (one cache line) at 70 % load: 11.4 bytes per baby step instead of 12.3, and a
  lookup compares the 8 tags of a cache line at once instead of walking a run of cells. The search is 20 to 30 % faster at equal RAM
  (measured on tables from 256 MiB to 3.5 GiB; on builds without SSE2 only the 8 % more baby steps remain). Tables are saved as `PZBSGS02`;
  those of earlier versions (`PZBSGS01`) are still read and used as they are (they need 12.3 bytes per baby step), but earlier versions
  cannot read the new files.
- The kangaroo thins its table of distinguished points in place: the peak memory drops from twice the limit to the limit (measured 521 to
  256 MiB at `--mem 0.25`), and the table grows only if the old and the new one fit in the limit together.
- The dashboard no longer waits for the table of the kangaroo while it is rebuilt (the pause was up to several seconds and could be taken for
  a system suspension), and the log of distinguished points is written outside the table lock, under a lock of its own: a slow or remote
  disk no longer stalls the search threads.
- GPU start-up: the devices are tested and tuned at the same time.
- Every environment variable is read once at start-up into a single structure (`src/envopts.c`); the names and values documented are the same.
  The flag variables follow one rule (on for any value but empty and "0"), a malformed number is reported once and ignored, and the warnings
  print at the start of every command.
- `bench -t` and the parameters of `kstat` are read strictly (`12abc` is refused, `bench -t 0` too). The Italian spellings of options
  (`--parte`, `--nodo`, `--limite`, `--ritara`, `--sveglio`, ...) are gone; the Italian state words in old `status.json` files are still read.
- `doctor` counts the suspensions in `log.txt` without reading the whole file, and `doctor --full` runs the GPU tests once.
- The state of a run (BSGS, linear workers, benchmark totals) lives in the run, so several runs in one process cannot interfere.
- One list of CUDA modules, one header for the numbers the kernel and the host share, one procedure for choosing the variant of either GPU
  kernel, one rule for the per-ISA objects of the Makefile. The code of the search loops is unchanged.
- The linear search ignores a hit beyond the end of `--range` (the last block is scanned in whole lanes): `-r 1:fff` no longer reports key
  0x1000.
- Documentation brought up to date (architecture notes, usage guides, tools table, release notes).

### Fixed
- A very small `--dp-bits` on a huge range (for example `hunt -p 140 --dp-bits 2`) overflowed a floating-point to integer conversion
  (undefined behaviour); the table size is now taken from the memory limit.

## [1.10.0] — 2026-10-28

### Added
- `shani`: on x86-64 CPUs with the SHA extensions (AMD Zen 2 and later, Intel Ice Lake and later) a hash160 stage that runs SHA-256 on
  `sha256rnds2`/`sha256msg1`/`sha256msg2` and RIPEMD-160 on the AVX2 lanes, as the pzcore batch implementation `"shani"` (selected by name)
  and as the CPU engine `--isa shani`, which `--isa auto` uses only if it measures faster. Without the extensions nothing changes. The
  start-up check of every engine now also compares each lane of its hash with the scalar code. `make test-shani` runs the SHA-extension code
  under QEMU.
- The CI prints the speed of the CPU engines on every Linux runner, runs the self-test against the simulated CUDA driver, checks the puzzle
  database, and has a job for the SHA-extension code under QEMU.

### Changed
- Release binaries are shipped without debug information and the CUDA modules are embedded compressed (a small LZ decoder, a dictionary per
  family of modules): the Linux binary goes from 22.7 MB to 2.8 MB, the release archives from 5 MB to 1.5 MB. Crash backtraces of release
  builds show no function names.
- `kangaroo.c` and `gpu.c` are split by concern (`kang_log.c`, `kang_cpu.c`, `kang_selftest.c`, `gpu_tune.c`, `gpu_selftest.c`); the code of
  the search loops is byte for byte the same.
- The algorithms document is split: `docs/en/ALGORITHMS.md` describes what the program does, `docs/en/RESEARCH.md` is the log of what was
  tried and measured. The time tables of the kangaroo and of the BSGS comparison are recomputed with the constant the program uses (1.06).
- `report.txt` and `status.json` are written without waiting for the disk (fewer syncs); `status.json` lists every worker.
- `hunt` checks everything that can make a campaign unusable before it creates the folder, and a command that created a campaign can resume it
  when an option was cut down to fit the range.
- The kangaroo accepts intervals up to 2^188 keys (puzzles up to 189), not 2^190: above 2^188 the 192-bit exponents of part of the walkers wrapped
  and a third of the herd was wasted.

### Fixed
- Two simultaneous starts of the same new campaign could leave the seed of the loser in `campaign.conf`, so that a later resume scanned other blocks
  than the ones recorded as done. The folder is now locked before the file is written, and `state.dat` records an identifier of the seed
  (a `state.dat` of another seed is refused; files without it still load).
- `where` on a BSGS campaign always printed the last step.
- Two processes writing the shared caches (`machine_profile.txt`, `gpu_tune.txt`) could corrupt them or lose an update; the temporary file of an
  atomic write is now unique and the profile is merged under a lock.
- The first-run measurement saved a result even when nothing had been measured, and spent two minutes at every start when the cache folder was not
  writable; Ctrl-C during it now ends it at once and removes its scratch folder.
- Options: a rejected option is named correctly (`hunt -xq`), empty or oversized values of `-d`, `-r`, `-f` are refused instead of being cut,
  `--mem` is capped; campaign names that Windows cannot have (`con`, `nul`, ...) are refused; log files are written in binary mode on Windows.
- Kangaroo log: a read error was taken for the end of the file and the log was truncated there; a file shorter than its header blocked the
  campaign; a log of a newer version was moved away as if older. The progress line showed the wrong counter for the points lost.
- GPU: the start-up check of the linear kernel missed the centre key and the edges of each group, so a defect there would have passed; after a
  failed launch the launch size is reduced; the recovery counter decays after ten good minutes (a laptop that sleeps every night no longer loses
  its GPU); the self-tests use a random generator per device; one transient API error at start-up no longer excludes the GPU; Ctrl-C interrupts
  a recovery wait; the kangaroo kernel is built once at start-up instead of eight times.
- Build: the objects that print the version are rebuilt when `VERSION` changes; an interrupted generation of an embedded file no longer breaks the
  next build; Apple clang is recognized from the first line of `--version` only; line endings are fixed for Windows checkouts. The release
  workflow runs one release per version at a time and refuses to replace the assets of a tag that points to another commit unless asked.

## [1.9.1] — 2026-10-28

### Removed
- The older kangaroo layouts (schemas 1 and 2) of the development builds, their log formats (`PZDP1` to `PZDP3`), the conversion of old logs and the
  variable `PZ_KANG_SCHEMA`. Every campaign now uses schema 3, which versions 1.8.0 to 1.9.0 already used for every campaign with at least 256
  kangaroos (all the open puzzles); campaigns started with them in that case resume unchanged (same jump table, same `dp.bin`).
- **Upgrade note.** Versions 1.8.0 to 1.9.0 used schema 1 for a new kangaroo campaign with fewer than 256 kangaroos, that is on small intervals
  (below about 2^28 to 2^32 keys, depending on the number of threads) or with `-m kangaroo` on a small range. Such a campaign has a `dp.bin` starting
  with `PZDP2`, whose points 1.9.1 cannot use: `hunt` keeps the file as `dp.bin.v2`, says so and goes on with a new log (the keys already scanned
  by that campaign are scanned again), and `merge` refuses the file with an explanation.
- Cost on small intervals: schema 3 takes more steps than schema 1 did when there are few kangaroos (`kstat`, 1 thread: W = 2^24, 1.54 instead of 1.40
  times √W; at 2^28 the means are about equal, 1.2, with a longer tail), and the same from 2^32 on.
- The option `--no-progress`: use `--ui log`.

### Changed
- The Italian documentation is reduced to the README and the usage guide (the translations of the algorithms, architecture, data, release and tools
  documents were behind the English ones); they point to the English documents.
- The simulators and measurement scripts behind `docs/en/ALGORITHMS.md` moved to `tools/research/`; `tools/export_public.sh` is no longer in the repository.

## [1.9.0] — 2026-10-24

### Added
- `lib/pzcore`: the crypto core (256-bit integers, secp256k1 field and curve, SHA-256 / RIPEMD-160 / HASH160, Base58Check, bech32 / bech32m,
  SegWit addresses, the keyed permutation, and HASH160 of many keys with the best SIMD code of the CPU) as a reusable C library with its own
  README, tests and examples. `make lib`, `lib-test`, `lib-examples`, `lib-cxx`. pzsolve links the same archive.

### Changed
- Every exported name of the core has the `pz_` prefix; functions return errors instead of exiting; the campaign permutation is unchanged
  (380 values produced by 1.8.1 are pinned by a test).
- Faster scalar code (median of interleaved runs, ns): RIPEMD-160 353 -> 160, SHA-256 of 33 bytes 313 -> 239, HASH160 of one key 640 -> 360,
  square root 8.7 us -> 3.9 us. The search engines run at the same speed as before (within the noise, checked by an independent review).
- A plain `make` builds `build/pzsolve` again after the library rules were added; the field code compiles with `-O0` and `-mbmi2 -madx`.

### Fixed
- Base58Check encoding overflowed a stack buffer for payloads above 124 bytes and cut texts that did not fit.
- 65-byte public keys with a coordinate written as x + p were accepted.
- The permutation could loop forever for a number outside its domain; the point at infinity of a Jacobian sum kept stale coordinates;
  a bit index of 256 or more read out of bounds; a valid empty Base58Check payload was refused.

## [1.8.1] — 2026-10-24

### Fixed
- The table of distinguished points of the kangaroo could be up to twice as large as the memory limit (`--mem` or the automatic limit):
  its size is a power of two and was rounded up past the limit. It is now rounded down.
- `doctor --full` no longer mixes the two output streams, and `doctor` reports a wrong option like every other command.

### Changed
- No line printed to the terminal is longer than 128 columns: the help, the log lines, `doctor` and `devices` are wrapped at a space
  with a hanging indent. The log file `log.txt` keeps whole lines.
- `selftest` no longer prints the log lines of the small campaigns it runs (temporary paths, "KEY FOUND"), and its results are one
  aligned list with one line per group.
- `bench`, `devices`, `puzzle`, `list`, `info`, `merge` and `kstat` print aligned tables and property lists, with shorter texts;
  `bench --together` is a table with one column per device. In `report.txt` some labels changed slightly: scripts should read
  `status.json`, whose keys are unchanged.
- `puzzle N`: no control characters when the output is not a terminal, digit groups in the key count, thousands separators in the
  years, a final line with the command that starts the campaign, and `--rate` accepts the suffixes k, M, G and T (`--rate 800M`).
- An unknown command or option gets a one-line message that points to `pzsolve help`, instead of the whole help text. All errors start
  with `ERROR:`.
- Long lines are broken at natural places: after a sentence, a semicolon, a colon or a comma, never between a number and its unit. The
  live panel drops whole fields instead of cutting one in half.

### Added
- arm64 (Linux and Apple silicon): the engines `neon` and `neon2` hash with NEON and the hardware SHA-256 of the CPU, chosen by the
  start-up benchmark (`--isa neon|neon2`); the linear scan is about 55-60% faster than the portable engine on Linux arm64 and faster on
  Apple silicon (measured on CI machines). CPUs without the SHA-256 instructions keep the portable engine.
- Native SegWit addresses (`bc1q...`, P2WPKH) are accepted as targets, besides `1...` addresses.
- `pzsolve help hunt` and `pzsolve help bench` show the options of a command, grouped by topic.
- The README shows what sets the program apart; the CI badge; a bug report form for issues.

## [1.8.0] — 2026-10-23

### First public release
- Linear scan, BSGS and kangaroo on the CPU (SIMD) and on GPUs (CUDA, OpenCL) for the 160 puzzles, with resumable campaigns, several
  computers on the same search, self-tuning and a live panel. The documentation is in English; the Italian one is under `docs/it/`.
- Builds for Linux x86-64, Linux arm64, macOS (Apple silicon and Intel) and Windows x86-64, published on GitHub Releases with
  `SHA256SUMS`. The macOS and Linux arm64 builds run the portable scalar engine on the CPU only.
- The jump table of schema 3 is computed with integer arithmetic only, so computers of any architecture working on the same campaign
  agree. The startup log prints `jump table id XXXXXXXX`, and it must be the same on every computer. Campaigns of schemas 1 and 2
  use `long double` and only work consistently on x86-64 (the program warns on other architectures).
- License: `AGPL-3.0-or-later`, with a commercial license available (see the README).

Earlier version numbers (1.0.0 to 1.7.9) were development builds that were never released.
