//! The QR air gap for `zecz` (EXPERIMENTAL): the signer view out as an
//! animated `ur:leek-pczt`, the board's `.zsig` back as `ur:leek-zsig`.
//!
//! The UR encoding is `leek-ur` (host/leek-ur), the one the Zcash LeekWallet
//! app uses, held to the firmware's vectors. This file only draws it: in the
//! terminal with half-block characters, or as a self-contained HTML page for
//! a browser, and reads it back from a parts file or (feature `qr-camera`)
//! a V4L2 webcam.
//!
//! The defaults, shared with the app: 90-byte fragments, 500 ms a frame,
//! where the Ethereum LeekWallet's board benches on this same ESP32-S3 board
//! settled (a 1.4 KB compact view is 16 frames, 8 s a pass). The rule from
//! those benches: each frame stays up at least twice the board camera's
//! capture interval (300 ms made it miss most frames). The Zodl fork signed
//! at 50 B / 500 ms; 50 stays one step down (`--qr-fragment 50`). For the
//! camera-tuning sweep, `zecz qr-show --sweep` steps through [`PRESETS`].

use std::fmt::Write as _;
use std::io::Write as _;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use leek_ur::{Decoder, Encoder, Received, UR_ZSIG};
use qrcode::{Color, EcLevel, QrCode, Version};

/// The default fragment size, bytes (the app's "Safe" preset). The board
/// bench of 2026-11-10 read 50 B in 71 s at 2x zoom and 40 B in 60 s; it
/// found 90 B codes but rarely decoded them (docs/zcash/QR-BENCH.md).
pub const DEFAULT_FRAGMENT: usize = 50;
/// The default frame interval, ms.
pub const DEFAULT_INTERVAL_MS: u64 = 500;

/// The sweep's presets, (fragment bytes, frame ms), safest first. The app's
/// Safe / Balanced / Fast are 50/500, 50/400 and 50/300. The board decodes
/// about 3.5 frames a second with a code in view, so frames under ~300 ms
/// and fragments over ~50 B are here to measure, not to use yet.
pub const PRESETS: [(usize, u64); 6] = [(50, 500), (40, 500), (30, 500), (50, 400), (50, 300), (90, 500)];
/// A quiet zone of four modules, as the QR standard asks.
const QUIET: usize = 4;

/// The QR code of each frame of `e`'s loop, all at one version (the
/// smallest that holds the longest frame, error correction L), so the code
/// keeps its size while it animates. Index `i` is frame `i`; the loop wraps
/// at [`leek_ur::SEQ_WRAP`] parts.
pub struct Frames {
    pub version: i16,
    pub frames: Vec<(String, QrCode)>,
    pub seq_len: u32,
}

pub fn frames(e: &Encoder) -> Result<Frames> {
    let n = if e.is_single_part() { 1 } else { leek_ur::SEQ_WRAP as u64 };
    let texts: Vec<String> = (0..n).map(|i| e.frame(i)).collect();
    let longest = texts.iter().max_by_key(|t| t.len()).expect("one frame at least");
    let version = (1..=40i16)
        .find(|&v| QrCode::with_version(longest.as_bytes(), Version::Normal(v), EcLevel::L).is_ok())
        .ok_or_else(|| anyhow!("a frame of {} characters fits no QR version", longest.len()))?;
    let frames = texts
        .into_iter()
        .map(|t| {
            let qr = QrCode::with_version(t.as_bytes(), Version::Normal(version), EcLevel::L)
                .map_err(|err| anyhow!("QR encoding: {err:?}"))?;
            Ok((t, qr))
        })
        .collect::<Result<Vec<_>>>()?;
    Ok(Frames { version, frames, seq_len: e.seq_len() })
}

/// Dark modules, row by row, with the quiet zone around them.
fn modules(qr: &QrCode) -> Vec<Vec<bool>> {
    let w = qr.width();
    let colors = qr.to_colors();
    let size = w + 2 * QUIET;
    (0..size)
        .map(|y| {
            (0..size)
                .map(|x| {
                    x >= QUIET && y >= QUIET && x < w + QUIET && y < w + QUIET && colors[(y - QUIET) * w + (x - QUIET)] == Color::Dark
                })
                .collect()
        })
        .collect()
}

