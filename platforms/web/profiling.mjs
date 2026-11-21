const clock = globalThis.performance;
const enabled = typeof clock?.now === "function" && typeof clock?.measure === "function" &&
    typeof clock?.clearMeasures === "function" && typeof globalThis.location?.search === "string" &&
    new URLSearchParams(globalThis.location.search).get("engine-profile") === "1";

export function profileStart() {
    return enabled ? clock.now() : undefined;
}

export function profileEnd(name, start) {
    if (start === undefined) return;
    try {
        clock.measure(name, { start });
        clock.clearMeasures(name);
    } catch {
        // Instrumentation must never interfere with runtime completion or failure.
    }
}
