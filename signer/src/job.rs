// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! The job of a session (frame 0x01), checked field by field before anything else happens:
//!
//! `{"op": "sign", "i": 0|1, "eid": hex32, "share": hex32, "pub": [hex33, hex33], "key": hex33, "primes": path, "digests": [hex32, ...]}`

use std::path::PathBuf;

use cggmp21::generic_ec::{NonZero, Point, Scalar, SecretScalar};
use cggmp21::supported_curves::Secp256k1;
use serde::Deserialize;
use zeroize::Zeroize;

use crate::Fail;

/// More digests than any session can need (a transaction has at most 8 inputs, docs/en/NETWORK.md 8, and a session signs two).
pub const MAX_DIGESTS: usize = 64;

pub struct Job {
    /// Index of this party: 0 is the owner with the smaller identity.
    pub i: u16,
    /// The session's execution id, from which each run derives its own.
    pub eid: [u8; 32],
    /// The own additive piece of the key.
    pub share: NonZero<SecretScalar<Secp256k1>>,
    /// The public pieces of both parties: `pieces[j] = x_j·G`.
    pub pieces: [NonZero<Point<Secp256k1>>; 2],
    /// The puzzle key, the sum of the pieces.
    pub key: NonZero<Point<Secp256k1>>,
    pub primes: PathBuf,
    /// The digests to sign (legacy signature hashes), in order.
    pub digests: Vec<[u8; 32]>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Raw {
    op: String,
    i: u16,
    eid: String,
    share: String,
    #[serde(rename = "pub")]
    pieces: Vec<String>,
    key: String,
    primes: String,
    digests: Vec<String>,
}

pub fn parse(json: &[u8]) -> Result<Job, Fail> {
    let mut raw: Raw = serde_json::from_slice(json).map_err(|e| Fail::new(format!("job: {e}")))?;
    let share = share(&raw.share);
    raw.share.zeroize();
    if raw.op != "sign" {
        return Err(Fail::new(format!("job: unknown op {:?}", raw.op)));
    }
    if raw.i > 1 {
        return Err(Fail::new(format!("job: party index {}", raw.i)));
    }
    if raw.pieces.len() != 2 {
        return Err(Fail::new(format!("job: {} public pieces instead of 2", raw.pieces.len())));
    }
    if raw.digests.is_empty() || raw.digests.len() > MAX_DIGESTS {
        return Err(Fail::new(format!("job: {} digests (1 to {MAX_DIGESTS})", raw.digests.len())));
    }
    if raw.primes.is_empty() {
        return Err(Fail::new("job: no primes file"));
    }
    Ok(Job {
        i: raw.i,
        eid: bytes("eid", &raw.eid)?,
        share: share?,
        pieces: [point("pub[0]", &raw.pieces[0])?, point("pub[1]", &raw.pieces[1])?],
        key: point("key", &raw.key)?,
        primes: PathBuf::from(raw.primes),
        digests: raw.digests.iter().map(|d| bytes("digest", d)).collect::<Result<_, _>>()?,
    })
}

fn share(hex: &str) -> Result<NonZero<SecretScalar<Secp256k1>>, Fail> {
    let mut b: [u8; 32] = bytes("share", hex)?;
    let mut x = Scalar::from_be_bytes(b).unwrap_or(Scalar::zero());
    b.zeroize();
    let x = NonZero::from_secret_scalar(SecretScalar::new(&mut x));
    x.ok_or(Fail::new("job: share is not a scalar in [1, n-1]"))
}

fn point(field: &str, hex: &str) -> Result<NonZero<Point<Secp256k1>>, Fail> {
    let b: [u8; 33] = bytes(field, hex)?;
    let p = Point::from_bytes(b).ok().and_then(NonZero::from_point);
    p.ok_or(Fail::new(format!("job: {field} is not a compressed point of the curve")))
}

fn bytes<const N: usize>(field: &str, hex: &str) -> Result<[u8; N], Fail> {
    let mut out = [0u8; N];
    hex::decode_to_slice(hex, &mut out).map_err(|_| Fail::new(format!("job: {field} is not {N} bytes of hex")))?;
    Ok(out)
}
