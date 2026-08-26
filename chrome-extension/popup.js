const $ = (id) => document.getElementById(id);
const receiver = $("receiver");
const overlay = $("overlay");
const sources = $("sources");
const notice = $("notice");

function bytes(value) {
  if (!value) return "";
  if (value >= 1024 * 1024) return `${(value / 1024 / 1024).toFixed(value >= 100 * 1024 * 1024 ? 0 : 1)} MB`;
  return `${Math.round(value / 1024)} KB`;
}

function setNotice(message, kind = "") {
  notice.hidden = !message;
  notice.textContent = message || "";
  notice.className = `notice ${kind}`;
}

function connection(text, kind = "") {
  const el = $("connection");
  el.className = `connection ${kind}`;
  el.querySelector("span").textContent = text;
}

function render(state) {
  receiver.value = state.receiver || "";
  overlay.checked = state.overlay !== false;
  const total = state.tabs.reduce((sum, tab) => sum + tab.candidates.length, 0);
  $("sourceCount").textContent = total ? `${total} detected` : "Listening";
  sources.replaceChildren();
  if (!state.tabs.length) {
    const empty = document.createElement("div");
    empty.className = "empty";
    empty.innerHTML = "<strong>Play a video in Chrome</strong>The Cast control appears when a non-DRM stream is detected.";
    sources.append(empty);
    return;
  }

  for (const tab of state.tabs) {
    const heading = document.createElement("div");
    heading.className = "tab-title";
    heading.textContent = tab.title;
    sources.append(heading);
    if (!tab.candidates.length) {
      const waiting = document.createElement("div");
      waiting.className = "empty";
      waiting.textContent = tab.drm ? "DRM-protected playback cannot be cast." : "Watching this player for a media manifest…";
      sources.append(waiting);
      continue;
    }
    for (const candidate of tab.candidates) {
      const row = document.createElement("div");
      row.className = "source";
      const icon = document.createElement("span"); icon.className = "source-icon"; icon.textContent = candidate.kind.toUpperCase();
      const copy = document.createElement("span"); copy.className = "source-copy";
      const title = document.createElement("span"); title.className = "source-title"; title.textContent = candidate.host || "Media source";
      const meta = document.createElement("span"); meta.className = "source-meta";
      const streamKind = candidate.adaptive ? "Adaptive master" : candidate.fixedVariant ? "Fixed rendition" : (candidate.contentType || candidate.kind.toUpperCase());
      meta.textContent = candidate.supported ? [candidate.exactPlaying ? "Playing frame" : "Recent request", streamKind, bytes(candidate.contentLength)].filter(Boolean).join(" · ") : "DASH is not supported yet";
      copy.append(title, meta);
      const button = document.createElement("button"); button.className = "cast-button"; button.textContent = "Cast";
      button.disabled = !candidate.supported || candidate.drm;
      button.addEventListener("click", async () => {
        button.disabled = true; button.textContent = "Sending…"; setNotice("");
        const result = await chrome.runtime.sendMessage({ type: "CAST_ID", tabId: tab.tabId, id: candidate.id });
        if (result?.ok) { button.textContent = "Sent"; setNotice(result.message || "Sent to PS4", "ok"); }
        else { button.disabled = false; button.textContent = "Retry"; setNotice(result?.error || "Cast failed", "error"); }
      });
      row.append(icon, copy, button); sources.append(row);
    }
  }
}

async function state() {
  const result = await chrome.runtime.sendMessage({ type: "GET_STATE" });
  if (result?.ok) render(result.state);
  else setNotice(result?.error || "Extension service is unavailable", "error");
}

async function save() {
  const result = await chrome.runtime.sendMessage({ type: "SAVE_CONFIG", receiver: receiver.value, overlay: overlay.checked });
  if (!result?.ok) { setNotice(result?.error || "Could not save receiver", "error"); return false; }
  receiver.value = result.receiver; return true;
}

$("test").addEventListener("click", async () => {
  setNotice(""); connection("Checking…");
  if (!await save()) { connection("Invalid address", "error"); return; }
  const result = await chrome.runtime.sendMessage({ type: "PING", receiver: receiver.value });
  if (result?.ok) connection(`PS4 Cast ${result.version}`, "online");
  else { connection("Offline", "error"); setNotice(result?.error || "Receiver did not answer", "error"); }
});
receiver.addEventListener("change", save);
receiver.addEventListener("keydown", (event) => { if (event.key === "Enter") $("test").click(); });
overlay.addEventListener("change", save);

if (globalThis.chrome?.runtime?.sendMessage) state();
else render({ receiver: "192.168.1.4:8080", overlay: true, tabs: [{ tabId: 1, title: "Sample movie", drm: false, candidates: [{ id: "demo", kind: "hls", supported: true, host: "media.example", contentType: "application/vnd.apple.mpegurl", contentLength: 0 }] }] });
