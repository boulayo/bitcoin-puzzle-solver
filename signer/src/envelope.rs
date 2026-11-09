// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Leonardo Boulay
//! The envelope of a message between the two pzsign (the payload of frames 0x81 and 0x02, opaque to pzpeer).
//!
//! A header, then the message of the library, both in postcard (serde, varints, no field names):
//!
//! | Field | postcard | Meaning |
//! |---|---|---|
//! | version | u8 | 1 |
//! | from | u16 varint | index of the sender, 0 or 1 |
//! | phase | enum: 0 aux, 1 sign + u32 varint index, 2 abort | the protocol run the message belongs to |
//! | broadcast | bool byte | broadcast (1) or point to point (0), as the library sent it |
//!
//! After the header comes the library's message of that phase, or, for an abort, the reason as a string. The receiver checks every
//! field against what it expects and refuses trailing bytes: the two parties run the same runs in the same order over an ordered
//! channel, so any other message is malformed.

use serde::{de::DeserializeOwned, Deserialize, Serialize};

use crate::Fail;

pub const VERSION: u8 = 1;

/// The protocol run a message belongs to.
#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, Debug)]
pub enum Phase {
    /// The auxiliary data generation.
    Aux,
    /// The signing of the digest with this index in the job.
    Sign(u32),
    /// The sender gave up: the payload is its reason. It lets the partner stop at once rather than wait for messages that will not
    /// come.
    Abort,
}

#[derive(Serialize, Deserialize)]
struct Header {
    version: u8,
    from: u16,
    phase: Phase,
    broadcast: bool,
}

/// The longest abort reason passed on: it ends up in the partner's logs and error frame.
const MAX_REASON: usize = 200;

pub fn encode<M: Serialize>(from: u16, phase: Phase, broadcast: bool, msg: &M) -> Result<Vec<u8>, Fail> {
    let header = Header { version: VERSION, from, phase, broadcast };
    let mut out = postcard::to_allocvec(&header).map_err(encode_error)?;
    out.extend(postcard::to_allocvec(msg).map_err(encode_error)?);
    Ok(out)
}

pub fn abort(from: u16, reason: &str) -> Result<Vec<u8>, Fail> {
    let reason: String = reason.chars().take(MAX_REASON).collect();
    encode(from, Phase::Abort, true, &reason)
}

/// Decodes a message that `from` sent in `phase`: whether it was a broadcast, and the message. An abort becomes the error that ends
/// the session; anything malformed blames the partner, who is the only one that writes these bytes.
pub fn decode<M: DeserializeOwned>(bytes: &[u8], from: u16, phase: Phase) -> Result<(bool, M), Fail> {
    let malformed = |what: String| Fail::blaming(format!("malformed message from the partner: {what}"), from);
    let (header, rest): (Header, &[u8]) = postcard::take_from_bytes(bytes).map_err(|e| malformed(format!("header: {e}")))?;
    if header.version != VERSION {
        return Err(malformed(format!("version {}", header.version)));
    }
    if header.phase == Phase::Abort {
        let reason: String = postcard::from_bytes(rest).map_err(|e| malformed(format!("abort: {e}")))?;
        // the partner's words go into our logs: no control characters
        let reason: String = reason.chars().take(MAX_REASON).map(|c| if c.is_control() { '?' } else { c }).collect();
        return Err(Fail::new(format!("the partner aborted: {reason}")));
    }
    if header.from != from {
        return Err(malformed(format!("sent by party {}", header.from)));
    }
    if header.phase != phase {
        return Err(malformed(format!("phase {:?} while running {phase:?}", header.phase)));
    }
    let (msg, rest): (M, &[u8]) = postcard::take_from_bytes(rest).map_err(|e| malformed(format!("{phase:?}: {e}")))?;
    if !rest.is_empty() {
        return Err(malformed(format!("{} bytes after the message", rest.len())));
    }
    Ok((header.broadcast, msg))
}

fn encode_error(err: postcard::Error) -> Fail {
    Fail::new(format!("encoding a message: {err}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_and_refusals() {
        let msg = encode(1, Phase::Sign(3), false, &(7u32, "x".to_string())).unwrap();
        assert_eq!(msg[..5], [VERSION, 1, 1, 3, 0]);
        let (broadcast, got): (bool, (u32, String)) = decode(&msg, 1, Phase::Sign(3)).unwrap();
        assert_eq!((broadcast, got), (false, (7, "x".to_string())));

        let refused = |bytes: &[u8], from, phase| decode::<(u32, String)>(bytes, from, phase).unwrap_err();
        assert_eq!(refused(&msg, 1, Phase::Sign(4)).blame, [1]);
        assert_eq!(refused(&msg, 0, Phase::Sign(3)).blame, [0]);
        assert_eq!(refused(&[&msg[..], &[0]].concat(), 1, Phase::Sign(3)).blame, [1]);
        assert_eq!(refused(&msg[..msg.len() - 1], 1, Phase::Sign(3)).blame, [1]);

        let abort = abort(0, "no\nway").unwrap();
        let fail = refused(&abort, 0, Phase::Aux);
        assert_eq!((fail.error.as_str(), fail.blame.len()), ("the partner aborted: no?way", 0));
    }
}
