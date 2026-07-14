#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec2 fragScreenPos;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D canvasTexture;

layout(push_constant) uniform PushConstants {
    vec4 regions[4];
    vec4 outputRects[4];
    float rotationRadians;
    int regionCount;
} pc;

// 90度旋转优化（顺时针）
vec2 rotateUV90(vec2 uv) {
    return vec2(1.0 - uv.y, uv.x);
}

// 180度旋转优化
vec2 rotateUV180(vec2 uv) {
    return vec2(1.0 - uv.x, 1.0 - uv.y);
}

// 270度旋转优化（顺时针）
vec2 rotateUV270(vec2 uv) {
    return vec2(uv.y, 1.0 - uv.x);
}

// 通用旋转函数
vec2 rotateUV(vec2 uv, float radians) {
    float c = cos(radians);
    float s = sin(radians);
    vec2 center = vec2(0.5);
    uv -= center;
    uv = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    return uv + center;
}

int getRegionIndex(vec2 pos) {
    // 遍历所有已配置的区域，检测当前像素座标是否落在该区域内
    for (int i = 0; i < pc.regionCount && i < 4; i++) {
        vec4 rect = pc.outputRects[i]; // [x, y, w, h] 均为 0.0-1.0 归一化值
        if (pos.x >= rect.x && pos.x <= (rect.x + rect.z) &&
            pos.y >= rect.y && pos.y <= (rect.y + rect.w)) {
            return i;
        }
    }
    return -1; // 不在任何区域内
}

// ==========================================
// High Quality SDF Font 渲染器 for "VJENGINE"
// ==========================================

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

float charV_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.15, 0.9), vec2(0.5, 0.1));
    d = min(d, sdSegment(p, vec2(0.85, 0.9), vec2(0.5, 0.1)));
    return d;
}

float charJ_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.65, 0.9), vec2(0.65, 0.25)); // 主干
    d = min(d, sdSegment(p, vec2(0.35, 0.9), vec2(0.95, 0.9))); // 顶部横线（衬线）
    d = min(d, sdSegment(p, vec2(0.65, 0.25), vec2(0.35, 0.1))); // 钩形 1
    d = min(d, sdSegment(p, vec2(0.35, 0.1), vec2(0.1, 0.35))); // 钩形 2
    return d;
}

float charE_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.2, 0.1), vec2(0.2, 0.9)); // 竖线
    d = min(d, sdSegment(p, vec2(0.2, 0.9), vec2(0.8, 0.9))); // 顶部
    d = min(d, sdSegment(p, vec2(0.2, 0.5), vec2(0.7, 0.5))); // 中段
    d = min(d, sdSegment(p, vec2(0.2, 0.1), vec2(0.8, 0.1))); // 底部
    return d;
}

float charN_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.2, 0.1), vec2(0.2, 0.9)); // 左侧
    d = min(d, sdSegment(p, vec2(0.8, 0.1), vec2(0.8, 0.9))); // 右侧
    d = min(d, sdSegment(p, vec2(0.2, 0.9), vec2(0.8, 0.1))); // 对角线
    return d;
}

float charG_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.8, 0.9), vec2(0.2, 0.9)); // 顶部
    d = min(d, sdSegment(p, vec2(0.2, 0.9), vec2(0.2, 0.1))); // 左侧
    d = min(d, sdSegment(p, vec2(0.2, 0.1), vec2(0.8, 0.1))); // 底部
    d = min(d, sdSegment(p, vec2(0.8, 0.1), vec2(0.8, 0.4))); // 右上段
    d = min(d, sdSegment(p, vec2(0.8, 0.4), vec2(0.5, 0.4))); // 内收段
    return d;
}

float charI_SDF(vec2 p) {
    float d = sdSegment(p, vec2(0.5, 0.1), vec2(0.5, 0.9)); // 竖线
    d = min(d, sdSegment(p, vec2(0.3, 0.1), vec2(0.7, 0.1))); // 底部衬线
    d = min(d, sdSegment(p, vec2(0.3, 0.9), vec2(0.7, 0.9))); // 顶部衬线
    return d;
}

float getCharSDF(vec2 p, int charIdx) {
    if (charIdx == 0) return charV_SDF(p);
    if (charIdx == 1) return charJ_SDF(p);
    if (charIdx == 2) return charE_SDF(p);
    if (charIdx == 3) return charN_SDF(p);
    if (charIdx == 4) return charG_SDF(p);
    if (charIdx == 5) return charI_SDF(p);
    return 1.0;
}

