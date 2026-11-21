// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
//
// Everything is built with createElement/textContent: step titles, notes and log
// lines come from files on this host, but they are data, never markup.
"use strict";

const GROUPS = [
  ["Build", ["env", "third_party", "stage_inputs", "decrypt", "patch", "repack",
             "wrapper", "packages", "image", "preflight"]],
  ["Rig", ["provision_pi", "wifi_fallback", "deploy_pi", "verify_pi"]],
  ["Card and stick", ["sd_boot", "copy_to_sd", "format_usb", "car_preflight"]],
];

const STATUS_LABEL = { done: "done", ready: "run next", blocked: "waiting", manual: "manual" };
const $ = (id) => document.getElementById(id);
let stream = null;
let lastState = null;

const el = (tag, props = {}, kids = []) => {
  const node = document.createElement(tag);
  Object.assign(node, props);
  for (const kid of kids) node.append(kid);
  return node;
};

function chip(status, text) {
  const cls = status === "done" ? "ok" : status === "ready" ? "todo"
            : status === "blocked" ? "blocked" : "";
  return el("span", { className: `chip ${cls}`, textContent: text || STATUS_LABEL[status] || status });
}

async function refresh() {
  try {
    const res = await fetch("/api/state", { cache: "no-store" });
    lastState = await res.json();
    render(lastState);
  } catch (err) {
    $("empty").textContent = "dashboard unreachable: " + err;
  }
}

function render(state) {
  const steps = state.steps;
  const done = steps.filter((s) => s.status === "done").length;
  $("rig").textContent = `${state.rig.user}@${state.rig.host}` +
    (state.secrets.ready ? "" : ` · ${state.secrets.missing.length} setting(s) still empty in ${state.secrets.file}`);
  $("bar").style.width = `${Math.round((done / steps.length) * 100)}%`;
  $("progress-text").textContent =
    `${done} of ${steps.length} steps done` +
    (state.secrets.ready ? "" : " · fill in the config to unlock the rig steps");

  renderNext(steps);
  renderGroups(steps);
  $("empty").textContent = "";
}

function nextStep(steps) {
  return steps.find((s) => s.status === "ready") ||
         steps.find((s) => s.status === "manual" && !s.note) || null;
}

function renderNext(steps) {
  const step = nextStep(steps);
  const box = $("next");
  if (!step) { box.hidden = true; return; }
  box.hidden = false;
  $("next-title").textContent = step.title;
  $("next-detail").textContent = step.detail;
  const actions = $("next-actions");
  actions.textContent = "";
  actions.append(...actionNodes(step, true));
}

function renderGroups(steps) {
  const byId = Object.fromEntries(steps.map((s) => [s.id, s]));
  const root = $("groups");
  root.textContent = "";
  for (const [label, ids] of GROUPS) {
    const group = ids.map((id) => byId[id]).filter(Boolean);
    if (!group.length) continue;
    const solved = group.filter((s) => s.status === "done").length;
    const shouldOpen = group.some((s) => s.status === "ready");
    const details = el("details", { className: "group", open: shouldOpen });
    details.append(el("summary", {}, [
      el("span", { textContent: label }),
      el("span", {
        className: "count",
        textContent: `${solved}/${group.length} done`,
      }),
    ]));
    const list = el("div", { className: "steps" });
    for (const step of group) list.append(renderStep(step));
    details.append(list);
    root.append(details);
  }
}

function renderStep(step) {
  const head = el("div", { className: "step-head" }, [
    el("h3", { textContent: step.title }),
    chip(step.status),
  ]);
  const detail = el("p", {
    className: "detail" + (step.status === "blocked" ? " blocked" : ""),
    textContent: step.detail,
  });
  const parts = [head, detail];
  if (step.script) {
    parts.push(el("details", { className: "more" }, [
      el("summary", { textContent: "details" }),
      el("code", { textContent: step.script }),
      step.note ? el("p", { className: "detail", textContent: step.note }) : el("span", {}),
    ]));
  }
  parts.push(el("div", { className: "actions" }, actionNodes(step, false)));
  return el("article", { className: "step" }, parts);
}

function actionNodes(step, primaryOnly) {
  const nodes = [];
  let deviceInput = null;
  if (step.device && !primaryOnly) {
    deviceInput = el("input", { type: "text", placeholder: "/dev/sdX", inputMode: "text" });
    nodes.push(deviceInput);
  }
  if (step.status === "done" && !primaryOnly) {
    const rerun = el("button", { type: "button", textContent: "Run again" });
    rerun.addEventListener("click", () => startRun(step, deviceInput));
    nodes.push(rerun);
    return nodes;
  }
  if (step.status === "blocked") {
    const why = el("button", { type: "button", textContent: "Why?", disabled: true });
    nodes.push(why);
    return nodes;
  }
  const label = step.on_rig ? "Run on the rig" : step.device ? "Write the device" : "Run";
  const button = el("button", {
    type: "button",
    textContent: label,
    className: step.device ? "danger" : "primary",
  });
  button.addEventListener("click", () => startRun(step, deviceInput));
  nodes.push(button);
  return nodes;
}

