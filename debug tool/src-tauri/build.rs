fn main() {
    // tauri-build 1.x only watches the configuration, not its Windows icon.
    // Recompile the PE resources whenever the ICO changes.
    println!("cargo:rerun-if-changed=icons/icon.ico");
    tauri_build::build()
}
