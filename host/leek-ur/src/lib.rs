//! BC-UR for the LeekWallet QR air gap.
//!
//! The companion shows the board's signer view as an animated
//! `ur:leek-pczt`, and reads the board's signatures back as `ur:leek-zsig`.
//! Both messages are a CBOR byte string around the bytes (a PCZT, a
//! `.zsig`), as `firmware/src/airgap.c` reads and writes them and as
//! Hummingbird's `UR.fromBytes` builds them on the Zodl fork.
//!
//! This is the host twin of `firmware/src/ur.c`, `ur-fountain.c`,
//! `ur-encoder.c` and `ur-decoder.c`, and it is held to the same vectors:
//! the parts Blockchain Commons' bc-ur printed for a known message and the
//! fountain choices it made (read straight out of `firmware/sim/test_ur_*.c`
//! by this crate's tests), and the shared fixtures in `vectors/ur/`, which
//! the firmware's simulator decodes too. Desktop, Android (the Tauri app)
//! and `zecz` use this one implementation.
//!
//! Why our own and not the `ur` crate: the decoder here keeps the board's
//! bounds for hostile input (the metadata pinned at the first part, at most
//! [`MAX_PARTS`] fragments, a message size cap, a capped queue of mixed
//! parts), it needs nothing but SHA-256, and it is short enough to review
//! against the C next to it. The algorithm and the Bytewords list are from
//! bc-ur (BSD-2-Clause Plus Patent); the implementation is ours.

use std::fmt;

pub mod display;
pub use display::{display_hints, DisplayHints};

use sha2::{Digest, Sha256};

/// The signer view going to the board.
pub const UR_PCZT: &str = "leek-pczt";
/// The board's `.zsig` coming back.
pub const UR_ZSIG: &str = "leek-zsig";

/// bc-ur's floor, so tiny frames never drive the fragment count up.
pub const MIN_FRAGMENT_LEN: usize = 10;
/// The largest fragment built, and accepted: the board's own ceiling
/// (`UR_MAX_FRAGMENT_LEN`, and its mixed buffers are sized for it).
pub const MAX_FRAGMENT_LEN: usize = 512;
/// The most fragments a message may have, on both ends (`UR_MAX_PARTS`).
pub const MAX_PARTS: u32 = 128;
/// Longest UR type accepted.
pub const TYPE_MAX: usize = 32;
/// Where a looping animation's sequence number starts over, as the board's
/// return path does (`QR_OUT_SEQ_WRAP`): past 255 the CBOR sequence number
/// grows a byte, the part grows, and so would the QR code mid-loop. A
/// receiver treats a repeat as redundant, so starting over costs nothing.
pub const SEQ_WRAP: u32 = 255;

/// Decoded bytes of one part we look at, as the board bounds it
/// (`UR_PART_MAX`): no QR frame carries more.
const PART_MAX: usize = 1024;

/// What went wrong. Every variant means "this input was refused"; nothing
/// in a refused part is used.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UrError {
    /// Not `ur:<type>/...` with a valid type.
    NotUr,
    /// The Bytewords did not decode or their CRC did not match.
    Bytewords,
    /// The part's CBOR envelope or sequence header is malformed or
    /// inconsistent with itself.
    Malformed,
    /// Bigger than the bounds allow (fragments, parts, message).
    TooLarge,
    /// Disagrees with the parts already taken (another message).
    Mismatch,
    /// Assembled, but the whole message's CRC-32 is wrong.
    Checksum,
    /// A complete message of another type than the one expected.
    WrongType { found: String, want: String },
    /// The message is not one CBOR byte string.
    NotBytes,
    /// An empty message, or an encoder asked for nothing.
    Empty,
    /// The parts given ran out before the message was complete.
    Incomplete { remaining: u32 },
}

