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

float sharpenCaptureLuma(vec2 uv, float yVal, int width, int height,
                          int stride) {
    if (width <= 1 || height <= 1) {
        return yVal;
    }

    vec2 clampedUv = clamp(uv, vec2(0.0), vec2(1.0));
    ivec2 p = ivec2(floor(clampedUv * vec2(float(width), float(height))));
    p.x = clamp(p.x, 0, width - 1);
    p.y = clamp(p.y, 0, height - 1);

    float left = fetchNv24LumaClamped(p.x - 1, p.y, width, height, stride);
    float right = fetchNv24LumaClamped(p.x + 1, p.y, width, height, stride);
    float up = fetchNv24LumaClamped(p.x, p.y - 1, width, height, stride);
    float down = fetchNv24LumaClamped(p.x, p.y + 1, width, height, stride);
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

    float yVal = samplePlaneLinear(texCoord, visibleWidth, visibleHeight,
                                   stride, 0);
    vec2 chroma = sampleNv24ChromaLinear(texCoord, visibleWidth, visibleHeight,
                                         stride, uvOffset);

    yVal = sharpenCaptureLuma(texCoord, yVal, visibleWidth, visibleHeight,
                               stride);

    bool hdCapture = visibleWidth >= 1280 || visibleHeight >= 720;
    vec3 rgbRaw = hdCapture
                    ? yuvToRgbBt709Limited(yVal, chroma.x, chroma.y)
                    : yuvToRgbBt601Limited(yVal, chroma.x, chroma.y);
    vec3 rgb = clamp(rgbRaw, 0.0, 1.0);

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
