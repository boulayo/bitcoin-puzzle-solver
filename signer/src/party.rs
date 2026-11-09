// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! One party of a signing session: the key share from the two additive pieces, the auxiliary data generation, then one signing run
//! per digest, in order. No presignatures: each signature is a full interactive run.

use std::time::Instant;

use cggmp21::generic_ec::serde::CurveName;
use cggmp21::generic_ec::{Point, Scalar};
use cggmp21::key_share::{DirtyIncompleteKeyShare, DirtyKeyInfo, IncompleteKeyShare, Validate};
use cggmp21::round_based::state_machine::{ProceedResult, StateMachine};
use cggmp21::round_based::{Incoming, MessageDestination, MessageType};
use cggmp21::security_level::SecurityLevel128;
use cggmp21::supported_curves::Secp256k1;
use cggmp21::{DataToSign, ExecutionId, KeyShare, PregeneratedPrimes};
use rand_core::OsRng;
use serde::{de::DeserializeOwned, Serialize};
use sha2::{Digest, Sha256};

use crate::envelope::{self, Phase};
use crate::job::Job;
use crate::Fail;

/// The channel to the other party: frames on stdin and stdout in a session, in-memory queues in the selftest.
pub trait Link {
    fn send(&mut self, msg: Vec<u8>) -> Result<(), Fail>;
    fn recv(&mut self) -> Result<Vec<u8>, Fail>;
}

/// An ECDSA signature, big-endian, `s` low.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Sig {
    pub r: [u8; 32],
    pub s: [u8; 32],
}

pub fn run(job: &Job, primes: PregeneratedPrimes<SecurityLevel128>, link: &mut impl Link) -> Result<Vec<Sig>, Fail> {
    let me = job.i;
    let core = core_share(job)?;

    // The auxiliary data (Paillier keys and ring-Pedersen parameters of both parties) does not depend on the key: it comes from the
    // primes and its own execution id.
    let started = Instant::now();
    let aux_eid = tagged(b"pznet1/aux", &[&job.eid]);
    let aux_gen = cggmp21::aux_info_gen(ExecutionId::new(&aux_eid), me, 2, primes);
    let aux = drive(aux_gen.into_state_machine(&mut OsRng), link, me, Phase::Aux)?;
    eprintln!("pzsign[{me}]: auxiliary data in {:.1} s", started.elapsed().as_secs_f64());
    let share = KeyShare::from_parts((core, aux)).map_err(|e| Fail::new(format!("key share: {e}")))?;

    let mut sigs = Vec::with_capacity(job.digests.len());
    for (index, digest) in job.digests.iter().enumerate() {
        let started = Instant::now();
        let index = u32::try_from(index).expect("job::MAX_DIGESTS keeps the index small");
        let eid = tagged(b"pznet1/eid", &[&job.eid, &index.to_be_bytes()]);
        // The digest is already the hash of the transaction (the legacy signature hash): it is signed as the number it is, reduced
        // mod n as ECDSA does, not hashed again.
        let data = DataToSign::from_scalar(Scalar::from_be_bytes_mod_order(digest));
        let signing = cggmp21::signing(ExecutionId::new(&eid), me, &[0, 1], &share);
        let sig = drive(signing.sign_sync(&mut OsRng, data), link, me, Phase::Sign(index))?.normalize_s();
        // A signature that does not verify must never leave: the library checks it too, but this is the check that counts.
        sig.verify(&job.key, &data).map_err(|_| Fail::new(format!("signature {index} does not verify")))?;
        let mut rs = [0u8; 64];
        sig.write_to_slice(&mut rs);
        sigs.push(Sig { r: rs[..32].try_into().unwrap(), s: rs[32..].try_into().unwrap() });
        eprintln!("pzsign[{me}]: signature {index} in {:.2} s", started.elapsed().as_secs_f64());
    }
    Ok(sigs)
}

/// The key share of an additive two-of-two sharing, `k = x0 + x1`, without a dealer: each party holds its piece and both public
/// pieces. Without a VSS setup the library takes the public shares as additive and checks that they add up to the key.
fn core_share(job: &Job) -> Result<IncompleteKeyShare<Secp256k1>, Fail> {
    let me = usize::from(job.i);
    if Point::generator() * &job.share != *job.pieces[me] {
        return Err(Fail::new("the share does not match its public piece"));
    }
    if *job.pieces[0] + *job.pieces[1] != *job.key {
        return Err(Fail::new("the public pieces do not add up to the key"));
    }
    let core = DirtyIncompleteKeyShare {
        i: job.i,
        key_info: DirtyKeyInfo {
            curve: CurveName::new(),
            shared_public_key: job.key,
            public_shares: job.pieces.to_vec(),
            vss_setup: None,
        },
        x: job.share.clone(),
    };
    core.validate().map_err(|e| Fail::new(format!("key share: {}", e.error())))
}

