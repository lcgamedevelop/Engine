import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { PerformanceObserver, performance } from "node:perf_hooks";
import { test } from "node:test";
import vm from "node:vm";

const source = await readFile(new URL("../../../platforms/web/bootstrap.mjs", import.meta.url), "utf8");
const profilingSource = await readFile(new URL("../../../platforms/web/profiling.mjs", import.meta.url), "utf8");
const tick = () => new Promise(resolve => setImmediate(resolve));

async function fixture(adapter = {}, profiling = false, memory = false) {
    const events = [];
    let callbacks, completeFactory, completeStorage;
    class Canvas { id = ""; transferControlToOffscreen() {} }
    const module = {
        FS: { mkdirTree() {}, writeFile() {} },
        callMain() { events.push("main"); },
        _engine_web_request_exit() { events.push("exit-request"); },
        PThread: { terminateAllThreads() { events.push("terminate"); } }
    };
    if (memory) module._engine_web_request_memory_snapshot = () => events.push("memory-request");
    const context = vm.createContext({ URL, URLSearchParams, DOMException, AbortController, SharedArrayBuffer,
        HTMLCanvasElement: Canvas, navigator: { gpu: { requestAdapter: async () => adapter } },
        isSecureContext: true, performance, location: { search: profiling ? "?engine-profile=1" : "" },
        crossOriginIsolated: true, console,
        fetch: async () => ({ ok: true, arrayBuffer: async () => new ArrayBuffer(8) }) });
    const exports = {
        "./engine.mjs": { default: options => {
            callbacks = options;
            return new Promise(resolve => { completeFactory = () => resolve(module); });
        } },
        "./catalog.mjs": { catalogCarriers: () => new Map() },
        "./application.mjs": { applicationName: "web-test" },
        "./storage/storage.mjs": {
            restore: async () => { events.push("restore"); },
            close: () => new Promise(resolve => {
                events.push("storage-close"); completeStorage = resolve;
            })
        }
    };
    const program = new vm.SourceTextModule(source, { context });
    await program.link(name => {
        if (name === "./profiling.mjs") return new vm.SourceTextModule(profilingSource, { context });
        const values = exports[name];
        return new vm.SyntheticModule(Object.keys(values), function() {
            for (const [key, value] of Object.entries(values)) this.setExport(key, value);
        }, { context });
    });
    await program.evaluate();
    return { api: program.namespace, events, Canvas, base: new URL("https://example.test/game/"),
        ready: () => callbacks.onEngineReady({ submissionId: "1", sceneId: "2" }),
        stopped: (code = 0) => callbacks.onEngineStopped(code, code ? "Device lost" : ""),
        factory: () => completeFactory(), storage: () => completeStorage(),
        callback: () => callbacks,
        onExit: (state, error) => events.push([state, error]) };
}

test("an unavailable adapter stops startup before loading Wasm and leaves the canvas unused", async () => {
    const f = await fixture(null);
    const canvas = new f.Canvas();
    const error = /WebGPU is unavailable/;
    await assert.rejects(f.api.start(canvas, f.base, f.onExit), error);
    assert.equal(f.callback(), undefined);
    assert.deepEqual(f.events, [["failed", "WebGPU is unavailable on this device or in this browser"]]);
    await assert.rejects(f.api.start(canvas, f.base, f.onExit), error);
});

test("readiness waits for a scene receipt; shutdown drains native and storage before workers", async () => {
    const f = await fixture(undefined, true);
    const canvas = new f.Canvas();
    const started = f.api.start(canvas, f.base, f.onExit);
    let ready = false;
    started.then(() => { ready = true; });
    await tick(); f.factory(); await tick();
    assert.deepEqual(f.events, ["restore", "main"]);
    assert.equal(ready, false);
    await assert.rejects(f.api.start(new f.Canvas(), f.base, f.onExit), { name: "InvalidStateError" });
    f.ready(); await started;
    await assert.rejects(f.api.memory_snapshot(), { name: "NotSupportedError" });
    const stopped = f.api.stop();
    assert.equal(f.api.stop(), stopped);
    await tick();
    assert.deepEqual(f.events, ["restore", "main", "exit-request"]);
    f.stopped(); await tick();
    assert.equal(f.events.includes("terminate"), false);
    f.storage(); await stopped;
    assert.deepEqual(f.events.slice(-3), ["storage-close", "terminate", ["stopped", ""]]);
    await assert.rejects(f.api.start(canvas, f.base, f.onExit), /fresh canvas/);
});

