(() => {
  if (window.__ps4CastContentLoaded) return;
  window.__ps4CastContentLoaded = true;

  let encrypted = false;
  let host;
  let button;
  let label;
  let currentMedia;
  let candidateCount = 0;
  let casting = false;
  let overlayEnabled = true;
  let lastPlaying = false;
  let lastSend = 0;
  let positionFrame = 0;
  let observedMedia;
  const mediaObserver = typeof ResizeObserver === "function" ? new ResizeObserver(() => schedulePosition()) : null;

  function activeMedia() {
    return [...document.querySelectorAll("video,audio")]
      .filter((media) => !media.paused && !media.ended && media.readyState >= 2)
      .sort((a, b) => {
        const ar = a.getBoundingClientRect(), br = b.getBoundingClientRect();
        return br.width * br.height - ar.width * ar.height;
      })[0] || null;
  }

  function resources(media) {
    const found = new Set();
    const add = (value) => {
      if (!value || /^blob:|^data:/i.test(value)) return;
      try {
        const url = new URL(value, location.href).href;
        if (/^https?:/i.test(url) && /\.m3u8?(?:$|[?#])|\.mpd(?:$|[?#])|\.(?:mp4|m4v|mkv|webm|mov|avi|m2ts)(?:$|[?#])/i.test(url)) found.add(url);
      } catch {}
    };
    add(media?.currentSrc); add(media?.src);
    media?.querySelectorAll("source").forEach((source) => add(source.src));
    try { performance.getEntriesByType("resource").slice(-250).forEach((entry) => add(entry.name)); } catch {}
    return [...found].slice(-40);
  }

  function updatePosition(media = currentMedia) {
    if (!button) return;
    const visualMedia = media?.tagName === "VIDEO" ? media : null;
    if (!visualMedia?.isConnected) {
      button.classList.remove("onscreen", "inside");
      return;
    }
    const rect = visualMedia.getBoundingClientRect();
    const viewport = window.visualViewport;
    const placement = globalThis.PS4CastOverlay.placeControl(rect, {
      left: viewport?.offsetLeft || 0,
      top: viewport?.offsetTop || 0,
      width: viewport?.width || innerWidth,
      height: viewport?.height || innerHeight
    });
    button.classList.toggle("onscreen", placement.visible);
    if (!placement.visible) return;
    button.style.setProperty("--cast-x", `${placement.x}px`);
    button.style.setProperty("--cast-y", `${placement.y}px`);
    button.classList.toggle("inside", placement.inside);
    button.classList.toggle("left-side", placement.side === "left");
  }

  function observeMedia(media) {
    const visualMedia = media?.tagName === "VIDEO" ? media : null;
    if (observedMedia === visualMedia) return;
    if (observedMedia) mediaObserver?.unobserve(observedMedia);
    observedMedia = visualMedia;
    if (observedMedia) mediaObserver?.observe(observedMedia);
  }

  function schedulePosition() {
    if (positionFrame) return;
    positionFrame = requestAnimationFrame(() => {
      positionFrame = 0;
      updatePosition();
    });
  }

  function setState(state, message) {
    if (!button) return;
    if (host) host.dataset.state = state || "ready";
    button.classList.remove("sending", "casting", "error");
    if (state) button.classList.add(state);
    button.disabled = state === "sending";
    button.setAttribute("aria-busy", state === "sending" ? "true" : "false");
    button.setAttribute("aria-pressed", state === "casting" ? "true" : "false");
    const text = message || (state === "sending" ? "Sending to PS4" :
      state === "casting" ? "Casting to PS4" : state === "error" ? "Cast failed" : "Cast to PS4");
    button.setAttribute("aria-label", text);
    button.title = text;
    label.textContent = text;
  }

  function ensureControl() {
    if (host?.isConnected) return;
    host = document.createElement("div");
    host.id = "ps4-cast-control";
    const shadow = host.attachShadow({ mode: "closed" });
    const style = document.createElement("style");
    style.textContent = `
      :host{all:initial}
      button{position:fixed;left:var(--cast-x,-80px);top:var(--cast-y,-80px);z-index:2147483647;width:44px;height:44px;padding:0;
        display:grid;place-items:center;border:1px solid rgba(240,247,243,.32);border-radius:50%;
        background:#151c21;color:#eef5f1;font:600 12px/1.2 ui-sans-serif,system-ui,sans-serif;letter-spacing:0;
        box-shadow:0 7px 20px rgba(4,10,13,.32);cursor:pointer;opacity:0;transform:scale(.86);
        pointer-events:none;transition:opacity .16s ease,transform .16s cubic-bezier(.22,1,.36,1),background .16s ease,border-color .16s ease}
      button.show.onscreen{opacity:.96;transform:scale(1);pointer-events:auto}
      button.inside{box-shadow:0 7px 22px rgba(4,10,13,.48)}
      button:hover{background:#202a30;border-color:rgba(240,247,243,.52)}
      button:focus-visible{outline:3px solid rgba(90,201,153,.55);outline-offset:3px}
      button:active{transform:scale(.93)}button:disabled{cursor:wait}
      button.casting{background:#237b5b;border-color:#76d5aa;color:#f1fbf6;animation:ps4cast-confirm .34s cubic-bezier(.22,1,.36,1)}
      button.error{background:#8c3b3d;border-color:#e7a4a5}
      button::after{content:attr(aria-label);position:absolute;right:52px;top:50%;transform:translateY(-50%) translateX(4px);
        width:max-content;max-width:180px;padding:7px 9px;border-radius:6px;background:#11181c;color:#edf4f0;
        box-shadow:0 5px 16px rgba(4,10,13,.34);font-size:12px;opacity:0;pointer-events:none;transition:opacity .12s ease,transform .12s ease}
      button.left-side::after{right:auto;left:52px;transform:translateY(-50%) translateX(-4px)}
      button:hover::after,button:focus-visible::after{opacity:1;transform:translateY(-50%) translateX(0)}
      .cast{width:20px;height:20px;position:relative;display:block}
      .screen{position:absolute;right:0;top:0;width:14px;height:10px;border:1.8px solid currentColor;border-radius:2px}
      .wave1,.wave2{position:absolute;left:0;bottom:0;border:1.8px solid currentColor;border-left:0;border-bottom:0;border-radius:0 10px 0 0}
      .wave1{width:5px;height:5px}.wave2{width:10px;height:10px}.dot{position:absolute;left:0;bottom:0;width:3px;height:3px;background:currentColor;border-radius:50%}
      .check{display:none;width:18px;height:10px;border-left:2px solid currentColor;border-bottom:2px solid currentColor;transform:translateY(-2px) rotate(-45deg)}
      .sending .cast{animation:ps4cast-spin .8s linear infinite}.casting .cast{display:none}.casting .check{display:block}
      .sr{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
      @keyframes ps4cast-spin{to{transform:rotate(360deg)}}
      @keyframes ps4cast-confirm{0%{transform:scale(.9)}100%{transform:scale(1)}}
      @media(prefers-reduced-motion:reduce){button{transition:none}.sending .cast,.casting{animation:none}}
    `;
    button = document.createElement("button");
    button.type = "button";
    button.innerHTML = '<span class="cast" aria-hidden="true"><span class="screen"></span><span class="wave2"></span><span class="wave1"></span><span class="dot"></span></span><span class="check" aria-hidden="true"></span><span class="label sr" aria-live="polite">Cast to PS4</span>';
    label = button.querySelector(".label");
    setState("");
    button.addEventListener("click", async () => {
      if (casting) return;
      setState("sending");
      try {
        const result = await chrome.runtime.sendMessage({ type: "CAST_BEST" });
        if (!result?.ok) throw new Error(result?.error || "Cast failed");
        casting = true;
        setState("casting");
      } catch (error) {
        casting = false;
        setState("error", error.message || "Cast failed");
        setTimeout(() => setState(""), 2600);
      }
    });
    shadow.append(style, button);
    (document.documentElement || document.body).appendChild(host);
  }

  async function report(force = false) {
    const media = activeMedia();
    const now = Date.now();
    if (!force && now - lastSend < 1200) return;
    lastSend = now;
    const playing = !!media;
    if (media !== currentMedia) {
      currentMedia = media;
      observeMedia(media);
      candidateCount = 0;
      casting = false;
      if (button) setState("");
    }
    if (playing && media?.tagName === "VIDEO") ensureControl();
    try {
      const response = await chrome.runtime.sendMessage({
        type: "FRAME_STATE",
        state: {
          playing,
          drm: encrypted || !!media?.mediaKeys,
          title: document.title || "Playing video",
          pageUrl: location.href,
          duration: Number.isFinite(media?.duration) ? media.duration : 0,
          currentTime: media?.currentTime || 0,
          resources: resources(media)
        }
      });
      if (typeof response?.count === "number") candidateCount = response.count;
      overlayEnabled = response?.overlay !== false;
      if (button) {
        updatePosition(media);
        const visible = playing && media?.tagName === "VIDEO" && candidateCount > 0 && overlayEnabled;
        button.classList.toggle("show", visible);
        host.dataset.visible = visible ? "true" : "false";
      }
    } catch {}
    lastPlaying = playing;
  }

  document.addEventListener("encrypted", () => { encrypted = true; report(true); }, true);
  document.addEventListener("play", () => report(true), true);
  document.addEventListener("playing", () => report(true), true);
  document.addEventListener("pause", () => setTimeout(() => report(true), 120), true);
  document.addEventListener("ended", () => report(true), true);
  new MutationObserver(() => report()).observe(document.documentElement, { childList: true, subtree: true });
  addEventListener("scroll", schedulePosition, true);
  addEventListener("resize", schedulePosition, { passive: true });
  addEventListener("fullscreenchange", schedulePosition, true);
  window.visualViewport?.addEventListener("resize", schedulePosition, { passive: true });
  window.visualViewport?.addEventListener("scroll", schedulePosition, { passive: true });
  setInterval(() => { if (activeMedia() || lastPlaying) report(true); }, 1800);

  chrome.runtime.onMessage.addListener((message) => {
    if (message.type !== "DETECTION" || !button) return;
    candidateCount = message.drm ? 0 : (message.count || 0);
    const visible = currentMedia?.tagName === "VIDEO" && candidateCount > 0 && overlayEnabled;
    button.classList.toggle("show", visible);
    host.dataset.visible = visible ? "true" : "false";
  });
  report(true);
})();
