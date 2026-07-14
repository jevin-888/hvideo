#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragGridPos;
layout(location = 2) in vec2 fragScreenPos;
layout(location = 3) in vec2 fragDeformedPos;
layout(location = 0) out vec4 outColor;


layout(binding = 0) uniform sampler2D canvasTexture;
layout(binding = 2) uniform sampler2D qrOverlayTexture;
layout(binding = 3) uniform sampler2D maskTexture;

layout(std140, set = 0, binding = 1) uniform CaveParams {
    mat4 view;
    mat4 proj;
    vec4 corners[4];
    vec4 logicalOutputRect; // 偏移 192
    vec4 projParams;
    vec4 projParams2;
    vec4 edgeSlope;         // 偏移 240
    vec4 stripStartEndH;    // 偏移 256 [startL, endL, startR, endR]
    vec4 stripStartEndV;    // 偏移 272 [startT, endT, startB, endB]
    vec4 edgeAnchor;        // 偏移 288 [anchorL, anchorR, anchorT, anchorB]
    vec4 outputSize;        // 偏移 304 [canvasW, canvasH, outputW, outputH]
    vec4 selectedPoints;    // 偏移 320 [geoX,geoY,maskX,maskY]
    vec4 blendBrightR;      // 偏移 336
    vec4 blendBrightG;      // 偏移 352
    vec4 blendBrightB;      // 偏移 368
    vec4 maskMeta;          // 偏移 384 [pointCount, aaPixels, 0, 0]
    vec4 maskBounds;        // 偏移 400 [minU,minV,maxU,maxV]
    vec4 maskPolygon[64];   // 偏移 416, xy/zw = two polygon points
} cave;

layout(push_constant) uniform PushConstants {
    vec4 regionRect;       // 偏移 0
    vec4 outputRect;       // 偏移 16
    vec4 lumContSatRot;    // 偏移 32: [luminance, contrast, saturation, rotationRadians]
    uint regionIdx;        // 偏移 48
    uint gridFlags;        // 偏移 52
    uint maskFlags;        // 偏移 56
    uint showLicenseWatermark; // 偏移 60
    vec4 blendParams;      // 偏移 64
    vec4 intensityGamma;   // 偏移 80: [强度, γL, γR, γT]
    float gammaBottom;     // 偏移 96
    uint activeRegionId, _pad98, _pad99;
    vec4 dmxParams;        // 偏移 112: [dmxR, dmxG, dmxB, enabled]
} pc;

#define pc_luminance (pc.lumContSatRot.x)
#define pc_contrast (pc.lumContSatRot.y)
#define pc_saturation (pc.lumContSatRot.z)
#define pc_rotationRadians (pc.lumContSatRot.w)
#define pc_dmxR (pc.dmxParams.x)
#define pc_dmxG (pc.dmxParams.y)
#define pc_dmxB (pc.dmxParams.z)
#define pc_dmxEnabled (uint(pc.dmxParams.w))
layout(binding = 4) uniform sampler2D blendTexture;

const int MASK_SHADER_MAX_VERTICES = 128;

// --- 辅助函数：状态解析 ---
bool getMaskEnabled() { return (pc.maskFlags & 1u) != 0u; }

float sampleLegacyBlendMask(vec2 localUV) {
    // 旧项目 CScreen::refreshBufers 为融合遮罩单独生成 0.01~0.99 的 UV。
    // 旧项目融合纹理使用 OpenGL 纹理坐标：v=0 是屏幕底部，v=1 是屏幕顶部。
    // 当前 Vulkan 局部坐标 y=0 是屏幕顶部，因此这里必须翻转 y 才等价。
    vec2 uv = clamp(localUV, vec2(0.0), vec2(1.0));
    vec2 fuseUV = vec2(0.01 + uv.x * 0.98, 0.01 + (1.0 - uv.y) * 0.98);
    return clamp(texture(blendTexture, fuseUV).r, 0.0, 1.0);
}

float gammaBlend(float anchor, float slope, float gamma, float x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    float a = clamp(anchor, 0.0, 1.0);
    float safeSlope = max(0.01, slope);
    float x1 = x < 0.5
        ? a * pow(2.0 * x, safeSlope)
        : 1.0 - (1.0 - a) * pow(2.0 * (1.0 - x), safeSlope);
    return pow(clamp(x1, 0.0, 1.0), max(0.1, gamma));
}

float edgeCurveValue(float local, float anchor, float slope, float gamma) {
    return clamp(1.0 - gammaBlend(anchor, slope, gamma, 1.0 - clamp(local, 0.0, 1.0)), 0.0, 1.0);
}

vec2 stripRange(float startValue, float endValue) {
    float s = clamp(startValue, 0.0, 1.0);
    float e = clamp(endValue, 0.0, 1.0);
    return vec2(min(s, e), max(s, e));
}

