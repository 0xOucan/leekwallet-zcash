//! The device client for the Android app, over a USB link Kotlin owns.
//!
//! Android only lets the JVM side open a USB device, so Kotlin implements
//! `io.leekwallet.zcash.hw.UsbIo` (write bytes, read bytes with a timeout)
//! and everything above that - framing, the v2 pairing handshake, the
//! encrypted session, the Zcash calls - is leek-device, the same Rust that
//! zecz uses with the board. Nothing protocol-level is rewritten in Kotlin.
//!
//! Every call blocks: run them off the main thread.

use std::time::{Duration, Instant};

use jni::objects::{GlobalRef, JByteArray, JClass, JObject, JString, JValue};
use jni::sys::{jboolean, jint, jlong};
use jni::{JNIEnv, JavaVM};
use leek_device::device::{self, Device, FramePort};
use leek_device::zsig::Zsig;
use leek_transport_serial::{encode_frame, FrameDecoder, TransportError};

/// FramePort over the Kotlin UsbIo object.
struct JniPort {
    vm: JavaVM,
    io: GlobalRef,
    decoder: FrameDecoder,
}

impl FramePort for JniPort {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError> {
        let bytes = encode_frame(frame_type, payload).map_err(TransportError::Wire)?;
        let mut env = self.vm.attach_current_thread().map_err(|e| TransportError::Serial(e.to_string()))?;
        let arr = env.byte_array_from_slice(&bytes).map_err(|e| TransportError::Serial(e.to_string()))?;
        let r = env.call_method(&self.io, "write", "([B)V", &[JValue::Object(&arr)]);
        if r.is_err() || env.exception_check().unwrap_or(false) {
            let _ = env.exception_clear();
            return Err(TransportError::Serial("USB write failed".into()));
        }
        Ok(())
    }

    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(frame) = self.decoder.next_frame().map_err(TransportError::Wire)? {
                return Ok(frame);
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(TransportError::Timeout);
            }
            let slice = left.min(Duration::from_millis(200)).as_millis() as jint;
            let mut env = self.vm.attach_current_thread().map_err(|e| TransportError::Serial(e.to_string()))?;
            let r = env.call_method(&self.io, "read", "(I)[B", &[JValue::Int(slice)]);
            let obj = match r.and_then(|v| v.l()) {
                Ok(o) if !env.exception_check().unwrap_or(true) => o,
                _ => {
                    let _ = env.exception_clear();
                    return Err(TransportError::Serial("USB read failed".into()));
                }
            };
            if !obj.is_null() {
                let bytes = env.convert_byte_array(JByteArray::from(obj)).map_err(|e| TransportError::Serial(e.to_string()))?;
                self.decoder.feed(&bytes);
            }
        }
    }
}

// SAFETY: JavaVM and GlobalRef are thread-safe handles; the decoder is plain data.
unsafe impl Send for JniPort {}

/// FramePort over a Kotlin BleIo object. BLE carries whole frames - the
/// GATT layer delimits them and Kotlin does the chunking (PROTOCOL.md 2) -
/// so there is no sync marker: a frame is a big-endian u16 length (type byte
/// included), the type, then the payload, exactly the USB frame without "LK".
struct JniBlePort {
    vm: JavaVM,
    io: GlobalRef,
}

// SAFETY: as for JniPort.
unsafe impl Send for JniBlePort {}

impl FramePort for JniBlePort {
    fn send(&mut self, frame_type: u8, payload: &[u8]) -> Result<(), TransportError> {
        let body = payload.len() + 1;
        let mut frame = Vec::with_capacity(body + 2);
        frame.extend_from_slice(&(body as u16).to_be_bytes());
        frame.push(frame_type);
        frame.extend_from_slice(payload);
        let mut env = self.vm.attach_current_thread().map_err(|e| TransportError::Serial(e.to_string()))?;
        let arr = env.byte_array_from_slice(&frame).map_err(|e| TransportError::Serial(e.to_string()))?;
        let r = env.call_method(&self.io, "sendFrame", "([B)V", &[JValue::Object(&arr)]);
        if r.is_err() || env.exception_check().unwrap_or(false) {
            let _ = env.exception_clear();
            return Err(TransportError::Serial("Bluetooth write failed".into()));
        }
        Ok(())
    }

