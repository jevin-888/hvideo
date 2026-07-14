const MAX_UNDO_STEPS = 20;

const undoStacks = {
    geometry: new Map(),
    mask: new Map(),
    blend: new Map()
};

function cloneRegion(region) {
    return JSON.parse(JSON.stringify(region));
}

function getUndoStack(type, regionId) {
    if (!undoStacks[type].has(regionId)) {
        undoStacks[type].set(regionId, []);
    }
    return undoStacks[type].get(regionId);
}

export function pushGeometryUndo(regionId, region) {
    if (!region) return;
    const stack = getUndoStack('geometry', regionId);
    stack.push(cloneRegion(region));
    if (stack.length > MAX_UNDO_STEPS) {
        stack.shift();
    }
}

export function pushMaskUndo(mask) {
    if (!mask) return;
    const stack = getUndoStack('mask', 'mask');
    stack.push(cloneRegion(mask));
    if (stack.length > MAX_UNDO_STEPS) {
        stack.shift();
    }
}

export function restoreGeometryUndo(regionId, region) {
    const stack = getUndoStack('geometry', regionId);
    if (stack.length === 0 || !region) return false;
    const snapshot = stack.pop();
    Object.keys(region).forEach((key) => delete region[key]);
    Object.assign(region, cloneRegion(snapshot));
    return true;
}

export function restoreMaskUndo(mask) {
    const stack = getUndoStack('mask', 'mask');
    if (stack.length === 0 || !mask) return false;
    const snapshot = stack.pop();
    Object.keys(mask).forEach((key) => delete mask[key]);
    Object.assign(mask, cloneRegion(snapshot));
    return true;
}

export function pushBlendUndo(regionId, region) {
    if (!region) return;
    const stack = getUndoStack('blend', regionId);
    stack.push(cloneRegion(region));
    if (stack.length > MAX_UNDO_STEPS) {
        stack.shift();
    }
}

export function restoreBlendUndo(regionId, region) {
    const stack = getUndoStack('blend', regionId);
    if (stack.length === 0 || !region) return false;
    const snapshot = stack.pop();
    Object.keys(region).forEach((key) => delete region[key]);
    Object.assign(region, cloneRegion(snapshot));
    return true;
}
