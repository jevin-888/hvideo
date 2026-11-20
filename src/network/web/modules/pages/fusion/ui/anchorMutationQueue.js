function normalizeRegionIds(ids) {
    const source = Array.isArray(ids) ? ids : [ids];
    return Array.from(new Set(source
        .map((id) => Number(id))
        .filter((id) => Number.isFinite(id) && id > 0)));
}

function cellKey(regionId, side) {
    return `${Number(regionId)}:${side}`;
}

function targetKey(ids, side) {
    return `${side}|${ids.slice().sort((a, b) => a - b).join(',')}`;
}

/**
 * Serializes Anchor writes and drops queued values superseded by a newer edit.
 * The caller updates its local state before calling schedule().
 */
export function createAnchorMutationQueue({
    write,
    onCommit,
    onError,
    debounceMs = 80
} = {}) {
    if (typeof write !== 'function') {
        throw new TypeError('createAnchorMutationQueue requires a write function');
    }

    let queue = Promise.resolve();
    let version = 0;
    const latestByCell = new Map();
    const latestByTarget = new Map();
    const lastAppliedByCell = new Map();
    const timers = new Map();
    const pendingEntries = new Map();

    const isCurrent = (entry, regionId) => latestByCell.get(cellKey(regionId, entry.side)) === entry.version;
    const enqueue = (task) => {
        const run = queue.catch(() => {}).then(task);
        queue = run.catch(() => {});
        return run;
    };

    const dispatch = (entry) => {
        if (entry.dispatched) return entry.promise;
        entry.dispatched = true;
        entry.status = 'queued';
        entry.promise = enqueue(async () => {
            // Re-check after waiting for an earlier request. A newer edit may
            // have arrived while this entry was waiting in the serialized queue.
            const ids = entry.ids.filter((regionId) => isCurrent(entry, regionId));
            if (!ids.length) {
                entry.status = 'superseded';
                return;
            }

            entry.status = 'running';
            const responses = [];
            const failures = [];
            for (const regionId of ids) {
                if (!isCurrent(entry, regionId)) continue;
                const cell = cellKey(regionId, entry.side);
                const previous = lastAppliedByCell.get(cell);
                if (previous && Object.is(previous.value, entry.value)) {
                    // A different target set can still address the same cell.
                    // Reuse the already applied response instead of issuing the
                    // same backend mutation a second time.
                    responses.push({ regionId, response: previous.response, deduped: true });
                    continue;
                }
                try {
                    const response = await write(regionId, entry.side, entry.value);
                    if (response === null || response === undefined) {
                        failures.push({
                            regionId,
                            error: new Error('Anchor API returned an empty response')
                        });
                        continue;
                    }
                    // The shared API wrapper uses null for failed requests. Do
                    // not cache that sentinel as a successful mutation, or a
                    // later retry of the same value would be suppressed.
                    lastAppliedByCell.set(cell, { value: entry.value, response });
                    responses.push({ regionId, response });
                } catch (error) {
                    failures.push({ regionId, error });
                }
            }

            entry.status = failures.length ? 'failed' : 'succeeded';
            const currentResponses = responses.filter(({ regionId }) => isCurrent(entry, regionId));
            if (currentResponses.length && typeof onCommit === 'function') {
                await onCommit({
                    ids: currentResponses.map(({ regionId }) => regionId),
                    side: entry.side,
                    value: entry.value,
                    version: entry.version,
                    responses: currentResponses
                });
            }

            const currentFailures = failures.filter(({ regionId }) => isCurrent(entry, regionId));
            if (currentFailures.length && typeof onError === 'function') {
                await onError({
                    ids: currentFailures.map(({ regionId }) => regionId),
                    side: entry.side,
                    value: entry.value,
                    version: entry.version,
                    failures: currentFailures
                });
            }
        });
        return entry.promise;
    };

    const schedule = ({ ids: rawIds, side, value, final = false } = {}) => {
        const ids = normalizeRegionIds(rawIds);
        if (!ids.length || !side || !Number.isFinite(Number(value))) return null;

        const numericValue = Number(value);
        const key = targetKey(ids, side);
        const pending = pendingEntries.get(key);
        if (pending && Object.is(pending.value, numericValue)) {
            if (final) {
                const timer = timers.get(key);
                if (timer) clearTimeout(timer);
                timers.delete(key);
                pendingEntries.delete(key);
                dispatch(pending);
            }
            return pending.version;
        }

        const previous = latestByTarget.get(key);
        if (previous && Object.is(previous.value, numericValue) && previous.dispatched &&
            previous.status !== 'failed' && previous.ids.every((regionId) => isCurrent(previous, regionId))) {
            // The final pointer sample often arrives just after the debounce
            // timer dispatched the same value. Keep one mutation generation.
            return previous.version;
        }

        const entry = {
            ids,
            side,
            value: numericValue,
            version: ++version
        };
        ids.forEach((regionId) => latestByCell.set(cellKey(regionId, side), entry.version));
        latestByTarget.set(key, entry);
        const previousTimer = timers.get(key);
        if (previousTimer) clearTimeout(previousTimer);
        timers.delete(key);
        pendingEntries.delete(key);

        if (final) {
            dispatch(entry);
        } else {
            pendingEntries.set(key, entry);
            const timer = setTimeout(() => {
                timers.delete(key);
                pendingEntries.delete(key);
                dispatch(entry);
            }, Math.max(0, Number(debounceMs) || 0));
            timers.set(key, timer);
        }
        return entry.version;
    };

    const flush = () => {
        const entries = Array.from(pendingEntries.entries());
        timers.forEach((timer) => clearTimeout(timer));
        timers.clear();
        pendingEntries.clear();
        entries.forEach(([, entry]) => dispatch(entry));
        return queue;
    };

    return {
        schedule,
        flush,
        getVersion: () => version
    };
}
