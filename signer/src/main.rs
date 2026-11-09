// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! pzsign, the two-party ECDSA signer of the shared kangaroo network (docs/en/NETWORK.md 7.6).
//!
//! pzpeer starts one `pzsign session` per signing session and relays its messages to the partner's pzsign. stdout carries only
//! frames; everything for people goes to stderr.

mod envelope;
mod frame;
mod job;
mod party;
mod primes;

use std::io::{self, BufReader, Stdin, Stdout};
use std::panic::{self, AssertUnwindSafe};
use std::path::Path;
use std::process::ExitCode;
use std::sync::mpsc;
use std::time::Instant;

use cggmp21::generic_ec::{Point, Scalar};
use cggmp21::supported_curves::Secp256k1;
use rand_core::{OsRng, RngCore};
use serde_json::json;
use zeroize::Zeroize;

use party::{Link, Sig};

/// Why a party stopped without signatures, and the parties to blame for it, if any.
#[derive(Debug)]
pub struct Fail {
    pub error: String,
    pub blame: Vec<u16>,
}

impl Fail {
    pub fn new(error: impl Into<String>) -> Self {
        Fail { error: error.into(), blame: Vec::new() }
    }

    pub fn blaming(error: impl Into<String>, party: u16) -> Self {
        Fail { error: error.into(), blame: vec![party] }
    }
}

const USAGE: &str = "usage: pzsign session          a signing session, framed on stdin and stdout (docs/en/NETWORK.md 7.6)
       pzsign primes FILE      pregenerate the primes of a session into FILE (seconds to minutes)
       pzsign selftest [P0 P1] both parties in one process, with primes from P0 and P1 or fresh ones
       pzsign version";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    match args.as_slice() {
        ["session"] => session(),
        ["primes", file] => make_primes(Path::new(file)),
        ["selftest"] => selftest(None),
        ["selftest", p0, p1] => selftest(Some([p0, p1])),
        ["version"] => {
            println!("pzsign {}", env!("CARGO_PKG_VERSION"));
            println!("Copyright (C) 2026 Leonardo Boulay");
            println!("License AGPL-3.0-or-later: GNU Affero General Public License version 3 or later (LICENSE, NOTICE).");
            println!("This is free software: you are free to change and redistribute it.");
            println!("There is NO WARRANTY, to the extent permitted by law.");
            ExitCode::SUCCESS
        }
        _ => {
            eprintln!("{USAGE}");
            ExitCode::from(2)
        }
    }
}

/// The party's side of the frames: messages for the partner go out as 0x81, the partner's come in as 0x02.
struct Stdio {
    input: BufReader<Stdin>,
    output: Stdout,
}

impl Link for Stdio {
    fn send(&mut self, msg: Vec<u8>) -> Result<(), Fail> {
        frame::write(&mut self.output, frame::TO_PARTNER, &msg).map_err(|e| Fail::new(format!("stdout: {e}")))
    }

    fn recv(&mut self) -> Result<Vec<u8>, Fail> {
        match frame::read(&mut self.input)? {
            (frame::FROM_PARTNER, msg) => Ok(msg),
            (kind, _) => Err(Fail::new(format!("frame of type {kind:#04x} during the session"))),
        }
    }
}

fn session() -> ExitCode {
    let mut io = Stdio { input: BufReader::new(io::stdin()), output: io::stdout() };
    let mut me = None;
    // A panic in the library must still end with an error frame, so pzpeer knows the session is over.
    let outcome = panic::catch_unwind(AssertUnwindSafe(|| run_session(&mut io, &mut me)))
        .unwrap_or_else(|_| Err(Fail::new("internal error (panic)")));
    match outcome {
        Ok(sigs) => {
            let sigs: Vec<_> = sigs.iter().map(|s| json!({"r": hex::encode(s.r), "s": hex::encode(s.s)})).collect();
            let result = json!({ "sigs": sigs }).to_string();
            match frame::write(&mut io.output, frame::RESULT, result.as_bytes()) {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("pzsign: stdout: {e}");
                    ExitCode::FAILURE
                }
            }
        }
        Err(fail) => {
            eprintln!("pzsign{}: {} (blame {:?})", me.map(|i| format!("[{i}]")).unwrap_or_default(), fail.error, fail.blame);
            // the partner first, so it stops at once; then the error, which is always the last frame
            if let Some(i) = me {
                if let Ok(abort) = envelope::abort(i, &fail.error) {
                    let _ = io.send(abort);
                }
            }
            let error = json!({ "error": fail.error, "blame": fail.blame }).to_string();
            let _ = frame::write(&mut io.output, frame::ERROR, error.as_bytes());
            ExitCode::FAILURE
        }
    }
}

fn run_session(io: &mut Stdio, me: &mut Option<u16>) -> Result<Vec<Sig>, Fail> {
    let (kind, mut payload) = frame::read(&mut io.input)?;
    if kind != frame::JOB {
        return Err(Fail::new(format!("first frame of type {kind:#04x}, not a job")));
    }
    // the job holds the share: its bytes do not outlive the parsing
    let job = job::parse(&payload);
    payload.zeroize();
    let job = job?;
    *me = Some(job.i);
    let primes = primes::read(&job.primes)?;
    party::run(&job, primes, io)
}