impl fmt::Display for UrError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            UrError::NotUr => write!(f, "not a UR (ur:<type>/...)"),
            UrError::Bytewords => write!(f, "the QR text did not decode (Bytewords or its checksum)"),
            UrError::Malformed => write!(f, "a malformed UR part"),
            UrError::TooLarge => write!(f, "the UR is larger than this reader accepts"),
            UrError::Mismatch => write!(f, "a part of another message (its header differs from the first part's)"),
            UrError::Checksum => write!(f, "the assembled message failed its checksum"),
            UrError::WrongType { found, want } => write!(f, "expected ur:{want}, scanned ur:{found}"),
            UrError::NotBytes => write!(f, "the UR does not carry one CBOR byte string"),
            UrError::Empty => write!(f, "an empty UR message"),
            UrError::Incomplete { remaining } => write!(f, "the parts ran out with {remaining} fragment(s) still missing"),
        }
    }
}

impl std::error::Error for UrError {}

// ----------------------------------------------------------------- CRC-32

const fn crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}
static CRC_TABLE: [u32; 256] = crc_table();

/// Standard CRC-32 (IEEE 802.3), as `ur_crc32`.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc = CRC_TABLE[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8);
    }
    crc ^ 0xFFFF_FFFF
}

// -------------------------------------------------------------- Bytewords

/// The 256 Bytewords, four letters each. Data from bc-ur (BSD-2-Clause Plus
/// Patent), copied from `firmware/src/ur.c`, which copied it verbatim.
const BYTEWORDS: &[u8; 1024] = b"ableacidalsoapexaquaarchatomauntawayaxisbackbaldbarnbeltbetabias\
bluebodybragbrewbulbbuzzcalmcashcatschefcityclawcodecolacookcost\
cruxcurlcuspcyandarkdatadaysdelidicedietdoordowndrawdropdrumdull\
dutyeacheasyechoedgeepicevenexamexiteyesfactfairfernfigsfilmfish\
fizzflapflewfluxfoxyfreefrogfuelfundgalagamegeargemsgiftgirlglow\
goodgraygrimgurugushgyrohalfhanghardhawkheathelphighhillholyhope\
hornhutsicedideaidleinchinkyintoirisironitemjadejazzjoinjoltjowl\
judojugsjumpjunkjurykeepkenokeptkeyskickkilnkingkitekiwiknoblamb\
lavalazyleaflegsliarlimplionlistlogoloudloveluaulucklungmainmany\
mathmazememomenumeowmildmintmissmonknailnavyneednewsnextnoonnote\
numbobeyoboeomitonyxopenovalowlspaidpartpeckplaypluspoempoolpose\
puffpumapurrquadquizraceramprealredorichroadrockroofrubyruinruns\
rustsafesagascarsetssilkskewslotsoapsolosongstubsurfswantacotask\
taxitenttiedtimetinytoiltombtoystriptunatwinuglyundouniturgeuser\
vastveryvetovialvibeviewvisavoidvowswallwandwarmwaspwavewaxywebs\
whatwhenwhizwolfworkyankyawnyellyogayurtzapszerozestzinczonezoom";

/// Byte for a first/last letter pair (lowercase), or None.
fn byteword_lookup(first: u8, last: u8) -> Option<u8> {
    let (first, last) = (first.to_ascii_lowercase(), last.to_ascii_lowercase());
    (0..256usize).find(|&i| BYTEWORDS[i * 4] == first && BYTEWORDS[i * 4 + 3] == last).map(|i| i as u8)
}

fn push_minimal(out: &mut String, data: &[u8]) {
    for &b in data {
        out.push(BYTEWORDS[b as usize * 4] as char);
        out.push(BYTEWORDS[b as usize * 4 + 3] as char);
    }
}

/// Bytewords "minimal" with the CRC-32 appended, as `ur_bytewords_encode`.
pub fn bytewords_encode(data: &[u8]) -> String {
    let mut s = String::with_capacity((data.len() + 4) * 2);
    push_minimal(&mut s, data);
    push_minimal(&mut s, &crc32(data).to_be_bytes());
    s
}

/// Decode Bytewords minimal (any case) and check its CRC-32, refusing a
/// payload over `max` bytes.
pub fn bytewords_decode(text: &str, max: usize) -> Result<Vec<u8>, UrError> {
    let t = text.as_bytes();
    if !t.len().is_multiple_of(2) || t.len() < 8 {
        return Err(UrError::Bytewords);
    }
    let total = t.len() / 2;
    if total - 4 > max {
        return Err(UrError::TooLarge);
    }
    let mut all = Vec::with_capacity(total);
    for pair in t.chunks_exact(2) {
        all.push(byteword_lookup(pair[0], pair[1]).ok_or(UrError::Bytewords)?);
    }
    let crc = u32::from_be_bytes([all[total - 4], all[total - 3], all[total - 2], all[total - 1]]);
    all.truncate(total - 4);
    if crc32(&all) != crc {
        return Err(UrError::Bytewords);
    }
    Ok(all)
}

