#version 450

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;
    vec4 cropInfo;
    vec4 shapeInfo;
    vec4 userCrop;
    vec4 extEffect;  // x=特效类型 (以 float 表示), y=强度, z=时间, w=保留
} pc;

float rectMask(vec2 p, vec2 halfSize, float feather) {
    vec2 d = abs(p) - halfSize;
    float sdf = min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
    return 1.0 - smoothstep(0.0, feather, sdf);
}

vec3 logoShowLook(vec3 rgb, vec2 frameUv, float t, float amplitude,
                  vec3 customColor) {
    float pxX = max(fwidth(frameUv.x), 1e-6);
    float pxY = max(fwidth(frameUv.y), 1e-6);
    vec2 p = frameUv - vec2(0.5);
    p.x *= pxY / pxX;
    float r = length(p);

    float intro = smoothstep(0.08, 1.05, t);
    float startupFlash = exp(-t * 1.55);
    float idleGlint = pow(max(0.0, sin(t * 0.82)), 8.0);
    float drive = clamp(max(amplitude * 0.58,
                            startupFlash * 0.45 + idleGlint * 0.18),
                        0.0, 1.0);

    bool useSolid = customColor.r + customColor.g + customColor.b > 0.001;
    vec3 warm = useSolid ? mix(customColor, vec3(1.0), 0.25)
                         : vec3(0.86, 0.80, 0.66);
    vec3 cool = useSolid ? mix(customColor, vec3(0.32, 0.70, 1.00), 0.30)
                         : vec3(0.34, 0.62, 0.95);
    vec3 ember = useSolid ? mix(customColor, vec3(1.00, 0.42, 0.15), 0.18)
                          : vec3(1.00, 0.46, 0.16);
    vec3 metal = useSolid ? customColor * 0.18 : vec3(0.10, 0.11, 0.115);

    float aa = max(fwidth(r), 0.0014);
    float hLeft = rectMask(p - vec2(-0.062, 0.0), vec2(0.021, 0.122), aa * 3.0);
    float hRight = rectMask(p - vec2(0.062, 0.0), vec2(0.021, 0.122), aa * 3.0);
    float hBar = rectMask(p, vec2(0.086, 0.022), aa * 3.0);
    float hBody = max(hBar, max(hLeft, hRight));
    float hLeftInner = rectMask(p - vec2(-0.062, 0.0), vec2(0.014, 0.108), aa * 3.0);
    float hRightInner = rectMask(p - vec2(0.062, 0.0), vec2(0.014, 0.108), aa * 3.0);
    float hBarInner = rectMask(p, vec2(0.074, 0.013), aa * 3.0);
    float hInner = max(hBarInner, max(hLeftInner, hRightInner));
    float bevel = clamp(hBody - hInner, 0.0, 1.0);

    float diamond = abs(p.x) + abs(p.y) - 0.216;
    float diamondLine = (1.0 - smoothstep(0.004, 0.016, abs(diamond))) *
                        (1.0 - smoothstep(0.18, 0.32, r));
    float logoMask = max(hBody, diamondLine * 0.76) * intro;

    vec2 g1 = (p - vec2(-0.178, 0.082)) * vec2(54.0, 150.0);
    vec2 g2 = (p - vec2(0.006, 0.151)) * vec2(70.0, 165.0);
    vec2 g3 = (p - vec2(0.145, -0.094)) * vec2(62.0, 150.0);
    vec2 g4 = (p - vec2(-0.040, -0.150)) * vec2(68.0, 160.0);
    float glints = exp(-dot(g1, g1)) + 0.82 * exp(-dot(g2, g2)) +
                   0.72 * exp(-dot(g3, g3)) + 0.70 * exp(-dot(g4, g4));
    glints *= intro * (0.72 + drive * 0.62);

    float upperCut = exp(-abs(p.y - 0.132) * 210.0) *
                     (1.0 - smoothstep(0.02, 0.18, abs(p.x)));
    float lowerCut = exp(-abs(p.y + 0.136) * 190.0) *
                     (1.0 - smoothstep(0.02, 0.16, abs(p.x + 0.035)));
    float edgeCuts = (upperCut + lowerCut) * intro;

    float sweepX = fract(t * 0.115 + 0.08) * 1.44 - 0.22;
    float sweep = (1.0 - smoothstep(0.0, 0.075, abs(frameUv.x - sweepX))) *
                  exp(-abs(frameUv.y - 0.50) * 18.0) * intro;
    float blade = exp(-abs(p.y) * 35.0) *
                  (1.0 - smoothstep(0.05, 0.58, abs(p.x))) *
                  (0.08 + drive * 0.06) * intro;
    float ring = (1.0 - smoothstep(0.004, 0.018,
                                   abs(r - (0.310 + drive * 0.006)))) *
                 (1.0 - smoothstep(0.20, 0.55, r)) * intro;
    float centerBloom = exp(-r * 7.2) * (0.05 + startupFlash * 0.20 + drive * 0.08);
    float fineScan = (0.5 + 0.5 * sin(frameUv.y * 760.0 + t * 1.3)) *
                     (1.0 - smoothstep(0.08, 0.54, r)) * 0.012 * intro;

    // LOGO 演艺使用纯黑底，避免被底层视频色彩和运动干扰。
    vec3 base = vec3(0.0);

    vec3 light = metal * (hBody * 0.20 + fineScan) +
                 warm * (bevel * (0.80 + drive * 0.45) +
                         diamondLine * 0.34 + sweep * 0.28 +
                         edgeCuts * 0.32) +
                 cool * (ring * 0.12 + blade * 0.54 + centerBloom * 0.52 +
                         glints * 0.62) +
                 ember * (glints * 0.40 + edgeCuts * 0.22);

    return clamp(base + light * (0.66 + drive * 0.34) * intro +
                 logoMask * metal * 0.22, 0.0, 1.0);
}

const float OLD_FX_PI = 3.14159265359;

