// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! The frames between pzpeer and pzfirma on stdin and stdout (docs/en/NETWORK.md 7.6): a 4-byte big-endian length L, then L bytes,
//! a type byte and its payload.

use std::io::{self, Read, Write};

use crate::Fail;

/// pzpeer → pzfirma: the JSON job.
pub const JOB: u8 = 0x01;
/// pzpeer → pzfirma: a message of the other party.
pub const FROM_PARTNER: u8 = 0x02;
/// pzfirma → pzpeer: a message for the other party.
pub const TO_PARTNER: u8 = 0x81;
/// pzfirma → pzpeer: the JSON result.
pub const RESULT: u8 = 0x82;
/// pzfirma → pzpeer: the JSON error.
pub const ERROR: u8 = 0x83;

/// The largest frame, type byte included. The largest message of a session (the third round of the auxiliary data) is about 210 KB;
/// a length far above that can only be garbage, and refusing it keeps us from allocating whatever a broken input asks for.
pub const MAX_LEN: usize = 1 << 20;

/// Reads one frame: its type and its payload. The end of the input, even at a frame boundary, is an error: pzfirma only reads when
/// it is waiting for something.
pub fn read(input: &mut impl Read) -> Result<(u8, Vec<u8>), Fail> {
    let mut head = [0u8; 5];
    input.read_exact(&mut head).map_err(input_error)?;
    let len = u32::from_be_bytes([head[0], head[1], head[2], head[3]]) as usize;
    if len == 0 || len > MAX_LEN {
        return Err(Fail::new(format!("frame of {len} bytes on stdin")));
    }
    let mut payload = vec![0u8; len - 1];
    input.read_exact(&mut payload).map_err(input_error)?;
    Ok((head[4], payload))
}

/// Writes one frame and flushes it, so pzpeer sees it at once.
pub fn write(output: &mut impl Write, kind: u8, payload: &[u8]) -> io::Result<()> {
    let len = payload.len() + 1;
    if len > MAX_LEN {
        return Err(io::Error::other(format!("frame of {len} bytes")));
    }
    output.write_all(&(len as u32).to_be_bytes())?;
    output.write_all(&[kind])?;
    output.write_all(payload)?;
    output.flush()
}

fn input_error(err: io::Error) -> Fail {
    if err.kind() == io::ErrorKind::UnexpectedEof {
        Fail::new("stdin closed")
    } else {
        Fail::new(format!("stdin: {err}"))
    }
}