/// The code as half-block lines: one character per module across, two
/// modules per line down (`▀` top dark, `▄` bottom dark, `█` both, space
/// neither), quiet zone included. Drawn black on white by [`show_terminal`].
pub fn half_blocks(qr: &QrCode) -> Vec<String> {
    let m = modules(qr);
    let mut lines = Vec::with_capacity(m.len().div_ceil(2));
    for pair in m.chunks(2) {
        let top = &pair[0];
        let bottom = pair.get(1);
        let line: String = (0..top.len())
            .map(|x| match (top[x], bottom.is_some_and(|b| b[x])) {
                (true, true) => '█',
                (true, false) => '▀',
                (false, true) => '▄',
                (false, false) => ' ',
            })
            .collect();
        lines.push(line);
    }
    lines
}

/// What a frame looks like on a terminal: black modules on bright white,
/// whatever the terminal's own colours.
pub fn terminal_frame(f: &Frames, index: usize, interval_ms: u64) -> String {
    let (text, qr) = &f.frames[index % f.frames.len()];
    let mut s = String::from("\x1b[H\x1b[2J");
    for line in half_blocks(qr) {
        let _ = writeln!(s, "\x1b[30;107m{line}\x1b[0m");
    }
    let seq = text.split('/').nth(1).filter(|x| x.contains('-')).unwrap_or("1-1");
    let _ = writeln!(
        s,
        "EXPERIMENTAL QR signing. Part {seq} ({} fragment(s)), QR version {}, {interval_ms} ms a frame.",
        f.seq_len, f.version
    );
    let _ = writeln!(s, "On the board: Scan, point its camera here, review and ACCEPT. Press Enter (or Ctrl-C) to stop.");
    s
}

/// Loops `e` in the terminal until Enter or Ctrl-C.
pub fn show_terminal(e: &Encoder, interval_ms: u64) -> Result<()> {
    loop_until_enter(e, interval_ms, None)?;
    println!("Stopped. When the board shows its ur:leek-zsig, run `zecz qr-scan --out <file.zsig>`.");
    Ok(())
}

/// Loops `e` until Enter; returns the time from its first frame to the
/// key. `label` (the sweep's preset) is drawn above the counter.
fn loop_until_enter(e: &Encoder, interval_ms: u64, label: Option<&str>) -> Result<Duration> {
    let f = frames(e)?;
    let side = f.frames[0].1.width() + 2 * QUIET;
    eprintln!(
        "The code needs a terminal of at least {side} columns and {} rows; if it is cut off, enlarge the window, use a \
         smaller font, or --qr-html <file> for a browser.",
        side.div_ceil(2) + 3
    );
    let stop = Arc::new(AtomicBool::new(false));
    {
        let stop = stop.clone();
        std::thread::spawn(move || {
            let mut line = String::new();
            let _ = std::io::stdin().read_line(&mut line);
            stop.store(true, Ordering::Relaxed);
        });
    }
    let mut out = std::io::stdout();
    let mut i = 0usize;
    let start = std::time::Instant::now();
    while !stop.load(Ordering::Relaxed) {
        let mut frame = terminal_frame(&f, i, interval_ms);
        if let Some(l) = label {
            let _ = writeln!(frame, "{l}  ({:.1} s)", start.elapsed().as_secs_f64());
        }
        out.write_all(frame.as_bytes())?;
        out.flush()?;
        // Sleep in slices so Enter is seen within ~50 ms, for the timing.
        let until = std::time::Instant::now() + Duration::from_millis(interval_ms);
        while std::time::Instant::now() < until && !stop.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_millis(10));
        }
        i = (i + 1) % f.frames.len();
    }
    Ok(start.elapsed())
}

/// The preset label the sweep shows.
pub fn preset_label(n: usize, total: usize, frag: usize, ms: u64, e: &Encoder) -> String {
    format!(
        "SWEEP preset {n}/{total}: {frag} B fragments, {ms} ms a frame, {} frame(s) a pass ({:.1} s). \
         Press Enter when the board says the scan is complete.",
        e.seq_len(),
        e.seq_len() as f64 * ms as f64 / 1000.0
    )
}

