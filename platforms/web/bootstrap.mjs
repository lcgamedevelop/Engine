import createEngineModule from "./engine.mjs";
import { catalogCarriers } from "./catalog.mjs";
import * as storage from "./storage/storage.mjs";
import { applicationName } from "./application.mjs";
import { profileStart, profileEnd } from "./profiling.mjs";

let current = null;
let generation = 0;
let terminalStop = Promise.resolve();
const transferred = new WeakSet();

function deferred() {
    const profileTime = profileStart();
    try {
        let resolve, reject;
        const promise = new Promise((yes, no) => {
            const profileTime = profileStart();
            try {
                resolve = yes;
                reject = no;
            } finally {
                profileEnd("Engine.web.bootstrap.bindDeferred", profileTime);
            }
        });
        return { promise, resolve, reject };
    } finally {
        profileEnd("Engine.web.bootstrap.deferred", profileTime);
    }
}
const cancelled = () => {
    const profileTime = profileStart();
    try {
        return new DOMException("Runtime startup was cancelled", "AbortError");
    } finally {
        profileEnd("Engine.web.bootstrap.cancelled", profileTime);
    }
};

function check(run) {
    const profileTime = profileStart();
    try {
        if (current !== run || run.cancelled) throw cancelled();
        if (run.fault) throw run.fault;
    } finally {
        profileEnd("Engine.web.bootstrap.check", profileTime);
    }
}

async function download(run, relative, minimum = 0) {
    const profileTime = profileStart();
    try {
        const response = await fetch(new URL(relative, run.base), { signal: run.network.signal });
        if (!response.ok) throw new Error("Cannot download " + relative + ": HTTP " + response.status);
        const bytes = new Uint8Array(await response.arrayBuffer());
        check(run);
        if (bytes.length < minimum) throw new Error("Incomplete resource carrier: " + relative);
        return bytes;
    } finally {
        profileEnd("Engine.web.bootstrap.download", profileTime);
    }
}

function write(module, path, bytes) {
    const profileTime = profileStart();
    try {
        module.FS.mkdirTree(path.slice(0, path.lastIndexOf("/")));
        module.FS.writeFile(path, bytes, { canOwn: true });
    } finally {
        profileEnd("Engine.web.bootstrap.write", profileTime);
    }
}

