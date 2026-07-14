#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D yTexture;
layout(binding = 1) uniform sampler2D uvTexture;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;
    vec4 cropInfo; // [原始宽度、原始高度、步幅、未使用]
    vec4 shapeInfo;  // x=形状类型, y=形状参数, z=黑色转透明, w=反转模式
    vec4 userCrop;
} pc;

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 d = abs(p) - b + r;
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;
}

// 星形 SDF - 使用极坐标方法，与前端实现保持一致
// 前端：angle = (i * angleStep) / 2 - Math.PI / 2，其中 angleStep = (Math.PI * 2) / n
// 前端：radius = i % 2 === 0 ? outerRadius : innerRadius
// 前端有 n*2 个点，每个点的角度步长是 angleStep/2 = PI/n
float sdStar(vec2 p, float n, float outerRadius, float innerRadius) {
    float angle = atan(p.y, p.x);
    float r = length(p);

    // 前端角度从 -PI/2 开始，调整角度到相同坐标系
    angle = mod(angle + 3.14159265359 * 0.5, 3.14159265359 * 2.0);

    // 前端每个点的角度步长是 PI/n
    float angleStep = 3.14159265359 / n;

    // 计算当前角度对应的点索引（0 到 n*2-1）
    // 注意：前端有 n*2 个点，覆盖 2*PI 范围
    float pointIndex = angle / angleStep;

    // 确保 pointIndex 在 [0, n*2) 范围内
    pointIndex = mod(pointIndex, n * 2.0);

    int i = int(floor(pointIndex));
    int nextI = (i + 1) % int(n * 2.0);

    // 前端：i % 2 === 0 ? outerRadius : innerRadius
    float currentRadius = (i % 2 == 0) ? outerRadius : innerRadius;
    float nextRadius = (nextI % 2 == 0) ? outerRadius : innerRadius;

    // 在当前点和下一个点之间线性插值
    float t = pointIndex - float(i);
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
// 与参考实现完全一致
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

float sampleNv16LumaFast(vec2 uv, int width, int height, int stride,
                         int yHeight) {
    vec2 imageSize = vec2(float(stride), float(max(yHeight, height)));
    vec2 samplePos = clamp(uv, vec2(0.0), vec2(1.0)) *
                     vec2(float(width - 1), float(height - 1)) + vec2(0.5);
    return texture(yTexture, samplePos / imageSize).r;
}

vec2 sampleNv16ChromaNearest(vec2 uv, int width, int height,
                             int stride, int yHeight) {
    int chromaWidth = max((width + 1) / 2, 1);
    int uvStride = max((stride + 1) / 2, chromaWidth);
    vec2 samplePos = clamp(uv, vec2(0.0), vec2(1.0)) *
                     vec2(float(chromaWidth - 1), float(height - 1)) +
                     vec2(0.5);
    return texture(uvTexture,
                   samplePos / vec2(float(uvStride), float(height))).rg;
}

vec3 yuvToRgbBt709Full(float yVal, float uVal, float vVal) {
    float y = yVal;
    float u = uVal - 0.5;
    float v = vVal - 0.5;
    return vec3(y + 1.5748 * v,
                y - 0.187324 * u - 0.468124 * v,
                y + 1.8556 * u);
}

float refineNv16Luma(vec2 uv, float yVal, int width, int height,
                     int stride, int yHeight) {
    if (width <= 1 || height <= 1) {
        return yVal;
    }

    vec2 texel = vec2(1.0 / float(width), 1.0 / float(height));
    float left = sampleNv16LumaFast(uv + vec2(-texel.x, 0.0),
                                    width, height, stride, yHeight);
    float right = sampleNv16LumaFast(uv + vec2(texel.x, 0.0),
                                     width, height, stride, yHeight);
    float up = sampleNv16LumaFast(uv + vec2(0.0, -texel.y),
                                  width, height, stride, yHeight);
    float down = sampleNv16LumaFast(uv + vec2(0.0, texel.y),
                                    width, height, stride, yHeight);

    float neighborAvg = (left + right + up + down) * 0.25;
    float localMin = min(min(left, right), min(up, down));
    float localMax = max(max(left, right), max(up, down));
    float localContrast = localMax - localMin;
    float detail = yVal - neighborAvg;

    vec2 srcPx = fwidth(uv) * vec2(float(width), float(height));
    float maxSrcPx = max(srcPx.x, srcPx.y);
    float upscaleGate = clamp(1.08 - maxSrcPx, 0.0, 1.0);
    float minifyFade = 1.0 - smoothstep(1.15, 1.85, maxSrcPx);
    float detailGate = smoothstep(0.012, 0.115, localContrast);
    float hardEdgeGate = smoothstep(0.060, 0.190, localContrast);
    float highlightGuard = 1.0 - smoothstep(0.58, 0.86, yVal);

    float darkTextMask = smoothstep(0.50, 0.86, neighborAvg) *
                         smoothstep(0.010, 0.110, -detail);
    float brightEdgeMask = smoothstep(0.70, 0.93, neighborAvg) *
                           smoothstep(0.008, 0.100, detail);

    float smoothLuma = yVal * 0.62 + neighborAvg * 0.38;
    float aaStrength = 0.11 * upscaleGate * hardEdgeGate *
                       (1.0 - 0.35 * darkTextMask);
    float refined = mix(yVal, smoothLuma, aaStrength);

    float strength = (0.018 + 0.038 * upscaleGate) * minifyFade *
                     detailGate * highlightGuard;
    strength *= 1.0 - 0.58 * hardEdgeGate;
    strength *= 1.0 - 0.72 * brightEdgeMask;
    strength *= 1.0 - 0.35 * darkTextMask;

    float sharpened = refined + (refined - neighborAvg) * strength;
    float ringGuard = 0.012 + 0.010 * upscaleGate;
    return clamp(sharpened,
                 max(0.0, localMin - ringGuard),
                 min(1.0, localMax + ringGuard));
}

float captureSkinMask(float uVal, float vVal) {
    float cbMask = smoothstep(0.30, 0.38, uVal) *
                   (1.0 - smoothstep(0.56, 0.66, uVal));
    float crMask = smoothstep(0.48, 0.56, vVal) *
                   (1.0 - smoothstep(0.78, 0.88, vVal));
    return clamp(cbMask * crMask, 0.0, 1.0);
}

float rollOffNv16Luma(float yVal, float skinMask) {
    float hi = smoothstep(0.58, 0.92, yVal);
    float skinHi = skinMask * smoothstep(0.46, 0.82, yVal);
    float over = max(yVal - 0.56, 0.0);
    float compressed = 0.56 + (1.0 - exp(-over * 1.55)) * 0.34;
    float amount = clamp(0.58 * hi + 0.30 * skinHi, 0.0, 0.82);
    return clamp(mix(yVal, compressed, amount), 0.0, 0.94);
}

vec3 toneMapNv16Highlights(vec3 rgb, float skinMask) {
    const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float luma = dot(rgb, lumaWeights);
    float peak = max(max(rgb.r, rgb.g), rgb.b);
    float hi = smoothstep(0.58, 0.94, max(luma, peak * 0.92));
    float skinHi = skinMask * smoothstep(0.46, 0.82, luma);

    float scale = 1.0 - 0.13 * hi - 0.08 * skinHi;
    vec3 compressed = rgb * scale;
    float compressedLuma = dot(compressed, lumaWeights);
    float sat = 1.0 - 0.10 * hi - 0.10 * skinHi;
    compressed = vec3(compressedLuma) + (compressed - vec3(compressedLuma)) * sat;

    float amount = clamp(0.82 * hi + 0.38 * skinHi, 0.0, 0.92);
    return clamp(mix(rgb, compressed, amount), 0.0, 0.94);
}

void main() {
    float shapeType = pc.shapeInfo.x;
    float shapeParam = pc.shapeInfo.y;
    float blackToTransparent = pc.shapeInfo.z;
    float invertMode = pc.shapeInfo.w;

    vec2 uv = fragTexCoord;
    bool discardPixel = false;
    int shapeTypeInt = int(shapeType);

    if (shapeTypeInt > 0) {
        vec2 p = uv - vec2(0.5); // 映射到 [-0.5, 0.5] 空间

        if (shapeTypeInt == 1) { // 圆形
            if (distance(uv, vec2(0.5)) > 0.5) discardPixel = true;
        } else if (shapeTypeInt == 2) { // 三角形
            if (uv.y < 2.0 * abs(uv.x - 0.5)) discardPixel = true;
        } else if (shapeTypeInt == 3) { // 圆角矩形
            float r = (shapeParam > 0.001) ? shapeParam : 0.1;
            p *= 2.0;
            r *= 1.0;
            float d = sdRoundRect(p, vec2(1.0), r);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 4) { // 星形
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 5.0;
            p *= 2.0;
            float d = sdStar(p, n, 0.5, 0.25);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 5) { // 六边形
            p *= 2.0;
            float d = sdHexagon(p, 0.5);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 6) { // 菱形
            p *= 2.0;
            float d = sdDiamond(p, vec2(0.5, 0.5));
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 7) { // 心形
            p *= 2.0;
            float d = sdHeart(p);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 8) { // 花瓣
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 4.0;
            p *= 2.0;
            float d = sdPetal(p, n);
            if (d > 0.0) discardPixel = true;
        }
    }

    if (discardPixel) discard;

    int visibleWidth = int(pc.cropInfo.x);
    int visibleHeight = int(pc.cropInfo.y);
    int stride = int(pc.cropInfo.z);
    int yHeight = int(pc.cropInfo.w);
    if (yHeight <= 0) yHeight = visibleHeight;

    // 应用图像反转
    vec2 texCoord = pc.userCrop.xy + fragTexCoord * pc.userCrop.zw;
    if (pc.userCrop.z <= 0.0 || pc.userCrop.w <= 0.0) {
        texCoord = fragTexCoord;
    }
    int mode = int(invertMode + 0.5);
    if (mode == 1) {
        texCoord.x = 1.0 - texCoord.x;
    } else if (mode == 2) {
        texCoord.y = 1.0 - texCoord.y;
    } else if (mode == 3) {
        texCoord.x = 1.0 - texCoord.x;
        texCoord.y = 1.0 - texCoord.y;
    }

    float yVal = sampleNv16LumaFast(texCoord, visibleWidth, visibleHeight,
                                    stride, yHeight);
    vec2 chroma = sampleNv16ChromaNearest(texCoord, visibleWidth, visibleHeight,
                                          stride, yHeight);
    float skinMask = captureSkinMask(chroma.x, chroma.y);
    yVal = refineNv16Luma(texCoord, yVal, visibleWidth, visibleHeight,
                          stride, yHeight);
    yVal = rollOffNv16Luma(yVal, skinMask);

    vec3 rgb = yuvToRgbBt709Full(yVal, chroma.x, chroma.y);
    rgb = toneMapNv16Highlights(clamp(rgb, 0.0, 1.0), skinMask);

    vec4 finalColor = vec4(rgb, 1.0) * pc.color;

    if (blackToTransparent > 0.5) {
        float luma = dot(finalColor.rgb, vec3(0.299, 0.587, 0.114));
        const float blackThreshold = 0.15;  // 提高阈值以更彻底过滤
        if (luma < blackThreshold) {
            finalColor.a *= smoothstep(0.0, blackThreshold, luma);
        }
    }

    outColor = finalColor;
}

