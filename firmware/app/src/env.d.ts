/**
 * The one build-time variable the app reads (src/app-mode.ts). Declared by
 * hand rather than through `vite/client`, whose types also declare asset
 * imports this project types itself (wasm-assets.d.ts). Optional, because
 * under plain Node (the tests) `import.meta.env` does not exist.
 */
interface ImportMetaEnv {
  readonly VITE_LEEK_APP_MODE?: string;
}

interface ImportMeta {
  readonly env?: ImportMetaEnv;
}
