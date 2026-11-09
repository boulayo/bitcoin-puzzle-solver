// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! Two `pzsign session` processes with this test as their pzpeer: it gives each one its job, relays the frames 0x81 of each to the
//! other as 0x02 (changing some of them on purpose), and checks the answers. Run with `cargo test --release`: the primes are made
//! once, by `pzsign primes`, and kept in the target directory.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::{mpsc, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

use cggmp21::generic_ec::{coords::HasAffineX, Point, Scalar};
use cggmp21::supported_curves::Secp256k1;
use rand_core::{OsRng, RngCore};
use serde_json::{json, Value};

const BIN: &str = env!("CARGO_BIN_EXE_pzsign");

/// (n - 1) / 2 of secp256k1: a low `s` is at most this.
const HALF_ORDER: &str = "7fffffffffffffffffffffffffffffff5d576e7357a4501ddfe92f46681b20a0";

/// What a party answered: the result of a frame 0x82, or the error of a frame 0x83.
type Answer = Result<Value, Value>;

/// The primes of the two parties, made on first use (tests share them; a real session must not).
fn primes() -> &'static [PathBuf; 2] {
    static PRIMES: OnceLock<[PathBuf; 2]> = OnceLock::new();
    PRIMES.get_or_init(|| {
        let files = [0, 1].map(|i| PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("primes{i}.json")));
        let started = Instant::now();
        let makers: Vec<_> = files
            .iter()
            .filter(|f| !f.exists())
            .map(|f| Command::new(BIN).arg("primes").arg(f).spawn().expect("pzsign primes"))
            .collect();
        let made = makers.len();
        for mut maker in makers {
            assert!(maker.wait().unwrap().success(), "pzsign primes failed");
        }
        if made > 0 {
            eprintln!("test: {made} primes files in {:.1} s", started.elapsed().as_secs_f64());
        }
        files
    })
}

/// A random key `k = x0 + x1` and the jobs of the two parties for these digests.
fn jobs(digests: &[[u8; 32]]) -> ([Value; 2], Point<Secp256k1>) {
    let x = [Scalar::<Secp256k1>::random(&mut OsRng), Scalar::<Secp256k1>::random(&mut OsRng)];
    let pieces = x.map(|x| Point::generator() * x);
    let key = pieces[0] + pieces[1];
    let mut eid = [0u8; 32];
    OsRng.fill_bytes(&mut eid);
    let job = |i: usize| {
        json!({
            "op": "sign", "i": i, "eid": hex::encode(eid), "share": hex::encode(x[i].to_be_bytes()),
            "pub": pieces.map(|p| hex::encode(p.to_bytes(true))), "key": hex::encode(key.to_bytes(true)),
            "primes": primes()[i], "digests": digests.iter().map(hex::encode).collect::<Vec<_>>(),
        })
    };
    ([job(0), job(1)], key)
}

fn read_frame(input: &mut impl Read) -> Option<(u8, Vec<u8>)> {
    let mut head = [0u8; 5];
    input.read_exact(&mut head).ok()?;
    let len = u32::from_be_bytes([head[0], head[1], head[2], head[3]]) as usize;
    let mut payload = vec![0u8; len - 1];
    input.read_exact(&mut payload).ok()?;
    Some((head[4], payload))
}

fn write_frame(output: &mut impl Write, kind: u8, payload: &[u8]) -> std::io::Result<()> {
    output.write_all(&(payload.len() as u32 + 1).to_be_bytes())?;
    output.write_all(&[kind])?;
    output.write_all(payload)?;
    output.flush()
}

/// The parts of an envelope's header that the tests look at, for the small numbers they use (one-byte varints): the phase (0 aux,
/// 1 sign, 2 abort), the index of the digest for a signing, and the variant of the library's message after the header.
fn peek(msg: &[u8]) -> (u8, u8, u8) {
    assert_eq!(msg[0], 1, "envelope version");
    match msg[2] {
        1 => (1, msg[3], msg[5]),
        phase => (phase, 0, msg[4]),
    }
}

