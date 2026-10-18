// --pre-js of gk_web: browser-only helpers.
// A trap in a pthread only reaches the page as a message; print its stack from the worker itself.
if (typeof WorkerGlobalScope !== "undefined" && self instanceof WorkerGlobalScope) {
  self.addEventListener("error", (e) => {
    if (e.error && e.error.stack) {
      console.error("[worker] " + e.error.stack);
    }
  });
}
