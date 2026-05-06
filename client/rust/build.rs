use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn repo_root(manifest_dir: &Path) -> PathBuf {
    manifest_dir
        .parent()
        .and_then(|p| p.parent())
        .expect("client/rust must live two levels below repo root")
        .to_path_buf()
}

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("missing CARGO_MANIFEST_DIR"));
    let repo_root = repo_root(&manifest_dir);
    let out_file = PathBuf::from(env::var("OUT_DIR").expect("missing OUT_DIR")).join("messages_generated.rs");
    let generator = repo_root.join("scripts/generate_messages.py");

    let proto_files = [
        "messages.proto",
        "resource_service/bsd_sockets/bsd_sockets.proto",
        "resource_service/bsd_sockets/bsd_sockets_logging.proto",
        "resource_service/sshd/sshd.proto",
        "name_service/name_service.proto",
        "logging/logging.proto",
    ]
    .into_iter()
    .map(|rel| repo_root.join(rel))
    .collect::<Vec<_>>();

    println!("cargo:rerun-if-changed={}", generator.display());
    for proto in &proto_files {
        println!("cargo:rerun-if-changed={}", proto.display());
    }

    let mut command = Command::new("python3");
    command.arg(generator);
    command.arg("--proto");
    for proto in &proto_files {
        command.arg(proto);
    }
    command.arg("--rust");
    command.arg(&out_file);

    let status = command.status().expect("failed to execute scripts/generate_messages.py");
    if !status.success() {
        panic!("message generation failed with status: {status}");
    }
}