/// One session between two processes. `tamper(from, msg)` sees every message before it is relayed and may change it. Returns the
/// two answers and the largest message of each kind (phase, variant).
fn session(jobs: &[Value; 2], mut tamper: impl FnMut(usize, &mut Vec<u8>)) -> ([Answer; 2], BTreeMap<(u8, u8), usize>) {
    let mut children: Vec<_> = (0..2)
        .map(|_| Command::new(BIN).arg("session").stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().expect("pzsign session"))
        .collect();
    let mut stdins: Vec<_> = children.iter_mut().map(|c| c.stdin.take()).collect();
    for (stdin, job) in stdins.iter_mut().zip(jobs) {
        write_frame(stdin.as_mut().unwrap(), 0x01, job.to_string().as_bytes()).unwrap();
    }
    let (tx, rx) = mpsc::channel();
    for (i, child) in children.iter_mut().enumerate() {
        let mut stdout = child.stdout.take().unwrap();
        let tx = tx.clone();
        thread::spawn(move || loop {
            let frame = read_frame(&mut stdout);
            let end = frame.is_none();
            tx.send((i, frame)).unwrap();
            if end {
                break;
            }
        });
    }

    let mut answers: [Option<Answer>; 2] = [None, None];
    let mut sizes = BTreeMap::new();
    let mut open = 2;
    while open > 0 {
        let (i, frame) = rx.recv_timeout(Duration::from_secs(300)).expect("a party is stuck");
        match frame {
            Some((0x81, mut msg)) => {
                let (phase, _, variant) = peek(&msg);
                let size = sizes.entry((phase, variant)).or_insert(0);
                *size = msg.len().max(*size);
                tamper(i, &mut msg);
                if let Some(stdin) = stdins[1 - i].as_mut() {
                    // the partner may have stopped already
                    let _ = write_frame(stdin, 0x02, &msg);
                }
            }
            Some((kind @ (0x82 | 0x83), body)) => {
                assert!(answers[i].is_none(), "party {i} answered twice");
                let body: Value = serde_json::from_slice(&body).expect("JSON answer");
                answers[i] = Some(if kind == 0x82 { Ok(body) } else { Err(body) });
            }
            Some((kind, _)) => panic!("party {i} wrote a frame of type {kind:#04x}"),
            None => {
                // nothing more will come from this party: the other one must not wait for it
                open -= 1;
                stdins[1 - i] = None;
            }
        }
    }
    for (i, child) in children.iter_mut().enumerate() {
        let status = child.wait().unwrap();
        let answer = answers[i].as_ref().unwrap_or_else(|| panic!("party {i} exited without an answer"));
        assert_eq!(status.success(), answer.is_ok(), "exit status of party {i}");
    }
    (answers.map(Option::unwrap), sizes)
}

/// The error of an answer that must be one, as (message, blame).
fn error(answer: &Answer) -> (String, Vec<u64>) {
    let e = answer.as_ref().expect_err("an error");
    let blame = e["blame"].as_array().expect("blame").iter().map(|j| j.as_u64().unwrap()).collect();
    (e["error"].as_str().expect("error").to_string(), blame)
}

/// Standard ECDSA verification over the digest taken as a number, written here with the curve operations only, apart from the code
/// of the library that made the signature.
fn verifies(key: &Point<Secp256k1>, digest: &[u8; 32], sig: &Value) -> bool {
    let r = Scalar::<Secp256k1>::from_be_bytes(hex::decode(sig["r"].as_str().unwrap()).unwrap()).unwrap();
    let s = Scalar::<Secp256k1>::from_be_bytes(hex::decode(sig["s"].as_str().unwrap()).unwrap()).unwrap();
    let w = s.invert().expect("s is not zero");
    let z = Scalar::from_be_bytes_mod_order(digest);
    let point = Point::generator() * (z * w) + key * (r * w);
    point.x().map(|x| x.to_scalar()) == Some(r)
}

#[test]
fn both_parties_return_the_same_valid_signatures() {
    let mut digests = [[0u8; 32], [0u8; 32], [0xff; 32]];
    OsRng.fill_bytes(&mut digests[0]);
    OsRng.fill_bytes(&mut digests[1]);
    let (jobs, key) = jobs(&digests);
    let started = Instant::now();
    let ([a, b], sizes) = session(&jobs, |_, _| {});
    eprintln!("test: session of {} signatures in {:.1} s", digests.len(), started.elapsed().as_secs_f64());
    for ((phase, variant), size) in &sizes {
        eprintln!("test: largest message of phase {phase}, variant {variant}: {size} bytes");
    }
    let (a, b) = (a.expect("party 0 signs"), b.expect("party 1 signs"));
    assert_eq!(a, b, "both parties report the same signatures");
    let sigs = a["sigs"].as_array().unwrap();
    assert_eq!(sigs.len(), digests.len());
    for (sig, digest) in sigs.iter().zip(&digests) {
        assert!(verifies(&key, digest, sig), "signature over {}", hex::encode(digest));
        assert!(sig["s"].as_str().unwrap() <= HALF_ORDER, "s is low");
    }
}

