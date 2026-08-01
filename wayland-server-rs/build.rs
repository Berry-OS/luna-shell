fn main() {
    println!("cargo:rerun-if-changed=src/render/gpu_shim.c");
    if std::env::var_os("CARGO_FEATURE_GPU").is_some()
        && std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux")
    {
        cc::Build::new().file("src/render/gpu_shim.c").compile("luna_gpu");
        println!("cargo:rustc-link-lib=gbm");
        println!("cargo:rustc-link-lib=EGL");
        println!("cargo:rustc-link-lib=GLESv2");
    }
}