vec3 oldSampleTexture(sampler2D tex, vec2 uv) {
    return texture(tex, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
}

float oldHash(float n) {
    return fract(sin(n) * 43758.5453123);
}

vec2 oldNoise(vec2 p) {
    return fract(4.34 * sin(2.4 * (fract(4.1 * p) + p.yx)));
}

float oldHeartSdf(vec2 p, float s) {
    p /= max(s, 0.001);
    vec2 q = p;
    q.x *= 0.5 + 0.5 * q.y;
    q.y -= abs(p.x) * 0.63;
    return (length(q) - 0.7) * s;
}

float oldStep(float e0, float e1, float x) {
    return clamp((x - e0) / (e1 - e0), 0.0, 1.0);
}

vec3 oldHeartLayer(vec2 polar, float time, float fft) {
    float l = clamp(polar.y, 0.0, 1.0);
    float tiling = 1.0 / OLD_FX_PI * 8.0;
    polar.y -= time;
    vec2 polarID = floor(polar * tiling);

    polar.x = polar.x + polarID.y * 0.03;
    polar.x = mod(polar.x + OLD_FX_PI * 2.0, OLD_FX_PI * 2.0);
    polarID = floor(polar * tiling);

    polar = fract(polar * tiling);
    polar = polar * 2.0 - 1.0;

    vec2 n = oldNoise(polarID + vec2(0.1)) * 0.75 + 0.25;
    vec2 n2 = 2.0 * oldNoise(polarID) - 1.0;
    vec2 offset = (1.0 - n.y) * n2;
    float heartDist = oldHeartSdf(polar + offset, n.y * 0.6);
    float a = smoothstep(0.0, 0.25, n.x * n.x);
    float heartGlow = oldStep(0.0, -0.05, heartDist) * 0.5 * a +
                      oldStep(0.3, -0.4, heartDist) * 0.75;
    vec3 heartCol = vec3(smoothstep(0.0, -0.05, heartDist), 0.0, 0.0) * a +
                    heartGlow * vec3(0.9, 0.5, 0.7);
    vec3 bgCol = vec3(0.15 + l * 0.5, 0.0, 0.0);
    return bgCol * (0.5 + fft) +
           heartCol * step(0.45, oldNoise(polarID + vec2(0.4)).x);
}

vec3 oldHeartScene(vec2 frameUv, float t, float amplitude) {
    vec2 uv = (frameUv - 0.5) * 2.0;
    vec2 polar = vec2(atan(uv.y, uv.x), log(max(length(uv), 0.001)));
    float fft = clamp(amplitude, 0.0, 1.0);
    float speed = 0.666;
    vec3 h = max(max(oldHeartLayer(polar, (t - fft * 0.25) * speed, fft),
                     oldHeartLayer(polar, (t - fft * 0.20) * speed * 0.3 + 3.0, fft)),
                 oldHeartLayer(polar, t * speed * 0.2 + 5.0, fft));
    return clamp(h, 0.0, 1.0);
}

vec3 oldSoulEffect(vec3 rgb, sampler2D tex, vec2 uv, float amplitude) {
    float pulse = clamp(amplitude, 0.0, 1.0);
    float alpha = 0.4 * pulse;
    float scale = 1.0 + 0.8 * pulse;
    vec2 weakUv = vec2(0.5) + (uv - vec2(0.5)) / scale;
    vec3 weak = oldSampleTexture(tex, weakUv);
    return clamp(mix(rgb, weak, alpha) + weak * pulse * 0.08, 0.0, 1.0);
}

vec3 oldShakeEffect(vec3 rgb, sampler2D tex, vec2 uv, float amplitude) {
    float pulse = clamp(amplitude, 0.0, 1.0);
    float offset = 0.02 * pulse;
    float scale = 1.0 + 0.10 * pulse;
    vec2 scaledUv = vec2(0.5) + (uv - vec2(0.5)) / scale;
    vec3 maskR = oldSampleTexture(tex, scaledUv + vec2(offset));
    vec3 maskB = oldSampleTexture(tex, scaledUv - vec2(offset));
    vec3 mask = oldSampleTexture(tex, scaledUv);
    vec3 shaken = vec3(maskR.r, maskB.g, mask.b);
    return mix(rgb, shaken, pulse);
}

vec3 oldGlitchEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                     float t, float amplitude) {
    float pulse = clamp(amplitude * 1.25, 0.0, 1.0);
    float timeSlice = floor(t * 24.0);
    float row = floor(frameUv.y * 180.0);
    float jitter = oldHash(row + timeSlice * 17.0) * 2.0 - 1.0;
    bool needOffset = abs(jitter) < 0.06 * pulse;
    float textureX = uv.x + (needOffset ? jitter : (jitter * pulse * 0.006));

    float block = floor(frameUv.y * 18.0);
    float blockGate = step(0.72, oldHash(block + timeSlice * 11.3)) * pulse;
    textureX += (oldHash(block + timeSlice * 3.1) * 2.0 - 1.0) * 0.08 * blockGate;

    vec2 glitchUv = vec2(textureX, uv.y);
    vec3 maskR = oldSampleTexture(tex, glitchUv + vec2(0.010 * pulse, 0.0));
    vec3 maskB = oldSampleTexture(tex, glitchUv + vec2(-0.025 * pulse, 0.0));
    vec3 mask = oldSampleTexture(tex, glitchUv);
    vec3 glitch = vec3(maskR.r, mask.g, maskB.b);
    return mix(rgb, glitch, pulse);
}

vec3 oldHuanjueMask(sampler2D tex, vec2 uv, float time, float padding) {
    vec2 translation = vec2(sin(time * OLD_FX_PI), cos(time * OLD_FX_PI));
    return oldSampleTexture(tex, uv + padding * translation);
}

float oldHuanjueAlphaProgress(float currentTime, float hideTime,
                              float startTime) {
    float duration = 2.0;
    float time = mod(duration + currentTime - startTime, duration);
    return min(time, hideTime);
}

vec3 oldHuanjueEffect(vec3 rgb, sampler2D tex, vec2 uv, float t,
                      float amplitude) {
    float pulse = clamp(amplitude, 0.0, 1.0);
    float duration = 2.0;
    float time = mod(t, duration);
    float scale = 1.2;
    float padding = 0.5 * (1.0 - 1.0 / scale);
    vec2 scaledUv = vec2(0.5) + (uv - vec2(0.5)) / scale;
    float hideTime = 0.9;
    float timeGap = 0.2;
    float maxAlphaR = 0.5;
    float maxAlphaG = 0.05;
    float maxAlphaB = 0.05;
    vec3 mask = oldHuanjueMask(tex, scaledUv, time, padding);
    float alphaR = 1.0;
    float alphaG = 1.0;
    float alphaB = 1.0;
    vec3 result = vec3(0.0);
    for (int i = 0; i < 10; ++i) {
        float tmpTime = float(i) * timeGap;
        vec3 tmpMask = oldHuanjueMask(tex, scaledUv, tmpTime, padding);
        float tmpAlphaR =
            maxAlphaR - maxAlphaR *
            oldHuanjueAlphaProgress(time, hideTime, tmpTime) / hideTime;
        float tmpAlphaG =
            maxAlphaG - maxAlphaG *
            oldHuanjueAlphaProgress(time, hideTime, tmpTime) / hideTime;
        float tmpAlphaB =
            maxAlphaB - maxAlphaB *
            oldHuanjueAlphaProgress(time, hideTime, tmpTime) / hideTime;
        result += vec3(tmpMask.r * tmpAlphaR,
                       tmpMask.g * tmpAlphaG,
                       tmpMask.b * tmpAlphaB);
        alphaR -= tmpAlphaR;
        alphaG -= tmpAlphaG;
        alphaB -= tmpAlphaB;
    }
    result += mask * vec3(alphaR, alphaG, alphaB);
    return mix(rgb, clamp(result, 0.0, 1.0), pulse);
}

mat3 oldRotX(float a) {
    float c = cos(a);
    float s = sin(a);
    return mat3(1.0, 0.0, 0.0,
                0.0, c, -s,
                0.0, s, c);
}

mat3 oldRotY(float a) {
    float c = cos(a);
    float s = sin(a);
    return mat3(c, 0.0, s,
                0.0, 1.0, 0.0,
                -s, 0.0, c);
}

mat3 oldRotZ(float a) {
    float c = cos(a);
    float s = sin(a);
    return mat3(c, -s, 0.0,
                s,  c, 0.0,
                0.0, 0.0, 1.0);
}

mat3 oldFullRotate(vec3 r) {
    return oldRotX(r.x) * oldRotY(r.y) * oldRotZ(r.z);
}

float oldSmin(float a, float b, float k) {
    float res = exp(-k * a) + exp(-k * b);
    return -log(res) / k;
}

float oldSdBox(vec3 p, vec3 b) {
    vec3 d = abs(p) - b;
    return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, 0.0));
}

float oldSdCross(vec3 p, float w) {
    float inf = 30.0;
    float da = oldSdBox(p.xyz, vec3(inf, w, w));
    float db = oldSdBox(p.yzx, vec3(w, inf, w));
    float dc = oldSdBox(p.zxy, vec3(w, w, inf));
    return min(da, min(db, dc));
}

vec2 oldLightDistance(vec3 p, float t, float vol) {
    float w = 1.7 - length(p) / (20.0 + vol * 20.0);
    w += distance(p, vec3(0.0)) / 20.0 * vol;
    w *= 2.0 + sin(t) * 2.0 - 2.0 * vol + abs(sin(t));
    w = max(w, 0.05);
    float crossA = oldSdCross(p * oldFullRotate(vec3(t * 2.0, 0.0, t)), w);
    float crossB = oldSdCross(p * oldFullRotate(vec3(-OLD_FX_PI * 0.25 + t * 2.0,
                                                     0.0,
                                                     OLD_FX_PI * 0.25 + t)),
                              w);
    return vec2(oldSmin(crossA, crossB, 0.3), 1.0);
}

vec3 oldOpTwist(vec3 p, float r) {
    float c = cos(r * p.y + r);
    float s = sin(r * p.y + r);
    mat2 m = mat2(c, -s, s, c);
    return vec3(m * p.xz, p.y);
}