// -------------------------------------------------------------------- CBOR

fn cbor_head(out: &mut Vec<u8>, major: u8, v: u64) {
    let m = major << 5;
    if v < 24 {
        out.push(m | v as u8);
    } else if v <= 0xFF {
        out.extend_from_slice(&[m | 24, v as u8]);
    } else if v <= 0xFFFF {
        out.push(m | 25);
        out.extend_from_slice(&(v as u16).to_be_bytes());
    } else if v <= 0xFFFF_FFFF {
        out.push(m | 26);
        out.extend_from_slice(&(v as u32).to_be_bytes());
    } else {
        out.push(m | 27);
        out.extend_from_slice(&v.to_be_bytes());
    }
}

/// Reads one head: (major type, argument, bytes used). Definite lengths
/// only, arguments up to 32 bits (nothing here is larger).
fn cbor_read_head(b: &[u8]) -> Option<(u8, u32, usize)> {
    let first = *b.first()?;
    let (major, ai) = (first >> 5, first & 0x1F);
    let (v, n) = match ai {
        0..=23 => (ai as u32, 1),
        24 => (*b.get(1)? as u32, 2),
        25 => (u16::from_be_bytes([*b.get(1)?, *b.get(2)?]) as u32, 3),
        26 => (u32::from_be_bytes([*b.get(1)?, *b.get(2)?, *b.get(3)?, *b.get(4)?]), 5),
        _ => return None,
    };
    Some((major, v, n))
}

/// The CBOR byte string around `payload`: the message of a `ur:leek-pczt`
/// or `ur:leek-zsig` (Hummingbird's `UR.fromBytes`).
pub fn wrap_bytes(payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + 5);
    cbor_head(&mut out, 2, payload.len() as u64);
    out.extend_from_slice(payload);
    out
}

/// The bytes of a message that is exactly one CBOR byte string, as
/// `airgap.c`'s `cbor_bstr` reads it.
pub fn unwrap_bytes(message: &[u8]) -> Result<&[u8], UrError> {
    let (major, len, n) = cbor_read_head(message).ok_or(UrError::NotBytes)?;
    if major != 2 || n + len as usize != message.len() {
        return Err(UrError::NotBytes);
    }
    Ok(&message[n..])
}

// ------------------------------------------------------------- the fountain

struct Xoshiro256 {
    s: [u64; 4],
}

impl Xoshiro256 {
    fn from_digest(d: &[u8; 32]) -> Self {
        let mut s = [0u64; 4];
        for (i, w) in s.iter_mut().enumerate() {
            *w = u64::from_be_bytes(d[i * 8..i * 8 + 8].try_into().unwrap());
        }
        Xoshiro256 { s }
    }

    fn next(&mut self) -> u64 {
        let s = &mut self.s;
        let result = s[1].wrapping_mul(5).rotate_left(7).wrapping_mul(9);
        let t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = s[3].rotate_left(45);
        result
    }

    fn next_double(&mut self) -> f64 {
        self.next() as f64 / 18446744073709551616.0
    }

    fn next_int(&mut self, low: u64, high: u64) -> u64 {
        (self.next_double() * (high - low + 1) as f64) as u64 + low
    }
}

