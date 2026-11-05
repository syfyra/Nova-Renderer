fn main() {
    // The payload (dist/nova zipped by build.ps1) is embedded: rebuild when it changes.
    println!("cargo:rerun-if-changed=payload/nova.zip");
    println!("cargo:rerun-if-env-changed=NOVA_VERSION");
    tauri_build::build()
}
