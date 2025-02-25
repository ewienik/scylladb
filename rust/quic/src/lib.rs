use quinn_proto::ClientConfig;

#[cxx::bridge(namespace = "rust::quic")]
mod ffi {
    extern "Rust" {
        fn alloc_test();
    }
}

pub fn alloc_test() {
    let _config = ClientConfig::with_platform_verifier();
}
