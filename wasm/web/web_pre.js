// --pre-js of gk_web: browser-only helpers.
const gkInWorker = typeof WorkerGlobalScope !== "undefined" && self instanceof WorkerGlobalScope;

// A trap in a pthread only reaches the page as a message; print its stack from the worker itself.
if (gkInWorker) {
  self.addEventListener("error", (e) => {
    if (e.error && e.error.stack) {
      console.error("[worker] " + e.error.stack);
    }
  });
}

// Gamepads: the Gamepad API exists only on the page, but SDL3's Emscripten joystick driver reads
// each pad's id (vendor, product, XInput) with JS that runs in the pthread that handles the
// connection, where navigator.getGamepads is missing and the thread dies. The page mirrors the
// pads' ids to the workers; there, navigator.getGamepads returns the mirror (an id-less
// placeholder for a pad not mirrored yet). Buttons and axes still go through the html5 API, which
// Emscripten proxies to the page.
const gkGamepadChannel = new BroadcastChannel("gk-gamepads");
if (gkInWorker) {
  let pads = [];
  gkGamepadChannel.onmessage = (e) => {
    pads = e.data;
  };
  navigator.getGamepads = () =>
    new Proxy(pads, {
      get: (target, key) =>
        typeof key === "string" && /^\d+$/.test(key) && !target[key]
          ? { id: "", index: Number(key) }
          : target[key],
    });
} else if (navigator.getGamepads) {
  const mirror = () =>
    gkGamepadChannel.postMessage(
      Array.from(navigator.getGamepads(), (p) => (p ? { id: p.id, index: p.index } : null))
    );
  // capture, before SDL's listeners queue the event to its thread
  addEventListener("gamepadconnected", mirror, true);
  addEventListener("gamepaddisconnected", mirror, true);
}
