const statusEl = document.getElementById("status");
const controlsEl = document.getElementById("controls");
const controlsMetaEl = document.getElementById("controlsMeta");
const metersEl = document.getElementById("meters");
const meterMetaEl = document.getElementById("meterMeta");
const snapshotButton = document.getElementById("snapshotButton");
let meterTimer = null;

function setStatus(text, cls = "") {
  statusEl.textContent = text;
  statusEl.className = `status ${cls}`.trim();
}

async function api(path, options) {
  const response = await fetch(path, { headers: { "Accept": "application/json", ...(options?.headers || {}) }, ...options });
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
  const body = await response.json();
  if (body.ok === false) throw new Error(body.error || `${path}: API error`);
  return Object.prototype.hasOwnProperty.call(body, "data") ? body.data : body;
}

function resolvedValue(control) {
  if (control.type === "enum") {
    const index = Number(control.value_index ?? control.value ?? 0);
    const label = control.value_label ?? (Array.isArray(control.items) ? control.items[index] : undefined);
    return `${Number.isFinite(index) ? index : control.value}${label ? ` (${label})` : ""}`;
  }
  if (control.value_text !== undefined) return String(control.value_text);
  if (control.value !== undefined) return String(control.value);
  if (Array.isArray(control.values)) return control.values.join(", ");
  return "";
}

function displayGroup(control) {
  const group = String(control.group || "Other");
  const name = String(control.name || "");
  if (/meter/i.test(group) || /meter/i.test(name)) return "Meters";
  if (/mix|mux|route/i.test(group) || /^Mix\s/i.test(name) || /^PCM \d+ Capture Enum$/.test(name)) return "Mixer";
  if (/input/i.test(group) || /Line In|Analogue|S\/PDIF|ADAT/.test(name)) return "Inputs";
  if (/output|playback|speaker|headphone/i.test(group) || /Output|Playback|Speaker|Headphone/.test(name)) return "Outputs";
  return group;
}

function renderControls(controls) {
  const grouped = new Map();
  for (const control of controls) {
    const key = displayGroup(control);
    if (!grouped.has(key)) grouped.set(key, []);
    grouped.get(key).push(control);
  }
  const order = ["Inputs", "Outputs", "Mixer", "Meters"];
  const keys = [...grouped.keys()].sort((a, b) => (order.indexOf(a) < 0 ? 99 : order.indexOf(a)) - (order.indexOf(b) < 0 ? 99 : order.indexOf(b)) || a.localeCompare(b));
  controlsEl.replaceChildren(...keys.map((key, index) => {
    const details = document.createElement("details");
    details.className = "group";
    details.open = index < 4;
    const items = grouped.get(key);
    const summary = document.createElement("summary");
    summary.textContent = `${key} (${items.length})`;
    const table = document.createElement("table");
    table.className = "control-list";
    const tbody = document.createElement("tbody");
    for (const control of items) {
      const tr = document.createElement("tr");
      const nameTd = document.createElement("td");
      const name = document.createElement("div");
      name.className = "name";
      name.textContent = control.name || "unnamed";
      const type = document.createElement("div");
      type.className = "type";
      type.textContent = `${control.group || "Other"} · ${control.type || "?"}`;
      nameTd.append(name, type);
      const valueTd = document.createElement("td");
      valueTd.className = "value";
      valueTd.textContent = resolvedValue(control);
      if (control.type === "enum" && /\(.+\)/.test(valueTd.textContent)) valueTd.classList.add("label");
      tr.append(nameTd, valueTd);
      tbody.appendChild(tr);
    }
    table.appendChild(tbody);
    details.append(summary, table);
    return details;
  }));
  controlsMetaEl.textContent = `${controls.length} controls`;
}

async function loadControls() {
  try {
    const controls = await api("/api/v1/controls");
    renderControls(Array.isArray(controls) ? controls : []);
    setStatus("api ok", "ok");
  } catch (err) {
    controlsEl.innerHTML = `<div class="error"></div>`;
    controlsEl.querySelector(".error").textContent = err.message;
    setStatus("api error", "err");
  }
}

function meterDb(raw) {
  const n = Number(raw);
  if (!Number.isFinite(n) || n <= 0) return -60;
  if (n <= 255) return Math.max(-60, Math.min(0, 20 * Math.log10(n / 255)));
  return Math.max(-60, Math.min(0, n / 100));
}

function renderMeters(values) {
  metersEl.replaceChildren(...values.map((value, index) => {
    const db = meterDb(value);
    const row = document.createElement("div");
    row.className = "meter-row";
    const label = document.createElement("div");
    label.className = "meter-label";
    label.textContent = `CH ${String(index + 1).padStart(2, "0")}`;
    const track = document.createElement("div");
    track.className = "meter-track";
    const fill = document.createElement("div");
    fill.className = "meter-fill";
    fill.style.width = `${Math.max(0, Math.min(100, ((db + 60) / 60) * 100))}%`;
    track.appendChild(fill);
    const text = document.createElement("div");
    text.className = "meter-db";
    text.textContent = `${db.toFixed(1)} dB`;
    row.append(label, track, text);
    return row;
  }));
  meterMetaEl.textContent = `${values.length} channels · ${new Date().toLocaleTimeString()}`;
}

async function pollMeter() {
  if (document.visibilityState === "hidden") return;
  try {
    const meter = await api("/api/v1/meter");
    const values = Array.isArray(meter.values) ? meter.values : String(meter.value_text || meter.value || "").split(",").map(Number).filter(Number.isFinite);
    renderMeters(values);
  } catch (err) {
    meterMetaEl.textContent = err.message;
    setStatus("meter error", "err");
  }
}

function startMeter() {
  if (meterTimer) clearInterval(meterTimer);
  if (document.visibilityState !== "hidden") {
    pollMeter();
    meterTimer = setInterval(pollMeter, 200);
  }
}

document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "hidden" && meterTimer) {
    clearInterval(meterTimer);
    meterTimer = null;
    meterMetaEl.textContent = "paused (tab hidden)";
  } else {
    startMeter();
  }
});

snapshotButton.addEventListener("click", async () => {
  snapshotButton.disabled = true;
  try {
    const data = await api("/api/v1/snapshot/save", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: "-" })
    });
    const text = String(data.output || "");
    const start = text.indexOf("{");
    const end = text.lastIndexOf("}");
    if (start < 0 || end <= start) throw new Error("snapshot response did not contain JSON");
    const snapshot = JSON.stringify(JSON.parse(text.slice(start, end + 1)), null, 2) + "\n";
    const blob = new Blob([snapshot], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `scarlett-snapshot-${new Date().toISOString().replace(/[:.]/g, "-")}.json`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  } catch (err) {
    alert(`Snapshot download failed: ${err.message}`);
  } finally {
    snapshotButton.disabled = false;
  }
});

loadControls();
startMeter();
