#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

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

float fetchYuyvByte(int byteX, int y, int stride) {
    return texelFetch(texSampler, ivec2(byteX, y), 0).r;
}

float fetchYuyvLuma(int x, int y, int stride) {
    int pairStart = (x / 2) * 4;
    return fetchYuyvByte(pairStart + ((x & 1) == 0 ? 0 : 2), y, stride);
}

float sampleYuyvLumaLinear(vec2 uv, int width, int height, int stride) {
    vec2 pos = clamp(uv, vec2(0.0), vec2(1.0)) *
               vec2(float(width), float(height)) - vec2(0.5);
    ivec2 p0 = ivec2(floor(pos));
    vec2 f = fract(pos);

    int x0 = clamp(p0.x, 0, width - 1);
    int y0 = clamp(p0.y, 0, height - 1);
    int x1 = clamp(p0.x + 1, 0, width - 1);
    int y1 = clamp(p0.y + 1, 0, height - 1);

    float y00 = fetchYuyvLuma(x0, y0, stride);
    float y10 = fetchYuyvLuma(x1, y0, stride);
    float y01 = fetchYuyvLuma(x0, y1, stride);
    float y11 = fetchYuyvLuma(x1, y1, stride);
    return mix(mix(y00, y10, f.x), mix(y01, y11, f.x), f.y);
}


float sharpenYuyvLuma(vec2 uv, float yVal, int width, int height, int stride) {
    if (width <= 1 || height <= 1) {
        return yVal;
    }

    vec2 texel = vec2(1.0 / float(width), 1.0 / float(height));
    float left = sampleYuyvLumaLinear(uv + vec2(-texel.x, 0.0),
                                      width, height, stride);
    float right = sampleYuyvLumaLinear(uv + vec2(texel.x, 0.0),
                                       width, height, stride);
    float up = sampleYuyvLumaLinear(uv + vec2(0.0, -texel.y),
                                    width, height, stride);
    float down = sampleYuyvLumaLinear(uv + vec2(0.0, texel.y),
                                      width, height, stride);
    float neighborAvg = (left + right + up + down) * 0.25;
    float localMin = min(min(left, right), min(up, down));
    float localMax = max(max(left, right), max(up, down));
    float localContrast = localMax - localMin;

    vec2 srcPx = fwidth(uv) * vec2(float(width), float(height));
    float maxSrcPx = max(srcPx.x, srcPx.y);
    float upscaleGate = 1.0 - smoothstep(0.95, 1.35, maxSrcPx);
    float minifyFade = 1.0 - smoothstep(1.15, 1.85, maxSrcPx);
    float detailGate = smoothstep(0.008, 0.100, localContrast);
    float hardEdgeGuard = 1.0 - 0.45 * smoothstep(0.18, 0.42, localContrast);

    float strength = (0.075 + 0.115 * upscaleGate) * minifyFade *
                     detailGate * hardEdgeGuard;
    float sharpened = yVal + (yVal - neighborAvg) * strength;
    float ringGuard = 0.010 + 0.012 * upscaleGate;
    return clamp(sharpened,
                 max(0.0, localMin - ringGuard),
                 min(1.0, localMax + ringGuard));
}

vec3 yuvToRgbBt601Limited(float yVal, float uVal, float vVal) {
    float y = (yVal - 16.0 / 255.0) * (255.0 / 219.0);
    float u = (uVal - 128.0 / 255.0) * (255.0 / 224.0);
    float v = (vVal - 128.0 / 255.0) * (255.0 / 224.0);
    return vec3(y + 1.402 * v,
                y - 0.344136 * u - 0.714136 * v,
                y + 1.772 * u);
}

vec3 yuvToRgbBt709Limited(float yVal, float uVal, float vVal) {
    float y = (yVal - 16.0 / 255.0) * (255.0 / 219.0);
    float u = (uVal - 128.0 / 255.0) * (255.0 / 224.0);
    float v = (vVal - 128.0 / 255.0) * (255.0 / 224.0);
    return vec3(y + 1.5748 * v,
                y - 0.187324 * u - 0.468124 * v,
                y + 1.8556 * u);
}

void main() {
    float shapeType = pc.shapeInfo.x;
    float shapeParam = pc.shapeInfo.y;
    float blackToTransparent = pc.shapeInfo.z;
    float invertMode = pc.shapeInfo.w;

    // 形状裁剪逻辑
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

    int origWidth = int(pc.cropInfo.x);
    int origHeight = int(pc.cropInfo.y);
    int stride = int(pc.cropInfo.z);

    if (stride <= 0) stride = origWidth * 2;

    // 应用图像反转
    vec2 texCoord = pc.userCrop.xy + fragTexCoord * pc.userCrop.zw;
    if (pc.userCrop.z <= 0.0 || pc.userCrop.w <= 0.0) {
        texCoord = fragTexCoord;
    }
    int rawMode = int(invertMode + 0.5);
    int rotateMode = rawMode & 0xF0;
    int mode = rawMode & 0x0F;
    if (rotateMode == 0x10) {
        texCoord = vec2(1.0 - texCoord.x, 1.0 - texCoord.y);
    } else if (rotateMode == 0x20) {
        texCoord = vec2(texCoord.y, 1.0 - texCoord.x);
    } else if (rotateMode == 0x30) {
        texCoord = vec2(1.0 - texCoord.y, texCoord.x);
    }
    if (mode == 1) {
        texCoord.x = 1.0 - texCoord.x;
    } else if (mode == 2) {
        texCoord.y = 1.0 - texCoord.y;
    } else if (mode == 3) {
        texCoord.x = 1.0 - texCoord.x;
        texCoord.y = 1.0 - texCoord.y;
    }

    int px = int(texCoord.x * float(origWidth));
    int py = int(texCoord.y * float(origHeight));

    px = clamp(px, 0, origWidth - 1);
    py = clamp(py, 0, origHeight - 1);

    float yVal = sampleYuyvLumaLinear(texCoord, origWidth, origHeight, stride);
    yVal = sharpenYuyvLuma(texCoord, yVal, origWidth, origHeight, stride);

    int colStart = (px / 2) * 4;
    float uVal = fetchYuyvByte(colStart + 1, py, stride);
    float vVal = fetchYuyvByte(colStart + 3, py, stride);

    bool hdCapture = origWidth >= 1280 || origHeight >= 720;
    vec3 rgbRaw = hdCapture
                    ? yuvToRgbBt709Limited(yVal, uVal, vVal)
                    : yuvToRgbBt601Limited(yVal, uVal, vVal);
    vec3 rgb = clamp(rgbRaw, 0.0, 1.0);

    vec4 finalColor = vec4(rgb, 1.0) * pc.color;

    // 黑色变透明处理
    if (blackToTransparent > 0.5) {
        float luma = dot(finalColor.rgb, vec3(0.299, 0.587, 0.114));
        const float blackThreshold = 0.15;  // 提高阈值以更彻底过滤
        if (luma < blackThreshold) {
            float alphaMultiplier = smoothstep(0.0, blackThreshold, luma);
            finalColor.a *= alphaMultiplier;
        }
    }

    outColor = finalColor;
}