/// Walker's alias method over 1/1 .. 1/n, built exactly as bc-ur (and
/// `ur-fountain.c`) builds it: the table is part of the wire behaviour.
fn choose_degree(n: usize, rng: &mut Xoshiro256) -> usize {
    let sum: f64 = (1..=n).map(|i| 1.0 / i as f64).sum();
    let mut p: Vec<f64> = (0..n).map(|i| (1.0 / (i + 1) as f64) * n as f64 / sum).collect();
    let mut small = Vec::with_capacity(n);
    let mut large = Vec::with_capacity(n);
    for i in (0..n).rev() {
        if p[i] < 1.0 {
            small.push(i);
        } else {
            large.push(i);
        }
    }
    let mut probs = vec![0.0f64; n];
    let mut aliases = vec![0usize; n];
    while !small.is_empty() && !large.is_empty() {
        let a = small.pop().unwrap();
        let g = large.pop().unwrap();
        probs[a] = p[a];
        aliases[a] = g;
        p[g] += p[a] - 1.0;
        if p[g] < 1.0 {
            small.push(g);
        } else {
            large.push(g);
        }
    }
    while let Some(g) = large.pop() {
        probs[g] = 1.0;
    }
    while let Some(a) = small.pop() {
        probs[a] = 1.0;
    }
    let r1 = rng.next_double();
    let r2 = rng.next_double();
    let i = (n as f64 * r1) as usize;
    (if r2 < probs[i] { i } else { aliases[i] }) + 1
}

/// The fragments (0-based, as a bit mask) part `seq_num` (1-based) carries.
/// Parts 1..=seq_len are the plain fragments in order; later ones are the
/// subset both ends derive from the sequence number and the checksum.
pub fn fountain_fragments(seq_num: u32, seq_len: u32, checksum: u32) -> Option<u128> {
    if seq_num == 0 || seq_len == 0 || seq_len > MAX_PARTS {
        return None;
    }
    if seq_num <= seq_len {
        return Some(1u128 << (seq_num - 1));
    }
    let mut seed = [0u8; 8];
    seed[..4].copy_from_slice(&seq_num.to_be_bytes());
    seed[4..].copy_from_slice(&checksum.to_be_bytes());
    let digest: [u8; 32] = Sha256::digest(seed).into();
    let mut rng = Xoshiro256::from_digest(&digest);
    let degree = choose_degree(seq_len as usize, &mut rng);
    let mut remaining: Vec<u32> = (0..seq_len).collect();
    let mut mask = 0u128;
    for _ in 0..degree {
        if remaining.is_empty() {
            break;
        }
        let index = rng.next_int(0, remaining.len() as u64 - 1) as usize;
        mask |= 1u128 << remaining.remove(index);
    }
    Some(mask)
}

/// The fragment indices of a mask, ascending.
pub fn mask_indices(mask: u128) -> Vec<u32> {
    (0..128).filter(|i| mask & (1u128 << i) != 0).collect()
}

// ----------------------------------------------------------------- the type

fn valid_type(t: &str) -> bool {
    !t.is_empty()
        && t.len() <= TYPE_MAX
        && !t.starts_with('-')
        && !t.ends_with('-')
        && t.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-')
}

/// A single-part UR, `ur:<type>/<bytewords>`, lowercase.
pub fn encode_single(ur_type: &str, message: &[u8]) -> Result<String, UrError> {
    if !valid_type(ur_type) {
        return Err(UrError::NotUr);
    }
    Ok(format!("ur:{ur_type}/{}", bytewords_encode(message)))
}

/// Uppercased for a QR code's alphanumeric mode (every UR character is in
/// that set once uppercased: 5.5 bits a character instead of 8). Every
/// reader here, the board's included, takes either case.
pub fn to_upper(part: &str) -> String {
    part.to_ascii_uppercase()
}

// -------------------------------------------------------------- the encoder

/// Animated-QR parts for one message, byte-identical to bc-ur's
/// `UREncoder` (and to `ur-encoder.c`).
#[derive(Clone, Debug)]
pub struct Encoder {
    ur_type: String,
    message: Vec<u8>,
    checksum: u32,
    fragment_len: usize,
    seq_len: u32,
}