async function startRun(step, deviceInput) {
  const body = {
    confirm: step.device ? step.id : "",
    device: deviceInput ? deviceInput.value.trim() : "",
  };
  const res = await fetch(`/api/steps/${encodeURIComponent(step.id)}/run`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await res.json();
  openLog(step.title, data.run_id, data.error);
  if (!data.error) follow(data.run_id);
}

function openLog(title, runId, error) {
  $("log").hidden = false;
  $("log-title").textContent = title;
  $("log-eyebrow").textContent = runId || "not run";
  $("log-state").textContent = error ? "not run" : "running";
  $("log-state").className = "chip " + (error ? "err" : "todo");
  $("log-body").textContent = error ? error + "\n" : "";
}

function follow(runId) {
  if (stream) stream.close();
  stream = new EventSource(`/api/runs/${encodeURIComponent(runId)}/stream`);
  const body = $("log-body");
  stream.onmessage = (event) => {
    body.textContent += JSON.parse(event.data) + "\n";
    body.scrollTop = body.scrollHeight;
  };
  stream.addEventListener("end", (event) => {
    const info = JSON.parse(event.data);
    $("log-state").textContent = info.state;
    $("log-state").className = "chip " + (info.state === "ok" ? "ok" : "err");
    stream.close(); stream = null;
    refresh();
  });
  stream.onerror = () => { stream.close(); stream = null; };
}

// --- wifi ------------------------------------------------------------------

async function refreshWifi() {
  const box = $("wifi");
  let wifi;
  try {
    const res = await fetch("/api/wifi", { cache: "no-store" });
    wifi = await res.json();
  } catch (err) {
    wifi = { reachable: false, detail: String(err) };
  }
  box.hidden = false;
  const change = $("wifi-change");
  if (!wifi.reachable) {
    $("wifi-title").textContent = "Rig not reachable on the network";
    $("wifi-detail").textContent =
      "If it could not join WiFi it raised its own access point after ~90 s: join " +
      "the setup AP, open http://10.42.0.1/ and pick a network there.";
    change.hidden = true;
    return;
  }
  // Connected is the normal state: say so plainly and keep the picker folded
  // away, so the page does not look like it is asking for credentials.
  if (wifi.connected) {
    $("wifi-title").textContent = `Connected · ${wifi.ssid || wifi.connection}`;
    $("wifi-detail").textContent = [
      wifi.signal ? `${wifi.signal}% signal` : "",
      wifi.ip ? `address ${wifi.ip}` : "",
      "nothing to do unless you want to move it to another network",
    ].filter(Boolean).join(" · ");
    change.open = false;
    $("wifi-change-label").textContent = "Change network";
  } else {
    $("wifi-title").textContent = "Not associated";
    $("wifi-detail").textContent =
      "The rig has no network. Pick one below — this is the same job the setup " +
      "page at 10.42.0.1 does while the fallback AP is up.";
    change.open = true;
    $("wifi-change-label").textContent = "Pick a network";
  }
  change.hidden = false;
  const select = $("wifi-ssid");
  select.textContent = "";
  for (const net of wifi.networks) {
    select.append(el("option", {
      value: net.ssid,
      textContent: `${net.ssid} · ${net.signal}% · ${net.security}`,
    }));
  }
  if (!wifi.networks.length) {
    select.append(el("option", { value: "", textContent: "(no networks visible)" }));
  }
}

async function connectWifi(event) {
  event.preventDefault();
  const ssid = $("wifi-ssid").value;
  const psk = $("wifi-psk").value;
  const button = $("wifi-connect");
  const msg = $("wifi-msg");
  button.disabled = true;
  msg.textContent = "connecting…";
  try {
    const res = await fetch("/api/wifi/connect", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ ssid, psk }),
    });
    const data = await res.json();
    msg.textContent = data.ok ? `connected to ${ssid}` : (data.message || data.detail || "failed");
  } catch (err) {
    msg.textContent = "request failed: " + err;
  } finally {
    button.disabled = false;
    $("wifi-psk").value = "";
    setTimeout(() => { refreshWifi(); refresh(); }, 3000);
  }
}

$("refresh").addEventListener("click", () => { refresh(); refreshWifi(); });
$("log-close").addEventListener("click", () => {
  $("log").hidden = true;
  if (stream) { stream.close(); stream = null; }
});
$("wifi-form").addEventListener("submit", connectWifi);

// Poll only while someone is looking: every tick costs an ssh round trip to the
// rig on the server side, and a dashboard left open in a background tab should
// not keep the Pi busy.
const tick = () => { if (!document.hidden) refresh(); };
document.addEventListener("visibilitychange", () => { if (!document.hidden) refresh(); });

refresh();
refreshWifi();
setInterval(tick, 20000);
