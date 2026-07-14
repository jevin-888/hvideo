#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;
    vec4 cropInfo;  // x=可见宽度, y=可见高度, z=步幅, w=UV 偏移行数
    vec4 shapeInfo; // x=形状类型, y=形状参数, z=黑色转透明, w=反转模式
    vec4 userCrop;
} pc;

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 d = abs(p) - b + r;
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;
}

float sdStar(vec2 p, float n, float outerRadius, float innerRadius) {
    float angle = atan(p.y, p.x);
    float r = length(p);
    angle = mod(angle + 3.14159265359 * 0.5, 3.14159265359 * 2.0);
    float angleStep = 3.14159265359 / n;
    float pointIndex = mod(angle / angleStep, n * 2.0);

    int i = int(floor(pointIndex));
    int nextI = (i + 1) % int(n * 2.0);
    float currentRadius = (i % 2 == 0) ? outerRadius : innerRadius;
    float nextRadius = (nextI % 2 == 0) ? outerRadius : innerRadius;
    float targetRadius = mix(currentRadius, nextRadius, pointIndex - float(i));
    return r - targetRadius;
}

float sdHexagon(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
}

float sdDiamond(vec2 p, vec2 size) {
    vec2 q = abs(p);
    return (q.x * size.y + q.y * size.x - size.x * size.y) / length(size);
}

float sdHeart(inout vec2 p) {
    p.y = -p.y;
    p *= 1.5;
    float x = p.x;
    float y = p.y;
    float a = ((x * x) + (y * y)) - 1.0;
    return ((a * a) * a) - ((((x * x) * y) * y) * y);
}

float sdPetal(vec2 p, float n) {
    float an = 3.14159265359 / n;
    float bn = mod(atan(p.y, p.x), 2.0 * an) - an;
    float r = length(p);
    return r - (0.5 + (0.3 * cos(n * bn)));
}

float fetchByte(int byteOffset, int stride) {
    ivec2 coord = ivec2(byteOffset % stride, byteOffset / stride);
    return texelFetch(texSampler, coord, 0).r;
}

float samplePlaneLinear(vec2 uv, int width, int height, int stride, int baseRow) {
    vec2 pos = clamp(uv, vec2(0.0), vec2(1.0)) *
               vec2(float(width), float(height)) - vec2(0.5);
    ivec2 p0 = ivec2(floor(pos));
    vec2 f = fract(pos);

    int x0 = clamp(p0.x, 0, width - 1);
    int y0 = clamp(p0.y, 0, height - 1);
    int x1 = clamp(p0.x + 1, 0, width - 1);
    int y1 = clamp(p0.y + 1, 0, height - 1);

    float v00 = fetchByte((baseRow + y0) * stride + x0, stride);
    float v10 = fetchByte((baseRow + y0) * stride + x1, stride);
    float v01 = fetchByte((baseRow + y1) * stride + x0, stride);
    float v11 = fetchByte((baseRow + y1) * stride + x1, stride);
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
}

vec2 fetchNv24ChromaPair(int x, int y, int stride, int uvOffset) {
    int byteOffset = uvOffset * stride + y * stride * 2 + x * 2;
    return vec2(fetchByte(byteOffset, stride),
                fetchByte(byteOffset + 1, stride));
}

vec2 sampleNv24ChromaLinear(vec2 uv, int width, int height,
                            int stride, int uvOffset) {
    vec2 pos = clamp(uv, vec2(0.0), vec2(1.0)) *
               vec2(float(width), float(height)) - vec2(0.5);
    ivec2 p0 = ivec2(floor(pos));
    vec2 f = fract(pos);

    int x0 = clamp(p0.x, 0, width - 1);
    int y0 = clamp(p0.y, 0, height - 1);
    int x1 = clamp(p0.x + 1, 0, width - 1);
    int y1 = clamp(p0.y + 1, 0, height - 1);

    vec2 uv00 = fetchNv24ChromaPair(x0, y0, stride, uvOffset);
    vec2 uv10 = fetchNv24ChromaPair(x1, y0, stride, uvOffset);
    vec2 uv01 = fetchNv24ChromaPair(x0, y1, stride, uvOffset);
    vec2 uv11 = fetchNv24ChromaPair(x1, y1, stride, uvOffset);
    return mix(mix(uv00, uv10, f.x), mix(uv01, uv11, f.x), f.y);
}

