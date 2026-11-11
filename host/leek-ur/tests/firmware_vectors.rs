//! Against the firmware's own vectors, read out of `firmware/sim/test_ur*.c`
//! rather than copied, so the two implementations cannot drift apart
//! silently: every vector there was printed by Blockchain Commons' bc-ur.

use std::path::PathBuf;

use leek_ur::{
    bytewords_decode, bytewords_encode, crc32, fountain_fragments, mask_indices, unwrap_bytes, Decoder, Encoder,
    Received, UrError,
};

fn sim(file: &str) -> String {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../firmware/sim").join(file);
    std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

/// The C array starting at `decl` up to its closing `};`.
fn c_block<'a>(src: &'a str, decl: &str) -> &'a str {
    let start = src.find(decl).unwrap_or_else(|| panic!("no {decl}"));
    let rest = &src[start..];
    &rest[..rest.find("};").expect("unterminated")]
}

fn quoted(s: &str) -> Vec<String> {
    s.split('"').skip(1).step_by(2).map(str::to_string).collect()
}

fn unhex(h: &str) -> Vec<u8> {
    (0..h.len()).step_by(2).map(|i| u8::from_str_radix(&h[i..i + 2], 16).unwrap()).collect()
}

/// 250 bytes, msg[i] = (i*7+3) & 0xFF: the message of the bc-ur parts.
fn reference_message() -> Vec<u8> {
    (0..250usize).map(|i| ((i * 7 + 3) & 0xFF) as u8).collect()
}

fn reference_parts(file: &str) -> Vec<String> {
    let src = sim(file);
    assert!(src.contains("msg[i] = (i*7+3) & 0xFF"), "{file}: the message rule changed");
    let parts = quoted(c_block(&src, "static const char *PARTS[]"));
    assert!(parts.len() >= 15, "{file}: only {} parts found", parts.len());
    parts
}

#[test]
fn bytewords_match_bc_ur() {
    let src = sim("test_ur.c");
    let v = quoted(c_block(&src, "VECTORS[] = {"));
    assert!(v.len() >= 8 && v.len() % 2 == 0);
    for pair in v.chunks(2) {
        let data = unhex(&pair[0]);
        assert_eq!(bytewords_encode(&data), pair[1], "encode {}", pair[0]);
        assert_eq!(bytewords_decode(&pair[1], 64).unwrap(), data);
        assert_eq!(bytewords_decode(&pair[1].to_uppercase(), 64).unwrap(), data);
    }
    assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
}

#[test]
fn every_single_character_corruption_is_refused() {
    let good = bytewords_encode(&[0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22]);
    for i in 0..good.len() {
        let mut b = good.clone().into_bytes();
        b[i] = if b[i] == b'a' { b'z' } else { b'a' };
        assert!(bytewords_decode(std::str::from_utf8(&b).unwrap(), 64).is_err(), "char {i}");
    }
    assert!(bytewords_decode("aeadao", 64).is_err());
    assert!(bytewords_decode("aeadaolalyzmspaacan", 64).is_err());
    assert!(bytewords_decode("qqqqqqqq", 64).is_err());
    assert_eq!(bytewords_decode("aeadaolalyzmspaacane", 2), Err(UrError::TooLarge));
}

#[test]
fn fountain_matches_bc_ur() {
    let src = sim("test_ur_fountain.c");
    let block = c_block(&src, "VECTORS[] = {");
    let mut n = 0;
    for line in block.lines().map(str::trim).filter(|l| l.starts_with("{ ") && l.ends_with("},")) {
        // { seq_num, seq_len, checksumu, { a,b,c }, count },
        let inner = &line[1..line.len() - 2];
        let (head, rest) = inner.split_once('{').unwrap();
        let (frags, count) = rest.split_once('}').unwrap();
        let head: Vec<u32> = head
            .split(',')
            .map(|x| x.trim().trim_end_matches('u'))
            .filter(|x| !x.is_empty())
            .map(|x| x.parse().unwrap())
            .collect();
        let want: Vec<u32> = frags.split(',').map(|x| x.trim().parse().unwrap()).collect();
        let count: usize = count.trim_matches(|c: char| c == ',' || c.is_whitespace()).parse().unwrap();
        assert_eq!(want.len(), count);
        let mask = fountain_fragments(head[0], head[1], head[2]).unwrap();
        assert_eq!(mask_indices(mask), want, "part {} of {} checksum {}", head[0], head[1], head[2]);
        n += 1;
    }
    assert!(n >= 40, "only {n} fountain vectors parsed");
}

#[test]
fn encoder_reproduces_bc_ur_parts() {
    let parts = reference_parts("test_ur_encoder.c");
    let e = Encoder::new("bytes", &reference_message(), 60).unwrap();
    assert_eq!((e.seq_len(), e.fragment_len()), (5, 50));
    for (i, want) in parts.iter().enumerate() {
        assert_eq!(&e.part(i as u32 + 1).unwrap(), want, "part {}", i + 1);
    }
    // The decoder suite holds the same parts.
    assert_eq!(reference_parts("test_ur_decoder.c"), parts);
}