    fn recv(&mut self, timeout: Duration) -> Result<(u8, Vec<u8>), TransportError> {
        let mut env = self.vm.attach_current_thread().map_err(|e| TransportError::Serial(e.to_string()))?;
        let ms = timeout.as_millis().min(i32::MAX as u128) as jint;
        let r = env.call_method(&self.io, "recvFrame", "(I)[B", &[JValue::Int(ms)]);
        let obj = match r.and_then(|v| v.l()) {
            Ok(o) if !env.exception_check().unwrap_or(true) => o,
            _ => {
                let _ = env.exception_clear();
                return Err(TransportError::Serial("Bluetooth read failed".into()));
            }
        };
        if obj.is_null() {
            return Err(TransportError::Timeout);
        }
        let f = env.convert_byte_array(JByteArray::from(obj)).map_err(|e| TransportError::Serial(e.to_string()))?;
        if f.len() < 3 {
            return Err(TransportError::Serial("short Bluetooth frame".into()));
        }
        let body = u16::from_be_bytes([f[0], f[1]]) as usize;
        if body + 2 != f.len() || body == 0 {
            return Err(TransportError::Serial("malformed Bluetooth frame".into()));
        }
        Ok((f[2], f[3..].to_vec()))
    }
}

fn throw(env: &mut JNIEnv, e: &anyhow::Error) {
    let _ = env.throw_new("java/lang/IllegalStateException", format!("{e:#}"));
}

fn device<'a>(handle: jlong) -> &'a mut Device {
    // SAFETY: handles come only from `open` and stay valid until `close`.
    unsafe { &mut *(handle as *mut Device) }
}

fn string(env: &mut JNIEnv, s: &JString) -> anyhow::Result<String> {
    Ok(env.get_string(s)?.into())
}

#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_open(
    mut env: JNIEnv, _c: JClass, io: JObject,
) -> jlong {
    let r = (|| -> anyhow::Result<jlong> {
        let port = JniPort { vm: env.get_java_vm()?, io: env.new_global_ref(io)?, decoder: FrameDecoder::new() };
        Ok(Box::into_raw(Box::new(Device::with_port(Box::new(port)))) as jlong)
    })();
    r.unwrap_or_else(|e| { throw(&mut env, &e); 0 })
}

/// The device over Bluetooth LE: `io` is a Kotlin BleIo (whole frames).
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_openBle(
    mut env: JNIEnv, _c: JClass, io: JObject,
) -> jlong {
    let r = (|| -> anyhow::Result<jlong> {
        let port = JniBlePort { vm: env.get_java_vm()?, io: env.new_global_ref(io)? };
        Ok(Box::into_raw(Box::new(Device::with_port(Box::new(port)))) as jlong)
    })();
    r.unwrap_or_else(|e| { throw(&mut env, &e); 0 })
}

#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_close(_env: JNIEnv, _c: JClass, handle: jlong) {
    if handle != 0 {
        // SAFETY: as in `device`; the handle is not used again.
        drop(unsafe { Box::from_raw(handle as *mut Device) });
    }
}

/// Pair; `listener.onPasskey(String)` gets the six digits to compare.
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_pair(
    mut env: JNIEnv, _c: JClass, handle: jlong, listener: JObject,
) {
    let vm = match env.get_java_vm() { Ok(v) => v, Err(e) => return throw(&mut env, &e.into()) };
    let listener = match env.new_global_ref(listener) { Ok(l) => l, Err(e) => return throw(&mut env, &e.into()) };
    let r = device(handle).pair(|passkey| {
        if let Ok(mut env) = vm.attach_current_thread() {
            if let Ok(s) = env.new_string(passkey) {
                let _ = env.call_method(&listener, "onPasskey", "(Ljava/lang/String;)V", &[JValue::Object(&s)]);
                let _ = env.exception_clear();
            }
        }
    });
    if let Err(e) = r { throw(&mut env, &e) }
}

