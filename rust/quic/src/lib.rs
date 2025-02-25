use {
    quiche::Config,
    std::{
        alloc::{GlobalAlloc, Layout},
        sync::atomic::{AtomicUsize, Ordering},
    },
};

extern "C" {
    fn aligned_alloc(align: usize, size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

struct ScyllaAllocator {
    counter: AtomicUsize,
}

unsafe impl GlobalAlloc for ScyllaAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        self.counter.fetch_add(1, Ordering::Relaxed);
        aligned_alloc(layout.align(), layout.size())
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr);
    }
}

#[global_allocator]
static GLOBAL: ScyllaAllocator = ScyllaAllocator {
    counter: AtomicUsize::new(0),
};

#[cxx::bridge(namespace = "rust")]
mod ffi {
    extern "Rust" {
        fn ppery() -> usize;
    }
}

pub fn ppery() -> usize {
    let tmp = vec![1; 100];
    let mut _config = quiche::Config::new(quiche::PROTOCOL_VERSION).unwrap();
    GLOBAL.counter.load(Ordering::Relaxed) * tmp.len()
}
