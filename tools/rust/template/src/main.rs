// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
//! Starting point for a SCOURGE Rust daemon.
//!
//! It writes a heartbeat status file and sleeps. Replace the loop with the
//! daemon's real work. For a full example, see
//! `tools/rust/examples/scourge-statusd/`.

use std::time::Duration;

const STATUS_PATH: &str = "/rw_data/apps/my-daemon/status.json";

fn main() {
    eprintln!("my-daemon: starting (replace src/main.rs)");
    loop {
        if let Some(parent) = std::path::Path::new(STATUS_PATH).parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let status = format!(
            "{{\"app\":\"my-daemon\",\"pid\":{},\"state\":\"running\"}}\n",
            std::process::id()
        );
        let _ = std::fs::write(STATUS_PATH, status);
        std::thread::sleep(Duration::from_secs(10));
    }
}