async function prepare(run) {
    const profileTime = profileStart();
    try {
        check(run);
        const adapterAvailable = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" })
            .then(adapter => {
                const profileTime = profileStart();
                try {
                    return adapter !== null;
                } finally {
                    profileEnd("Engine.web.bootstrap.adapterResult", profileTime);
                }
            });
        check(run);
        if (!adapterAvailable) throw new Error("WebGPU is unavailable on this device or in this browser");

        const moduleTime = profileStart();
        const module = await createEngineModule({ canvas: run.canvas, noInitialRun: true,
            locateFile: path => {
                const profileTime = profileStart();
                try {
                    return new URL(path, run.base).href;
                } finally {
                    profileEnd("Engine.web.bootstrap.locateFile", profileTime);
                }
            },
            print: text => {
                const profileTime = profileStart();
                try {
                    console.log(text);
                } finally {
                    profileEnd("Engine.web.bootstrap.print", profileTime);
                }
            },
            printErr: text => {
                const profileTime = profileStart();
                try {
                    console.error(text);
                } finally {
                    profileEnd("Engine.web.bootstrap.printError", profileTime);
                }
            },
            onEngineReady: receipt => {
                const profileTime = profileStart();
                try {
                    if (current !== run || run.cancelled || run.fault || run.phase !== "starting") return;
                    if (!receipt?.submissionId || !receipt?.sceneId) {
                        run.fault = new Error("Runtime returned an invalid first-frame receipt");
                        void finish(run).catch(() => {});
                        return;
                    }
                    run.phase = "running";
                    run.startSettled = true;
                    run.start.resolve();
                } finally {
                    profileEnd("Engine.web.bootstrap.onEngineReady", profileTime);
                }
            },
            onEngineStopped: (code, error) => {
                const profileTime = profileStart();
                try {
                    if (current !== run) return;
                    if (code && !run.fault) run.fault = new Error(error || "Native runtime failed");
                    run.nativeStopped.resolve();
                    void finish(run).catch(() => {});
                } finally {
                    profileEnd("Engine.web.bootstrap.onEngineStopped", profileTime);
                }
            },
            onEngineMemorySnapshot: snapshot => {
                const profileTime = profileStart();
                try {
                    if (current !== run || !run.memoryRequest) return;
                    const request = run.memoryRequest;
                    run.memoryRequest = null;
                    request.resolve(snapshot);
                } finally {
                    profileEnd("Engine.web.bootstrap.onEngineMemorySnapshot", profileTime);
                }
            },
            onAbort: error => {
                const profileTime = profileStart();
                try {
                    if (current !== run) return;
                    run.fault ||= new Error("WebAssembly runtime aborted: " + String(error));
                    run.nativeStopped.resolve();
                    void finish(run).catch(() => {});
                } finally {
                    profileEnd("Engine.web.bootstrap.onAbort", profileTime);
                }
            }
        });
        profileEnd("Engine.web.bootstrap.module", moduleTime);
        run.module = module;
        check(run);
        const [catalog, settings] = await Promise.all([
            download(run, "assets/catalog.bin", 8), download(run, "settings/runtime.json", 2)
        ]);
        const entries = [...catalogCarriers(catalog)];
        let next = 0;
        await Promise.all(Array.from({ length: Math.min(entries.length, 4) }, async () => {
            const profileTime = profileStart();
            try {
                for (;;) {
                    const entry = entries[next++];
                    if (!entry) return;
                    const [path, minimum] = entry;
                    const bytes = await download(run, "assets/" + path, minimum);
                    write(module, "/assets/" + path, bytes);
                }
            } finally {
                profileEnd("Engine.web.bootstrap.downloadCarriers", profileTime);
            }
        }));
        check(run);
        write(module, "/assets/catalog.bin", catalog);
        write(module, "/settings/runtime.json", settings);
        await storage.restore(module, "/persistent/" + applicationName);
        run.storageReady = true;
        check(run);
        transferred.add(run.canvas);
        run.enteredNative = true;
        const callMainTime = profileStart();
        try {
            module.callMain([]);
        } finally {
            profileEnd("Engine.web.bootstrap.callMain", callMainTime);
        }
    } finally {
        profileEnd("Engine.web.bootstrap.prepare", profileTime);
    }
}

function finish(run) {
    const profileTime = profileStart();
    try {
        if (run.stop) return run.stop;
        run.phase = "stopping";
        run.cancelled = true;
        run.memoryRequest?.reject(run.fault ||
            new DOMException("Runtime stopped before publishing a memory snapshot", "AbortError"));
        run.memoryRequest = null;
        run.network.abort();
        run.stop = (async () => {
            const profileTime = profileStart();
            try {
                try {
                    await run.preparation.catch(error => {
                        const profileTime = profileStart();
                        try {
                            if (error?.name !== "AbortError") run.fault ||= error;
                        } finally {
                            profileEnd("Engine.web.bootstrap.finishPreparationError", profileTime);
                        }
                    });
                    if (run.module && run.enteredNative) {
                        run.module._engine_web_request_exit();
                        await run.nativeStopped.promise;
                    }
                    if (run.storageReady) await storage.close();
                } catch (error) {
                    run.fault ||= error;
                } finally {
                    run.module?.PThread?.terminateAllThreads();
                    run.module = null;
                    run.signal?.removeEventListener("abort", run.abort);
                }
                if (!run.startSettled) {
                    run.startSettled = true;
                    run.start.reject(run.fault || cancelled());
                }
                run.phase = run.fault ? "failed" : "stopped";
                if (current === run) current = null;
                try { run.onExit(run.fault ? "failed" : "stopped", run.fault?.message || ""); }
                catch (error) { console.error("Runtime exit callback failed", error); }
                if (run.fault) throw run.fault;
            } finally {
                profileEnd("Engine.web.bootstrap.shutdown", profileTime);
            }
        })();
        terminalStop = run.stop;
        run.stop.catch(() => {});
        return run.stop;
    } finally {
        profileEnd("Engine.web.bootstrap.finish", profileTime);
    }
}