vec3 oldLightSourceScene(vec2 frameUv, float t, float amplitude) {
    float pxX = max(fwidth(frameUv.x), 1e-6);
    float pxY = max(fwidth(frameUv.y), 1e-6);
    vec2 vPos = frameUv - 0.5;
    vPos.x *= pxY / pxX;
    float vol = clamp(amplitude, 0.0, 1.0);
    float k = t / 1.6;
    float sk = sin(k);
    float ck = cos(k);

    vec3 vuv = vec3(0.0, sk, ck);
    vec3 prp = vec3(sk * 60.0, 1.0, ck * -34.0);
    vec3 vrp = vec3(10.0, sk * 10.0, 0.0);
    vec3 vpn = normalize(vrp - prp);
    vec3 u = normalize(cross(vuv, vpn));
    vec3 v = cross(vpn, u);
    vec3 scrCoord = prp + vpn + vPos.x * u + vPos.y * v;
    vec3 scp = normalize(scrCoord - prp);

    float glow = 0.0;
    float minDist = 100.0;
    float march = 2.0;
    vec2 d = vec2(0.1, 0.0);
    for (int i = 0; i < 5; ++i) {
        if (abs(d.x) < 0.001 || march > 70.0) {
            break;
        }
        march += max(d.x, 0.03);
        vec3 p = prp + scp * march;
        p = oldOpTwist(p, 0.08 * sk) * oldFullRotate(vec3(k * 1.2));
        d = oldLightDistance(p, t, vol);
        minDist = min(minDist, abs(d.x) * 1.5);
        glow = pow(1.0 / max(minDist, 0.02), 1.35);
    }

    vec3 viewDir = -normalize(scp);
    float az = atan(viewDir.z, viewDir.x);
    float bands = 0.5 + 0.5 * sin(az * 6.0 + t * 0.7);
    vec3 color = vec3(0.0, 0.0, 0.08) * 0.8;
    color -= vec3(mod(t, 5.45 / 8.0) * 0.1 + vol * 0.18);
    color += vec3(0.05, 0.08, 0.16) * (0.45 + bands * 0.55);
    color *= vec3(1.0, 0.25 + 0.75 * sin(vol), 0.8);
    color += pow(glow, 0.9) * vec3(3.0, 2.0, 1.0) * 0.45;

    vec2 q = frameUv;
    float vig = max(32.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.0);
    color *= 0.4 + 0.4 * pow(vig, 0.1);
    color += pow(max(color - 0.2, 0.0), vec3(1.4)) * 0.5;
    return clamp(color, 0.0, 1.0);
}

vec3 oldCubeEffect(vec3 rgb, sampler2D tex, vec2 frameUv, float t,
                   float amplitude) {
    float energy = clamp(amplitude, 0.0, 1.0);
    float pxX = max(fwidth(frameUv.x), 1e-6);
    float pxY = max(fwidth(frameUv.y), 1e-6);
    vec2 p = frameUv * 2.0 - 1.0;
    p.x *= pxY / pxX;

    float angle = t * (0.24 + energy * 1.05);
    mat3 rot = oldRotY(angle) * oldRotX(angle * 0.72 + t * 0.10);
    vec3 ro = vec3(0.0, 0.0, 3.6);
    vec3 rd = normalize(vec3(p * 1.25, -2.1));
    vec3 roL = transpose(rot) * ro;
    vec3 rdL = transpose(rot) * rd;
    vec3 safeRd = vec3(abs(rdL.x) < 1e-5 ? (rdL.x < 0.0 ? -1e-5 : 1e-5) : rdL.x,
                       abs(rdL.y) < 1e-5 ? (rdL.y < 0.0 ? -1e-5 : 1e-5) : rdL.y,
                       abs(rdL.z) < 1e-5 ? (rdL.z < 0.0 ? -1e-5 : 1e-5) : rdL.z);
    vec3 t0 = (-vec3(1.0) - roL) / safeRd;
    vec3 t1 = ( vec3(1.0) - roL) / safeRd;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar = min(min(tMax.x, tMax.y), tMax.z);
    vec3 background = vec3(0.0);
    if (tFar < max(tNear, 0.0)) {
        return background;
    }

    vec3 hit = roL + rdL * tNear;
    vec3 ah = abs(hit);
    vec2 faceUv;
    vec3 normalL;
    if (ah.z >= ah.x && ah.z >= ah.y) {
        faceUv = vec2(hit.x, -hit.y) * 0.5 + 0.5;
        normalL = vec3(0.0, 0.0, sign(hit.z));
    } else if (ah.x >= ah.y) {
        faceUv = vec2(-hit.z * sign(hit.x), -hit.y) * 0.5 + 0.5;
        normalL = vec3(sign(hit.x), 0.0, 0.0);
    } else {
        faceUv = vec2(hit.x, hit.z * sign(hit.y)) * 0.5 + 0.5;
        normalL = vec3(0.0, sign(hit.y), 0.0);
    }

    vec3 normalW = normalize(rot * normalL);
    float shade = 0.58 + 0.42 * max(dot(normalW, normalize(vec3(0.45, 0.65, 1.0))), 0.0);
    vec3 cube = oldSampleTexture(tex, faceUv) * shade;
    float cubeMix = 0.72 + energy * 0.28;
    return mix(background, cube, cubeMix);
}

// ─────────────────────────────────────────────────────────────
// 音频联动效果（effect 1-11）—— 与 effect_color.frag 一致
// DRM_PRIME 路径（RKMPP 零拷贝视频）和默认 RGBA 路径都走这个 shader
// ─────────────────────────────────────────────────────────────
// shapeD: signed distance to 当前 shape edge (negative=inside, positive=outside, +1.0=no shape)
// customColor: 描边/流光/边缘跑马颜色覆盖；vec3(0) 表示用默认彩虹
vec3 applyEdgeMarqueeEffect(vec3 rgb, vec2 frameUv, float t,
                            vec3 customColor, float outlineWidthPercent) {
    float pxX = max(fwidth(frameUv.x), 1e-6);
    float pxY = max(fwidth(frameUv.y), 1e-6);
    float widthPx = max(1.0, 1.0 / pxX);
    float heightPx = max(1.0, 1.0 / pxY);
    float minDimPx = min(widthPx, heightPx);
    float bandPx = clamp(minDimPx * (outlineWidthPercent * 0.01), 2.0, minDimPx * 0.22);
    float glowPx = max(2.0, bandPx * 1.60);

    float xPx = clamp(frameUv.x, 0.0, 1.0) * widthPx;
    float yPx = clamp(frameUv.y, 0.0, 1.0) * heightPx;
    float dxL = xPx;
    float dxR = widthPx - xPx;
    float dyT = yPx;
    float dyB = heightPx - yPx;
    float edgeDistPx = min(min(dxL, dxR), min(dyT, dyB));
    if (edgeDistPx > bandPx + glowPx * 1.75) return rgb;

    float perim = max(1.0, 2.0 * (widthPx + heightPx));
    float s;
    if (dyT <= dxL && dyT <= dxR && dyT <= dyB) {
        s = xPx / perim;
    } else if (dxR <= dxL && dxR <= dyB) {
        s = (widthPx + yPx) / perim;
    } else if (dyB <= dxL) {
        s = (widthPx + heightPx + (widthPx - xPx)) / perim;
    } else {
        s = (2.0 * widthPx + heightPx + (heightPx - yPx)) / perim;
    }
    s = fract(s);

    float segmentCount = clamp(floor(perim / 150.0), 8.0, 18.0);
    float cell = fract(s * segmentCount - fract(t) * segmentCount);
    float segmentBody = smoothstep(0.00, 0.055, cell) *
                        (1.0 - smoothstep(0.30, 0.45, cell));
    float segmentHead = 1.0 - smoothstep(0.00, 0.13, cell);
    float dash = max(segmentBody, segmentHead * 0.92);
    if (dash < 0.01) return rgb;

    float solid = 1.0 - smoothstep(bandPx - 1.25, bandPx + 1.25, edgeDistPx);
    float core = 1.0 - smoothstep(bandPx * 0.45, bandPx + 1.0, edgeDistPx);
    float bloom = exp(-max(edgeDistPx - bandPx, 0.0) / glowPx);
    float edgeMask = max(core, solid * 0.78 + bloom * 0.38);
    if (edgeMask < 0.01) return rgb;

    vec3 baseColor;
    bool useSolid = customColor.r + customColor.g + customColor.b > 0.001;
    if (useSolid) {
        baseColor = customColor;
    } else {
        float lane = floor(s * segmentCount);
        float hue = (s + lane * 0.075 + t * 0.20) * 6.2831853;
        baseColor = vec3(0.5 + 0.5 * sin(hue),
                         0.5 + 0.5 * sin(hue + 2.0944),
                         0.5 + 0.5 * sin(hue + 4.1888));
    }

    float solidMix = clamp(dash * solid * 0.92, 0.0, 0.92);
    vec3 result = mix(rgb, baseColor * 1.22, solidMix);
    result += baseColor * dash * edgeMask * (0.28 + 0.50 * bloom);
    return clamp(result, 0.0, 1.0);
}

