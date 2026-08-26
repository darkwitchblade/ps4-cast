import { buildCastForm, classifyRequest, displayHost, normalizeReceiver, parseReceiverInput, rankCandidate } from "./detector.js";

const frames = new Map();
const candidates = new Map();
const requests = new Map();
const MAX_CANDIDATES_PER_TAB = 30;
const REQUEST_TTL = 180000;

const frameKey = (tabId, frameId) => `${tabId}:${frameId}`;

function framesFor(tabId) {
  return [...frames.values()].filter((frame) => frame.tabId === tabId && Date.now() - frame.seenAt < 12000);
}

function candidatesFor(tabId, frameId = null) {
  const list = [...(candidates.get(tabId)?.values() || [])];
  const currentFrames = framesFor(tabId);
  return list
    .filter((candidate) => Date.now() - candidate.seenAt < 300000)
    .map((candidate) => ({
      ...candidate,
      exactFrame: frameId != null && candidate.frameId === frameId,
      rank: rankCandidate(candidate, currentFrames) + (frameId != null && candidate.frameId === frameId ? 45 : 0)
    }))
    .sort((a, b) => b.rank - a.rank);
}

function requestHeaders(headers = []) {
  const result = {};
  for (const header of headers) {
    const name = header.name?.toLowerCase();
    if (name === "referer") result.referer = header.value || "";
    else if (name === "origin") result.origin = header.value || "";
    else if (name === "user-agent") result.userAgent = header.value || "";
  }
  return result;
}

function addCandidate(tabId, frameId, info, headers = {}) {
  if (tabId < 0 || !info) return;
  let tabCandidates = candidates.get(tabId);
  if (!tabCandidates) candidates.set(tabId, tabCandidates = new Map());
  const previous = tabCandidates.get(info.url);
  tabCandidates.set(info.url, {
    ...previous,
    ...info,
    ...headers,
    id: `${tabId}:${frameId}:${info.url}`,
    tabId,
    frameId,
    seenAt: Date.now()
  });
  while (tabCandidates.size > MAX_CANDIDATES_PER_TAB) tabCandidates.delete(tabCandidates.keys().next().value);
  updateTab(tabId);
}

function prune() {
  const now = Date.now();
  for (const [id, request] of requests) if (now - request.seenAt > REQUEST_TTL) requests.delete(id);
  for (const [key, frame] of frames) if (now - frame.seenAt > 30000) frames.delete(key);
  for (const [tabId, list] of candidates) {
    for (const [url, candidate] of list) if (now - candidate.seenAt > 300000) list.delete(url);
    if (!list.size) candidates.delete(tabId);
  }
}

async function updateTab(tabId) {
  const activeFrames = framesFor(tabId).filter((frame) => frame.playing);
  const count = candidatesFor(tabId).filter((candidate) => candidate.supported).length;
  await chrome.action.setBadgeText({ tabId, text: activeFrames.length && count ? String(Math.min(count, 9)) : "" }).catch(() => {});
  if (count) await chrome.action.setBadgeBackgroundColor({ tabId, color: "#2B8A66" }).catch(() => {});
  for (const frame of activeFrames) {
    const frameCount = candidatesFor(tabId, frame.frameId)
      .filter((candidate) => candidate.supported && candidate.frameId === frame.frameId).length;
    chrome.tabs.sendMessage(tabId, { type: "DETECTION", count: frameCount, drm: frame.drm }, { frameId: frame.frameId }).catch(() => {});
  }
}

chrome.webRequest.onBeforeRequest.addListener((details) => {
  if (details.tabId < 0 || !/^https?:/i.test(details.url)) return;
  requests.set(details.requestId, { ...details, seenAt: Date.now(), headers: {} });
  addCandidate(details.tabId, details.frameId, classifyRequest(details));
}, { urls: ["http://*/*", "https://*/*"], types: ["media", "xmlhttprequest", "other"] });

chrome.webRequest.onBeforeSendHeaders.addListener((details) => {
  const request = requests.get(details.requestId);
  if (request) request.headers = requestHeaders(details.requestHeaders);
}, {
  urls: ["http://*/*", "https://*/*"],
  types: ["media", "xmlhttprequest", "other"]
}, ["requestHeaders", "extraHeaders"]);

chrome.webRequest.onHeadersReceived.addListener((details) => {
  const request = requests.get(details.requestId) || details;
  addCandidate(details.tabId, details.frameId, classifyRequest(details), request.headers);
}, { urls: ["http://*/*", "https://*/*"], types: ["media", "xmlhttprequest", "other"] }, ["responseHeaders", "extraHeaders"]);

chrome.webRequest.onCompleted.addListener((details) => requests.delete(details.requestId), { urls: ["http://*/*", "https://*/*"] });
chrome.webRequest.onErrorOccurred.addListener((details) => requests.delete(details.requestId), { urls: ["http://*/*", "https://*/*"] });

chrome.tabs.onRemoved.addListener((tabId) => {
  candidates.delete(tabId);
  for (const key of frames.keys()) if (key.startsWith(`${tabId}:`)) frames.delete(key);
});
chrome.tabs.onUpdated.addListener((tabId, change) => {
  if (change.status === "loading") {
    candidates.delete(tabId);
    for (const key of frames.keys()) if (key.startsWith(`${tabId}:`)) frames.delete(key);
    updateTab(tabId);
  }
});