export function start(canvas, base_url, on_exit, signal) {
    const profileTime = profileStart();
    try {
        if (current) return Promise.reject(new DOMException("Runtime is already active", "InvalidStateError"));
        if (signal?.aborted) return Promise.reject(cancelled());
        if (!(canvas instanceof HTMLCanvasElement) || !(base_url instanceof URL) ||
            typeof on_exit !== "function" || transferred.has(canvas) ||
            (canvas.id && !/^[a-zA-Z][a-zA-Z0-9_-]*$/.test(canvas.id))) {
            return Promise.reject(new TypeError("Startup requires a fresh canvas, URL and exit callback"));
        }
        if (!/^https?:$/.test(base_url.protocol) || !base_url.pathname.endsWith("/") ||
            base_url.search || base_url.hash) {
            return Promise.reject(new TypeError("The runtime base URL must identify an HTTP directory"));
        }
        if (!globalThis.isSecureContext || !globalThis.crossOriginIsolated || !navigator.gpu ||
            typeof SharedArrayBuffer !== "function" || !canvas.transferControlToOffscreen) {
            return Promise.reject(new Error("WebGPU requires a secure, cross-origin isolated browser with OffscreenCanvas"));
        }
        if (!/^[a-z][a-z0-9_-]*$/.test(applicationName)) {
            return Promise.reject(new Error("The exported application identity is invalid"));
        }
        const run = { generation: ++generation, phase: "starting", canvas, base: base_url,
            onExit: on_exit, signal, network: new AbortController(), module: null,
            nativeStopped: deferred(), start: deferred(), startSettled: false,
            enteredNative: false, storageReady: false, cancelled: false, fault: null,
            memoryRequest: null, stop: null };
        if (!canvas.id) canvas.id = "engine-canvas-" + run.generation;
        current = run;
        terminalStop = Promise.resolve();
        run.abort = () => {
            const profileTime = profileStart();
            try {
                void finish(run).catch(() => {});
            } finally {
                profileEnd("Engine.web.bootstrap.signalAbort", profileTime);
            }
        };
        signal?.addEventListener("abort", run.abort, { once: true });
        run.preparation = Promise.resolve().then(() => {
            const profileTime = profileStart();
            try {
                return prepare(run);
            } finally {
                profileEnd("Engine.web.bootstrap.beginPreparation", profileTime);
            }
        });
        run.preparation.catch(error => {
            const profileTime = profileStart();
            try {
                if (error?.name !== "AbortError") run.fault ||= error;
                void finish(run).catch(() => {});
            } finally {
                profileEnd("Engine.web.bootstrap.handlePreparationError", profileTime);
            }
        });
        return run.start.promise;
    } finally {
        profileEnd("Engine.web.bootstrap.start", profileTime);
    }
}

export function memory_snapshot() {
    const profileTime = profileStart();
    try {
        const run = current;
        if (!run || !run.enteredNative || run.cancelled) {
            return Promise.reject(new DOMException("Runtime is not active", "InvalidStateError"));
        }
        if (!run.module?._engine_web_request_memory_snapshot) {
            return Promise.reject(new DOMException("Memory profiling is disabled", "NotSupportedError"));
        }
        if (run.memoryRequest) return run.memoryRequest.promise;
        const request = deferred();
        run.memoryRequest = request;
        try { run.module._engine_web_request_memory_snapshot(); }
        catch (error) {
            run.memoryRequest = null;
            request.reject(error);
        }
        return request.promise;
    } finally {
        profileEnd("Engine.web.bootstrap.memorySnapshot", profileTime);
    }
}

export function stop() {
    const profileTime = profileStart();
    try {
        return current ? finish(current) : terminalStop;
    } finally {
        profileEnd("Engine.web.bootstrap.stop", profileTime);
    }
}