// 渲染 "VJENGINE"
float renderVJEngine(vec2 uv) {
    // 纵向翻转以修正上下颠倒显示
    uv.y = 1.0 - uv.y;
    
    // 原始顺序：V(0) J(1) E(2) N(3) G(4) I(5) N(3) E(2)
    int chars[8];
    chars[0]=0; chars[1]=1; chars[2]=2; chars[3]=3; 
    chars[4]=4; chars[5]=5; chars[6]=3; chars[7]=2;
    
    // 平衡字体宽度
    float charWidth = 0.55; 
    float gap = 0.15;
    float totalW = 8.0 * charWidth + 7.0 * gap;
    
    // 居中对齐
    uv.x += totalW * 0.5;
    
    float minDist = 1.0;
    
    for (int i = 0; i < 8; i++) {
        float xLeft = float(i) * (charWidth + gap);
        float xRight = xLeft + charWidth;
        
        if (uv.x >= xLeft - 0.2 && uv.x <= xRight + 0.2) {
            vec2 charUV = vec2(clamp((uv.x - xLeft) / charWidth, -0.2, 1.2), clamp(uv.y, -0.2, 1.2));
            minDist = min(minDist, getCharSDF(charUV, chars[i]));
        }
    }
    
    // 平滑边缘
    return 1.0 - smoothstep(0.005, 0.05, minDist - 0.08); 
}

void main() {
    vec4 videoColor = vec4(0.0, 0.0, 0.0, 1.0);
    int regionIdx = getRegionIndex(fragScreenPos);
    vec2 localPos = vec2(0.5); // 默认居中
    bool inRegion = (regionIdx >= 0);

    // 如果像素位于有效的视频区域内，则采样视频内容
    if (inRegion) {
        vec4 outRect = pc.outputRects[regionIdx];
        localPos = (fragScreenPos - outRect.xy) / outRect.zw;
        
        vec2 rotatedPos;
        // 优化常见角度的旋转计算
        // 90° = 1.5708 弧度, 180° = 3.1416 弧度, 270° = 4.7124 弧度
        if (abs(pc.rotationRadians) < 0.1) {
            // 0度：无旋转
            rotatedPos = localPos;
        } else if (abs(pc.rotationRadians - 1.5708) < 0.1) {
            // 90度旋转
            rotatedPos = rotateUV90(localPos);
        } else if (abs(pc.rotationRadians - 3.1416) < 0.1) {
            // 180度旋转
            rotatedPos = rotateUV180(localPos);
        } else if (abs(pc.rotationRadians - 4.7124) < 0.1) {
            // 270度旋转
            rotatedPos = rotateUV270(localPos);
        } else {
            // 其他角度使用通用旋转
            rotatedPos = rotateUV(localPos, pc.rotationRadians);
        }
        
        vec4 srcRegion = pc.regions[regionIdx];
        vec2 canvasUV = srcRegion.xy + rotatedPos * (srcRegion.zw - srcRegion.xy);
        
        if (canvasUV.x >= 0.0 && canvasUV.x <= 1.0 && 
            canvasUV.y >= 0.0 && canvasUV.y <= 1.0) {
            videoColor = texture(canvasTexture, canvasUV);
        }
    }
    
    // === Watermark (在每个区域内居中，跟随视频旋转) ===
    // 如果不在任何区域内，则不显示水印（或根据需求决定）
    float wmAlpha = 0.0;
    if (inRegion) {
        // 水印需要使用与视频相反的UV变换来达到相同的视觉效果
        // 因为视频UV变换是逆变换（用于采样），水印需要正向变换
        // 所以 90° 视频旋转 → 270° 水印UV变换，270° 视频旋转 → 90° 水印UV变换
        vec2 wmPos = localPos;
        if (abs(pc.rotationRadians - 1.5708) < 0.01) {
            // 视频90度 → 水印使用270度UV变换
            wmPos = rotateUV270(localPos);
        } else if (abs(pc.rotationRadians - 3.1416) < 0.01) {
            // 视频180度 → 水印使用180度UV变换（180°的逆是自己）
            wmPos = rotateUV180(localPos);
        } else if (abs(pc.rotationRadians - 4.7124) < 0.01) {
            // 视频270度 → 水印使用90度UV变换
            wmPos = rotateUV90(localPos);
        } else if (abs(pc.rotationRadians) >= 0.01) {
            // 其他角度使用逆旋转
            wmPos = rotateUV(localPos, -pc.rotationRadians);
        }
        
        // 从旋转后的位置计算水印坐标
        vec2 centered = wmPos - 0.5;
        
        // 保持文字纵横比 (16:9 区域)
        // 字符顺序已反转，不需要X轴翻转
        vec2 rotated = centered;
        rotated.y *= 1.777; 
        
        vec2 textUV = rotated / 0.13; 
        textUV.y += 0.5; 
        
        if (abs(textUV.y - 0.5) < 0.6 && abs(textUV.x) < 4.0) {
            wmAlpha = renderVJEngine(textUV);
        }
    }
    
    
    vec4 wmColor = vec4(0.8, 0.8, 0.8, 0.5); 
    vec3 mixed = mix(videoColor.rgb, wmColor.rgb, wmAlpha * wmColor.a);
    
    outColor = vec4(mixed, 1.0);
}
