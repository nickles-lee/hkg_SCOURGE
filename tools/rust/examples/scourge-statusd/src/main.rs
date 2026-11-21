// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
//! SCOURGE sample Rust daemon.
//!
//! A small static-musl Tokio daemon that proves the host-built app workflow:
//! HTTP status over loopback, atomic JSON status mirror, and clean SIGTERM /
//! SIGINT shutdown. It is a sample, not a production SCOURGE app.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use anyhow::{bail, Context, Result};
use serde_json::json;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::signal::unix::{signal, SignalKind};

const APP: &str = "scourge-statusd";
const VERSION: &str = env!("CARGO_PKG_VERSION");
const DEFAULT_LISTEN: &str = "127.0.0.1:18088";
const DEFAULT_STATUS: &str = "/rw_data/apps/scourge-statusd/status.json";

struct Config {
    listen: String,
    status: PathBuf,
}

fn usage() {
    eprintln!("{APP} {VERSION}");
    eprintln!("usage: {APP} [--listen ADDR] [--status PATH] [--version]");
}

fn parse_args() -> Result<Option<Config>> {
    let mut listen = DEFAULT_LISTEN.to_string();
    let mut status = PathBuf::from(DEFAULT_STATUS);
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--listen" => {
                listen = args.next().context("--listen needs an address")?;
            }
            "--status" => {
                status = PathBuf::from(args.next().context("--status needs a path")?);
            }
            "--version" | "-V" => {
                println!("{APP} {VERSION}");
                return Ok(None);
            }
            "--help" | "-h" => {
                usage();
                return Ok(None);
            }
            other => bail!("unknown argument: {other}"),
        }
    }
    Ok(Some(Config { listen, status }))
}

fn smack_label() -> String {
    std::fs::read_to_string("/proc/self/attr/current")
        .map(|value| {
            value
                .trim_matches(|ch: char| ch == '\0' || ch.is_whitespace())
                .to_string()
        })
        .unwrap_or_else(|_| "unknown".to_string())
}

fn write_status(path: &Path, value: &serde_json::Value) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create status directory {}", parent.display()))?;
    }
    let temporary = path.with_extension("json.tmp");
    let body = serde_json::to_vec_pretty(value)?;
    std::fs::write(&temporary, body)
        .with_context(|| format!("write {}", temporary.display()))?;
    std::fs::rename(&temporary, path)
        .with_context(|| format!("rename {} -> {}", temporary.display(), path.display()))?;
    Ok(())
}

fn status_value(
    state: &str,
    started: Instant,
    listen: std::net::SocketAddr,
    label: &str,
) -> serde_json::Value {
    json!({
        "app": APP,
        "version": VERSION,
        "state": state,
        "pid": std::process::id(),
        "smack": label,
        "static": cfg!(target_feature = "crt-static"),
        "listen": listen.to_string(),
        "uptime_s": started.elapsed().as_secs(),
    })
}

async fn handle(
    mut stream: TcpStream,
    started: Instant,
    listen: std::net::SocketAddr,
    label: Arc<String>,
) -> Result<()> {
    let mut buffer = [0u8; 1024];
    let count = stream.read(&mut buffer).await.unwrap_or(0);
    let request = String::from_utf8_lossy(&buffer[..count]);
    let path = request
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .unwrap_or("/");

    let (reason, body) = if path == "/status" {
        let value = status_value("running", started, listen, &label);
        ("200 OK", serde_json::to_vec(&value)?)
    } else {
        ("404 Not Found", br#"{"error":"not found"}"#.to_vec())
    };

    let header = format!(
        "HTTP/1.1 {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    stream.write_all(header.as_bytes()).await?;
    stream.write_all(&body).await?;
    let _ = stream.shutdown().await;
    Ok(())
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    let config = match parse_args()? {
        Some(config) => config,
        None => return Ok(()),
    };

    let started = Instant::now();
    let label = Arc::new(smack_label());
    let status_path = Arc::new(config.status);
    let listener = TcpListener::bind(&config.listen)
        .await
        .with_context(|| format!("bind {}", config.listen))?;
    let listen = listener.local_addr().context("local address")?;

    let running = status_value("running", started, listen, &label);
    write_status(&status_path, &running)?;
    eprintln!("{APP} {VERSION}: listening on {listen}");

    let mut sigterm = signal(SignalKind::terminate()).context("install SIGTERM handler")?;
    let mut sigint = signal(SignalKind::interrupt()).context("install SIGINT handler")?;

    loop {
        tokio::select! {
            _ = sigterm.recv() => {
                eprintln!("{APP}: SIGTERM");
                break;
            }
            _ = sigint.recv() => {
                eprintln!("{APP}: SIGINT");
                break;
            }
            accepted = listener.accept() => {
                let (stream, _peer) = accepted.context("accept")?;
                let label = Arc::clone(&label);
                tokio::spawn(async move {
                    if let Err(error) = handle(stream, started, listen, label).await {
                        eprintln!("{APP}: request error: {error:#}");
                    }
                });
            }
        }
    }

    let stopped = status_value("stopped", started, listen, &label);
    write_status(&status_path, &stopped)?;
    eprintln!("{APP}: stopped");
    Ok(())
}