float fetchNv24LumaClamped(int x, int y, int width, int height, int stride) {
    int sx = clamp(x, 0, width - 1);
    int sy = clamp(y, 0, height - 1);
    return fetchByte(sy * stride + sx, stride);
}

float antialiasCaptureLuma(vec2 uv, float yVal, int width, int height,
                           int stride) {
    vec2 clampedUv = clamp(uv, vec2(0.0), vec2(1.0));
    ivec2 p = ivec2(floor(clampedUv * vec2(float(width), float(height))));
    p.x = clamp(p.x, 0, width - 1);
    p.y = clamp(p.y, 0, height - 1);

    float left = fetchNv24LumaClamped(p.x - 1, p.y, width, height, stride);
    float right = fetchNv24LumaClamped(p.x + 1, p.y, width, height, stride);
    float up = fetchNv24LumaClamped(p.x, p.y - 1, width, height, stride);
    float down = fetchNv24LumaClamped(p.x, p.y + 1, width, height, stride);
    float neighborAvg = (left + right + up + down) * 0.25;

    vec2 srcPx = fwidth(uv) * vec2(float(width), float(height));
    float maxSrcPx = max(srcPx.x, srcPx.y);
    float upscaleGate = clamp(1.20 - maxSrcPx, 0.0, 1.0);
    float localContrast = max(max(abs(yVal - left), abs(yVal - right)),
                              max(abs(yVal - up), abs(yVal - down)));
    float detail = yVal - neighborAvg;
    float detailGate = smoothstep(0.012, 0.120, localContrast);
    float hardEdgeGate = smoothstep(0.070, 0.220, localContrast);
    float highlightGuard = 1.0 - smoothstep(0.64, 0.88, yVal);

    float darkTextMask = smoothstep(0.54, 0.84, neighborAvg) *
                         smoothstep(0.010, 0.115, -detail);
    float textPreserved = yVal - (-detail) * 0.30 * darkTextMask;
    float smoothLuma = yVal * 0.42 + neighborAvg * 0.58;
    float aaStrength = 0.16 * upscaleGate * hardEdgeGate *
                       (1.0 - darkTextMask);
    float refined = mix(yVal, smoothLuma, aaStrength);
    refined = mix(refined, textPreserved, darkTextMask);

    float sharpenStrength = (0.045 + 0.080 * upscaleGate) *
                            detailGate * highlightGuard * (1.0 - aaStrength);
    return clamp(refined + (refined - neighborAvg) * sharpenStrength, 0.0, 1.0);
}

vec3 yuvToRgbBt709Full(float yVal, float uVal, float vVal) {
    float y = yVal;
    float u = uVal - 0.5;
    float v = vVal - 0.5;
    return vec3(y + 1.5748 * v,
                y - 0.187324 * u - 0.468124 * v,
                y + 1.8556 * u);
}

float rollOffCaptureLuma(float yVal) {
    float knee = smoothstep(0.50, 0.88, yVal);
    float over = max(yVal - 0.50, 0.0);
    float compressed = 0.50 + (1.0 - exp(-over * 2.25)) * 0.34;
    return clamp(mix(yVal, compressed, knee * 0.88), 0.0, 1.0);
}

float captureSkinMask(float uVal, float vVal) {
    float cbMask = smoothstep(0.30, 0.37, uVal) *
                   (1.0 - smoothstep(0.56, 0.64, uVal));
    float crMask = smoothstep(0.49, 0.56, vVal) *
                   (1.0 - smoothstep(0.78, 0.86, vVal));
    return clamp(cbMask * crMask, 0.0, 1.0);
}

float rollOffCaptureSkinLuma(float yVal, float skinMask) {
    float knee = smoothstep(0.48, 0.82, yVal) * skinMask;
    float over = max(yVal - 0.48, 0.0);
    float compressed = 0.48 + (1.0 - exp(-over * 2.05)) * 0.27;
    return clamp(mix(yVal, compressed, knee * 0.62), 0.0, 1.0);
}

