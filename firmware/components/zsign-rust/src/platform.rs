//! Glue to the C side: allocator, RNG and panic.

use core::alloc::{GlobalAlloc, Layout};

extern "C" {
    fn zs_platform_alloc(size: usize, align: usize) -> *mut u8;
    fn zs_platform_free(ptr: *mut u8);
    fn zs_platform_random(buf: *mut u8, len: usize);
    fn zs_platform_abort(msg: *const u8, len: usize) -> !;
}

struct PlatformAlloc;

unsafe impl GlobalAlloc for PlatformAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        zs_platform_alloc(layout.size(), layout.align())
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        zs_platform_free(ptr)
    }
}

#[global_allocator]
static ALLOC: PlatformAlloc = PlatformAlloc;

/// Hardware RNG supplied by the firmware (esp_fill_random on the device).
pub struct PlatformRng;

impl rand_core::TryRng for PlatformRng {
    type Error = core::convert::Infallible;
    fn try_next_u32(&mut self) -> Result<u32, Self::Error> {
        let mut b = [0u8; 4];
        self.try_fill_bytes(&mut b)?;
        Ok(u32::from_le_bytes(b))
    }
    fn try_next_u64(&mut self) -> Result<u64, Self::Error> {
        let mut b = [0u8; 8];
        self.try_fill_bytes(&mut b)?;
        Ok(u64::from_le_bytes(b))
    }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), Self::Error> {
        unsafe { zs_platform_random(dest.as_mut_ptr(), dest.len()) }
        Ok(())
    }
}

impl rand_core::TryCryptoRng for PlatformRng {}


#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    const MSG: &[u8] = b"zsign-rust panic";
    unsafe { zs_platform_abort(MSG.as_ptr(), MSG.len()) }
}