#[test]
fn a_damaged_envelope_aborts_both_and_blames_the_sender() {
    let (jobs, _) = jobs(&[[7u8; 32]]);
    // the first message of party 0 in the signing, with a version nobody speaks
    let mut done = false;
    let ([a, b], _) = session(&jobs, |from, msg| {
        if from == 0 && peek(msg).0 == 1 && !done {
            msg[0] = 9;
            done = true;
        }
    });
    let (e1, blame1) = error(&b);
    assert!(e1.contains("malformed message") && e1.contains("version 9"), "party 1: {e1}");
    assert_eq!(blame1, [0]);
    let (e0, blame0) = error(&a);
    assert!(e0.contains("the partner aborted"), "party 0: {e0}");
    assert!(blame0.is_empty());
}

#[test]
fn a_corrupted_proof_aborts_both_and_blames_the_sender() {
    let (jobs, _) = jobs(&[[7u8; 32], [8u8; 32]]);
    // a bit of the middle of party 1's round 2 message of the first signing: its proofs fail, or it does not decode
    let ([a, b], _) = session(&jobs, |from, msg| {
        if from == 1 && peek(msg) == (1, 0, 2) {
            let middle = msg.len() / 2;
            msg[middle] ^= 1;
        }
    });
    let (e0, blame0) = error(&a);
    assert_eq!(blame0, [1], "party 0: {e0}");
    let (e1, blame1) = error(&b);
    assert!(e1.contains("the partner aborted"), "party 1: {e1}");
    assert!(blame1.is_empty());
}

#[test]
fn a_corrupted_broadcast_aborts_both() {
    let (jobs, _) = jobs(&[[7u8; 32]]);
    // the last byte of party 1's commitment in the first round of the auxiliary data: still a valid message, but the two parties no
    // longer saw the same broadcast, and each can only blame the other
    let (answers, _) = session(&jobs, |from, msg| {
        if from == 1 && peek(msg) == (0, 0, 0) {
            *msg.last_mut().unwrap() ^= 1;
        }
    });
    for (i, answer) in answers.iter().enumerate() {
        let (e, blame) = error(answer);
        assert!(blame.iter().all(|&j| j == 1 - i as u64), "party {i} blames itself: {e}");
    }
}

#[test]
fn a_wrong_share_is_refused() {
    let (mut jobs, _) = jobs(&[[7u8; 32]]);
    // a share that is not the discrete log of its public piece
    let x = Scalar::<Secp256k1>::from_be_bytes(hex::decode(jobs[0]["share"].as_str().unwrap()).unwrap()).unwrap();
    jobs[0]["share"] = json!(hex::encode((x + Scalar::one()).to_be_bytes()));
    let ([a, b], _) = session(&jobs, |_, _| {});
    assert!(error(&a).0.contains("the share does not match its public piece"), "{a:?}");
    assert!(error(&b).0.contains("the partner aborted"), "{b:?}");

    // a share that matches its piece, but the pieces do not add up to the key
    let (mut jobs, _) = self::jobs(&[[7u8; 32]]);
    let x = Scalar::<Secp256k1>::random(&mut OsRng);
    jobs[1]["share"] = json!(hex::encode(x.to_be_bytes()));
    jobs[1]["pub"][1] = json!(hex::encode((Point::generator() * x).to_bytes(true)));
    let ([a, b], _) = session(&jobs, |_, _| {});
    assert!(error(&b).0.contains("the public pieces do not add up to the key"), "{b:?}");
    assert!(error(&a).0.contains("the partner aborted"), "{a:?}");
}

