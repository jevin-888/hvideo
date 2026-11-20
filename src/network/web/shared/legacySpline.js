const EPSILON = 1e-6;

function clamp01(value) {
    return Math.max(0, Math.min(1, Number(value) || 0));
}

export function lerpPoint(a, b, t) {
    return {
        u: a.u * (1 - t) + b.u * t,
        v: a.v * (1 - t) + b.v * t
    };
}

function linearCurvePoint(points, t) {
    const n = points.length;
    if (n <= 0) return { u: 0, v: 0 };
    if (n === 1) return points[0];
    const scaled = clamp01(t) * (n - 1);
    const index = Math.min(n - 2, Math.max(0, Math.floor(scaled)));
    return lerpPoint(points[index], points[index + 1], scaled - index);
}

export function legacySpline1D(input, t, xyScale = 1) {
    const n = input.length;
    if (n <= 0) return { u: 0, v: 0 };
    if (n === 1) return input[0];

    const linearAt = () => linearCurvePoint(input, t);
    if (n === 2) return linearAt();

    const scale = Math.abs(xyScale) > EPSILON ? xyScale : 1;
    const pts = input.map((point) => ({ u: point.u / scale, v: point.v }));
    const distance = (a, b) => Math.hypot(a.u - b.u, a.v - b.v);
    const safeDiv = (numerator, denominator) => Math.abs(denominator) > EPSILON ? numerator / denominator : 0;
    const L = n - 1;
    const N = n + 1;
    const M = 4;
    const curveTot = n + 2;
    const knot = new Array(n + 6).fill(0);
    knot[0] = 0;
    knot[1] = distance(pts[0], pts[1]);
    for (let i = 2; i < n; i += 1) {
        knot[i] = knot[i - 1] + distance(pts[i - 1], pts[i]);
    }
    if (knot[L] <= EPSILON) return input[0];

    const delta = new Array(n + 1).fill(0);
    for (let i = 1; i < n; i += 1) {
        delta[i] = knot[i] - knot[i - 1];
        if (delta[i] <= EPSILON) return linearAt();
    }

    const coeffA = new Array(n).fill(0);
    const coeffB = new Array(n).fill(0);
    const coeffC = new Array(n).fill(0);
    const coeffP = new Array(n).fill(0);
    const coeffQ = new Array(n).fill(0);
    const coeffR = new Array(n).fill(0);
    const coeffZ = new Array(n).fill(0);
    const coeffr = new Array(n).fill(0);

    coeffA[1] = safeDiv(delta[2] * delta[2], delta[1] + delta[2]);
    coeffB[0] = 1;
    coeffB[L] = 1;
    coeffB[1] = safeDiv(delta[2] * delta[1], delta[1] + delta[2]) +
        safeDiv(delta[1] * (delta[2] + delta[3]), delta[1] + delta[2] + delta[3]);
    coeffC[1] = safeDiv(delta[1] * delta[1], delta[1] + delta[2] + delta[3]);
    for (let i = 2; i <= L - 1; i += 1) {
        coeffA[i] = safeDiv(delta[i + 1] * delta[i + 1], delta[i - 1] + delta[i] + delta[i + 1]);
        coeffB[i] = safeDiv(delta[i + 1] * (delta[i - 1] + delta[i]), delta[i - 1] + delta[i] + delta[i + 1]) +
            safeDiv(delta[i] * (delta[i + 1] + delta[i + 2]), delta[i] + delta[i + 1] + delta[i + 2]);
        coeffC[i] = safeDiv(delta[i] * delta[i], delta[i] + delta[i + 1] + delta[i + 2]);
    }

    coeffQ[0] = coeffB[0];
    for (let i = 0; i < L; i += 1) coeffR[i] = coeffC[i];
    for (let i = 1; i <= L; i += 1) {
        if (Math.abs(coeffQ[i - 1]) <= EPSILON) return linearAt();
        coeffP[i] = coeffA[i] / coeffQ[i - 1];
        coeffQ[i] = coeffB[i] - coeffP[i] * coeffC[i - 1];
    }

    let beta = delta[1] + delta[2];
    if (Math.abs(beta) <= EPSILON) return linearAt();
    const startVec = {
        u: safeDiv(-(2 * delta[1] + delta[2]) * pts[0].u, delta[1] * beta) +
            safeDiv(beta * pts[1].u, delta[1] * delta[2]) -
            safeDiv(delta[1] * pts[2].u, delta[2] * beta),
        v: safeDiv(-(2 * delta[1] + delta[2]) * pts[0].v, delta[1] * beta) +
            safeDiv(beta * pts[1].v, delta[1] * delta[2]) -
            safeDiv(delta[1] * pts[2].v, delta[2] * beta)
    };

    beta = delta[L - 1] + delta[L];
    if (Math.abs(beta) <= EPSILON) return linearAt();
    const endVec = {
        u: safeDiv(delta[L] * pts[L - 2].u, delta[L - 1] * beta) -
            safeDiv(beta * pts[L - 1].u, delta[L - 1] * delta[L]) +
            safeDiv((2 * delta[L] + delta[L - 1]) * pts[L].u, beta * delta[L]),
        v: safeDiv(delta[L] * pts[L - 2].v, delta[L - 1] * beta) -
            safeDiv(beta * pts[L - 1].v, delta[L - 1] * delta[L]) +
            safeDiv((2 * delta[L] + delta[L - 1]) * pts[L].v, beta * delta[L])
    };

    const normalize = (vec) => {
        const len = Math.hypot(vec.u, vec.v);
        if (len <= EPSILON) return false;
        vec.u /= len;
        vec.v /= len;
        return true;
    };
    if (!normalize(startVec) || !normalize(endVec)) return linearAt();

    const curvePts = Array.from({ length: curveTot }, () => ({ u: 0, v: 0 }));
    curvePts[0] = pts[0];
    curvePts[1] = {
        u: pts[0].u + distance(pts[0], pts[1]) * startVec.u / 3,
        v: pts[0].v + distance(pts[0], pts[1]) * startVec.v / 3
    };
    curvePts[curveTot - 2] = {
        u: pts[L].u - distance(pts[L - 1], pts[L]) * endVec.u / 3,
        v: pts[L].v - distance(pts[L - 1], pts[L]) * endVec.v / 3
    };
    curvePts[N] = pts[L];

    const solveAxis = (key) => {
        coeffr[0] = curvePts[1][key];
        for (let i = 1; i < L; i += 1) coeffr[i] = (delta[i] + delta[i + 1]) * pts[i][key];
        coeffr[L] = curvePts[L + 1][key];
        coeffZ[0] = coeffr[0];
        for (let i = 1; i <= L; i += 1) coeffZ[i] = coeffr[i] - coeffP[i] * coeffZ[i - 1];
        if (Math.abs(coeffQ[L]) <= EPSILON) return false;
        curvePts[L + 1][key] = coeffZ[L] / coeffQ[L];
        for (let i = L - 1; i >= 0; i -= 1) {
            if (Math.abs(coeffQ[i]) <= EPSILON) return false;
            curvePts[i + 1][key] = (coeffZ[i] - coeffR[i] * curvePts[i + 2][key]) / coeffQ[i];
        }
        return true;
    };
    if (!solveAxis('u') || !solveAxis('v')) return linearAt();

    knot[N + M] = knot[N + M - 1] = knot[N + M - 2] = knot[N + M - 3] = knot[L];
    for (let i = L - 1; i > 0; i -= 1) knot[i + 3] = knot[i];
    knot[3] = knot[2] = knot[1] = knot[0] = 0;

    const scaledT = clamp01(t) * (n - 1);
    const seg = Math.min(n - 2, Math.max(0, Math.floor(scaledT)));
    const local = scaledT - seg;
    const i = 3 + seg;
    const segmentLength = knot[i + 1] - knot[i];
    if (segmentLength <= EPSILON) return linearAt();
    const kt = knot[i] + local * segmentLength;

    const N22 = safeDiv(knot[i + 1] - kt, knot[i + 1] - knot[i]);
    const N32 = safeDiv(kt - knot[i], knot[i + 1] - knot[i]);
    const N13 = safeDiv((knot[i + 1] - kt) * N22, knot[i + 1] - knot[i - 1]);
    const N23 = safeDiv((kt - knot[i - 1]) * N22, knot[i + 1] - knot[i - 1]) +
        safeDiv((knot[i + 2] - kt) * N32, knot[i + 2] - knot[i]);
    const N33 = safeDiv((kt - knot[i]) * N32, knot[i + 2] - knot[i]);
    const N04 = safeDiv((knot[i + 1] - kt) * N13, knot[i + 1] - knot[i - 2]);
    const N14 = safeDiv((kt - knot[i - 2]) * N13, knot[i + 1] - knot[i - 2]) +
        safeDiv((knot[i + 2] - kt) * N23, knot[i + 2] - knot[i - 1]);
    const N24 = safeDiv((kt - knot[i - 1]) * N23, knot[i + 2] - knot[i - 1]) +
        safeDiv((knot[i + 3] - kt) * N33, knot[i + 3] - knot[i]);
    const N34 = safeDiv((kt - knot[i]) * N33, knot[i + 3] - knot[i]);
    const denominator = N04 + N14 + N24 + N34;
    if (Math.abs(denominator) <= EPSILON) return linearAt();

    return {
        u: ((N04 * curvePts[i - 3].u + N14 * curvePts[i - 2].u + N24 * curvePts[i - 1].u + N34 * curvePts[i].u) / denominator) * scale,
        v: (N04 * curvePts[i - 3].v + N14 * curvePts[i - 2].v + N24 * curvePts[i - 1].v + N34 * curvePts[i].v) / denominator
    };
}