/// `qr-show --sweep`: each preset in turn, Enter for the next, the time from
/// its first frame to Enter printed for each, then a table to copy into the
/// bench notes (docs/zcash/QR-BENCH.md). Restart the board's scan between
/// presets: a new fragment size is a new message to its decoder.
pub fn sweep(view: &[u8], presets: &[(usize, u64)]) -> Result<Vec<(usize, u64, u32, Duration)>> {
    let mut results = Vec::new();
    for (n, &(frag, ms)) in presets.iter().enumerate() {
        let e = Encoder::leek_pczt(view, frag).map_err(|e| anyhow!("{e}"))?;
        let label = preset_label(n + 1, presets.len(), frag, ms, &e);
        let took = loop_until_enter(&e, ms, Some(&label))?;
        results.push((frag, ms, e.seq_len(), took));
        println!("\x1b[H\x1b[2JPreset {}/{}: {frag} B / {ms} ms -> {:.1} s", n + 1, presets.len(), took.as_secs_f64());
        if n + 1 < presets.len() {
            println!("On the board: Cancel, then Scan again. Press Enter for the next preset.");
            let mut line = String::new();
            std::io::stdin().read_line(&mut line)?;
        }
    }
    println!("{}", sweep_table(&results));
    Ok(results)
}

/// The sweep's results as a Markdown table (docs/zcash/QR-BENCH.md's columns).
pub fn sweep_table(results: &[(usize, u64, u32, Duration)]) -> String {
    let mut t = String::from("| Fragment | Frame | Frames a pass | Time to complete |\n|---|---|---|---|\n");
    for (frag, ms, n, took) in results {
        let _ = writeln!(t, "| {frag} B | {ms} ms | {n} | {:.1} s |", took.as_secs_f64());
    }
    t
}

/// A self-contained page (no network: inline script and style, a CSP that
/// allows nothing else) that loops the code full-screen on a canvas, at a
/// whole number of pixels per module, black on white, with slower / faster /
/// pause and a frame counter.
pub fn html(e: &Encoder, interval_ms: u64) -> Result<String> {
    let f = frames(e)?;
    let size = f.frames[0].1.width() + 2 * QUIET;
    let mut data = String::from("[");
    for (i, (text, qr)) in f.frames.iter().enumerate() {
        if i > 0 {
            data.push(',');
        }
        let bits: String = modules(qr).iter().flatten().map(|&d| if d { '1' } else { '0' }).collect();
        let seq = text.split('/').nth(1).filter(|x| x.contains('-')).unwrap_or("1-1");
        let _ = write!(data, "[\"{seq}\",\"{bits}\"]");
    }
    data.push(']');
    Ok(HTML
        .replace("@SIZE@", &size.to_string())
        .replace("@INTERVAL@", &interval_ms.to_string())
        .replace("@VERSION@", &f.version.to_string())
        .replace("@SEQLEN@", &f.seq_len.to_string())
        .replace("@FRAGMENT@", &e.fragment_len().to_string())
        .replace("@FRAMES@", &data))
}