fn make_primes(path: &Path) -> ExitCode {
    let started = Instant::now();
    let (p, q) = primes::generate();
    if let Err(e) = primes::write(path, &p, &q) {
        eprintln!("pzsign: {}: {e}", path.display());
        return ExitCode::FAILURE;
    }
    eprintln!("pzsign: primes in {:.1} s, written to {}", started.elapsed().as_secs_f64(), path.display());
    ExitCode::SUCCESS
}

/// The selftest's link: the partner is a thread of the same process.
struct Queue {
    tx: mpsc::Sender<Vec<u8>>,
    rx: mpsc::Receiver<Vec<u8>>,
}

impl Link for Queue {
    fn send(&mut self, msg: Vec<u8>) -> Result<(), Fail> {
        self.tx.send(msg).map_err(|_| Fail::new("the partner is gone"))
    }

    fn recv(&mut self) -> Result<Vec<u8>, Fail> {
        self.rx.recv().map_err(|_| Fail::new("the partner is gone"))
    }
}

/// Both parties in one process: a random key split in two pieces, the auxiliary data, three digests (the last one above the group
/// order, to cover its reduction), and the same verified signatures from both sides.
fn selftest(files: Option<[&str; 2]>) -> ExitCode {
    let primes = match files {
        Some(files) => files.map(|f| primes::read(Path::new(f))),
        None => {
            let started = Instant::now();
            let primes = std::thread::scope(|s| {
                let other = s.spawn(primes::generate);
                let own = primes::generate();
                [own, other.join().expect("prime generation thread")]
            });
            eprintln!("pzsign: two sets of primes in {:.1} s", started.elapsed().as_secs_f64());
            primes.map(|(p, q)| cggmp21::PregeneratedPrimes::new(p, q).ok_or(Fail::new("primes too small")))
        }
    };
    let [primes0, primes1] = match primes {
        [Ok(a), Ok(b)] => [a, b],
        [Err(e), _] | [_, Err(e)] => {
            eprintln!("pzsign: {}", e.error);
            return ExitCode::FAILURE;
        }
    };

    let k = Scalar::<Secp256k1>::random(&mut OsRng);
    let x0 = Scalar::<Secp256k1>::random(&mut OsRng);
    let x1 = k - x0;
    let pieces = [Point::generator() * x0, Point::generator() * x1];
    let key = Point::generator() * k;
    let mut eid = [0u8; 32];
    OsRng.fill_bytes(&mut eid);
    let mut digests = [[0u8; 32], [0u8; 32], [0xff; 32]];
    OsRng.fill_bytes(&mut digests[0]);
    OsRng.fill_bytes(&mut digests[1]);
    let jobs = [(0, x0), (1, x1)].map(|(i, x)| {
        let text = json!({
            "op": "sign", "i": i, "eid": hex::encode(eid), "share": hex::encode(x.to_be_bytes()),
            "pub": pieces.map(|p| hex::encode(p.to_bytes(true))), "key": hex::encode(key.to_bytes(true)),
            "primes": "selftest", "digests": digests.map(hex::encode),
        });
        job::parse(text.to_string().as_bytes())
    });
    let [Ok(job0), Ok(job1)] = jobs else {
        eprintln!("pzsign: selftest: the jobs do not parse");
        return ExitCode::FAILURE;
    };

    let (to1, from0) = mpsc::channel();
    let (to0, from1) = mpsc::channel();
    let started = Instant::now();
    let (sigs0, sigs1) = std::thread::scope(|s| {
        let party1 = s.spawn(|| party::run(&job1, primes1, &mut Queue { tx: to0, rx: from0 }));
        let sigs0 = party::run(&job0, primes0, &mut Queue { tx: to1, rx: from1 });
        (sigs0, party1.join().expect("party 1 thread"))
    });
    let (sigs0, sigs1) = match (sigs0, sigs1) {
        (Ok(a), Ok(b)) => (a, b),
        (Err(e), _) | (_, Err(e)) => {
            eprintln!("pzsign: selftest: {} (blame {:?})", e.error, e.blame);
            return ExitCode::FAILURE;
        }
    };
    if sigs0 != sigs1 || sigs0.len() != digests.len() {
        eprintln!("pzsign: selftest: the two parties have different signatures");
        return ExitCode::FAILURE;
    }
    println!("key {}", hex::encode(key.to_bytes(true)));
    for (sig, digest) in sigs0.iter().zip(&digests) {
        println!("digest {} r {} s {}", hex::encode(digest), hex::encode(sig.r), hex::encode(sig.s));
    }
    println!("selftest OK: {} signatures verified, in {:.1} s", sigs0.len(), started.elapsed().as_secs_f64());
    ExitCode::SUCCESS
}
