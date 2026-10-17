//! The wasm-bindgen surface: `initStorage`, the `Engine` with its one
//! `call`, `cancelSync`, and `initThreadPool` (wasm-bindgen-rayon).

use std::cell::RefCell;
use std::rc::Rc;

use wasm_bindgen::prelude::*;

#[cfg(feature = "threads")]
pub use wasm_bindgen_rayon::init_thread_pool;

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = console)]
    fn log(s: &str);
    #[wasm_bindgen(js_namespace = console, js_name = error)]
    fn error(s: &str);
}

/// Forwards librustzcash's `tracing` events (INFO and above) to the console.
struct ConsoleTracing;

impl tracing::Subscriber for ConsoleTracing {
    fn enabled(&self, m: &tracing::Metadata<'_>) -> bool {
        *m.level() <= tracing::Level::INFO
    }
    fn new_span(&self, _: &tracing::span::Attributes<'_>) -> tracing::span::Id {
        tracing::span::Id::from_u64(1)
    }
    fn record(&self, _: &tracing::span::Id, _: &tracing::span::Record<'_>) {}
    fn record_follows_from(&self, _: &tracing::span::Id, _: &tracing::span::Id) {}
    fn event(&self, ev: &tracing::Event<'_>) {
        struct V(String);
        impl tracing::field::Visit for V {
            fn record_debug(&mut self, f: &tracing::field::Field, v: &dyn std::fmt::Debug) {
                if f.name() == "message" {
                    self.0 += &format!("{v:?} ");
                } else {
                    self.0 += &format!("{}={v:?} ", f.name());
                }
            }
        }
        let mut v = V(String::new());
        ev.record(&mut v);
        let line: String = v.0.chars().take(300).collect();
        log(&format!("{} {}: {line}", ev.metadata().level(), ev.metadata().target()));
    }
    fn enter(&self, _: &tracing::span::Id) {}
    fn exit(&self, _: &tracing::span::Id) {}
}

/// Sets up logging and installs the OPFS VFS. Call once, in the dedicated
/// worker, before `new Engine()`.
#[wasm_bindgen(js_name = initStorage)]
pub async fn init_storage() -> Result<(), JsError> {
    // panic = "abort": the message is all that is left, so log it.
    std::panic::set_hook(Box::new(|info| error(&format!("engine panic: {info}"))));
    let _ = tracing::subscriber::set_global_default(ConsoleTracing);
    crate::store::install_opfs().await.map_err(|e| JsError::new(&format!("{e:#}")))
}

#[wasm_bindgen(js_name = threadsEnabled)]
pub fn threads_enabled() -> bool {
    cfg!(feature = "threads")
}

/// Asks a running sync to stop after its current batch. Callable while the
/// sync holds the engine.
#[wasm_bindgen(js_name = cancelSync)]
pub fn cancel_sync() {
    crate::lwd::request_cancel();
}

/// WASM linear memory in MB (it only grows: the peak).
#[wasm_bindgen(js_name = wasmMemoryMb)]
pub fn wasm_memory_mb() -> f64 {
    let mem: js_sys::WebAssembly::Memory = wasm_bindgen::memory().unchecked_into();
    let buf: js_sys::Object = mem.buffer().unchecked_into();
    js_sys::Reflect::get(&buf, &"byteLength".into()).ok().and_then(|v| v.as_f64()).unwrap_or(0.0) / 1e6
}

#[wasm_bindgen]
pub struct Engine {
    state: crate::api::State,
}

#[wasm_bindgen]
impl Engine {
    #[wasm_bindgen(constructor)]
    pub fn new() -> Engine {
        Engine { state: Rc::new(RefCell::new(None)) }
    }

    /// Runs `method` with `params` (a JSON string). Resolves to the JSON
    /// result; rejects with an Error whose message says what went wrong.
    /// `onEvent` receives each progress event as a JSON string.
    pub fn call(&self, method: String, params: String, on_event: js_sys::Function) -> js_sys::Promise {
        let state = self.state.clone();
        wasm_bindgen_futures::future_to_promise(async move {
            let params: serde_json::Value = if params.trim().is_empty() {
                serde_json::Value::Object(Default::default())
            } else {
                serde_json::from_str(&params).map_err(|e| JsValue::from(JsError::new(&format!("params are not JSON: {e}"))))?
            };
            let emit = |v: serde_json::Value| {
                let _ = on_event.call1(&JsValue::NULL, &JsValue::from_str(&v.to_string()));
            };
            match crate::api::dispatch(&state, &method, params, &emit).await {
                Ok(v) => Ok(JsValue::from_str(&v.to_string())),
                Err(e) => Err(JsError::new(&format!("{e:#}")).into()),
            }
        })
    }
}

impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}