const HTML: &str = r##"<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LeekWallet: sign with QR (experimental)</title>
<style>
  html, body { margin: 0; height: 100%; background: #fff; color: #000; font: 16px system-ui, sans-serif; }
  main { display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100%; gap: 8px; }
  canvas { image-rendering: pixelated; }
  .bar { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; justify-content: center; }
  button { font: inherit; padding: 6px 14px; }
  p { margin: 0 8px; text-align: center; }
</style>
</head>
<body>
<main>
  <p><strong>EXPERIMENTAL</strong>: QR signing. On the board: Scan, point its camera here, review and ACCEPT.
     Then <code>zecz qr-scan --out signed.zsig</code>.</p>
  <canvas id="qr"></canvas>
  <div class="bar">
    <button id="slower">Slower</button>
    <button id="pause">Pause</button>
    <button id="faster">Faster</button>
    <span id="count"></span>
  </div>
  <p>ur:leek-pczt, @SEQLEN@ fragment(s) of @FRAGMENT@ B, QR version @VERSION@ (error correction L).
     A different fragment size: run <code>zecz qr-show --qr-fragment N --qr-html ...</code> again.</p>
</main>
<script>
"use strict";
const FRAMES = @FRAMES@;
const SIZE = @SIZE@;
let interval = @INTERVAL@, index = 0, paused = false, timer = 0;
const canvas = document.getElementById("qr"), ctx = canvas.getContext("2d");
function draw() {
  const avail = Math.min(window.innerWidth * 0.96, window.innerHeight * 0.80);
  const px = Math.max(1, Math.floor(avail / SIZE));
  canvas.width = canvas.height = px * SIZE;
  ctx.fillStyle = "#fff"; ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#000";
  const bits = FRAMES[index][1];
  for (let y = 0; y < SIZE; y++) for (let x = 0; x < SIZE; x++)
    if (bits.charCodeAt(y * SIZE + x) === 49) ctx.fillRect(x * px, y * px, px, px);
  document.getElementById("count").textContent =
    "part " + FRAMES[index][0] + " · " + interval + " ms · frame " + (index + 1) + "/" + FRAMES.length;
}
function tick() { draw(); if (!paused) index = (index + 1) % FRAMES.length; timer = setTimeout(tick, interval); }
function speed(f) { interval = Math.min(2000, Math.max(100, Math.round(interval * f / 50) * 50)); }
document.getElementById("slower").onclick = () => speed(1.5);
document.getElementById("faster").onclick = () => speed(1 / 1.5);
document.getElementById("pause").onclick = (ev) => { paused = !paused; ev.target.textContent = paused ? "Play" : "Pause"; };
window.onresize = draw;
tick();
</script>
</body>
</html>
"##;

pub fn write_html(e: &Encoder, interval_ms: u64, path: &Path) -> Result<()> {
    std::fs::write(path, html(e, interval_ms)?).with_context(|| format!("writing {}", path.display()))?;
    println!("QR page (experimental) -> {} : open it in a browser, full screen.", path.display());
    Ok(())
}

/// Progress of a scan, for the status line.
pub fn progress_line(d: &Decoder, parts: usize) -> String {
    if d.seq_len() == 0 {
        format!("{parts} QR read, no ur:leek-zsig part yet")
    } else {
        format!("{} of {} fragment(s), {parts} QR read", d.received(), d.seq_len())
    }
}

/// Feed one scanned text. Ok(Some(zsig)) once complete; a code of another
/// kind is ignored (Ok(None)), a broken ur:leek-zsig part is reported.
pub fn feed(d: &mut Decoder, text: &str) -> Result<Option<Vec<u8>>> {
    let text = text.trim();
    match leek_ur::part_type(text) {
        Some(t) if t == UR_ZSIG => {}
        Some(t) if t == leek_ur::UR_PCZT => return Ok(None), // our own code, reflected
        Some(t) => bail!("this is a ur:{t}, not the board's ur:leek-zsig"),
        None => return Ok(None),
    }
    match d.receive(text) {
        Ok(Received::Complete) => Ok(Some(leek_ur::unwrap_bytes(d.message().expect("complete"))?.to_vec())),
        Ok(_) => Ok(None),
        // One misread or foreign part does not end a scan; the fountain
        // keeps sending.
        Err(leek_ur::UrError::Checksum) => {
            eprintln!("The assembled signature failed its checksum; starting over.");
            Ok(None)
        }
        Err(_) => Ok(None),
    }
}

/// The `.zsig` in a file of UR parts, one per line (blank lines and `#`
/// comments skipped), in any order, repeats and fountain parts welcome.
pub fn zsig_from_parts_file(path: &Path) -> Result<Vec<u8>> {
    let text = std::fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
    zsig_from_parts(text.lines())
}

pub fn zsig_from_parts<'a>(lines: impl IntoIterator<Item = &'a str>) -> Result<Vec<u8>> {
    let mut d = Decoder::new();
    let mut n = 0usize;
    for line in lines {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        n += 1;
        if let Some(z) = feed(&mut d, line)? {
            return Ok(z);
        }
    }
    bail!("the parts ran out before the signature was complete: {}", progress_line(&d, n))
}

/// Reads the board's animated ur:leek-zsig off a V4L2 webcam (`qr-camera`).
#[cfg(all(feature = "qr-camera", target_os = "linux"))]
pub mod camera {
    use std::io::Write as _;
    use std::path::Path;
    use std::time::{Duration, Instant};

    use anyhow::{bail, Context, Result};
    use leek_ur::Decoder;
    use linuxvideo::format::{PixFormat, PixelFormat};
    use linuxvideo::Device;

    /// Every QR text in a greyscale image.
    pub fn decode_grey(width: usize, height: usize, grey: &[u8]) -> Vec<String> {
        let mut img = rqrr::PreparedImage::prepare_from_greyscale(width, height, |x, y| grey[y * width + x]);
        img.detect_grids().into_iter().filter_map(|g| g.decode().ok().map(|(_, s)| s)).collect()
    }

    /// The luma of a YUYV frame: every other byte, row by row.
    pub fn yuyv_luma(buf: &[u8], width: usize, height: usize, stride: usize) -> Vec<u8> {
        let mut grey = Vec::with_capacity(width * height);
        for y in 0..height {
            let row = &buf[y * stride..];
            grey.extend((0..width).map(|x| row[x * 2]));
        }
        grey
    }

