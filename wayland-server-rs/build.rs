fn main() {
    println!("cargo:rerun-if-changed=src/render/gpu_shim.c");
    println!("cargo:rerun-if-changed=src/render/title_text.c");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        cc::Build::new().file("src/render/title_text.c").opt_level(2).warnings(false).compile("luna_title_text");
    }
    if std::env::var_os("CARGO_FEATURE_GPU").is_some()
        && std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux")
    {
        cc::Build::new().file("src/render/gpu_shim.c").compile("luna_gpu");
        println!("cargo:rustc-link-lib=gbm");
        println!("cargo:rustc-link-lib=EGL");
        println!("cargo:rustc-link-lib=GLESv2");
    }
}