vec3 hueColor(float hue) {
    float p = hue * 6.2831853;
    return vec3(0.5 + 0.5 * sin(p),
                0.5 + 0.5 * sin(p + 2.0944),
                0.5 + 0.5 * sin(p + 4.1888));
}

vec2 rotate2(vec2 p, float a) {
    float c = cos(a);
    float s = sin(a);
    return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

vec3 beatEchoEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                    float t, float amplitude) {
    float pulse = clamp(amplitude, 0.0, 1.0);
    if (pulse <= 0.002) return rgb;
    vec2 center = vec2(0.5);
    vec2 dir = frameUv - center;
    vec3 acc = rgb;
    float weight = 1.0;
    for (int i = 1; i <= 5; ++i) {
        float f = float(i);
        float spread = pulse * f * 0.034;
        vec2 offset = rotate2(dir, sin(t * 1.7 + f) * 0.055 * pulse);
        vec2 p = uv + offset * spread +
                 vec2(sin(t * 2.3 + f * 1.9),
                      cos(t * 1.9 + f * 1.4)) * 0.010 * pulse * f;
        float w = (0.36 / f) * pulse;
        acc += oldSampleTexture(tex, p) * w;
        weight += w;
    }
    vec3 echo = acc / weight;
    vec3 tint = hueColor(fract(t * 0.12 + pulse * 0.17));
    float edge = smoothstep(0.18, 0.72, length(dir) * 1.45);
    echo += tint * pulse * edge * 0.22;
    return clamp(mix(rgb, echo, 0.35 + pulse * 0.50), 0.0, 1.0);
}

vec3 neonOutlineEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                       float t, float amplitude, vec3 customColor) {
    float pulse = clamp(amplitude, 0.0, 1.0);
    vec2 px = vec2(max(fwidth(uv.x), 1e-5), max(fwidth(uv.y), 1e-5));
    vec3 cL = oldSampleTexture(tex, uv - vec2(px.x * 2.2, 0.0));
    vec3 cR = oldSampleTexture(tex, uv + vec2(px.x * 2.2, 0.0));
    vec3 cU = oldSampleTexture(tex, uv - vec2(0.0, px.y * 2.2));
    vec3 cD = oldSampleTexture(tex, uv + vec2(0.0, px.y * 2.2));
    float lL = dot(cL, vec3(0.299, 0.587, 0.114));
    float lR = dot(cR, vec3(0.299, 0.587, 0.114));
    float lU = dot(cU, vec3(0.299, 0.587, 0.114));
    float lD = dot(cD, vec3(0.299, 0.587, 0.114));
    float edge = clamp(length(vec2(lR - lL, lD - lU)) * 4.6, 0.0, 1.0);
    float glow = smoothstep(0.05, 0.42, edge);
    bool useSolid = customColor.r + customColor.g + customColor.b > 0.001;
    vec3 neon = useSolid ? customColor : hueColor(fract(frameUv.x * 0.35 + frameUv.y * 0.55 + t * 0.065));
    float brightness = 0.52 + pulse * 1.10;
    vec3 result = rgb * (1.0 + glow * 0.10);
    result = mix(result, neon * (1.05 + pulse * 0.55), glow * (0.34 + pulse * 0.42));
    result += neon * pow(edge, 1.7) * brightness;
    return clamp(result, 0.0, 1.0);
}

vec3 liquidGlassEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                       float t, float amplitude) {
    float drive = clamp(amplitude, 0.0, 1.0);
    vec2 p = frameUv - vec2(0.5);
    float r = length(p);
    float wave = sin((p.x * 8.5 + t * 0.70) + sin(p.y * 9.0 - t * 0.42)) +
                 sin((p.y * 7.5 - t * 0.55) + r * 9.0);
    vec2 normal = vec2(
        sin(p.y * 15.0 + t * 0.82) + 0.6 * cos(r * 18.0 - t * 0.56),
        cos(p.x * 13.0 - t * 0.74) + 0.6 * sin(r * 16.0 + t * 0.48));
    float amount = 0.006 + drive * 0.024;
    vec2 refractUv = uv + normal * amount + p * wave * amount * 0.22;
    vec3 glass = oldSampleTexture(tex, refractUv);
    vec3 ca;
    ca.r = oldSampleTexture(tex, refractUv + normal * amount * 0.55).r;
    ca.g = glass.g;
    ca.b = oldSampleTexture(tex, refractUv - normal * amount * 0.55).b;
    float shine = pow(max(0.0, sin(wave * 1.4 + t * 0.9)), 8.0) * (0.10 + drive * 0.35);
    return clamp(mix(rgb, ca + vec3(shine), 0.26 + drive * 0.46), 0.0, 1.0);
}

vec2 kaleidoscopeUv(vec2 frameUv, float t, float amplitude) {
    vec2 p = frameUv - vec2(0.5);
    float r = length(p);
    float a = atan(p.y, p.x) + t * (0.05 + amplitude * 0.18);
    float slices = mix(6.0, 10.0, smoothstep(0.35, 0.95, amplitude));
    float sector = 6.2831853 / slices;
    a = mod(a, sector);
    a = abs(a - sector * 0.5);
    vec2 q = vec2(cos(a), sin(a)) * r;
    return clamp(q + vec2(0.5), vec2(0.0), vec2(1.0));
}

vec3 kaleidoscopeEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                        float t, float amplitude) {
    vec2 kUv = kaleidoscopeUv(frameUv, t, amplitude);
    vec3 k = oldSampleTexture(tex, kUv);
    float vignette = smoothstep(0.86, 0.18, length(frameUv - vec2(0.5)));
    vec3 tint = hueColor(fract(t * 0.035 + amplitude * 0.12));
    k = mix(k, k * tint * 1.18, 0.12 + amplitude * 0.20);
    return clamp(mix(rgb, k, 0.58 + amplitude * 0.34) * (0.72 + vignette * 0.35), 0.0, 1.0);
}

vec3 beatShapeSplitEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                          float t, float amplitude, vec3 customColor,
                          bool colorMode) {
    float pulse = clamp(amplitude, 0.0, 1.0);
    if (pulse <= 0.002) return rgb;
    vec2 p = frameUv - vec2(0.5);
    float mode = floor(mod(t * 3.0, 4.0));
    float mask;
    if (mode < 1.0) {
        float stripes = fract((frameUv.x + frameUv.y * 0.35) * 7.0 + t * 0.22);
        mask = step(stripes, 0.52);
    } else if (mode < 2.0) {
        float ang = atan(p.y, p.x);
        mask = step(0.0, sin(ang * 6.0 + t * 0.7));
    } else if (mode < 3.0) {
        vec2 grid = fract(frameUv * vec2(4.0, 3.0) + vec2(t * 0.12, 0.0)) - 0.5;
        mask = step(max(abs(grid.x), abs(grid.y)), 0.32);
    } else {
        float star = abs(p.x) + abs(p.y);
        mask = step(fract(star * 5.5 - t * 0.25), 0.50);
    }
    vec2 push = normalize(p + vec2(0.001)) * (0.035 + pulse * 0.090);
    push += vec2(sin(t * 2.1), cos(t * 1.7)) * 0.012 * pulse;
    vec2 splitUv = uv + push * mix(-1.0, 1.0, mask);
    vec3 split = oldSampleTexture(tex, splitUv);
    vec3 tint = hueColor(fract(mask * 0.33 + t * 0.10));
    if (colorMode) {
        bool useSolid = customColor.r + customColor.g + customColor.b > 0.001;
        vec3 barColor = useSolid
            ? customColor
            : hueColor(fract(t * 0.11 + frameUv.x * 0.42 + frameUv.y * 0.27 + mask * 0.22));
        float edge = 0.0;
        edge = max(edge, 1.0 - smoothstep(0.00, 0.030, abs(fract((frameUv.x + frameUv.y * 0.35) * 7.0 + t * 0.22) - 0.52)));
        edge = max(edge, 1.0 - smoothstep(0.00, 0.040, abs(sin(atan(p.y, p.x) * 6.0 + t * 0.7))));
        vec2 gridEdge = abs(fract(frameUv * vec2(4.0, 3.0) + vec2(t * 0.12, 0.0)) - 0.5);
        edge = max(edge, 1.0 - smoothstep(0.00, 0.045, abs(max(gridEdge.x, gridEdge.y) - 0.32)));
        edge = max(edge, 1.0 - smoothstep(0.00, 0.035, abs(fract((abs(p.x) + abs(p.y)) * 5.5 - t * 0.25) - 0.50)));
        float luma = dot(split, vec3(0.299, 0.587, 0.114));
        float videoTexture = smoothstep(0.10, 0.92, luma);
        float bar = clamp(mask * (0.66 + videoTexture * 0.34) + edge * 0.45, 0.0, 1.0);
        vec3 background = vec3(0.0);
        vec3 litBar = barColor * (0.88 + pulse * 0.55 + edge * 0.28);
        litBar += split * barColor * (0.18 + pulse * 0.20);
        return clamp(mix(background, litBar, bar), 0.0, 1.0);
    }
    split = mix(split, split * tint * 1.35, pulse * 0.30);
    return clamp(mix(rgb, split, 0.45 + pulse * 0.48), 0.0, 1.0);
}

