#version 450

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;
    vec4 cropInfo;  // x=纹理宽度, y=纹理高度, z=noSignalMode(0.0=有信号,1.0=无信号), w=未使用
    vec4 shapeInfo; // x=形状类型, y=形状参数, z=黑色转透明, w=invert
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

// ==========================================
// SDF (Signed Distance Field) 文字渲染
// 用于渲染 "NO SIGNAL" 英文文字
// ==========================================

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

float sdCircle(vec2 p, vec2 center, float r) {
    return length(p - center) - r;
}

float sdBox(vec2 p, vec2 center, vec2 size) {
    vec2 d = abs(p - center) - size;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

// 技术标识：N
float charN_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.2, 0.15), vec2(0.2, 0.85)); // 左侧竖线
    d = min(d, sdSegment(p, vec2(0.2, 0.85), vec2(0.8, 0.15))); // 对角线
    d = min(d, sdSegment(p, vec2(0.8, 0.15), vec2(0.8, 0.85))); // 右侧竖线
    return d;
}

// 技术标识：O
float charO_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.2, 0.15), vec2(0.2, 0.85)); // 左侧
    d = min(d, sdSegment(p, vec2(0.8, 0.15), vec2(0.8, 0.85))); // 右侧
    d = min(d, sdSegment(p, vec2(0.2, 0.15), vec2(0.8, 0.15))); // 底部
    d = min(d, sdSegment(p, vec2(0.2, 0.85), vec2(0.8, 0.85))); // 顶部
    return d;
}

// 技术标识：S
float charS_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.8, 0.85), vec2(0.2, 0.85)); // 顶部
    d = min(d, sdSegment(p, vec2(0.2, 0.85), vec2(0.2, 0.5))); // 左上段
    d = min(d, sdSegment(p, vec2(0.2, 0.5), vec2(0.8, 0.5))); // 中段
    d = min(d, sdSegment(p, vec2(0.8, 0.5), vec2(0.8, 0.15))); // 右下段
    d = min(d, sdSegment(p, vec2(0.8, 0.15), vec2(0.2, 0.15))); // 底部
    return d;
}

// 技术标识：I
float charI_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.5, 0.15), vec2(0.5, 0.85)); // 竖线
    d = min(d, sdSegment(p, vec2(0.3, 0.15), vec2(0.7, 0.15))); // 底部
    d = min(d, sdSegment(p, vec2(0.3, 0.85), vec2(0.7, 0.85))); // 顶部
    return d;
}

// 技术标识：G
float charG_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.8, 0.85), vec2(0.2, 0.85)); // 顶部
    d = min(d, sdSegment(p, vec2(0.2, 0.85), vec2(0.2, 0.15))); // 左侧
    d = min(d, sdSegment(p, vec2(0.2, 0.15), vec2(0.8, 0.15))); // 底部
    d = min(d, sdSegment(p, vec2(0.8, 0.15), vec2(0.8, 0.45))); // 右侧
    d = min(d, sdSegment(p, vec2(0.8, 0.45), vec2(0.5, 0.45))); // 内收段
    return d;
}

// 技术标识：A
float charA_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.2, 0.15), vec2(0.5, 0.85)); // 左侧
    d = min(d, sdSegment(p, vec2(0.5, 0.85), vec2(0.8, 0.15))); // 右侧
    d = min(d, sdSegment(p, vec2(0.3, 0.4), vec2(0.7, 0.4))); // 中段
    return d;
}

// 技术标识：L
float charL_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.25, 0.85), vec2(0.25, 0.15)); // 左侧
    d = min(d, sdSegment(p, vec2(0.25, 0.15), vec2(0.8, 0.15))); // 底部
    return d;
}