    pub fn scan(device: &Path, timeout: Duration) -> Result<Vec<u8>> {
        let dev = Device::open(device).with_context(|| format!("opening {}", device.display()))?;
        let card = dev.capabilities().map(|c| c.card().to_string()).unwrap_or_default();
        let cap = dev
            .video_capture(PixFormat::new(1280, 720, PixelFormat::YUYV))
            .with_context(|| format!("{} would not capture", device.display()))?;
        let fmt = cap.format();
        if fmt.pixel_format() != PixelFormat::YUYV {
            bail!(
                "{} offers {:?}, not YUYV; this reader needs YUYV. Use --from-file with parts from another scanner.",
                device.display(),
                fmt.pixel_format()
            );
        }
        let (w, h, stride) = (fmt.width() as usize, fmt.height() as usize, fmt.bytes_per_line() as usize);
        eprintln!("Camera: {card} ({}), {w}x{h}. Point it at the board's QR; Ctrl-C stops.", device.display());
        let mut stream = cap.into_stream()?;
        let mut d = Decoder::new();
        let mut reads = 0usize;
        let start = Instant::now();
        loop {
            if start.elapsed() > timeout {
                bail!("no complete signature within {} s: {}", timeout.as_secs(), super::progress_line(&d, reads));
            }
            let grey = stream.dequeue(|buf| Ok(yuyv_luma(&buf, w, h, stride.max(w * 2))))?;
            for text in decode_grey(w, h, &grey) {
                reads += 1;
                if let Some(z) = super::feed(&mut d, &text)? {
                    eprintln!();
                    return Ok(z);
                }
            }
            eprint!("\r{}   ", super::progress_line(&d, reads));
            let _ = std::io::stderr().flush();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Back from half blocks to modules: what a reader of the terminal sees.
    fn unblock(lines: &[String]) -> Vec<Vec<bool>> {
        let mut m = Vec::new();
        for l in lines {
            let (top, bottom): (Vec<bool>, Vec<bool>) =
                l.chars().map(|c| (c == '█' || c == '▀', c == '█' || c == '▄')).unzip();
            m.push(top);
            m.push(bottom);
        }
        m
    }

    #[test]
    fn half_block_snapshot() {
        let qr = QrCode::with_version(b"UR:LEEK-ZSIG/AE", Version::Normal(1), EcLevel::L).unwrap();
        let lines = half_blocks(&qr);
        // 21 modules plus 8 of quiet zone: 29 across, 15 lines down.
        assert_eq!(lines.len(), 15);
        assert!(lines.iter().all(|l| l.chars().count() == 29));
        // Every module survives the drawing (the odd last line's bottom half
        // is quiet zone).
        let mut back = unblock(&lines);
        back.truncate(29);
        assert_eq!(back, modules(&qr));
        // And the drawing itself is pinned.
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/snapshots/half_block_v1.txt");
        let got = lines.join("\n") + "\n";
        if std::env::var_os("ZECZ_SNAPSHOT_REGEN").is_some() {
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(&path, &got).unwrap();
        }
        let want = std::fs::read_to_string(&path).expect("tests/snapshots/half_block_v1.txt");
        assert_eq!(got, want, "\n{got}");
        // The terminal frame draws it black on bright white.
        let e = Encoder::leek_zsig(&[1; 4], 512).unwrap();
        let f = frames(&e).unwrap();
        let t = terminal_frame(&f, 0, 500);
        assert!(t.starts_with("\x1b[H\x1b[2J") && t.contains("\x1b[30;107m"));
    }

    #[test]
    fn sweep_presets_and_table() {
        assert_eq!(PRESETS[0], (DEFAULT_FRAGMENT, DEFAULT_INTERVAL_MS));
        assert_eq!((DEFAULT_FRAGMENT, DEFAULT_INTERVAL_MS), (50, 500));
        let view = [0x5A; 1373];
        for &(frag, _) in &PRESETS {
            let e = Encoder::leek_pczt(&view, frag).unwrap();
            assert!(e.fragment_len() <= frag && e.seq_len() <= leek_ur::MAX_PARTS);
        }
        let e = Encoder::leek_pczt(&view, 90).unwrap();
        let l = preset_label(1, 6, 90, 500, &e);
        assert!(l.contains("1/6") && l.contains("90 B") && l.contains("500 ms") && l.contains("16 frame(s)"), "{l}");
        let t = sweep_table(&[(90, 500, 16, Duration::from_millis(9400))]);
        assert!(t.contains("| 90 B | 500 ms | 16 | 9.4 s |"), "{t}");
    }

    #[test]
    fn frames_keep_one_version() {
        let e = Encoder::leek_pczt(&[0xA5; 1373], DEFAULT_FRAGMENT).unwrap();
        let f = frames(&e).unwrap();
        assert_eq!(f.frames.len(), 255);
        assert!(f.frames.iter().all(|(_, q)| q.version() == Version::Normal(f.version)));
        assert!(f.version <= 6, "50-byte fragments should stay at version 6 or less, got v{}", f.version);
        let t = terminal_frame(&f, 0, 500);
        assert!(t.contains("Part 1-28") && t.contains("EXPERIMENTAL"));
    }

    #[test]
    fn html_is_offline_and_complete() {
        let e = Encoder::leek_pczt(&[7; 300], 90).unwrap();
        let h = html(&e, 500).unwrap();
        assert!(!h.contains('@'), "a placeholder was left");
        assert!(!h.contains("http://") && !h.contains("https://"));
        assert!(h.contains("default-src 'none'"));
        assert!(h.contains("EXPERIMENTAL"));
        assert_eq!(h.matches("[\"").count(), 255);
    }

    fn zsig_parts(zsig: &[u8], frag: usize, n: u64) -> Vec<String> {
        let e = Encoder::leek_zsig(zsig, frag).unwrap();
        (0..n).map(|i| e.frame(i)).collect()
    }

    #[test]
    fn parts_file_in_any_order() {
        let zsig: Vec<u8> = (0..108u8).collect();
        // Shuffled, duplicated, fountain parts in, plain ones out.
        let mut parts = zsig_parts(&zsig, 20, 30);
        parts.remove(1);
        parts.remove(3);
        let dup = parts[4].clone();
        parts.insert(0, dup);
        parts.reverse();
        let mut lines = vec!["# from a phone scanner".to_string(), String::new(), "zcash:t1notaur".into()];
        lines.extend(parts.iter().map(|p| p.to_lowercase()));
        assert_eq!(zsig_from_parts(lines.iter().map(String::as_str)).unwrap(), zsig);
        // A still single-part code.
        let one = zsig_parts(&zsig, 512, 1);
        assert_eq!(zsig_from_parts(one.iter().map(String::as_str)).unwrap(), zsig);
        // The shared fixture, as the board would send it.
        let fixture = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../vectors/ur/iw_send_shielded.30.leek-zsig.txt");
        let want = std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../vectors/zsig/iw_send_shielded.zsig")).unwrap();
        assert_eq!(zsig_from_parts_file(&fixture).unwrap(), want);
    }

    #[test]
    fn parts_file_refusals() {
        let zsig = [9u8; 108];
        let parts = zsig_parts(&zsig, 20, 2);
        let err = zsig_from_parts(parts.iter().map(String::as_str)).unwrap_err().to_string();
        assert!(err.contains("ran out"), "{err}");
        let pczt = Encoder::leek_pczt(&[1; 40], 512).unwrap().frame(0);
        assert!(zsig_from_parts([pczt.as_str()]).is_err());
        assert!(zsig_from_parts(["UR:CRYPTO-PSBT/AEAEAEAE"]).unwrap_err().to_string().contains("not the board's"));
    }

    #[cfg(all(feature = "qr-camera", target_os = "linux"))]
    #[test]
    fn a_rendered_frame_reads_back() {
        let zsig: Vec<u8> = (0..108u8).collect();
        let e = Encoder::leek_zsig(&zsig, 40).unwrap();
        let f = frames(&e).unwrap();
        let mut d = Decoder::new();
        let mut out = None;
        for (_, qr) in f.frames.iter().take(10) {
            // 4 px a module, as a camera sees a screen at best.
            let m = modules(qr);
            let px = 4;
            let side = m.len() * px;
            let grey: Vec<u8> =
                (0..side * side).map(|i| if m[(i / side) / px][(i % side) / px] { 0 } else { 255 }).collect();
            for t in camera::decode_grey(side, side, &grey) {
                if let Some(z) = feed(&mut d, &t).unwrap() {
                    out = Some(z);
                }
            }
            if out.is_some() {
                break;
            }
        }
        assert_eq!(out.unwrap(), zsig);
    }
}
