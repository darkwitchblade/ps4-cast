const HLS_TYPES = ["application/vnd.apple.mpegurl", "application/x-mpegurl", "audio/mpegurl"];
const DASH_TYPES = ["application/dash+xml"];
const SEGMENT_RE = /(?:^|\/)(?:init(?:ialization)?|segment|seg-|chunk-|frag(?:ment)?)[^/]*\.(?:m4s|mp4|cmfv|cmfa)(?:$|[?#])|\.(?:m4s|cmfv|cmfa|ts|aac)(?:$|[?#])/i;
const AD_HINT_RE = /doubleclick|googlesyndication|imasdk|adserver|adservice|\/vast(?:\/|\?|$)|\/ads(?:\/|\?|$)|tracking|analytics/i;
const MASTER_HINT_RE = /(?:^|\/)(?:master|main|multivariant)(?:[-_.]|\/|$)/i;
const FIXED_VARIANT_RE = /(?:^|[-_/])(?:2160|1440|1080|720|576|540|480|360|240)p(?:[-_.?/]|$)|index-s\d+p/i;

export function headerValue(headers = [], name) {
  const match = headers.find((header) => header.name?.toLowerCase() === name.toLowerCase());
  return match?.value || "";
}

export function classifyRequest(details) {
  const rawUrl = details?.url || "";
  if (!/^https?:\/\//i.test(rawUrl) || SEGMENT_RE.test(rawUrl)) return null;

  let parsed;
  try { parsed = new URL(rawUrl); } catch { return null; }
  const path = parsed.pathname.toLowerCase();
  const contentType = headerValue(details.responseHeaders, "content-type").split(";", 1)[0].trim().toLowerCase();
  const contentLength = Number(headerValue(details.responseHeaders, "content-length")) || 0;
  const byUrl = {
    hls: /\.m3u8?$/.test(path),
    dash: /\.mpd$/.test(path),
    file: /\.(?:mp4|m4v|mkv|webm|mov|avi|m2ts)$/.test(path)
  };

  let kind = "";
  if (byUrl.hls || HLS_TYPES.includes(contentType)) kind = "hls";
  else if (byUrl.dash || DASH_TYPES.includes(contentType)) kind = "dash";
  else if (byUrl.file || contentType.startsWith("video/")) kind = "file";
  if (!kind) return null;

  let score = kind === "hls" ? 120 : kind === "file" ? 90 : 55;
  if (details.type === "media") score += 18;
  if (byUrl.hls || byUrl.file || byUrl.dash) score += 12;
  if (contentLength > 32 * 1024 * 1024) score += 8;
  if (AD_HINT_RE.test(rawUrl)) score -= 90;
  const adaptive = kind === "hls" && MASTER_HINT_RE.test(path);
  const fixedVariant = kind === "hls" && FIXED_VARIANT_RE.test(path);
  if (adaptive) score += 45;
  if (fixedVariant) score -= 25;

  return {
    url: rawUrl,
    kind,
    supported: kind !== "dash",
    contentType,
    contentLength,
    host: parsed.hostname,
    adaptive,
    fixedVariant,
    score
  };
}

export function rankCandidate(candidate, frames) {
  let score = candidate.score || 0;
  const exact = frames.find((frame) => frame.frameId === candidate.frameId && frame.playing);
  const anyPlaying = frames.some((frame) => frame.playing);
  if (exact) {
    score += 100;
    if (exact.duration >= 60) score += 20;
    if (candidate.kind === "file" && exact.duration >= 600 && candidate.contentLength > 0 && candidate.contentLength < 8 * 1024 * 1024) score -= 80;
  } else if (anyPlaying) score += 12;
  if (!candidate.supported) score -= 200;
  if (candidate.seenAt && Date.now() - candidate.seenAt > 120000) score -= 80;
  return score;
}

export function normalizeReceiver(input) {
  const value = String(input || "").trim();
  if (!value) return "";

  let parsed;
  try { parsed = new URL(/^https?:\/\//i.test(value) ? value : `http://${value}`); }
  catch { return ""; }
  if (parsed.protocol !== "http:" || parsed.username || parsed.password ||
      parsed.pathname !== "/" || parsed.search || parsed.hash) return "";

  const host = parsed.hostname.toLowerCase();
  const octets = host.split(".").map(Number);
  const validV4 = octets.length === 4 && octets.every((part) => Number.isInteger(part) && part >= 0 && part <= 255);
  const privateV4 = validV4 && (octets[0] === 10 ||
    (octets[0] === 192 && octets[1] === 168) ||
    (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31));
  const localName = host === "localhost" || (/^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.local$/i.test(host));
  const port = parsed.port || "8080";
  if ((!privateV4 && !localName) || Number(port) < 1 || Number(port) > 65535) return "";
  return `${host}:${port}`;
}

export function buildCastForm(candidate, frame, userAgent = "") {
  const body = new URLSearchParams();
  body.set("url", candidate.url);
  // Only forward context observed on this exact media request. Synthesizing the
  // outer page as Referer/Origin breaks iframe/CDN players whose successful
  // manifest request intentionally has no page headers.
  if (candidate.referer) body.set("referer", candidate.referer);
  if (candidate.origin) body.set("origin", candidate.origin);
  if (candidate.userAgent || userAgent) body.set("ua", candidate.userAgent || userAgent);
  body.set("kind", candidate.kind || "file");
  return body.toString();
}

export function displayHost(url) {
  try { return new URL(url).hostname.replace(/^www\./, ""); } catch { return "media source"; }
}

// Accept either "host:port" or a full receiver URL copied from the TV
// ("http://192.168.1.4:8080/?t=XXXXXXXX"); the ?t= token is stored separately
// and sent as X-PS4Cast-Token on cast handoff.
export function parseReceiverInput(input) {
  const value = String(input || "").trim();
  let token = "";
  let origin = value;
  try {
    const parsed = new URL(/^https?:\/\//i.test(value) ? value : `http://${value}`);
    token = parsed.searchParams.get("t") || "";
    origin = parsed.origin;
  } catch { /* fall through to plain host:port handling */ }
  const receiver = normalizeReceiver(origin);
  if (!receiver) return { receiver: "", token: "" };
  return { receiver, token: /^[A-Z2-9]{8}$/.test(token) ? token : "" };
}