impl Encoder {
    /// Prepare `message` in fragments of at most `max_fragment_len` bytes
    /// (clamped to [`MIN_FRAGMENT_LEN`]..=[`MAX_FRAGMENT_LEN`]): the fewest
    /// fragments that fit, then the smallest equal length that covers the
    /// message in that many (bc-ur's nominal fragment length).
    pub fn new(ur_type: &str, message: &[u8], max_fragment_len: usize) -> Result<Self, UrError> {
        if !valid_type(ur_type) {
            return Err(UrError::NotUr);
        }
        if message.is_empty() {
            return Err(UrError::Empty);
        }
        if message.len() > u32::MAX as usize {
            return Err(UrError::TooLarge);
        }
        let max = max_fragment_len.clamp(MIN_FRAGMENT_LEN, MAX_FRAGMENT_LEN);
        let count = message.len().div_ceil(max);
        if count > MAX_PARTS as usize {
            return Err(UrError::TooLarge);
        }
        Ok(Encoder {
            ur_type: ur_type.to_string(),
            message: message.to_vec(),
            checksum: crc32(message),
            fragment_len: message.len().div_ceil(count),
            seq_len: count as u32,
        })
    }

    /// `ur:leek-pczt` for a signer view: the view in a CBOR byte string.
    pub fn leek_pczt(view: &[u8], max_fragment_len: usize) -> Result<Self, UrError> {
        if view.is_empty() {
            return Err(UrError::Empty);
        }
        Encoder::new(UR_PCZT, &wrap_bytes(view), max_fragment_len)
    }

    /// `ur:leek-zsig` for a `.zsig` (the board's reply; for tests and tools).
    pub fn leek_zsig(zsig: &[u8], max_fragment_len: usize) -> Result<Self, UrError> {
        if zsig.is_empty() {
            return Err(UrError::Empty);
        }
        Encoder::new(UR_ZSIG, &wrap_bytes(zsig), max_fragment_len)
    }

    pub fn seq_len(&self) -> u32 {
        self.seq_len
    }
    pub fn fragment_len(&self) -> usize {
        self.fragment_len
    }
    pub fn message_len(&self) -> usize {
        self.message.len()
    }
    pub fn checksum(&self) -> u32 {
        self.checksum
    }
    pub fn ur_type(&self) -> &str {
        &self.ur_type
    }
    /// One fragment holds the message: a still `ur:<type>/<body>`.
    pub fn is_single_part(&self) -> bool {
        self.seq_len == 1
    }

    fn frag_byte(&self, index: u32, i: usize) -> u8 {
        let at = index as usize * self.fragment_len + i;
        self.message.get(at).copied().unwrap_or(0)
    }

    /// The part with sequence number `seq_num` (1-based), lowercase. A
    /// single-part message ignores `seq_num`; 0 is refused.
    pub fn part(&self, seq_num: u32) -> Result<String, UrError> {
        if self.is_single_part() {
            return encode_single(&self.ur_type, &self.message);
        }
        let mask = fountain_fragments(seq_num, self.seq_len, self.checksum).ok_or(UrError::Malformed)?;
        let mut fragment = vec![0u8; self.fragment_len];
        for f in mask_indices(mask) {
            for (i, b) in fragment.iter_mut().enumerate() {
                *b ^= self.frag_byte(f, i);
            }
        }
        let mut cbor = Vec::with_capacity(self.fragment_len + 24);
        cbor_head(&mut cbor, 4, 5);
        cbor_head(&mut cbor, 0, seq_num as u64);
        cbor_head(&mut cbor, 0, self.seq_len as u64);
        cbor_head(&mut cbor, 0, self.message.len() as u64);
        cbor_head(&mut cbor, 0, self.checksum as u64);
        cbor_head(&mut cbor, 2, self.fragment_len as u64);
        cbor.extend_from_slice(&fragment);
        Ok(format!("ur:{}/{}-{}/{}", self.ur_type, seq_num, self.seq_len, bytewords_encode(&cbor)))
    }

    /// Frame `index` (0-based) of an endless loop: sequence numbers
    /// 1..=[`SEQ_WRAP`] and round again, so every frame is no longer than
    /// [`Encoder::longest_frame`]. Uppercase, ready for the QR encoder.
    pub fn frame(&self, index: u64) -> String {
        let seq = (index % SEQ_WRAP as u64) as u32 + 1;
        // seq is in 1..=255 and seq_len in 1..=128, so this cannot fail.
        to_upper(&self.part(seq).unwrap_or_default())
    }