vec2 evalLeadingEdge(float blend, float coord, vec2 strip, float anchor, float slope, float gamma, bool solid) {
    if (blend <= 1e-6 || coord >= blend) return vec2(1.0, 0.0);
    if (solid) return vec2(0.0, 1.0);
    float t = clamp(coord / max(blend, 1e-6), 0.0, 1.0);
    if (t <= strip.x) return vec2(0.0, 1.0);
    if (t >= strip.y) return vec2(1.0, 0.0);
    float local = (t - strip.x) / max(1e-6, strip.y - strip.x);
    return vec2(edgeCurveValue(local, anchor, slope, gamma), 1.0);
}

vec2 evalTrailingEdge(float blend, float coord, vec2 strip, float anchor, float slope, float gamma, bool solid) {
    if (blend <= 1e-6 || coord <= 1.0 - blend) return vec2(1.0, 0.0);
    if (solid) return vec2(0.0, 1.0);
    float t = clamp((1.0 - coord) / max(blend, 1e-6), 0.0, 1.0);
    if (t <= strip.x) return vec2(0.0, 1.0);
    if (t >= strip.y) return vec2(1.0, 0.0);
    float local = (t - strip.x) / max(1e-6, strip.y - strip.x);
    return vec2(edgeCurveValue(local, anchor, slope, gamma), 1.0);
}

float combineBlendAxis(vec2 a, vec2 b) {
    return (a.y > 0.5 && b.y > 0.5) ? max(a.x, b.x) : min(a.x, b.x);
}

bool solidBlendSide(uint bitValue) {
    return (uint(cave.maskMeta.z + 0.5) & bitValue) != 0u;
}

float calculateBlendMask(vec2 localUV) {
    float bL = pc.blendParams.x;
    float bR = pc.blendParams.y;
    float bT = pc.blendParams.z;
    float bB = pc.blendParams.w;
    bool hasBlend = (bL > 1e-6 || bR > 1e-6 || bT > 1e-6 || bB > 1e-6);
    if (!hasBlend) return 1.0;

    vec2 stripL = stripRange(cave.stripStartEndH.x, cave.stripStartEndH.y);
    vec2 stripR = stripRange(cave.stripStartEndH.z, cave.stripStartEndH.w);
    vec2 stripT = stripRange(cave.stripStartEndV.x, cave.stripStartEndV.y);
    vec2 stripB = stripRange(cave.stripStartEndV.z, cave.stripStartEndV.w);
    vec2 left = evalLeadingEdge(bL, localUV.x, stripL, cave.edgeAnchor.x, cave.edgeSlope.x, pc.intensityGamma.y, solidBlendSide(1u));
    vec2 right = evalTrailingEdge(bR, localUV.x, stripR, cave.edgeAnchor.y, cave.edgeSlope.y, pc.intensityGamma.z, solidBlendSide(2u));
    vec2 top = evalLeadingEdge(bT, localUV.y, stripT, cave.edgeAnchor.z, cave.edgeSlope.z, pc.intensityGamma.w, solidBlendSide(4u));
    vec2 bottom = evalTrailingEdge(bB, localUV.y, stripB, cave.edgeAnchor.w, cave.edgeSlope.w, pc.gammaBottom, solidBlendSide(8u));
    return clamp(combineBlendAxis(left, right) * combineBlendAxis(top, bottom), 0.0, 1.0);
}

vec2 getMaskPolygonPoint(int index) {
    vec4 packed = cave.maskPolygon[index >> 1];
    return ((index & 1) == 0) ? packed.xy : packed.zw;
}

float sampleDynamicMask(vec2 canvasUV) {
    return clamp(texture(maskTexture, clamp(canvasUV, vec2(0.0), vec2(1.0))).r, 0.0, 1.0);
}

// 计算点 p 到线段 ab 的距离 (SDF)
float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