/// `SHA256(tag || parts...)`: the hashes of the network all carry a domain tag (docs/en/NETWORK.md 3).
fn tagged(tag: &[u8], parts: &[&[u8]]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(tag);
    for part in parts {
        h.update(part);
    }
    h.finalize().into()
}

/// Carries one protocol run to its end: the messages the state machine sends go to the partner in envelopes, the ones it asks for
/// come from the partner.
fn drive<O, E, M>(
    mut machine: impl StateMachine<Output = Result<O, E>, Msg = M>,
    link: &mut impl Link,
    me: u16,
    phase: Phase,
) -> Result<O, Fail>
where
    E: std::error::Error + 'static,
    M: Serialize + DeserializeOwned,
{
    let partner = 1 - me;
    let mut next_id = 0;
    loop {
        match machine.proceed() {
            ProceedResult::SendMsg(out) => {
                // with two parties, a point-to-point message can only be for the partner
                if out.recipient == MessageDestination::OneParty(me) {
                    return Err(Fail::new("the library sent a message to its own party"));
                }
                link.send(envelope::encode(me, phase, out.recipient.is_broadcast(), &out.msg)?)?;
            }
            ProceedResult::NeedsOneMoreMessage => {
                let (broadcast, msg) = envelope::decode(&link.recv()?, partner, phase)?;
                let msg_type = if broadcast { MessageType::Broadcast } else { MessageType::P2P };
                let incoming = Incoming { id: next_id, sender: partner, msg_type, msg };
                next_id += 1;
                if machine.received_msg(incoming).is_err() {
                    return Err(Fail::new("the state machine refused a message"));
                }
            }
            ProceedResult::Yielded => {}
            ProceedResult::Output(out) => return out.map_err(|e| library_fail(&e)),
            ProceedResult::Error(e) => return Err(Fail::new(format!("state machine: {e}"))),
        }
    }
}

/// An error of the library: its message with all its causes, and the culprits it names.
fn library_fail(err: &(dyn std::error::Error + 'static)) -> Fail {
    let mut error = err.to_string();
    let mut cause = err.source();
    while let Some(e) = cause {
        error.push_str(": ");
        error.push_str(&e.to_string());
        cause = e.source();
    }
    Fail { error, blame: culprits(&format!("{err:?}")) }
}

/// The parties that the library blames for an abort. It keeps them in private fields, so their Debug form is the only way to read
/// them (the crate is pinned, so the form cannot change under us): `faulty_party: N` in the auxiliary data generation, and the first
/// element of each `(N, ...)` tuple in the list of an aborted signing, as in `SigningError(Aborted(EncProofOfK([(1, 3, 4)])))`.
/// Aborts without a culprit (a signature that does not verify, for example) give an empty list.
fn culprits(debug: &str) -> Vec<u16> {
    let mut out: Vec<u16> = debug.split("faulty_party: ").skip(1).filter_map(leading_number).collect();
    if let Some(rest) = debug.strip_prefix("SigningError(Aborted(") {
        if let Some(start) = rest.find("([") {
            let list = &rest[start + 2..];
            let mut depth = 0;
            for (k, c) in list.char_indices() {
                match c {
                    '(' | '[' | '{' => {
                        if depth == 0 {
                            out.extend(leading_number(&list[k + 1..]));
                        }
                        depth += 1;
                    }
                    ')' | ']' | '}' if depth == 0 => break,
                    ')' | ']' | '}' => depth -= 1,
                    _ => {}
                }
            }
        }
    }
    out.retain(|&j| j < 2);
    out.sort_unstable();
    out.dedup();
    out
}

fn leading_number(s: &str) -> Option<u16> {
    let end = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    s[..end].parse().ok()
}

#[cfg(test)]
mod tests {
    use super::culprits;

    #[test]
    fn culprits_from_debug() {
        assert_eq!(culprits("SigningError(Aborted(EncProofOfK([(1, 3, 4)])))"), [1]);
        assert_eq!(
            culprits("SigningError(Aborted(InvalidPsi([(0, 3, 4, (Some(X(1)), None, None)), (1, 5, 6, (None, None, None))])))"),
            [0, 1]
        );
        assert_eq!(culprits("SigningError(Aborted(SignatureInvalid))"), Vec::<u16>::new());
        assert_eq!(
            culprits("KeyRefreshError(Aborted(ProtocolAborted { reason: InvalidDecommitment, parties: [AbortBlame { faulty_party: 1, data_message: 2, proof_message: 3 }] }))"),
            [1]
        );
        assert_eq!(culprits("KeyRefreshError(IoError(ReceiveMessageEof))"), Vec::<u16>::new());
    }
}
