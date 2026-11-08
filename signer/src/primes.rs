// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! The pregenerated primes of the auxiliary data: two safe primes of 1536 bits whose product is the party's Paillier modulus, so
//! they are a secret key. Finding them takes from seconds to a minute or two (it is a matter of luck, and of the machine), so pzpeer
//! has them made ahead with `pzfirma primes FILE`. A file is for one session: pzfirma only reads it, pzpeer removes it once used.
//!
//! The file is JSON, `{"p": hex, "q": hex}`, readable by its owner only.

use std::fs;
use std::io::Write;
use std::path::Path;

use cggmp21::fast_paillier::utils::generate_safe_prime;
use cggmp21::rug::{integer::IsPrime, Integer};
use cggmp21::security_level::{KeygenSecurityLevel, SecurityLevel128};
use cggmp21::PregeneratedPrimes;
use rand_core::OsRng;
use serde::{Deserialize, Serialize};

use crate::Fail;

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct File {
    p: String,
    q: String,
}

/// The size of each prime that the security level asks for.
const BITS: u32 = 4 * SecurityLevel128::SECURITY_BITS;

/// Generates the two primes, one per thread: they are independent, and the time is the time of the slower one.
pub fn generate() -> (Integer, Integer) {
    std::thread::scope(|s| {
        let p = s.spawn(|| generate_safe_prime(&mut OsRng, BITS));
        let q = generate_safe_prime(&mut OsRng, BITS);
        (p.join().expect("prime generation thread"), q)
    })
}

/// Writes the file through a temporary one, so that a file with the final name is always complete.
pub fn write(path: &Path, p: &Integer, q: &Integer) -> std::io::Result<()> {
    let json = serde_json::to_vec(&File { p: p.to_string_radix(16), q: q.to_string_radix(16) })?;
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let mut options = fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    let mut file = options.open(&tmp)?;
    file.write_all(&json)?;
    file.sync_all()?;
    fs::rename(&tmp, path)
}

/// Reads a file and checks the primes: a damaged file would otherwise fail later, in a proof of the protocol, with a less clear error.
pub fn read(path: &Path) -> Result<PregeneratedPrimes<SecurityLevel128>, Fail> {
    let fail = |what: &str| Fail::new(format!("primes {}: {what}", path.display()));
    let text = fs::read(path).map_err(|e| fail(&e.to_string()))?;
    let file: File = serde_json::from_slice(&text).map_err(|e| fail(&e.to_string()))?;
    let (p, q) = (number(&file.p).ok_or(fail("p is not hex"))?, number(&file.q).ok_or(fail("q is not hex"))?);
    if p == q || !is_safe_prime(&p) || !is_safe_prime(&q) {
        return Err(fail("not two different safe primes"));
    }
    PregeneratedPrimes::new(p, q).ok_or(fail(&format!("primes shorter than {BITS} bits")))
}

fn number(hex: &str) -> Option<Integer> {
    // only digits: rug alone would also take a sign and spaces
    if hex.is_empty() || !hex.bytes().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    Integer::from_str_radix(hex, 16).ok()
}

fn is_safe_prime(p: &Integer) -> bool {
    let half = Integer::from(p - 1u32) >> 1u32;
    p.is_probably_prime(32) != IsPrime::No && half.is_probably_prime(32) != IsPrime::No
}
