import assert from "node:assert/strict";
import test from "node:test";

await import("../overlay.js");
const { placeControl } = globalThis.PS4CastOverlay;
const viewport = { left: 0, top: 0, width: 1200, height: 800 };

test("places the control beside the video's right edge when space is available", () => {
  assert.deepEqual(
    placeControl({ left: 100, top: 80, right: 900, bottom: 530, width: 800, height: 450 }, viewport),
    { visible: true, x: 910, y: 92, inside: false, side: "right" }
  );
});

test("uses the video's left edge before covering it", () => {
  assert.deepEqual(
    placeControl({ left: 100, top: 80, right: 1180, bottom: 687, width: 1080, height: 607 }, viewport),
    { visible: true, x: 46, y: 92, inside: false, side: "left" }
  );
});

test("moves inside the video only when neither outer edge has room", () => {
  assert.deepEqual(
    placeControl({ left: 8, top: 4, right: 1192, bottom: 670, width: 1184, height: 666 }, viewport),
    { visible: true, x: 1136, y: 16, inside: true, side: "inside" }
  );
});

test("hides for tiny or offscreen media instead of leaving a floating control", () => {
  assert.equal(placeControl({ left: 20, top: 20, right: 100, bottom: 60, width: 80, height: 40 }, viewport).visible, false);
  assert.equal(placeControl({ left: 20, top: 900, right: 900, bottom: 1400, width: 880, height: 500 }, viewport).visible, false);
});
