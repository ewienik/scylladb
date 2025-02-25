use std::{
    alloc::{GlobalAlloc, Layout},
    sync::atomic::{AtomicUsize, Ordering},
};

extern "C" {
    fn aligned_alloc(align: usize, size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

struct ScyllaAllocator {
    alloc: AtomicUsize,
    dealloc: AtomicUsize,
}

unsafe impl GlobalAlloc for ScyllaAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        self.alloc.fetch_add(1, Ordering::Relaxed);
        aligned_alloc(layout.align(), layout.size())
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        self.dealloc.fetch_add(1, Ordering::Relaxed);
        free(ptr);
    }
}

#[global_allocator]
static SCYLLA_ALLOCATOR: ScyllaAllocator = ScyllaAllocator {
    alloc: AtomicUsize::new(0),
    dealloc: AtomicUsize::new(0),
};

#[cxx::bridge(namespace = "rust::seastar::allocator")]
mod ffi {
    extern "Rust" {
        fn allocs() -> usize;
        fn deallocs() -> usize;
    }
}

pub fn allocs() -> usize {
    SCYLLA_ALLOCATOR.alloc.load(Ordering::Relaxed)
}

pub fn deallocs() -> usize {
    SCYLLA_ALLOCATOR.dealloc.load(Ordering::Relaxed)
}