float mosaicDigitMask(vec2 p, float digit) {
    vec2 q = (p - vec2(0.5)) * vec2(1.15, 1.45);
    float x = q.x;
    float y = q.y;
    float left = smoothstep(0.25, 0.16, abs(x + 0.28));
    float right = smoothstep(0.25, 0.16, abs(x - 0.28));
    float top = smoothstep(0.18, 0.09, abs(y + 0.42));
    float mid = smoothstep(0.18, 0.09, abs(y));
    float bottom = smoothstep(0.18, 0.09, abs(y - 0.42));
    float upper = 1.0 - smoothstep(-0.02, 0.12, y);
    float lower = smoothstep(-0.12, 0.02, y);
    float verticalExtent = smoothstep(0.58, 0.38, abs(y));
    float horizontalExtent = smoothstep(0.50, 0.30, abs(x));
    float mask = 0.0;
    if (digit < 0.5) {
        float ring = smoothstep(0.58, 0.46, length(q * vec2(0.78, 1.0))) *
                     (1.0 - smoothstep(0.32, 0.42, length(q * vec2(0.78, 1.0))));
        mask = ring;
    } else if (digit < 1.5) {
        mask = right * verticalExtent;
    } else if (digit < 2.5) {
        mask = max(max(top * horizontalExtent, mid * horizontalExtent),
                   max(bottom * horizontalExtent, max(right * upper, left * lower) * verticalExtent));
    } else if (digit < 3.5) {
        mask = max(max(top * horizontalExtent, mid * horizontalExtent),
                   max(bottom * horizontalExtent, right * verticalExtent));
    } else if (digit < 4.5) {
        mask = max(max(left * upper, right * verticalExtent) * verticalExtent,
                   mid * horizontalExtent);
    } else if (digit < 5.5) {
        mask = max(max(top * horizontalExtent, mid * horizontalExtent),
                   max(bottom * horizontalExtent, max(left * upper, right * lower) * verticalExtent));
    } else if (digit < 6.5) {
        mask = max(max(top * horizontalExtent, mid * horizontalExtent),
                   max(bottom * horizontalExtent, max(left, right * lower) * verticalExtent));
    } else if (digit < 7.5) {
        mask = max(top * horizontalExtent, right * verticalExtent);
    } else if (digit < 8.5) {
        mask = max(max(top * horizontalExtent, mid * horizontalExtent),
                   max(bottom * horizontalExtent, max(left, right) * verticalExtent));
    } else {
        mask = max(max(top * horizontalExtent, mid * horizontalExtent),
                   max(bottom * horizontalExtent, max(left * upper, right) * verticalExtent));
    }
    return clamp(mask, 0.0, 1.0);
}

float stitchHash(float n) {
    return fract(sin(n * 12.9898 + 37.719) * 43758.5453);
}

float stitchRoundRectMask(vec2 p, vec2 halfSize, float radius) {
    float feather = max(max(fwidth(p.x), fwidth(p.y)) * 1.5, 0.0015);
    vec2 q = abs(p) - halfSize + vec2(radius);
    float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    return 1.0 - smoothstep(0.0, feather, d);
}

vec2 shapeMosaicStitchSourceUv(vec2 frameUv, float t,
                               out float pieceMask) {
    pieceMask = 0.0;

    const int cols = 4;
    float laneWidth = 0.242;
    float halfW = laneWidth * 0.5;
    float minHalfH = halfW;
    float maxHalfH = 0.64;
    float stepValue = clamp(t, 0.0, 4.999);
    float stepIndex = floor(stepValue);
    float stepProgress = smoothstep(0.0, 1.0, fract(stepValue));

    for (int x = 0; x < cols; ++x) {
        float fi = float(x);
        float centerX = (fi + 0.5) / float(cols);
        float rectStep = fi + 1.0;
        float visible = 1.0;
        float grow = 0.0;
        if (stepIndex > rectStep) {
            grow = 1.0;
        } else if (abs(stepIndex - rectStep) < 0.5) {
            grow = stepProgress;
        }

        float halfH = mix(minHalfH, maxHalfH, grow);
        vec2 p = frameUv - vec2(centerX, 0.5);
        float radius = halfW;
        float mask = stitchRoundRectMask(p, vec2(halfW, halfH), radius) *
                     visible;
        if (mask > pieceMask) {
            pieceMask = mask;
        }
    }
    return frameUv;
}

vec3 shapeMosaicStitchEffect(vec3 rgb, sampler2D tex, vec2 uv, vec2 frameUv,
                             float t, float amplitude, vec3 customColor,
                             bool colorMode) {
    return rgb;
}

