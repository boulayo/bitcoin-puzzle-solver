# Running on a machine you do not control

A checklist for a rented computer (a cloud instance, a GPU rental, a machine at a friend's), in order. It uses the encryption of the found
key described in [USAGE](USAGE.md) section 11: whoever uses the machine after you must not be able to read the key. Every command is
given for Linux; on Windows it is `.\pzsolve.exe` in PowerShell and the same options.

## 1. Before uploading: check the binary

Download the archive and `SHA256SUMS` from the [Releases page](https://github.com/boulayo/bitcoin-puzzle-solver/releases) and check the
archive, so that what runs is the published program and not a copy somebody altered. Upload the archive, not the unpacked binary, and
run the same check on the rented machine before unpacking it there.

```sh
sha256sum -c SHA256SUMS --ignore-missing          # Linux: "pzsolve-1.21.1-linux-x86_64.tar.gz: OK"
```

```powershell
Get-FileHash .\pzsolve-1.21.1-windows-x86_64.zip   # Windows: the hash must be the one on the "windows" line of SHA256SUMS
Get-Content .\SHA256SUMS                           # (letters in upper or lower case: both are the same hash)
```

## 2. At home: the key and the rehearsal

Make the public key on a computer you trust. It asks for a password twice (nothing is shown) and prints the public key (66 digits)
and its key id (8 digits); nothing is written to disk. Use 7 random words or 20 random characters, drawn by dice or by a program, never
a word, a name or a date: anyone who sees the public key can try passwords against it offline. **A lost password is a lost key**: write
it on paper, in a place that is not the rented machine, and keep the public key where you can copy it.

```sh
pzsolve encrypt-keygen
```

Rehearse the whole path once on a solved puzzle before any real rental, with the same command line as the real one apart from the
puzzle: puzzle 40 ends in seconds. Do it on the same kind of system as the rental if you can (the same operating system, the same shell
for the `--on-found` command), and once on Windows if the rental is a Windows machine: the hidden password prompt of the Windows console
is not exercised by the tests of this project (they run the exe under Wine and feed the password through a pipe), so try it yourself.

```sh
pzsolve hunt -p 40 --encrypt-pubkey PUBKEY -c rehearsal --on-found 'curl -s -d "$PZ_FOUND_SEALED" https://example.com/hook'
pzsolve decrypt-key campaigns/rehearsal/FOUND.txt       # the password; it must end with "check: OK"
pzsolve puzzle 40                                        # its key is public: it must be the one decrypt-key printed
```

Check that the box on the screen and `FOUND.txt` show a `PZENC1:` line and no key, that what `--on-found` sent reached you
(`log.txt` says how the command ended), and that `decrypt-key` printed `check: OK`. Then delete `campaigns/rehearsal`.

## 3. On the rented machine

Start the campaign inside `screen` or `tmux`, so that it survives the closing of your connection (`Ctrl-A D` detaches,
`screen -r pzsolve` comes back; with tmux `tmux new -s pzsolve`, `Ctrl-B D` and `tmux attach -t pzsolve`). Without either,
`nohup pzsolve hunt ... --ui log > /dev/null &` does the same, with one log line per minute in `log.txt`.

```sh
screen -S pzsolve
pzsolve hunt -p 71 --encrypt-pubkey PUBKEY --keep-awake --on-found 'curl -s -d "$PZ_FOUND_SEALED" https://example.com/hook'
```

* `--encrypt-pubkey PUBKEY`: the key of step 2. It is stored in the campaign, so a resume does not need it again, and another key is
  refused. The log says `the key will be encrypted to the public key with id XXXXXXXX`: compare the id with the one `encrypt-keygen` printed.
* `--on-found COMMAND` sends you the encrypted line the moment it exists, even if the machine is taken away a minute later. The line is
  safe to send anywhere; `PZ_FOUND_SEALED` holds it (on Windows `%PZ_FOUND_SEALED%`, run by `cmd.exe`). A `curl` to a webhook as above, or
  a mail: `--on-found 'printf "%s\n" "$PZ_FOUND_SEALED" | mail -s "pzsolve" me@example.com'`. It is not stored: give it at every start.
  Do not put a password or a token in it: other users of the machine can read the command line of a process.
* `--keep-awake` prevents sleep on Windows and macOS; on Linux start with `systemd-inhibit pzsolve hunt ...` instead.
* `--limit 50` if the machine is shared: half of the CPU threads and of the GPU work-items, at full speed, no pauses.

The first start of a version on a machine with a GPU measures it before searching: the variants of the GPU kernels and the length of
their launches (about a minute per kernel on a laptop; an NVIDIA driver also compiles the kernels once, about a minute), then the best
mix of CPU threads and GPUs (about 2 minutes). The result is saved in the cache folder (`~/.cache/pzsolve`, on Windows
`%LOCALAPPDATA%\pzsolve`) and the next start is immediate.

Progress: `pzsolve list` from the same folder (state, progress, speed, time left); `campaigns/p71/report.txt` is the readable report
and `status.json` the same for programs, both rewritten every minute. Stop with Ctrl-C (or `kill PID`): the state is saved. Resume
with the same command, `--on-found` included. Several machines on the same puzzle: the same seed on all of them and one share each,
`pzsolve hunt -p 71 -s SEED --part 0/2 ...` on the first and `--part 1/2` on the second (USAGE section 8.2; `pzsolve split` divides
the shares among machines of different speeds).

## 4. What the owner of the machine can and cannot see

* Protected: whoever reads, after you, the files of the campaign, the screen or its scrollback, the log, a backup, a disk image or a
  returned disk: they find the encrypted line, which opens only with your password.
* Not protected: an administrator (root, the provider of a virtual machine) who reads the memory of the process at the moment the key is
  found, or who replaces the binary or the public key on the command line (the key id in the log tells you after the fact, not before).
* In the clear by nature: the address, the puzzle, the range, the seed, the progress and the time of the find. USAGE section 11.5 has the
  whole threat model.

## 5. After

Copy `FOUND.txt`, or the line you received, home. The line alone is enough, and it stays valid for ever.

```sh
pzsolve decrypt-key FOUND.txt                            # the password; prints the key, its WIF and the two addresses
```

It must print `check: OK` with the address you were hunting: a line can be made by anyone who knows the public key, so only the address
check proves that the key is the right one. Move the funds with a wallet that imports the WIF. When the rental ends, delete the campaign
folder on the rented machine (`rm -rf campaigns/p71`): it holds the range, the target, the seed, the public key and the progress of the
search, never the key, but there is no reason to leave them behind.