vec3 enhanceCaptureLook(vec3 rgb) {
    const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float luma = dot(rgb, lumaWeights);

    float highlightProtect = 1.0 - smoothstep(0.46, 0.78, luma);
    float shadowProtect = smoothstep(0.03, 0.16, luma);
    float midAmount = highlightProtect * shadowProtect;

    vec3 enhanced = mix(vec3(luma), rgb, 1.04 + 0.03 * midAmount);
    enhanced = (enhanced - vec3(0.5)) * (1.0 + 0.025 * midAmount) + vec3(0.5);

    float hi = smoothstep(0.58, 0.92, luma);
    enhanced = mix(enhanced, vec3(luma) + (enhanced - vec3(luma)) * 0.92, hi);
    enhanced = min(enhanced, rgb + vec3(0.035));
    enhanced = mix(enhanced, rgb * 0.965, smoothstep(0.72, 1.0, luma));

    float amount = 0.48 * shadowProtect;
    return clamp(mix(rgb, enhanced, amount), 0.0, 1.0);
}

vec3 rollOffCaptureHighlights(vec3 rgb) {
    const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float luma = dot(rgb, lumaWeights);
    float peak = max(max(rgb.r, rgb.g), rgb.b);
    float pressure = max(luma, peak * 0.92);
    float knee = smoothstep(0.55, 0.90, pressure);
    vec3 soft = vec3(1.0) - exp(-rgb * 1.60);
    soft *= 0.86;
    vec3 compressed = mix(rgb, soft, knee);
    return clamp(mix(rgb, compressed, 0.82), 0.0, 1.0);
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
        vec2 p = uv - vec2(0.5);

        if (shapeTypeInt == 1) {
            if (distance(uv, vec2(0.5)) > 0.5) discardPixel = true;
        } else if (shapeTypeInt == 2) {
            if (uv.y < 2.0 * abs(uv.x - 0.5)) discardPixel = true;
        } else if (shapeTypeInt == 3) {
            float r = (shapeParam > 0.001) ? shapeParam : 0.1;
            p *= 2.0;
            float d = sdRoundRect(p, vec2(1.0), r);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 4) {
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 5.0;
            p *= 2.0;
            float d = sdStar(p, n, 0.5, 0.25);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 5) {
            p *= 2.0;
            float d = sdHexagon(p, 0.5);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 6) {
            p *= 2.0;
            float d = sdDiamond(p, vec2(0.5, 0.5));
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 7) {
            p *= 2.0;
            float d = sdHeart(p);
            if (d > 0.0) discardPixel = true;
        } else if (shapeTypeInt == 8) {
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
    int uvOffset = int(pc.cropInfo.w);
    if (stride <= 0) stride = visibleWidth;
    if (uvOffset <= 0) uvOffset = visibleHeight;

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

    float yVal = samplePlaneLinear(texCoord, visibleWidth, visibleHeight,
                                   stride, 0);
    vec2 chroma = sampleNv24ChromaLinear(texCoord, visibleWidth, visibleHeight,
                                         stride, uvOffset);

    yVal = antialiasCaptureLuma(texCoord, yVal, visibleWidth, visibleHeight,
                                stride);
    yVal = rollOffCaptureSkinLuma(yVal, captureSkinMask(chroma.x, chroma.y));
    yVal = rollOffCaptureLuma(yVal);

    vec3 rgb = yuvToRgbBt709Full(yVal, chroma.x, chroma.y);
    rgb = rollOffCaptureHighlights(clamp(rgb, 0.0, 1.0));
    rgb = enhanceCaptureLook(rgb);

    vec4 finalColor = vec4(rgb, 1.0) * pc.color;

    if (blackToTransparent > 0.5) {
        float luma = dot(finalColor.rgb, vec3(0.299, 0.587, 0.114));
        const float blackThreshold = 0.15;
        if (luma < blackThreshold) {
            finalColor.a *= smoothstep(0.0, blackThreshold, luma);
        }
    }

    outColor = finalColor;
}
