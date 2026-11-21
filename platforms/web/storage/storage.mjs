import { profileStart, profileEnd } from "../profiling.mjs";

let active = null;

function sync(module, populate) {
    const profileTime = profileStart();
    try {
        return new Promise((resolve, reject) => {
            const profileTime = profileStart();
            try {
                module.FS.syncfs(populate, error => {
                    const profileTime = profileStart();
                    try {
                        return error ? reject(error) : resolve();
                    } finally {
                        profileEnd("Engine.web.storage.syncCompleted", profileTime);
                    }
                });
            } finally {
                profileEnd("Engine.web.storage.startSync", profileTime);
            }
        });
    } finally {
        profileEnd("Engine.web.storage.sync", profileTime);
    }
}

function plainFile(state, path) {
    const profileTime = profileStart();
    try {
        if (typeof path !== "string" || !path.startsWith(state.root + "/") ||
            path.includes("\\") || path.includes("\0") ||
            path.split("/").some(part => {
                const profileTime = profileStart();
                try {
                    return part === "." || part === "..";
                } finally {
                    profileEnd("Engine.web.storage.relativePathPart", profileTime);
                }
            })) {
            throw new Error("Persistent file path is invalid");
        }
    } finally {
        profileEnd("Engine.web.storage.plainFile", profileTime);
    }
}

export async function restore(module, root) {
    const profileTime = profileStart();
    try {
        if (active) throw new Error("Persistent storage is already open");
        const state = { module, root, tail: Promise.resolve(), closing: null, poison: "", open: false };
        active = state;
        try {
            module.FS.mkdirTree(root);
            module.FS.mount(module.IDBFS, { autoPersist: false }, root);
            await sync(module, true);
            if (!module._engine_web_storage_open()) throw new Error("Cannot open the storage proxy");
            state.open = true;
            module.webStorage = { publish };
        } catch (error) {
            module.IDBFS.quit();
            active = null;
            throw error;
        }
    } finally {
        profileEnd("Engine.web.storage.restore", profileTime);
    }
}

export function publish(temporary, target) {
    const profileTime = profileStart();
    try {
        const state = active;
        if (!state || !state.open || state.closing || state.poison) {
            return Promise.reject(new Error(state?.poison || "Persistent storage is closed"));
        }
        const operation = state.tail.then(async () => {
            const profileTime = profileStart();
            try {
                if (state.poison) throw new Error(state.poison);
                plainFile(state, temporary);
                plainFile(state, target);
                if (temporary === target || temporary.slice(0, temporary.lastIndexOf("/")) !==
                    target.slice(0, target.lastIndexOf("/"))) {
                    throw new Error("Persistent publishing requires separate files in one directory");
                }
                const fs = state.module.FS;
                const exists = fs.analyzePath(target).exists;
                const prior = exists ? { bytes: fs.readFile(target).slice(), info: fs.stat(target) } : null;
                fs.rename(temporary, target);
                try {
                    const time = Math.max(Date.now(), (prior?.info.mtime.getTime() || 0) + 1);
                    fs.utime(target, time, time);
                    await sync(state.module, false);
                } catch (error) {
                    try {
                        if (prior) {
                            fs.writeFile(target, prior.bytes);
                            fs.chmod(target, prior.info.mode);
                            fs.utime(target, prior.info.atime, prior.info.mtime);
                        } else if (fs.analyzePath(target).exists) {
                            fs.unlink(target);
                        }
                    } catch (rollback) {
                        state.poison = "Persistent rollback failed: " + String(rollback);
                    }
                    throw error;
                }
            } finally {
                profileEnd("Engine.web.storage.commit", profileTime);
            }
        });
        state.tail = operation.catch(() => {});
        return operation;
    } finally {
        profileEnd("Engine.web.storage.publish", profileTime);
    }
}

export function close() {
    const profileTime = profileStart();
    try {
        const state = active;
        if (!state) return Promise.resolve();
        if (state.closing) return state.closing;
        state.closing = (async () => {
            const profileTime = profileStart();
            try {
                try {
                    await state.tail;
                    if (state.poison) throw new Error(state.poison);
                    await sync(state.module, false);
                } finally {
                    state.open = false;
                    state.module.webStorage = null;
                    state.module._engine_web_storage_close();
                    state.module.IDBFS.quit();
                    active = null;
                }
            } finally {
                profileEnd("Engine.web.storage.closeTransaction", profileTime);
            }
        })();
        return state.closing;
    } finally {
        profileEnd("Engine.web.storage.close", profileTime);
    }
}