test("memory snapshot requests share a native frame and stop cancels outstanding reads", async () => {
    const f = await fixture(undefined, false, true);
    const started = f.api.start(new f.Canvas(), f.base, f.onExit);
    await tick(); f.factory(); await tick();
    const first = f.api.memory_snapshot();
    assert.equal(f.api.memory_snapshot(), first);
    assert.equal(f.events.filter(event => event === "memory-request").length, 1);
    const snapshot = { sequence: 3, linearBytes: 134217728, allocatorLiveBytes: 1024 };
    f.callback().onEngineMemorySnapshot(snapshot);
    assert.equal(await first, snapshot);
    f.ready(); await started;
    const next = f.api.memory_snapshot();
    const stopped = f.api.stop();
    await assert.rejects(next, { name: "AbortError" });
    f.callback().onEngineMemorySnapshot(snapshot);
    f.stopped(); await tick(); f.storage(); await stopped;
    await assert.rejects(f.api.memory_snapshot(), { name: "InvalidStateError" });
});

test("profiling reports failed startup and releases its timing entries", async () => {
    const seen = [];
    const observer = new PerformanceObserver(records => seen.push(...records.getEntries()));
    observer.observe({ entryTypes: ["measure"] });
    try {
        const f = await fixture(null, true);
        await assert.rejects(f.api.start(new f.Canvas(), f.base, f.onExit), /WebGPU is unavailable/);
        await tick();
        const prepare = "Engine.web.bootstrap.prepare";
        const shutdown = "Engine.web.bootstrap.shutdown";
        assert.ok(seen.some(entry => entry.name === prepare));
        assert.ok(seen.some(entry => entry.name === shutdown));
        assert.equal(performance.getEntriesByName(prepare).length, 0);
        assert.equal(performance.getEntriesByName(shutdown).length, 0);
    } finally {
        observer.disconnect();
    }
});

test("cancelling an unresolved factory waits for it and emits one terminal event", async () => {
    const f = await fixture();
    const controller = new AbortController();
    const started = f.api.start(new f.Canvas(), f.base, f.onExit, controller.signal);
    const rejected = assert.rejects(started, { name: "AbortError" });
    await tick(); controller.abort();
    const stopped = f.api.stop();
    await tick(); assert.deepEqual(f.events, []);
    f.factory(); await stopped; await rejected;
    assert.deepEqual(f.events, ["terminate", ["stopped", ""]]);
    f.ready(); f.stopped(); await tick();
    assert.equal(f.events.filter(Array.isArray).length, 1);
});

test("faults after readiness reach on_exit and repeated stop retains the failure", async () => {
    const f = await fixture();
    const started = f.api.start(new f.Canvas(), f.base, f.onExit);
    await tick(); f.factory(); await tick(); f.ready(); await started;
    f.stopped(1); await tick();
    const failed = f.api.stop();
    const rejected = assert.rejects(failed, /Device lost/);
    f.storage(); await rejected;
    assert.equal(f.api.stop(), failed);
    assert.deepEqual(f.events.at(-1), ["failed", "Device lost"]);
});

test("startup abort during native execution still performs native shutdown", async () => {
    const f = await fixture();
    const started = f.api.start(new f.Canvas(), f.base, f.onExit);
    const rejected = assert.rejects(started, { name: "AbortError" });
    await tick(); f.factory(); await tick();
    const stopped = f.api.stop();
    await tick(); f.ready();
    assert.equal(f.events.at(-1), "exit-request");
    f.stopped(); await tick(); f.storage(); await stopped; await rejected;
    assert.equal(f.events.filter(Array.isArray).length, 1);
});