vec3 applyColorEffect(vec3 rgb, int effectType, float amplitude, vec2 uv, vec2 frameUv,
                      float t, sampler2D tex, float shapeD, vec3 customColor,
                      float outlineWidthPercent, bool colorMode) {
    if (effectType == 0) return rgb;
    if (effectType == 1) return mix(rgb, vec3(1.0), amplitude);
    if (effectType == 2) {
        float blend = min(1.0, amplitude * 2.5);
        return mix(rgb, vec3(0.0), blend);
    }
    if (effectType >= 3 && effectType <= 5) {
        float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
        vec3 target = (effectType == 3) ? vec3(1.0, 0.0, 0.0)
                    : (effectType == 4) ? vec3(0.0, 1.0, 0.0)
                                        : vec3(0.0, 0.0, 1.0);
        return mix(rgb, vec3(luma) * target * 1.5, amplitude);
    }
    if (effectType == 6) {
        // 黑条扫描：循环左右弹跳，速度跟音频包络
        //   amplitude=0（静音）→ 慢速来回（~6s 一次往返）
        //   amplitude=1（鼓点）→ 快速来回（~1.5s 一次往返）
        float speed = 0.15 + amplitude * 0.55;
        float phase = fract(t * speed);                    // 0..1 循环
        float tri = abs(phase * 2.0 - 1.0);                // 0→1→0 三角波（匀速）
        float barPos = mix(-0.20, 1.20, tri);              // 留 20% 屏外余量
        float d = abs(frameUv.x - barPos);
        float mask = smoothstep(0.32, 0.10, d);            // 宽度 ~32%，更醒目
        return mix(rgb, vec3(0.0), mask);
    }
    if (effectType == 7) {
        // 中心散开
        vec2 c = uv - vec2(0.5);
        float dist = length(c) * 1.414;
        float radius = 1.0 - amplitude * 0.85;
        float mask = smoothstep(radius, radius - 0.1, dist);
        return rgb * mask;
    }
    if (effectType == 8) {
        // LED 灯带描边：按图层矩形像素计算，边框实色覆盖素材，角落连续不断裂。
        float pxX = max(fwidth(frameUv.x), 1e-6);
        float pxY = max(fwidth(frameUv.y), 1e-6);
        float minDimPx = min(1.0 / pxX, 1.0 / pxY);
        float bandPx = clamp(minDimPx * (outlineWidthPercent * 0.01), 2.0, minDimPx * 0.25);
        float dxPx = min(frameUv.x, 1.0 - frameUv.x) / pxX;
        float dyPx = min(frameUv.y, 1.0 - frameUv.y) / pxY;
        float edgeDistPx = min(dxPx, dyPx);
        float dist = max(edgeDistPx - bandPx, 0.0);

        float beatDip = amplitude * amplitude;
        float aaPx = 1.25;
        float solid = 1.0 - smoothstep(bandPx - aaPx, bandPx + aaPx, edgeDistPx);
        float glowW = max(1.0, bandPx * 0.82);
        float bloom = exp(-dist / glowW);
        if (solid + bloom < 0.01) return rgb;

        // 颜色：固定色或彩虹（极慢，绕一圈 ~30s）
        vec2 framePx = vec2(frameUv.x / pxX, frameUv.y / pxY);
        vec2 frameSizePx = vec2(1.0 / pxX, 1.0 / pxY);
        vec2 cc = framePx - frameSizePx * 0.5;
        float ang = atan(cc.y, cc.x);
        float s = fract(ang / 6.2831853 + 0.5);
        bool useSolid = customColor.r + customColor.g + customColor.b > 0.001;
        vec3 baseColor;
        if (useSolid) {
            baseColor = customColor;
        } else {
            float phase = (s + t * 0.033) * 6.2831853;
            baseColor = vec3(
                0.5 + 0.5 * sin(phase),
                0.5 + 0.5 * sin(phase + 2.0944),
                0.5 + 0.5 * sin(phase + 4.1888)
            );
        }

        // 常驻描边：鼓点瞬间短暂压暗，随后随脉冲衰减自然回亮。
        float flowMod = 0.74 + 0.26 * sin((s - t * 0.05) * 12.566371);
        flowMod = mix(0.84, flowMod, 0.35);
        float beatGate = mix(1.0, 0.18, beatDip);
        float solidBrightness = 1.08 * beatGate;
        float glowBrightness = 0.95 * flowMod * beatGate;

        vec3 result = mix(rgb, baseColor * solidBrightness, solid);
        result += baseColor * bloom * (1.0 - solid) * glowBrightness;
        return result;
    }
    if (effectType == 9) {
        return mix(rgb, vec3(1.0) - rgb, amplitude);
    }
    if (effectType == 10) {
        float line = 0.5 + 0.5 * sin(uv.y * 400.0);
        float dim = 1.0 - amplitude * 0.6 * (1.0 - line);
        return rgb * dim;
    }
    if (effectType == 12) {
        // 完整内嵌灯框（4 条边） + 亮段沿周长流动，角点使用连续周长坐标。
        float pxX = max(fwidth(frameUv.x), 1e-6);
        float pxY = max(fwidth(frameUv.y), 1e-6);
        float widthPx = 1.0 / pxX;
        float heightPx = 1.0 / pxY;
        float minDimPx = min(widthPx, heightPx);
        float bandPx = clamp(minDimPx * (outlineWidthPercent * 0.01), 2.0, minDimPx * 0.25);
        float corePx  = max(0.75, bandPx * 0.26);
        float bloomPx = max(2.0, bandPx * 1.25);

        float xPx = frameUv.x * widthPx;
        float yPx = frameUv.y * heightPx;
        float dxL = xPx;
        float dxR = widthPx - xPx;
        float dyT = yPx;
        float dyB = heightPx - yPx;
        float edgeDistPx = min(min(dxL, dxR), min(dyT, dyB));
        float dist = max(edgeDistPx - bandPx, 0.0);
        float perim = max(1.0, 2.0 * (widthPx + heightPx));
        float s;
        if (dyT <= dxL && dyT <= dxR && dyT <= dyB) {
            s = xPx / perim;
        } else if (dxR <= dxL && dxR <= dyB) {
            s = (widthPx + yPx) / perim;
        } else if (dyB <= dxL) {
            s = (widthPx + heightPx + (widthPx - xPx)) / perim;
        } else {
            s = (2.0 * widthPx + heightPx + (heightPx - yPx)) / perim;
        }
        s = fract(s);
        float core  = exp(-dist / corePx);
        float bloom = exp(-dist / bloomPx);
        float solid = 1.0 - smoothstep(bandPx - 1.25, bandPx + 1.25, edgeDistPx);
        if (solid + core + bloom < 0.01) return rgb;
        // C++ 端传入连续相位；节拍只改变推进速度，不改变显隐。
        float head = fract(t);
        float dS = fract(head - s);          // 0 在头部，沿运动反方向（拖尾）递增
        float headGlow = exp(-dS / 0.028);   // 小范围亮头
        float tailGlow = (1.0 - smoothstep(0.04, 0.62, dS)) * 0.72;
        float seg = max(headGlow * 0.82, tailGlow);
        if (seg < 0.005) return rgb;
        vec3 baseColor;
        bool useSolid12 = customColor.r + customColor.g + customColor.b > 0.001;
        if (useSolid12) {
            baseColor = customColor;
        } else {
            float hue = t * 0.035 * 6.2831853;
            baseColor = vec3(0.5 + 0.5 * sin(hue),
                             0.5 + 0.5 * sin(hue + 2.0944),
                             0.5 + 0.5 * sin(hue + 4.1888));
        }
        vec3 result = mix(rgb, baseColor * 1.18, solid * seg);
        result += baseColor * core  * (1.0 - solid) * (0.75 * headGlow + 0.35 * tailGlow);
        result += baseColor * bloom * (1.0 - solid) * (0.20 * headGlow + 0.95 * tailGlow);
        return result;
    }
    if (effectType == 34) {
        return applyEdgeMarqueeEffect(rgb, frameUv, t, customColor,
                                      outlineWidthPercent);
    }
    if (effectType == 13) {
        // 全黑中间对开（帘幕展开）：
        //   amplitude=1（鼓点峰值）→ 整屏黑
        //   amplitude 衰减 → 从中心向两边打开，露出视频
        //   amplitude=0 → 完全打开（无黑遮罩）
        float gapHalfWidth = (1.0 - amplitude) * 0.55;
        float dx = abs(uv.x - 0.5);
        float revealMask = smoothstep(gapHalfWidth - 0.02,
                                       gapHalfWidth + 0.02, dx);
        return mix(rgb, vec3(0.0), revealMask);
    }
    if (effectType == 16) {
        // 色彩扫光：彩色柔光带横向掠过画面，鼓点增强亮度与速度。
        float pulse = amplitude * amplitude;
        float speed = 0.12 + pulse * 0.42;
        float head = fract(t * speed);
        float d = abs(fract(frameUv.x - head + 0.5) - 0.5);
        float core = smoothstep(0.20, 0.00, d);
        float halo = smoothstep(0.42, 0.00, d) * 0.55;
        float vertical = 0.78 + 0.22 * sin((frameUv.y + t * 0.08) * 6.2831853);
        float hue = head + frameUv.y * 0.22 + t * 0.04;
        vec3 sweepColor = vec3(
            0.5 + 0.5 * sin(hue * 6.2831853),
            0.5 + 0.5 * sin(hue * 6.2831853 + 2.0944),
            0.5 + 0.5 * sin(hue * 6.2831853 + 4.1888)
        );
        float strength = (core * (0.65 + pulse * 1.25) + halo * (0.25 + pulse * 0.55)) * vertical;
        vec3 lifted = rgb + sweepColor * strength;
        vec3 tinted = mix(rgb, lifted, clamp(0.45 + pulse * 0.45, 0.0, 0.95));
        return clamp(tinted, 0.0, 1.0);
    }
    if (effectType == 11) {
        // 星轨隧道：消失点只做小幅漂移，避免整张画面被大范围拉散。
        vec2 center = vec2(0.5) + vec2(0.075 * sin(t * 0.23),
                                       0.055 * sin(t * 0.17 + 1.3));
        vec2 dir = uv - center;
        vec3 streak = vec3(0.0);
        const int N = 12;
        float pulse = amplitude * amplitude;
        float stretch = 0.08 + pulse * 0.24;
        for (int i = 1; i <= N; i++) {
            float f = float(i) / float(N);
            vec2 p = center + dir * (1.0 - f * stretch);
            float edgeMask = smoothstep(0.0, 0.025, p.x) *
                             smoothstep(0.0, 0.025, p.y) *
                             (1.0 - smoothstep(0.975, 1.0, p.x)) *
                             (1.0 - smoothstep(0.975, 1.0, p.y));
            vec3 c = texture(tex, clamp(p, vec2(0.0), vec2(1.0))).rgb;
            float luma = dot(c, vec3(0.299, 0.587, 0.114));
            float keep = smoothstep(0.42, 0.90, luma);
            float w = (1.0 - f * 0.55) * edgeMask;
            streak = max(streak, c * keep * w);
        }
        float dist = length((uv - center) * vec2(1.0, 1.15));
        float dim = 1.0 - amplitude * 0.18 * smoothstep(0.20, 0.95, dist);
        vec3 tunneled = max(rgb * dim, streak * (1.0 + amplitude * 0.95));
        float mixAmt = amplitude * (0.28 + pulse * 0.37);
        return mix(rgb, tunneled, mixAmt);
    }
    if (effectType == 26) {
        return logoShowLook(rgb, frameUv, t, amplitude, customColor);
    }
    if (effectType == 27) {
        vec3 heart = oldHeartScene(frameUv, t, amplitude);
        return heart;
    }
    if (effectType == 28) {
        return oldSoulEffect(rgb, tex, uv, amplitude);
    }
    if (effectType == 29) {
        return oldShakeEffect(rgb, tex, uv, amplitude);
    }
    if (effectType == 30) {
        return oldLightSourceScene(frameUv, t, amplitude);
    }
    if (effectType == 31) {
        return oldGlitchEffect(rgb, tex, uv, frameUv, t, amplitude);
    }
    if (effectType == 32) {
        return oldHuanjueEffect(rgb, tex, uv, t, amplitude);
    }
    if (effectType == 33) {
        return oldCubeEffect(rgb, tex, frameUv, t, amplitude);
    }
    if (effectType == 35) {
        return beatEchoEffect(rgb, tex, uv, frameUv, t, amplitude);
    }
    if (effectType == 36) {
        return neonOutlineEffect(rgb, tex, uv, frameUv, t, amplitude,
                                 customColor);
    }
    if (effectType == 37) {
        return liquidGlassEffect(rgb, tex, uv, frameUv, t, amplitude);
    }
    if (effectType == 38) {
        return kaleidoscopeEffect(rgb, tex, uv, frameUv, t, amplitude);
    }
    if (effectType == 39) {
        return beatShapeSplitEffect(rgb, tex, uv, frameUv, t, amplitude,
                                    customColor, colorMode);
    }
    if (effectType == 40) {
        return shapeMosaicStitchEffect(rgb, tex, uv, frameUv, t, amplitude,
                                       customColor, colorMode);
    }
    return rgb;
}