fn feed_all(d: &mut Decoder, parts: &[String]) -> Option<usize> {
    for (i, p) in parts.iter().enumerate() {
        match d.receive(p).unwrap_or_else(|e| panic!("part {} refused: {e}", i + 1)) {
            Received::Complete => return Some(i + 1),
            Received::Accepted | Received::Redundant => {}
        }
    }
    None
}

#[test]
fn decoder_reads_bc_ur_parts() {
    let parts = reference_parts("test_ur_decoder.c");
    let msg = reference_message();

    let mut d = Decoder::new();
    assert_eq!(feed_all(&mut d, &parts), Some(5));
    assert_eq!(d.message().unwrap(), &msg[..]);

    // Joining late: the first three plain fragments only exist mixed.
    let mut d = Decoder::new();
    let late: Vec<String> = parts[3..].iter().cycle().take(3 * (parts.len() - 3)).cloned().collect();
    assert!(feed_all(&mut d, &late).is_some());
    assert_eq!(d.message().unwrap(), &msg[..]);

    // Every third frame dropped, uppercase as a QR reader hands it over.
    let mut d = Decoder::new();
    let dropped: Vec<String> =
        parts.iter().enumerate().filter(|(i, _)| i % 3 != 2).map(|(_, p)| p.to_uppercase()).collect();
    assert!(feed_all(&mut d, &dropped).is_some());
    assert_eq!(d.message().unwrap(), &msg[..]);

    // The single-part vector: the message is deadbeef itself (the suite
    // checks four bytes), so it is not a CBOR byte string.
    let mut d = Decoder::new();
    assert_eq!(d.receive("ur:bytes/uepmrnwskensotht"), Ok(Received::Complete));
    assert_eq!(d.message().unwrap(), &[0xDE, 0xAD, 0xBE, 0xEF]);
    assert_eq!(unwrap_bytes(d.message().unwrap()), Err(UrError::NotBytes));
}

#[test]
fn decoder_refuses_what_the_board_refuses() {
    let parts = reference_parts("test_ur_decoder.c");
    let mut d = Decoder::new();
    for bad in [
        "ur:bytes/0-5/lpadahcszscyvadpiyhnhdey",
        "ur:bytes/1-0/lpadahcszscyvadpiyhnhdey",
        "ur:bytes/1-5/",
        "ur:bytes/a-5/lpadahcszscyvadpiyhnhdey",
        "bytes/1-5/lpadahcszscyvadpiyhnhdey",
        "ur:bytes/1-5/x/y",
    ] {
        assert!(d.receive(bad).is_err(), "{bad} accepted");
    }
    // A header that disagrees with the CBOR inside.
    let lie = parts[0].replacen("/1-5/", "/2-5/", 1);
    assert!(d.receive(&lie).is_err());

    // Pinned at the first part: another message cannot join in.
    let mut d = Decoder::new();
    assert_eq!(d.receive(&parts[0]), Ok(Received::Accepted));
    let other = Encoder::new("bytes", &[7u8; 250], 60).unwrap().part(2).unwrap();
    assert_eq!(d.receive(&other), Err(UrError::Mismatch));
    let other_type = parts[1].replacen("ur:bytes/", "ur:leek-zsig/", 1);
    assert_eq!(d.receive(&other_type), Err(UrError::Mismatch));
    // And the assembly still finishes.
    assert!(feed_all(&mut d, &parts[1..]).is_some());
    assert_eq!(d.message().unwrap(), &reference_message()[..]);
}

#[test]
fn round_trips_with_frames_dropped() {
    let msg: Vec<u8> = (0..1100usize).map(|i| (i * 13 + 5) as u8).collect();
    for len in [1usize, 9, 10, 11, 59, 60, 61, 90, 91, 250, 777, 1100] {
        for frag in [10usize, 11, 50, 60, 90, 150, 250] {
            if len.div_ceil(frag) > 128 {
                continue;
            }
            for drop in [0u32, 30] {
                let e = Encoder::new("leek-pczt", &msg[..len], frag).unwrap();
                let mut d = Decoder::new();
                let mut seed = 1u32;
                let mut done = false;
                for i in 0..2000u64 {
                    seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
                    if (seed >> 16) % 100 < drop {
                        continue;
                    }
                    if d.receive(&e.frame(i)).unwrap() == Received::Complete {
                        done = true;
                        break;
                    }
                }
                assert!(done, "{len} bytes at {frag}, {drop}% dropped");
                assert_eq!(d.message().unwrap(), &msg[..len]);
            }
        }
    }
}

#[test]
fn frames_loop_without_growing() {
    let e = Encoder::leek_pczt(&[0x55; 1430], 50).unwrap();
    assert_eq!(e.seq_len(), 29);
    let longest = e.longest_frame();
    for i in 0..600u64 {
        let f = e.frame(i);
        assert!(f.len() <= longest, "frame {i} is {} > {longest}", f.len());
        assert!(f.bytes().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || b":/-".contains(&c)));
    }
    assert_eq!(e.frame(0), e.frame(255));
    assert!(e.frame(0).starts_with("UR:LEEK-PCZT/1-29/"));
}