    /// The longest frame of the loop, in characters: what the QR version
    /// is chosen for, so the code keeps one size while it animates.
    pub fn longest_frame(&self) -> usize {
        if self.is_single_part() {
            return self.frame(0).len();
        }
        // The length grows only with the digits and CBOR width of seq_num.
        [1, 9, 10, 23, 24, 99, 100, SEQ_WRAP]
            .iter()
            .map(|&s: &u32| self.part(s).map(|p| p.len()).unwrap_or(0))
            .max()
            .unwrap_or(0)
    }
}

// -------------------------------------------------------------- the decoder

/// What a part did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Received {
    /// Taken; keep scanning.
    Accepted,
    /// Valid, but nothing new (a repeat, or the queue of mixed parts full).
    Redundant,
    /// The message is assembled and its checksum matched.
    Complete,
}

/// The decoder's bounds. Every field of a part is attacker-chosen; these
/// are refused at the first part rather than part-way through.
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    /// Largest message accepted, in bytes (CBOR envelope included).
    pub max_message: usize,
    /// Mixed parts held back waiting to be reducible.
    pub max_mixed: usize,
}

impl Default for Limits {
    /// 64 KiB, and 32 mixed parts (the board holds 8; a host has room).
    fn default() -> Self {
        Limits { max_message: 64 * 1024, max_mixed: 32 }
    }
}

#[derive(Clone, Debug)]
struct Pinned {
    ur_type: String,
    seq_len: u32,
    message_len: u32,
    checksum: u32,
    fragment_len: usize,
}

/// Assembles a message from single- or multi-part UR strings, the way
/// `ur-decoder.c` does: metadata pinned at the first accepted part, each
/// arrival reduced by the fragments already known, a mixed part that
/// collapses to one fragment promoted, and the CRC-32 of the whole checked
/// at the end.
#[derive(Clone, Debug, Default)]
pub struct Decoder {
    limits: Limits,
    pinned: Option<Pinned>,
    received: u128,
    fragments: Vec<Vec<u8>>,
    mixed: Vec<(u128, Vec<u8>)>,
    message: Option<Vec<u8>>,
}

struct Part {
    ur_type: String,
    seq_num: u32,
    seq_len: u32,
    message_len: u32,
    checksum: u32,
    data: Vec<u8>,
}

/// "<seq_num>-<seq_len>", digits only, as `parse_seq`.
fn parse_seq(s: &str) -> Option<(u32, u32)> {
    let (a, b) = s.split_once('-')?;
    let num = |x: &str| -> Option<u32> {
        if x.is_empty() || x.len() > 7 || !x.bytes().all(|c| c.is_ascii_digit()) {
            return None;
        }
        x.parse().ok()
    };
    Some((num(a)?, num(b)?))
}

fn parse_part(ur: &str) -> Result<Part, UrError> {
    let ur = ur.trim();
    if ur.len() < 4 || !ur.is_char_boundary(3) || !ur[..3].eq_ignore_ascii_case("ur:") {
        return Err(UrError::NotUr);
    }
    let rest = &ur[3..];
    let pieces: Vec<&str> = rest.split('/').collect();
    let ur_type = pieces[0].to_ascii_lowercase();
    if !valid_type(&ur_type) {
        return Err(UrError::NotUr);
    }
    match pieces.len() {
        2 => {
            let raw = bytewords_decode(pieces[1], PART_MAX)?;
            Ok(Part {
                ur_type,
                seq_num: 1,
                seq_len: 1,
                message_len: raw.len() as u32,
                checksum: crc32(&raw),
                data: raw,
            })
        }
        3 => {
            let (hdr_num, hdr_len) = parse_seq(pieces[1]).ok_or(UrError::Malformed)?;
            let raw = bytewords_decode(pieces[2], PART_MAX)?;
            // [seq_num, seq_len, message_len, checksum, fragment]
            let mut at = 0usize;
            let mut next = |want: u8| -> Result<u32, UrError> {
                let (major, v, n) = cbor_read_head(&raw[at..]).ok_or(UrError::Malformed)?;
                if major != want {
                    return Err(UrError::Malformed);
                }
                at += n;
                Ok(v)
            };
            if next(4)? != 5 {
                return Err(UrError::Malformed);
            }
            let seq_num = next(0)?;
            let seq_len = next(0)?;
            let message_len = next(0)?;
            let checksum = next(0)?;
            let data_len = next(2)? as usize;
            if at + data_len != raw.len() {
                return Err(UrError::Malformed); // short, or trailing bytes
            }
            if hdr_num != seq_num || hdr_len != seq_len {
                return Err(UrError::Malformed);
            }
            Ok(Part { ur_type, seq_num, seq_len, message_len, checksum, data: raw[at..].to_vec() })
        }
        _ => Err(UrError::NotUr),
    }
}

