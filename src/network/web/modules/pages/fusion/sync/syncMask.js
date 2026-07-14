import { loadMaskState, updateMaskState, saveMask, setMaskGuideVisibility, resizeMaskGridOp, moveMaskOp, seedMaskFromGeometryState } from '../api.js?v=2.95';
import { ensureGlobalMask, getState } from '../actions.js?v=2.95';
import { hydrateMask } from '../mask/actions.js?v=2.95';

const pendingMaskMoves = new Map();
let maskMoveLoopRunning = false;
let maskMoveFlushTimer = null;
let maskMoveFlushPromise = Promise.resolve();

async function flushMaskMoveQueue() {
    if (maskMoveLoopRunning) return maskMoveFlushPromise;
    maskMoveLoopRunning = true;
    maskMoveFlushPromise = (async () => {
        while (pendingMaskMoves.size > 0) {
            const moves = Array.from(pendingMaskMoves.values());
            pendingMaskMoves.clear();
            for (const move of moves) {
                try {
                    const response = await moveMaskOp(move.op, move.du, move.dv, move.row, move.col);
                    const dragging = getState().interaction.dragging;
                    if (response && !(dragging.active && dragging.target === 'mask-point')) hydrateMask(response);
                } catch (error) {
                    console.warn('fusion mask move sync failed:', error);
                }
            }
        }
    })().finally(() => {
        maskMoveLoopRunning = false;
        if (pendingMaskMoves.size > 0) void flushMaskMoveQueue();
    });
    return maskMoveFlushPromise;
}

export async function flushPendingMaskMoves() {
    if (maskMoveFlushTimer) {
        clearTimeout(maskMoveFlushTimer);
        maskMoveFlushTimer = null;
    }
    await flushMaskMoveQueue();
}

export async function syncMask() {
    await flushPendingMaskMoves();
    const mask = ensureGlobalMask();
    return updateMaskState(mask);
}

export async function seedMaskFromGeometry() {
    await flushPendingMaskMoves();
    const response = await seedMaskFromGeometryState();
    if (response) hydrateMask(response);
    return response;
}

export async function persistMask() {
    await flushPendingMaskMoves();
    return saveMask();
}

export async function syncMaskGuideVisibility(showGrid) {
    return setMaskGuideVisibility(showGrid);
}

export async function resizeMaskGridByOp(op) {
    await flushPendingMaskMoves();
    const response = await resizeMaskGridOp(op);
    if (response) hydrateMask(response);
}

export async function syncMaskMoveOp(op, du, dv) {
    const mask = ensureGlobalMask();
    const row = mask.selected.row;
    const col = mask.selected.col;
    if ((op === 'point' || op === 'row' || op === 'col') && (row < 0 || col < 0)) return;
    const key = `${op}:${row}:${col}`;
    const current = pendingMaskMoves.get(key) || { op, du: 0, dv: 0, row, col };
    current.du += du;
    current.dv += dv;
    pendingMaskMoves.set(key, current);
    if (!maskMoveFlushTimer) {
        maskMoveFlushTimer = setTimeout(() => {
            maskMoveFlushTimer = null;
            void flushMaskMoveQueue();
        }, 16);
    }
}

export async function loadMask() {
    const response = await loadMaskState();
    if (response) {
        hydrateMask(response);
    }
    return response;
}
