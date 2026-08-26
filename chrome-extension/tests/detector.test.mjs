import assert from "node:assert/strict";
import test from "node:test";
import { buildCastForm, classifyRequest, normalizeReceiver, rankCandidate } from "../detector.js";

test("recognizes a master HLS response", () => {
  const candidate = classifyRequest({
    url: "https://media.example/master?id=7",
    type: "xmlhttprequest",
    responseHeaders: [{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }]
  });
  assert.equal(candidate.kind, "hls");
  assert.equal(candidate.supported, true);
});

test("adaptive master outranks a fixed-resolution HLS rendition", () => {
  const master = classifyRequest({ url: "https://media.example/master.m3u8", type: "xmlhttprequest" });
  const fixed = classifyRequest({ url: "https://media.example/sd/91/index-s1080p-v1-a1.m3u8", type: "xmlhttprequest" });
  assert.equal(master.adaptive, true);
  assert.equal(fixed.fixedVariant, true);
  assert.ok(master.score > fixed.score);
});

test("rejects media segments and marks DASH unsupported", () => {
  assert.equal(classifyRequest({ url: "https://media.example/segment-12.m4s", type: "media" }), null);
  assert.equal(classifyRequest({ url: "https://media.example/manifest.mpd", type: "xmlhttprequest" }).supported, false);
});

test("exact playing iframe outranks a tiny long-duration MP4", () => {
  const frames = [{ frameId: 8, playing: true, duration: 6200 }];
  const hls = { score: 120, frameId: 8, kind: "hls", supported: true, seenAt: Date.now() };
  const ad = { score: 90, frameId: 0, kind: "file", supported: true, contentLength: 3000000, seenAt: Date.now() };
  assert.ok(rankCandidate(hls, frames) > rankCandidate(ad, frames));
});

test("accepts only private LAN receivers", () => {
  assert.equal(normalizeReceiver("http://192.168.1.4"), "192.168.1.4:8080");
  assert.equal(normalizeReceiver("10.0.0.8:9000"), "10.0.0.8:9000");
  assert.equal(normalizeReceiver("living-room.local"), "living-room.local:8080");
  assert.equal(normalizeReceiver("example.com:8080"), "");
  assert.equal(normalizeReceiver("192.168.1.4@evil.example:8080"), "");
  assert.equal(normalizeReceiver("192.168.1.4:8080/other"), "");
});

test("cast form forwards only exact-request compatibility headers", () => {
  const form = new URLSearchParams(buildCastForm(
    {
      url: "https://media.example/master.m3u8?a=1&b=2", kind: "hls", userAgent: "Chrome Test",
      referer: "https://player.example/embed/7", origin: "https://player.example"
    },
    { pageUrl: "https://player.example/watch?id=4&server=2" }
  ));
  assert.equal(form.get("url"), "https://media.example/master.m3u8?a=1&b=2");
  assert.equal(form.get("referer"), "https://player.example/embed/7");
  assert.equal(form.get("origin"), "https://player.example");
  assert.equal(form.get("cookie"), null);
});

test("cast form does not invent page headers when the manifest sent none", () => {
  const form = new URLSearchParams(buildCastForm(
    { url: "https://media.example/master.m3u8", kind: "hls" },
    { pageUrl: "https://outer-page.example/watch" },
    "Chrome Test"
  ));
  assert.equal(form.get("referer"), null);
  assert.equal(form.get("origin"), null);
  assert.equal(form.get("ua"), "Chrome Test");
});

import { parseReceiverInput } from "../detector.js";
test("parseReceiverInput accepts host:port", () => {
  const r = parseReceiverInput("192.168.1.4:8080");
  assert.equal(r.receiver, "192.168.1.4:8080");
  assert.equal(r.token, "");
});
test("parseReceiverInput extracts the pairing token from a TV URL", () => {
  const r = parseReceiverInput("http://192.168.1.4:8080/?t=ABCD2345");
  assert.equal(r.receiver, "192.168.1.4:8080");
  assert.equal(r.token, "ABCD2345");
});
test("parseReceiverInput rejects bad tokens but keeps the receiver", () => {
  const r = parseReceiverInput("http://192.168.1.4:8080/?t=zz");
  assert.equal(r.receiver, "192.168.1.4:8080");
  assert.equal(r.token, "");
});