float getNoSignalCharSDF(vec2 p, int charIdx) {
    // 说明："NO SIGNAL"
    // 说明：0:N, 1:O, 2:space, 3:S, 4:I, 5:G, 6:N, 7:A, 8:L
    if (charIdx == 0) return charN_SDF(p);
    if (charIdx == 1) return charO_SDF(p);
    if (charIdx == 2) return 1.0; // 空格
    if (charIdx == 3) return charS_SDF(p);
    if (charIdx == 4) return charI_SDF(p);
    if (charIdx == 5) return charG_SDF(p);
    if (charIdx == 6) return charN_SDF(p);
    if (charIdx == 7) return charA_SDF(p);
    if (charIdx == 8) return charL_SDF(p);
    return 1.0;
}

// 渲染 "NO SIGNAL" 文字
float renderNoSignalText(vec2 uv) {
    // 垂直翻转修正
    uv.y = 1.0 - uv.y;
    
    // 9个字符
    float charWidth = 0.5;
    float gap = 0.15;
    float totalW = 9.0 * charWidth + 8.0 * gap;
    
    // 居中对齐
    uv.x += totalW * 0.5;
    
    float minDist = 1.0;
    
    for (int i = 0; i < 9; i++) {
        float xLeft = float(i) * (charWidth + gap);
        float xRight = xLeft + charWidth;
        
        // 示例/字段：跳过空格（idx 2）
        if (i == 2) continue;
        
        if (uv.x >= xLeft - 0.1 && uv.x <= xRight + 0.1) {
            vec2 charUV = vec2(clamp((uv.x - xLeft) / charWidth, 0.0, 1.0), 
                               clamp(uv.y, 0.0, 1.0));
            minDist = min(minDist, getNoSignalCharSDF(charUV, i));
        }
    }
    
    // 平滑边缘
    return 1.0 - smoothstep(0.02, 0.06, minDist - 0.04);
}

