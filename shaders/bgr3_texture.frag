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

    int origWidth = int(pc.cropInfo.x);
    int origHeight = int(pc.cropInfo.y);
    int stride = int(pc.cropInfo.z);
    if (stride <= 0) stride = origWidth * 3;

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

    int px = int(texCoord.x * float(origWidth));
    int py = int(texCoord.y * float(origHeight));
    px = clamp(px, 0, origWidth - 1);
    py = clamp(py, 0, origHeight - 1);

    int byteOffset = py * stride + px * 3;
    int bByte = byteOffset;
    int gByte = byteOffset + 1;
    int rByte = byteOffset + 2;

    ivec2 bCoord = ivec2(bByte % stride, bByte / stride);
    ivec2 gCoord = ivec2(gByte % stride, gByte / stride);
    ivec2 rCoord = ivec2(rByte % stride, rByte / stride);

    float b = texelFetch(texSampler, bCoord, 0).r;
    float g = texelFetch(texSampler, gCoord, 0).r;
    float r = texelFetch(texSampler, rCoord, 0).r;

    vec4 finalColor = vec4(r, g, b, 1.0) * pc.color;

    if (blackToTransparent > 0.5) {
        float luma = dot(finalColor.rgb, vec3(0.299, 0.587, 0.114));
        const float blackThreshold = 0.15;  // 提高阈值以更彻底过滤
        if (luma < blackThreshold) {
            finalColor.a *= smoothstep(0.0, blackThreshold, luma);
        }
    }

    outColor = finalColor;
}