/// The UR type a scanned string names, lowercased, if it looks like a UR.
pub fn part_type(ur: &str) -> Option<String> {
    let ur = ur.trim();
    if ur.len() < 4 || !ur.is_char_boundary(3) || !ur[..3].eq_ignore_ascii_case("ur:") {
        return None;
    }
    let t = ur[3..].split('/').next()?.to_ascii_lowercase();
    valid_type(&t).then_some(t)
}

impl Decoder {
    pub fn new() -> Self {
        Decoder::with_limits(Limits::default())
    }

    pub fn with_limits(limits: Limits) -> Self {
        Decoder { limits, ..Default::default() }
    }

    /// Forget everything, keeping the limits.
    pub fn reset(&mut self) {
        *self = Decoder::with_limits(self.limits);
    }

    /// The type of the message being assembled, once a part was taken.
    pub fn ur_type(&self) -> Option<&str> {
        self.pinned.as_ref().map(|p| p.ur_type.as_str())
    }

    /// Fragments in the message (0 before the first part).
    pub fn seq_len(&self) -> u32 {
        self.pinned.as_ref().map_or(0, |p| p.seq_len)
    }

    /// Fragments known so far.
    pub fn received(&self) -> u32 {
        self.received.count_ones()
    }

    /// Fragments still missing, for a progress line.
    pub fn remaining(&self) -> u32 {
        self.seq_len() - self.received()
    }

    pub fn is_complete(&self) -> bool {
        self.message.is_some()
    }

    /// The assembled message, once complete.
    pub fn message(&self) -> Option<&[u8]> {
        self.message.as_deref()
    }

    /// Feed one scanned string. An `Err` means the part was refused and
    /// nothing changed, except [`UrError::Checksum`], which also drops the
    /// assembly (it cannot be repaired).
    pub fn receive(&mut self, ur: &str) -> Result<Received, UrError> {
        if self.is_complete() {
            return Ok(Received::Redundant);
        }
        let part = parse_part(ur)?;
        let data_len = part.data.len();
        if part.seq_num == 0 || part.seq_len == 0 || data_len == 0 || part.message_len == 0 {
            return Err(UrError::Malformed);
        }
        if part.seq_len > MAX_PARTS {
            return Err(UrError::TooLarge);
        }
        // The fragment length is implied: no shorter last fragment, no slack.
        let (sl, dl, ml) = (part.seq_len as u64, data_len as u64, part.message_len as u64);
        if sl * dl < ml || (sl - 1) * dl >= ml {
            return Err(UrError::Malformed);
        }
        match &self.pinned {
            None => {
                if part.message_len as usize > self.limits.max_message {
                    return Err(UrError::TooLarge);
                }
                self.pinned = Some(Pinned {
                    ur_type: part.ur_type.clone(),
                    seq_len: part.seq_len,
                    message_len: part.message_len,
                    checksum: part.checksum,
                    fragment_len: data_len,
                });
                self.fragments = vec![Vec::new(); part.seq_len as usize];
            }
            Some(p) => {
                if p.seq_len != part.seq_len
                    || p.message_len != part.message_len
                    || p.checksum != part.checksum
                    || p.fragment_len != data_len
                    || p.ur_type != part.ur_type
                {
                    return Err(UrError::Mismatch);
                }
            }
        }
        let (seq_len, checksum) = (part.seq_len, part.checksum);
        let mut mask = fountain_fragments(part.seq_num, seq_len, checksum).ok_or(UrError::Malformed)?;
        let mut work = part.data;
        self.reduce(&mut mask, &mut work);
        match mask.count_ones() {
            0 => return Ok(Received::Redundant),
            1 => {
                let idx = mask.trailing_zeros();
                self.absorb(idx, work);
            }
            _ => {
                if self.mixed.len() >= self.limits.max_mixed {
                    return Ok(Received::Redundant);
                }
                self.mixed.push((mask, work));
            }
        }
        if self.received() < seq_len {
            return Ok(Received::Accepted);
        }
        let p = self.pinned.as_ref().expect("pinned above");
        let mut msg: Vec<u8> = self.fragments.concat();
        msg.truncate(p.message_len as usize);
        if crc32(&msg) != p.checksum {
            self.reset();
            return Err(UrError::Checksum);
        }
        self.mixed.clear();
        self.message = Some(msg);
        Ok(Received::Complete)
    }