#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_ensureUnlocked(mut env: JNIEnv, _c: JClass, handle: jlong) {
    if let Err(e) = device(handle).ensure_unlocked() { throw(&mut env, &e) }
}

/// JSON: {"account","network","ufvk","ua","tAddress","seedFingerprint"(hex),"birthday"?}
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_getAccount<'l>(
    mut env: JNIEnv<'l>, _c: JClass<'l>, handle: jlong, account: jint, network: JString<'l>,
) -> JString<'l> {
    let r = (|| -> anyhow::Result<String> {
        let net = string(&mut env, &network)?;
        let a = device(handle).z_get_account(account as u32, &net)?;
        let birthday = device::get_uint(&a, "birthday").ok();
        Ok(format!(
            "{{\"account\":{},\"network\":\"{}\",\"ufvk\":\"{}\",\"ua\":\"{}\",\"tAddress\":\"{}\",\"seedFingerprint\":\"{}\"{}}}",
            account,
            net,
            device::get_text(&a, "ufvk")?,
            device::get_text(&a, "ua")?,
            device::get_text(&a, "tAddress")?,
            hex::encode(device::get_bytes(&a, "seedFingerprint")?),
            birthday.map(|b| format!(",\"birthday\":{b}")).unwrap_or_default(),
        ))
    })();
    match r.and_then(|s| Ok(env.new_string(s)?)) {
        Ok(s) => s,
        Err(e) => { throw(&mut env, &e); JString::default() }
    }
}

/// True if the device stored it in flash, false if only until it locks.
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_setBirthday(
    mut env: JNIEnv, _c: JClass, handle: jlong, account: jint, network: JString, birthday: jlong,
) -> jboolean {
    let r = string(&mut env, &network).and_then(|net| device(handle).z_set_birthday(account as u32, &net, birthday as u32));
    match r {
        Ok(stored) => stored as jboolean,
        Err(e) => { throw(&mut env, &e); 0 }
    }
}

#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_showAddress(
    mut env: JNIEnv, _c: JClass, handle: jlong, account: jint, network: JString, expected_ua: JString,
) {
    let r = (|| -> anyhow::Result<()> {
        let net = string(&mut env, &network)?;
        let ua = string(&mut env, &expected_ua)?;
        device(handle).z_show_address(account as u32, &net, &ua)
    })();
    if let Err(e) = r { throw(&mut env, &e) }
}

/// Upload `pczt`, let the user review it on the device, and return the
/// device's signatures as a .zsig (pass it to PcztSignatures.apply).
#[no_mangle]
pub extern "system" fn Java_io_leekwallet_zcash_hw_LeekDeviceNative_sign<'l>(
    mut env: JNIEnv<'l>, _c: JClass<'l>, handle: jlong, pczt: JByteArray<'l>, account: jint, network: JString<'l>,
) -> JByteArray<'l> {
    let r = (|| -> anyhow::Result<Vec<u8>> {
        let bytes = env.convert_byte_array(&pczt)?;
        let net = string(&mut env, &network)?;
        let (_sighash, entries) = device(handle).z_sign(&bytes, account as u32, &net, |_, _| {})?;
        Zsig::new(&bytes, entries).to_bytes().map_err(|e| anyhow::anyhow!("{e}"))
    })();
    match r.and_then(|z| Ok(env.byte_array_from_slice(&z)?)) {
        Ok(a) => a,
        Err(e) => { throw(&mut env, &e); JByteArray::default() }
    }
}
