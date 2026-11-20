export function defaultBlendWidthForGrid(gridCount) {
    const count = Math.floor(Number(gridCount));
    return Number.isFinite(count) && count >= 2 ? 1 / (count - 1) : 0;
}