#[test]
fn a_malformed_job_is_refused() {
    let (good, _) = jobs(&[[7u8; 32]]);
    let mut bad = Vec::new();
    for (field, value) in [
        ("op", json!("presign")),
        ("i", json!(2)),
        ("eid", json!("00")),
        ("share", json!("00".repeat(32))),
        ("key", json!(format!("05{}", "00".repeat(32)))),
        ("digests", json!([])),
        ("extra", json!(1)),
    ] {
        let mut job = good[0].clone();
        job[field] = value;
        bad.push(job.to_string().into_bytes());
    }
    bad.push(b"{not json".to_vec());
    for job in bad {
        let mut child = Command::new(BIN).arg("session").stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap();
        write_frame(child.stdin.as_mut().unwrap(), 0x01, &job).unwrap();
        let (kind, body) = read_frame(child.stdout.as_mut().unwrap()).expect("an answer");
        assert_eq!(kind, 0x83, "job {}", String::from_utf8_lossy(&job));
        let body: Value = serde_json::from_slice(&body).unwrap();
        assert!(body["error"].as_str().unwrap().starts_with("job: "), "{body}");
        assert!(!child.wait().unwrap().success());
    }
}

/// The answers of a session with a partner that cheats: no party may hold a signature unless it is a valid one over its own digest.
fn no_bad_signature(answers: &[Answer; 2], key: &Point<Secp256k1>, digests: [&[u8; 32]; 2]) {
    for (answer, digest) in answers.iter().zip(digests) {
        if let Ok(result) = answer {
            let sigs = result["sigs"].as_array().unwrap();
            assert!(sigs.len() == 1 && verifies(key, digest, &sigs[0]), "a signature that is not over the own digest left a party");
        }
    }
}

#[test]
fn a_partner_with_another_digest_or_session_gets_nothing() {
    // the partner asks for a signature over another digest: nobody signs, neither its digest nor ours
    let (mut jobs, key) = jobs(&[[7u8; 32]]);
    jobs[1]["digests"] = json!([hex::encode([8u8; 32])]);
    let (answers, _) = session(&jobs, |_, _| {});
    assert!(answers.iter().all(Result::is_err), "{answers:?}");
    no_bad_signature(&answers, &key, [&[7u8; 32], &[8u8; 32]]);

    // the partner runs under another execution id: the proofs of one do not hold for the other
    let (mut jobs, _) = self::jobs(&[[7u8; 32]]);
    jobs[1]["eid"] = json!(hex::encode([9u8; 32]));
    let (answers, _) = session(&jobs, |_, _| {});
    assert!(answers.iter().all(Result::is_err), "{answers:?}");
}

#[test]
fn every_message_counts() {
    // One byte changed in the middle of one message of party 1, for every message it sends in a session of one signature: the session
    // always ends without a signature for party 0, which is the one that received the changed message.
    let digest = [7u8; 32];
    let (jobs, key) = jobs(&[digest]);
    let mut total = 0;
    let _ = session(&jobs, |from, _| total += usize::from(from == 1));
    assert!(total >= 8, "party 1 sent only {total} messages");
    for target in 0..total {
        let mut seen = 0;
        let (answers, _) = session(&jobs, |from, msg| {
            if from == 1 {
                if seen == target {
                    let middle = msg.len() / 2;
                    msg[middle] ^= 0x10;
                }
                seen += 1;
            }
        });
        assert!(answers[0].is_err(), "party 0 signed after message {target} of {total} was changed");
        no_bad_signature(&answers, &key, [&digest, &digest]);
    }
}

#[test]
fn a_replayed_session_gets_nothing() {
    // Party 1 does not run the protocol: it sends again, message by message, what an honest party 1 sent in an earlier session with
    // the same key. Party 0 has new random values (and, the second time, a new execution id): it never signs.
    let digest = [7u8; 32];
    let (mut jobs, key) = jobs(&[digest]);
    let mut tape = Vec::new();
    let ([first, _], _) = session(&jobs, |from, msg| {
        if from == 1 {
            tape.push(msg.clone());
        }
    });
    assert!(first.is_ok(), "the session to replay: {first:?}");
    for new_eid in [false, true] {
        if new_eid {
            jobs[0]["eid"] = json!(hex::encode([9u8; 32]));
            jobs[1]["eid"] = json!(hex::encode([9u8; 32]));
        }
        let mut next = 0;
        let (answers, _) = session(&jobs, |from, msg| {
            if from == 1 {
                if let Some(old) = tape.get(next) {
                    *msg = old.clone();
                }
                next += 1;
            }
        });
        assert!(answers[0].is_err(), "party 0 signed with a replayed partner (new eid: {new_eid})");
        no_bad_signature(&answers, &key, [&digest, &digest]);
    }
}