void main() {
    // 1. 基础采样与颜色处理 (使用 fragUV - 保持视频跟随网格变形)
    float c_rot = cos(pc_rotationRadians);
    float s_rot = sin(pc_rotationRadians);
    vec2 rotatedUV;
    rotatedUV.x = (fragUV.x - 0.5) * c_rot - (fragUV.y - 0.5) * s_rot + 0.5;
    rotatedUV.y = (fragUV.x - 0.5) * s_rot + (fragUV.y - 0.5) * c_rot + 0.5;

    vec2 regionSize = pc.regionRect.zw - pc.regionRect.xy;
    vec2 canvasTexSize = max(cave.outputSize.xy, vec2(1.0));
    vec2 halfTexel = 0.5 / canvasTexSize;
    vec2 localInset = min(halfTexel / max(abs(regionSize), 1.0 / canvasTexSize),
                          vec2(0.499));
    vec2 uv01 = clamp(rotatedUV, localInset, vec2(1.0) - localInset);
    vec2 canvasUV = pc.regionRect.xy + uv01 * regionSize;
    if (canvasUV.x < 0.0) canvasUV.x += 1.0;
    if (canvasUV.x > 1.0) canvasUV.x -= 1.0;
    vec2 uvMin = halfTexel;
    vec2 uvMax = vec2(1.0) - halfTexel;
    canvasUV = vec2(clamp(canvasUV.x, uvMin.x, uvMax.x),
                    clamp(canvasUV.y, uvMin.y, uvMax.y));
    vec4 color = texture(canvasTexture, canvasUV);

    vec3 rgb = color.rgb;
    rgb = (rgb - 0.5) * pc_contrast + 0.5;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luma), rgb, pc_saturation);
    vec3 result = rgb * pc_luminance;

    if (pc_dmxEnabled != 0u) {
        result *= vec3(pc_dmxR, pc_dmxG, pc_dmxB);
    }

    // 2. 边缘融合：按参数直接计算，避免 I 融合管理模式每次调网格都重建/上传融合纹理。
    float blendMask = calculateBlendMask(clamp(fragUV, vec2(0.0), vec2(1.0)));
    // 3.5. 二维码叠加层
    vec4 qrColor = texture(qrOverlayTexture, canvasUV);
    if (qrColor.a > 0.01) {
        result = mix(result, qrColor.rgb, qrColor.a);
    }

    // 4. 网格线逻辑已被移至独立的 gridLinePipeline 渲染管线，此处保持清洁
    // 遮罩热点也已移至 gridLinePipeline 处理，此处不再需要 per-pixel 计算

    float maskValue = 1.0;
    // 5. 遮罩逻辑：对齐旧项目 ZheZhao，遮罩作用于输入布局合成的整张幕布。
    if (getMaskEnabled()) {
        maskValue = sampleDynamicMask(canvasUV);
    }

    // 6. 授权水印 (不随视频变形)
    if (pc.showLicenseWatermark != 0u) {
        if (pc.showLicenseWatermark >= 3u) result = vec3(0.0);
        vec2 uv = fragDeformedPos;
        float textScale = 0.08;
        vec2 centered = (uv - vec2(0.31, 0.46)) / textScale;
        float charWidth = 0.5, gap = 0.12, minDistWatermark = 1.0;
        for (int i = 0; i < 8; i++) {
            float xLeft = float(i) * (charWidth + gap);
            float xRight = xLeft + charWidth;
            if (centered.x >= xLeft - 0.1 && centered.x <= xRight + 0.1) {
                vec2 charUV = vec2(clamp((centered.x - xLeft) / charWidth, 0.0, 1.0), 1.0 - clamp(centered.y, 0.0, 1.0));
                float d = 1.0;
                if (i == 0) d = min(sdSegment(charUV, vec2(0.2, 0.85), vec2(0.5, 0.15)), sdSegment(charUV, vec2(0.5, 0.15), vec2(0.8, 0.85)));
                else if (i == 1) d = min(sdSegment(charUV, vec2(0.7, 0.85), vec2(0.7, 0.3)), sdSegment(charUV, vec2(0.7, 0.3), vec2(0.3, 0.15)));
                else if (i == 2 || i == 7) d = min(min(min(sdSegment(charUV, vec2(0.25, 0.15), vec2(0.25, 0.85)), sdSegment(charUV, vec2(0.25, 0.85), vec2(0.75, 0.85))), sdSegment(charUV, vec2(0.25, 0.5), vec2(0.65, 0.5))), sdSegment(charUV, vec2(0.25, 0.15), vec2(0.75, 0.15)));
                else if (i == 3 || i == 6) d = min(min(sdSegment(charUV, vec2(0.2, 0.15), vec2(0.2, 0.85)), sdSegment(charUV, vec2(0.2, 0.85), vec2(0.8, 0.15))), sdSegment(charUV, vec2(0.8, 0.15), vec2(0.8, 0.85)));
                else if (i == 4) d = min(min(min(min(sdSegment(charUV, vec2(0.8, 0.85), vec2(0.2, 0.85)), sdSegment(charUV, vec2(0.2, 0.85), vec2(0.2, 0.15))), sdSegment(charUV, vec2(0.2, 0.15), vec2(0.8, 0.15))), sdSegment(charUV, vec2(0.8, 0.15), vec2(0.8, 0.45))), sdSegment(charUV, vec2(0.8, 0.45), vec2(0.5, 0.45)));
                else if (i == 5) d = min(min(sdSegment(charUV, vec2(0.5, 0.15), vec2(0.5, 0.85)), sdSegment(charUV, vec2(0.3, 0.15), vec2(0.7, 0.15))), sdSegment(charUV, vec2(0.3, 0.85), vec2(0.7, 0.85)));
                minDistWatermark = min(minDistWatermark, d);
            }
        }
        float textAlpha = 1.0 - smoothstep(0.02, 0.08, minDistWatermark - 0.04);
        result = mix(result, vec3(1.0), textAlpha * 0.7);
    }

    result *= blendMask * maskValue;

    // 区域之间应该无缝拼接，swapchain 始终写不透明颜色，避免系统合成层露出白缝。
    outColor = vec4(result, 1.0);
}
