//! Generates the Thrift client from the node's IDL with the `thrift` compiler (0.19.0; set THRIFT
//! to use another path) and IDL directory (CREDITS_IDL, by default the node repository's
//! third-party/thrift-interface-definitions).
use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let idl = env::var("CREDITS_IDL").map(PathBuf::from).unwrap_or_else(|_| {
        PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../../third-party/thrift-interface-definitions")
    });
    let thrift = env::var("THRIFT").unwrap_or_else(|_| "thrift".into());
    println!("cargo:rerun-if-env-changed=THRIFT");
    println!("cargo:rerun-if-env-changed=CREDITS_IDL");
    for f in ["api.thrift", "general.thrift"] {
        println!("cargo:rerun-if-changed={}", idl.join(f).display());
    }
    let status = Command::new(&thrift)
        .args(["-r", "--gen", "rs", "-out"])
        .arg(&out)
        .arg("-I")
        .arg(&idl)
        .arg(idl.join("api.thrift"))
        .status()
        .unwrap_or_else(|e| panic!("cannot run {thrift}: {e} (install the Thrift compiler 0.19.0)"));
    assert!(status.success(), "thrift failed");
    // the generated files start with inner attributes (#![...]), which include!() does not accept:
    // main.rs puts the same allowances on the modules instead
    for f in ["api.rs", "general.rs"] {
        let path = out.join(f);
        let text = fs::read_to_string(&path).unwrap();
        let kept: Vec<&str> = text.lines().filter(|l| !l.starts_with("#![")).collect();
        fs::write(&path, kept.join("\n")).unwrap();
    }
}