async function receiverConfig() {
  const saved = await chrome.storage.local.get({ receiver: "192.168.1.4:8080", token: "", overlay: true });
  return { receiver: normalizeReceiver(saved.receiver), token: saved.token || "", overlay: saved.overlay !== false };
}

async function castCandidate(candidate, frame) {
  if (!candidate) throw new Error("No playable stream detected yet");
  if (!candidate.supported) throw new Error("DASH streams are not supported by PS4 Cast yet");
  if (frame?.drm) throw new Error("This player reported DRM-protected playback");
  const { receiver, token } = await receiverConfig();
  if (!receiver) throw new Error("Enter a private LAN PS4 address first");

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 6000);
  try {
    const response = await fetch(`http://${receiver}/cast`, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
                 ...(token ? { "X-PS4Cast-Token": token } : {}) },
      body: buildCastForm(candidate, frame, navigator.userAgent),
      signal: controller.signal
    });
    if (!response.ok) {
      if (response.status === 404) throw new Error("Install the PS4 Cast build that includes extension support");
      throw new Error(`PS4 rejected the cast (${response.status})`);
    }
    return { ok: true, message: `Sent ${candidate.kind.toUpperCase()} from ${displayHost(candidate.url)}` };
  } catch (error) {
    if (error.name === "AbortError") throw new Error("PS4 Cast did not answer in time");
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

async function popupState() {
  prune();
  const config = await receiverConfig();
  const tabs = [];
  for (const tabId of new Set([...frames.values()].filter((frame) => frame.playing).map((frame) => frame.tabId))) {
    const tabFrames = framesFor(tabId).filter((frame) => frame.playing);
    const ranked = candidatesFor(tabId);
    tabs.push({
      tabId,
      title: tabFrames[0]?.title || "Playing video",
      pageUrl: tabFrames[0]?.pageUrl || "",
      drm: tabFrames.some((frame) => frame.drm),
      candidates: ranked.slice(0, 8).map(({ id, kind, supported, host, contentType, contentLength, rank, frameId, adaptive, fixedVariant }) => ({
        id, kind, supported, host, contentType, contentLength, rank, frameId, adaptive, fixedVariant,
        drm: !!tabFrames.find((frame) => frame.frameId === frameId)?.drm,
        exactPlaying: tabFrames.some((frame) => frame.frameId === frameId)
      }))
    });
  }
  return { ...config, tabs };
}

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  (async () => {
    if (message.type === "FRAME_STATE" && sender.tab?.id != null) {
      const frameId = sender.frameId ?? 0;
      const state = { ...message.state, tabId: sender.tab.id, frameId, seenAt: Date.now() };
      frames.set(frameKey(sender.tab.id, frameId), state);
      for (const url of message.state.resources || []) {
        const request = [...requests.values()].reverse().find((item) => item.url === url && item.tabId === sender.tab.id);
        addCandidate(sender.tab.id, frameId, classifyRequest({ url, type: "xmlhttprequest" }), request?.headers);
      }
      await updateTab(sender.tab.id);
      const count = candidatesFor(sender.tab.id, frameId)
        .filter((candidate) => candidate.supported && candidate.frameId === frameId).length;
      sendResponse({ ok: true, overlay: (await receiverConfig()).overlay, count });
      return;
    }
    if (message.type === "CAST_BEST" && sender.tab?.id != null) {
      const ranked = candidatesFor(sender.tab.id, sender.frameId ?? 0);
      const frameId = sender.frameId ?? 0;
      const candidate = ranked.find((item) => item.supported && item.frameId === frameId);
      if (!candidate) throw new Error("No stream detected inside this player yet; use the toolbar picker");
      const frame = frames.get(frameKey(sender.tab.id, candidate?.frameId ?? sender.frameId ?? 0));
      sendResponse(await castCandidate(candidate, frame));
      return;
    }
    if (message.type === "CAST_ID") {
      const candidate = candidatesFor(message.tabId).find((item) => item.id === message.id);
      const frame = frames.get(frameKey(message.tabId, candidate?.frameId ?? 0));
      sendResponse(await castCandidate(candidate, frame));
      return;
    }
    if (message.type === "GET_STATE") {
      sendResponse({ ok: true, state: await popupState() });
      return;
    }
    if (message.type === "SAVE_CONFIG") {
      const { receiver, token } = parseReceiverInput(message.receiver);
      if (!receiver) throw new Error("Use a private LAN address such as 192.168.1.4:8080");
      await chrome.storage.local.set({ receiver, token, overlay: message.overlay !== false });
      sendResponse({ ok: true, receiver });
      return;
    }
    if (message.type === "PING") {
      const receiver = normalizeReceiver(message.receiver);
      if (!receiver) throw new Error("Invalid private LAN address");
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 3500);
      try {
        const response = await fetch(`http://${receiver}/status`, { signal: controller.signal });
        if (!response.ok) throw new Error(`Receiver answered ${response.status}`);
        const status = await response.json();
        sendResponse({ ok: true, version: status.ver || "online" });
      } finally { clearTimeout(timer); }
      return;
    }
  })().catch((error) => sendResponse({ ok: false, error: error.message || String(error) }));
  return true;
});

setInterval(prune, 30000);