bool effectStackContainsId(uint effectBits, int singleEffectType, int wantedId) {
    if ((effectBits & 0x80000000u) == 0u) {
        return singleEffectType == wantedId;
    }
    int count = int((effectBits >> 24) & 0x0Fu);
    int effectA = int((effectBits >> 16) & 0xFFu);
    int effectB = int((effectBits >>  8) & 0xFFu);
    int effectC = int(effectBits & 0xFFu);
    return (count >= 1 && effectA == wantedId) ||
           (count >= 2 && effectB == wantedId) ||
           (count >= 3 && effectC == wantedId);
}

vec2 autoSplitTileUv(vec2 frameUv, float amplitude, out vec2 grid) {
    if (amplitude < 0.30) {
        grid = vec2(1.0);
        return frameUv;
    }
    float cols = 2.0;
    float rows = 2.0;
    if (amplitude >= 0.78) {
        cols = 4.0;
        rows = 3.0;
    } else if (amplitude >= 0.54) {
        cols = 3.0;
        rows = 3.0;
    } else if (amplitude >= 0.30) {
        cols = 3.0;
        rows = 2.0;
    }
    grid = vec2(cols, rows);
    return fract(frameUv * grid);
}

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 d = abs(p) - b + r;
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;
}

// 星形 SDF - 使用极坐标方法，与前端实现保持一致
// 前端：angle = (i * angleStep) / 2 - Math.PI / 2，其中 angleStep = (Math.PI * 2) / n
// 前端：radius = i % 2 === 0 ? outerRadius : innerRadius
// 前端有 n*2+1 个点（i 从 0 到 n*2），每个点的角度步长是 angleStep/2 = PI/n
float sdStar(vec2 p, float n, float outerRadius, float innerRadius) {
    // 计算极坐标
    float angle = atan(p.y, p.x);
    float r = length(p);

    // 前端角度从 -PI/2 开始，调整角度到相同坐标系
    // atan 返回 [-PI, PI]，前端从 -PI/2 开始，所以需要加 PI/2 然后 mod 2*PI
    angle = mod(angle + 3.14159265359 * 0.5, 3.14159265359 * 2.0);

    // 前端每个点的角度步长是 PI/n
    float angleStep = 3.14159265359 / n;

    // 计算当前角度对应的点索引（0 到 n*2-1）
    float pointIndex = angle / angleStep;
    pointIndex = mod(pointIndex, n * 2.0);

    int i = int(floor(pointIndex));
    int nextI = (i + 1) % int(n * 2.0);

    // 前端：i % 2 === 0 ? outerRadius : innerRadius
    float currentRadius = (i % 2 == 0) ? outerRadius : innerRadius;
    float nextRadius = (nextI % 2 == 0) ? outerRadius : innerRadius;

    // 计算当前点和下一个点的角度
    float currentAngle = float(i) * angleStep;
    float nextAngle = float(nextI) * angleStep;

    // 如果 nextI 是 0（回到起点），需要特殊处理
    if (nextI == 0) {
        nextAngle = n * 2.0 * angleStep;
    }

    // 在当前点和下一个点之间线性插值半径
    float t = (angle - currentAngle) / (nextAngle - currentAngle);
    t = clamp(t, 0.0, 1.0);
    float targetRadius = mix(currentRadius, nextRadius, t);

    return r - targetRadius;
}

// 六边形 SDF
float sdHexagon(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
}

// 菱形 SDF
float sdDiamond(vec2 p, vec2 size) {
    vec2 q = abs(p);
    return (q.x * size.y + q.y * size.x - size.x * size.y) / length(size);
}

// 心形 SDF - 使用隐式方程 (x² + y² - 1)³ - x²y³ = 0
// 增大缩放因子使心形完全适配坐标空间，避免顶部被裁剪
float sdHeart(inout vec2 p) {
    p.y = -p.y;
    p *= 1.5;  // 增大缩放因子，使心形变小以完全显示
    float x = p.x;
    float y = p.y;
    float a = ((x * x) + (y * y)) - 1.0;
    float d = ((a * a) * a) - ((((x * x) * y) * y) * y);
    return d;
}

// 花瓣 SDF - 使用简化的极坐标方程
// 与参考实现完全一致
float sdPetal(vec2 p, float n) {
    float an = 3.14159265359 / n;
    float bn = mod(atan(p.y, p.x), 2.0 * an) - an;
    float r = length(p);
    float d = r - (0.5 + (0.3 * cos(n * bn)));
    return d;
}

