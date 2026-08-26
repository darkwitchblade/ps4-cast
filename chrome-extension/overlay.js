(() => {
  function placeControl(rect, viewport, options = {}) {
    const size = options.size || 44;
    const gap = options.gap || 10;
    const edge = options.edge || 10;
    const leftEdge = viewport.left || 0;
    const topEdge = viewport.top || 0;
    const rightEdge = leftEdge + viewport.width;
    const bottomEdge = topEdge + viewport.height;
    const visible = rect.width >= 120 && rect.height >= 72 &&
      rect.right > leftEdge && rect.bottom > topEdge &&
      rect.left < rightEdge && rect.top < bottomEdge;

    if (!visible) return { visible: false, x: -size * 2, y: -size * 2, inside: false, side: "hidden" };

    let x;
    let inside = false;
    let side = "right";
    if (rect.right + gap + size <= rightEdge - edge) {
      x = rect.right + gap;
    } else if (rect.left - gap - size >= leftEdge + edge) {
      x = rect.left - gap - size;
      side = "left";
    } else {
      x = rect.right - size - 12;
      inside = true;
      side = "inside";
    }

    const minX = leftEdge + edge;
    const maxX = Math.max(minX, rightEdge - edge - size);
    const minY = topEdge + edge;
    const maxY = Math.max(minY, bottomEdge - edge - size);
    return {
      visible: true,
      x: Math.round(Math.min(maxX, Math.max(minX, x))),
      y: Math.round(Math.min(maxY, Math.max(minY, rect.top + 12))),
      inside,
      side
    };
  }

  globalThis.PS4CastOverlay = Object.freeze({ placeControl });
})();