void main() {
    int shapeTypeInt = int(pc.shapeInfo.x);
    float shapeParam = pc.shapeInfo.y;
    bool noSignalMode = (shapeTypeInt >= 100);
    int realShapeType = shapeTypeInt % 100;
    
    // 形状裁剪逻辑 (基于归一化的 UV 坐标 [0, 1])
    vec2 pos_uv = fragTexCoord;
    bool discardPixel = false;
    
    if (realShapeType > 0) {
        vec2 p = pos_uv - vec2(0.5); // 映射到 [-0.5, 0.5] 空间
        
        if (realShapeType == 1) { // 圆形
            if (distance(pos_uv, vec2(0.5)) > 0.5) discardPixel = true;
        } else if (realShapeType == 2) { // 三角形
            if (pos_uv.y < 2.0 * abs(pos_uv.x - 0.5)) discardPixel = true;
        } else if (realShapeType == 3) { // 圆角矩形
            float r = (shapeParam > 0.001) ? shapeParam : 0.1;
            p *= 2.0;
            r *= 1.0;
            float d = sdRoundRect(p, vec2(1.0), r);
            if (d > 0.0) discardPixel = true;
        } else if (realShapeType == 4) { // 星形
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 5.0;
            p *= 2.0;
            float d = sdStar(p, n, 0.5, 0.25);
            if (d > 0.0) discardPixel = true;
        } else if (realShapeType == 5) { // 六边形
            p *= 2.0;
            float d = sdHexagon(p, 0.5);
            if (d > 0.0) discardPixel = true;
        } else if (realShapeType == 6) { // 菱形
            p *= 2.0;
            float d = sdDiamond(p, vec2(0.5, 0.5));
            if (d > 0.0) discardPixel = true;
        } else if (realShapeType == 7) { // 心形
            p *= 2.0;
            float d = sdHeart(p);
            if (d > 0.0) discardPixel = true;
        } else if (realShapeType == 8) { // 花瓣
            float n = max(3.0, floor(shapeParam + 0.5));
            if (shapeParam < 0.5) n = 4.0;
            p *= 2.0;
            float d = sdPetal(p, n);
            if (d > 0.0) discardPixel = true;
        }
    }
    
    if (discardPixel) discard;
    if (noSignalMode) {
        // 无信号模式：渲染黑色背景 + 居中的特效文字
        
        // 计算文字UV坐标（居中显示）
        vec2 centered = fragTexCoord - 0.5;
        
        // 保持文字纵横比
        centered.y *= (pc.cropInfo.y / pc.cropInfo.x);  // 根据纹理尺寸调整
        
        // [HSVJ_Aesthetic] 缩小并淡化 "NO SIGNAL" 文字
        float textScale = 0.08;
        vec2 textUV = centered / textScale;
        textUV.y += 0.5;
        
        float textAlpha = 0.0;
        // 增加范围检查，确保在这个范围外不渲染文字 (总宽5.7, 居中后 偏移 2.85)
        if (abs(textUV.y - 0.5) < 0.5 && abs(textUV.x) < 3.0) {
            textAlpha = renderNoSignalText(textUV);
        }
        
        // 黑色背景 + 灰色文字
        vec3 bgColor = vec3(0.0, 0.0, 0.0);
        vec3 textColor = vec3(0.4, 0.4, 0.4);  
        vec3 finalColor = mix(bgColor, textColor, textAlpha * 0.8);
        
        outColor = vec4(finalColor, 1.0) * pc.color;
    } else {
        // 有信号模式：正常渲染采集画面
        // 使用统一的 userCrop 进行坐标映射。
        // C++ 端已将 cropInfo (硬件填充) 与用户裁剪合并计算到了 userCrop 中。
        vec2 texCoord = pc.userCrop.xy + fragTexCoord * pc.userCrop.zw;
        
        // 采集旋转补偿与图像反转处理（在纹理采样前应用）
        float invertMode = pc.shapeInfo.w;
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
            texCoord.x = 1.0 - texCoord.x;  // 水平反转
        } else if (mode == 2) {
            texCoord.y = 1.0 - texCoord.y;  // 垂直反转
        } else if (mode == 3) {
            texCoord.x = 1.0 - texCoord.x;  // 水平+垂直反转
            texCoord.y = 1.0 - texCoord.y;
        }
        
        vec4 texColor = texture(texSampler, texCoord);

        // CAS 锐化：采样上下左右邻域像素，增强边缘对比度
        // 适用于低分辨率采集内容拉伸显示时改善模糊
        {
            vec2 texelSize = vec2(1.0) / vec2(pc.cropInfo.x, pc.cropInfo.y);
            vec3 n = texture(texSampler, texCoord + vec2(0.0, -texelSize.y)).rgb;
            vec3 s = texture(texSampler, texCoord + vec2(0.0,  texelSize.y)).rgb;
            vec3 w = texture(texSampler, texCoord + vec2(-texelSize.x, 0.0)).rgb;
            vec3 e = texture(texSampler, texCoord + vec2( texelSize.x, 0.0)).rgb;

            vec3 minNeighbor = min(min(n, s), min(w, e));
            vec3 maxNeighbor = max(max(n, s), max(w, e));

            // 自适应锐化强度：对比度低时加强，对比度高时减弱，避免振铃
            vec3 rcpRange = vec3(1.0) / (maxNeighbor - minNeighbor + 0.04);
            vec3 peak = clamp(min(texColor.rgb - minNeighbor, maxNeighbor - texColor.rgb) * rcpRange, 0.0, 1.0);
            float sharpenAmount = 0.4; // 锐化强度 0.0~1.0
            vec3 w_cas = peak * peak * sharpenAmount;

            vec3 avg = (n + s + w + e) * 0.25;
            texColor.rgb = mix(texColor.rgb, texColor.rgb + (texColor.rgb - avg), w_cas);
            texColor.rgb = clamp(texColor.rgb, 0.0, 1.0);
        }

        vec4 finalColor = texColor * pc.color;
        
        // 黑色变透明处理（Luma Key）
        float blackToTransparent = pc.shapeInfo.z;
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
}