void main() {
    float shapeType = pc.shapeInfo.x;
    float shapeParam = pc.shapeInfo.y;
    float blackToTransparent = pc.shapeInfo.z;
    float invertMode = pc.shapeInfo.w;

    // 形状裁剪逻辑（使用 SDF + f宽度 做边缘抗锯齿）
    vec2 uv = fragTexCoord;
    float shapeMask = 1.0;
    int shapeTypeInt = int(shapeType);
    float shapeD = 1.0;  // SDF 距离：内部<0、外部>0、无 shape 时为 +1.0（供 effect 8 跑马灯使用）

    if (shapeTypeInt > 0) {
        vec2 p = uv - vec2(0.5); // 映射到 [-0.5, 0.5] 空间
        float d = -1.0;          // SDF: 内部<=0，外部>0

        if (shapeTypeInt == 1) { // 圆形
            d = length(p) - 0.5;
        } else if (shapeTypeInt == 2) { // 三角形
            // 与原逻辑保持一致：uv.y < 2*abs(uv.x-0.5) 视为外部
            d = 2.0 * abs(uv.x - 0.5) - uv.y;
        } else if (shapeTypeInt == 3) { // 圆角矩形
            float r = (shapeParam > 0.001) ? shapeParam : 0.1;
            p *= 2.0;
            r *= 1.0;
            d = sdRoundRect(p, vec2(1.0), r);
        } else if (shapeTypeInt == 4) { // 星形
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 5.0;
            p *= 2.0;
            d = sdStar(p, n, 0.5, 0.25);
        } else if (shapeTypeInt == 5) { // 六边形
            p *= 2.0;
            d = sdHexagon(p, 0.5);
        } else if (shapeTypeInt == 6) { // 菱形
            p *= 2.0;
            d = sdDiamond(p, vec2(0.5, 0.5));
        } else if (shapeTypeInt == 7) { // 心形
            p *= 2.0;
            d = sdHeart(p);
        } else if (shapeTypeInt == 8) { // 花瓣
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 4.0;
            p *= 2.0;
            d = sdPetal(p, n);
        }

        // 使用导数估计边缘过渡宽度，低速/静止时可显著降低锯齿闪烁
        float aa = max(fwidth(d), 1e-4);
        shapeMask = 1.0 - smoothstep(0.0, aa, d);
        shapeD = d;  // 暴露给 effect 8（跑马灯）使用
        if (shapeMask <= 0.001) discard;
    }

    uint effectBitsForSample = floatBitsToUint(pc.extEffect.x);
    float ampForSample = clamp(pc.extEffect.y * 2.0, 0.0, 1.0);
    vec2 sampleFrameUv = fragTexCoord;
    vec2 splitGrid = vec2(1.0);
    if (effectStackContainsId(effectBitsForSample, int(pc.extEffect.x), 17)) {
        sampleFrameUv = autoSplitTileUv(fragTexCoord, ampForSample, splitGrid);
    }
    float stitchPieceMask = 1.0;
    bool shapeMosaicStitchSample =
        effectStackContainsId(effectBitsForSample, int(pc.extEffect.x), 40);
    if (shapeMosaicStitchSample) {
        sampleFrameUv = shapeMosaicStitchSourceUv(
            fragTexCoord, pc.extEffect.z, stitchPieceMask);
    }

    // 使用统一的 userCrop 进行坐标映射。
    // C++ 端已将 cropInfo (硬件填充) 与用户裁剪合并计算到了 userCrop 中。
    vec2 adjustedTexCoord = pc.userCrop.xy + sampleFrameUv * pc.userCrop.zw;

    // 应用采集旋转补偿与图像反转（纹理坐标翻转）
    int rawMode = int(invertMode + 0.5);
    int rotateMode = rawMode & 0xF0;
    int mode = rawMode & 0x0F;
    if (rotateMode == 0x10) {
        adjustedTexCoord = vec2(1.0 - adjustedTexCoord.x, 1.0 - adjustedTexCoord.y);
    } else if (rotateMode == 0x20) {
        adjustedTexCoord = vec2(adjustedTexCoord.y, 1.0 - adjustedTexCoord.x);
    } else if (rotateMode == 0x30) {
        adjustedTexCoord = vec2(1.0 - adjustedTexCoord.y, adjustedTexCoord.x);
    }
    if (mode == 1) {
        adjustedTexCoord.x = 1.0 - adjustedTexCoord.x;
    } else if (mode == 2) {
        adjustedTexCoord.y = 1.0 - adjustedTexCoord.y;
    } else if (mode == 3) {
        adjustedTexCoord.x = 1.0 - adjustedTexCoord.x;
        adjustedTexCoord.y = 1.0 - adjustedTexCoord.y;
    }

    vec4 texColor = texture(texSampler, adjustedTexCoord);

    // ⭐ Luma Key 必须在 applyColorEffect 之前算，
    //   否则像 flash_black / curtain / iris 这种把画面涂黑的效果会被
    //   keying 当成"原本就该透明"，导致密集闪黑变成"透明闪"
    float lumaKeyAlpha = 1.0;
    if (blackToTransparent > 0.5 && !shapeMosaicStitchSample) {
        float origLuma = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        const float blackThreshold = 0.15;
        if (origLuma < blackThreshold) {
            lumaKeyAlpha = smoothstep(0.0, blackThreshold, origLuma);
        }
    }
    if (shapeMosaicStitchSample && stitchPieceMask < 0.5) {
        texColor.rgb = vec3(0.0);
    }

    // ⭐ 应用音频联动效果；parallel 模式时 extEffect.x 打包最多 3 个 effect id。
    uint effectBits = effectBitsForSample;
    bool isParallelStack = (effectBits & 0x80000000u) != 0u;
    int stackCount = int((effectBits >> 24) & 0x0Fu);
    int effectType = isParallelStack ? 0 : int(pc.extEffect.x);
    if ((isParallelStack && stackCount > 1) || effectType > 0) {
        float amp = clamp(pc.extEffect.y * 2.0, 0.0, 1.0);
        // 解码效果颜色：extEffect.w 是 packed (mode<<24|B<<16|G<<8|R)
        // mode bit0=1 → 用 (R,G,B) 固定色；bit1-5=描边宽度；bit6=七彩/颜色覆盖
        uint packed = floatBitsToUint(pc.extEffect.w);
        uint styleMode = (packed >> 24) & 0xFFu;
        float outlineWidthPercent = max(2.5, float((styleMode >> 1) & 0x1Fu) * 0.5);
        bool colorOverrideMode = (styleMode & 0x41u) != 0u;
        vec3 customColor = vec3(0.0);
        if ((styleMode & 1u) == 1u) {
            customColor = vec3(
                float((packed >>  0) & 0xFFu) / 255.0,
                float((packed >>  8) & 0xFFu) / 255.0,
                float((packed >> 16) & 0xFFu) / 255.0
            );
        }
        if (isParallelStack && stackCount > 1) {
            vec3 baseRgb = texColor.rgb;
            vec3 stackedRgb = baseRgb;
            int effectA = int((effectBits >> 16) & 0xFFu);
            int effectB = int((effectBits >>  8) & 0xFFu);
            int effectC = int(effectBits & 0xFFu);
            if (effectA > 0) {
                vec3 effectRgb = applyColorEffect(baseRgb, effectA, amp,
                                                   adjustedTexCoord, sampleFrameUv,
                                                   pc.extEffect.z, texSampler, shapeD,
                                                   customColor, outlineWidthPercent,
                                                   colorOverrideMode);
                stackedRgb += effectRgb - baseRgb;
            }
            if (stackCount >= 2 && effectB > 0) {
                vec3 effectRgb = applyColorEffect(baseRgb, effectB, amp,
                                                   adjustedTexCoord, sampleFrameUv,
                                                   pc.extEffect.z, texSampler, shapeD,
                                                   customColor, outlineWidthPercent,
                                                   colorOverrideMode);
                stackedRgb += effectRgb - baseRgb;
            }
            if (stackCount >= 3 && effectC > 0) {
                vec3 effectRgb = applyColorEffect(baseRgb, effectC, amp,
                                                   adjustedTexCoord, sampleFrameUv,
                                                   pc.extEffect.z, texSampler, shapeD,
                                                   customColor, outlineWidthPercent,
                                                   colorOverrideMode);
                stackedRgb += effectRgb - baseRgb;
            }
            texColor.rgb = clamp(stackedRgb, 0.0, 1.0);
        } else {
            texColor.rgb = applyColorEffect(texColor.rgb, effectType, amp,
                                             adjustedTexCoord, sampleFrameUv,
                                             pc.extEffect.z, texSampler, shapeD,
                                             customColor, outlineWidthPercent,
                                             colorOverrideMode);
        }
        if (shapeMosaicStitchSample && stitchPieceMask < 0.5) {
            texColor.rgb = vec3(0.0);
        }
    }

    vec4 finalColor = texColor * pc.color;
    finalColor.a *= shapeMask * lumaKeyAlpha;

    outColor = finalColor;
}