    fn reduce(&self, mask: &mut u128, data: &mut [u8]) {
        for i in mask_indices(*mask & self.received) {
            for (d, s) in data.iter_mut().zip(&self.fragments[i as usize]) {
                *d ^= s;
            }
            *mask &= !(1u128 << i);
        }
    }

    fn absorb(&mut self, index: u32, data: Vec<u8>) {
        self.fragments[index as usize] = data;
        self.received |= 1u128 << index;
        let mut progressed = true;
        while progressed {
            progressed = false;
            let mut keep = Vec::with_capacity(self.mixed.len());
            for (mut m, mut d) in std::mem::take(&mut self.mixed) {
                self.reduce(&mut m, &mut d);
                match m.count_ones() {
                    0 => {}
                    1 => {
                        let idx = m.trailing_zeros();
                        self.fragments[idx as usize] = d;
                        self.received |= 1u128 << idx;
                        progressed = true;
                    }
                    _ => keep.push((m, d)),
                }
            }
            self.mixed = keep;
        }
    }
}

/// Feed `parts` (blank lines and `#` comments skipped) until a message of
/// type `want` is complete; returns the bytes inside its CBOR byte string.
/// For `zecz finish --qr-zsig` and the tests: a whole recording at once.
pub fn decode_bytes<'a>(want: &str, parts: impl IntoIterator<Item = &'a str>) -> Result<Vec<u8>, UrError> {
    let mut d = Decoder::new();
    for line in parts {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some(t) = part_type(line) {
            if t != want {
                return Err(UrError::WrongType { found: t, want: want.to_string() });
            }
        }
        if d.receive(line)? == Received::Complete {
            return Ok(unwrap_bytes(d.message().expect("complete"))?.to_vec());
        }
    }
    Err(UrError::Incomplete { remaining: if d.seq_len() == 0 { 1 } else { d.remaining() } })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_check_value() {
        assert_eq!(crc32(b""), 0);
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn bytes_wrap_round_trip() {
        for n in [0usize, 1, 23, 24, 255, 256, 1430, 70_000] {
            let p: Vec<u8> = (0..n).map(|i| i as u8).collect();
            let w = wrap_bytes(&p);
            assert_eq!(unwrap_bytes(&w).unwrap(), &p[..]);
        }
        assert_eq!(unwrap_bytes(&[0x43, 1, 2]), Err(UrError::NotBytes)); // short
        assert_eq!(unwrap_bytes(&[0x42, 1, 2, 3]), Err(UrError::NotBytes)); // trailing
        assert_eq!(unwrap_bytes(&[0x62, b'a', b'b']), Err(UrError::NotBytes)); // text
    }

    #[test]
    fn seq_header_is_digits_only() {
        assert_eq!(parse_seq("1-5"), Some((1, 5)));
        assert_eq!(parse_seq("a-5"), None);
        assert_eq!(parse_seq("1-"), None);
        assert_eq!(parse_seq("-5"), None);
        assert_eq!(parse_seq("+1-5"), None);
    }

    #[test]
    fn types_are_checked() {
        assert!(Encoder::new("Leek", b"x", 50).is_err());
        assert!(Encoder::new("-leek", b"x", 50).is_err());
        assert!(Encoder::new("leek-pczt", b"", 50).is_err());
        assert_eq!(part_type("UR:LEEK-ZSIG/1-2/AB"), Some("leek-zsig".into()));
        assert_eq!(part_type("zcash:t1abc"), None);
    }
}
