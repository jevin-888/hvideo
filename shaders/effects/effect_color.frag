#version 450

// 音频联动颜色效果 Fragment Shader
// 基于 huoshanVJ RenderEffectColor.cpp 实现
// 效果类型:
//   0 = 无效果
//   1 = 闪白 (Flash White)
//   2 = 闪黑 (Flash Black)
//   3 = 红色效果 (Red Color)
//   4 = 绿色效果 (Green Color)
//   5 = 蓝色效果 (Blue Color)
//   6 = 黑条扫描 (Scan Bar) - 横向黑条从上往下扫过，击鼓时推进
//   7 = 中心散开 (Iris Out) - 外圈全黑，中心圆随击鼓扩展露出画面
//   8 = RGB 描边 (Chromatic Split) - R/G/B 三通道分离形成彩色描边
//   9 = 反色 (Invert) - 颜色取反（霓虹/迷幻感）
//  10 = 扫描线 (Scanlines) - 经典电视机水平扫描线
//  11 = 星轨隧道 (Warp Tunnel) - 亮点从漂移消失点向外拖长尾，经典炸场效果
//
// 并行效果模式：最多支持4种效果同时叠加

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    float alpha;
    float intensity;    // 音频强度 [0, 1]
    int effectType;     // 主效果类型
    float time;         // 时间（秒）
    int effectType2;    // 第二效果类型 (0=无)
    int effectType3;    // 第三效果类型 (0=无)
    int effectType4;    // 第四效果类型 (0=无)
    float reserved;     // 预留对齐
} push;

float rectMask(vec2 p, vec2 halfSize, float feather) {
    vec2 d = abs(p) - halfSize;
    float sdf = min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
    return 1.0 - smoothstep(0.0, feather, sdf);
}

vec3 logoShowLook(vec3 rgb, vec2 uv, float t, float amplitude,
                  vec3 customColor) {
    float pxX = max(fwidth(uv.x), 1e-6);
    float pxY = max(fwidth(uv.y), 1e-6);
    vec2 p = uv - vec2(0.5);
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
    float sweep = (1.0 - smoothstep(0.0, 0.075, abs(uv.x - sweepX))) *
                  exp(-abs(uv.y - 0.50) * 18.0) * intro;
    float blade = exp(-abs(p.y) * 35.0) *
                  (1.0 - smoothstep(0.05, 0.58, abs(p.x))) *
                  (0.08 + drive * 0.06) * intro;
    float ring = (1.0 - smoothstep(0.004, 0.018,
                                   abs(r - (0.310 + drive * 0.006)))) *
                 (1.0 - smoothstep(0.20, 0.55, r)) * intro;
    float centerBloom = exp(-r * 7.2) * (0.05 + startupFlash * 0.20 + drive * 0.08);
    float fineScan = (0.5 + 0.5 * sin(uv.y * 760.0 + t * 1.3)) *
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

// 应用单个效果到颜色（uv/时间 让空间型和动画型效果可用）
vec4 applyEffect(vec4 rgba, int effectType, float amplitude, vec2 uv, float t) {
    if (effectType == 0) {
        return rgba;
    }
    else if (effectType == 1) {
        vec4 whiteColor = vec4(1.0, 1.0, 1.0, 1.0);
        return mix(rgba, whiteColor, amplitude);
    }
    else if (effectType == 2) {
        vec4 blackColor = vec4(0.0, 0.0, 0.0, 1.0);
        float blend = min(1.0, amplitude * 2.5);
        return mix(rgba, blackColor, blend);
    }
    else if (effectType >= 3 && effectType <= 5) {
        float luma = dot(rgba.rgb, vec3(0.299, 0.587, 0.114));
        vec3 targetColor;
        if (effectType == 3) targetColor = vec3(1.0, 0.0, 0.0);
        else if (effectType == 4) targetColor = vec3(0.0, 1.0, 0.0);
        else targetColor = vec3(0.0, 0.0, 1.0);
        vec3 tinted = vec3(luma) * targetColor * 1.5;
        vec3 finalRGB = mix(rgba.rgb, tinted, amplitude);
        return vec4(finalRGB, rgba.a);
    }
    else if (effectType == 6) {
        // 黑条扫描：3Hz 从上往下循环扫过，黑条厚度占屏幕 15%
        //   amplitude=0 时不显示；=1 时完整黑条。鼓点期间自动扫一轮。
        float barPos = fract(t * 3.0);              // 0..1 循环
        float barThickness = 0.15;
        float d = abs(uv.y - barPos);
        float barMask = smoothstep(barThickness, barThickness * 0.5, d);
        float blend = barMask * amplitude;
        return vec4(mix(rgba.rgb, vec3(0.0), blend), rgba.a);
    }
    else if (effectType == 7) {
        // 中心散开（Iris Out）：外围全黑，中心圆随 amplitude 扩展
        //   amplitude=0：露出全屏 (无效果)
        //   amplitude=1：露出中心小圆，周围漆黑
        //   配合鼓点 → 击鼓瞬间"收缩一下"然后恢复，像被重击的心跳
        vec2 centered = uv - vec2(0.5);
        float dist = length(centered) * 1.414;      // 归一化 0..1
        float radius = 1.0 - amplitude * 0.85;       // 鼓点瞬间收缩到 15%
        float mask = smoothstep(radius, radius - 0.1, dist);
        return vec4(rgba.rgb * mask, rgba.a);
    }
    else if (effectType == 8) {
        // RGB 描边 / 色散：R 向左偏，B 向右偏，G 保持
        //   amplitude 控制偏移量，鼓点瞬间画面"裂开"成彩色描边
        float off = amplitude * 0.015;
        float r = texture(texSampler, uv + vec2(-off,  0.0)).r;
        float g = rgba.g;
        float b = texture(texSampler, uv + vec2( off,  0.0)).b;
        return vec4(r, g, b, rgba.a);
    }
    else if (effectType == 9) {
        // 反色（Invert）：amplitude 驱动 0..1 过渡
        vec3 inverted = vec3(1.0) - rgba.rgb;
        return vec4(mix(rgba.rgb, inverted, amplitude), rgba.a);
    }
    else if (effectType == 10) {
        // 扫描线：水平条纹叠加（鼓点时变得更明显）
        float line = 0.5 + 0.5 * sin(uv.y * 400.0);  // ~200 条线
        float dim = 1.0 - amplitude * 0.6 * (1.0 - line);
        return vec4(rgba.rgb * dim, rgba.a);
    }
    else if (effectType == 11) {
        // 星轨隧道（Warp Tunnel / Radial Zoom Blur）
        // 消失点保持在画面中心附近，避免大幅漂移造成整屏混乱拖影。
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
            vec3 c = texture(texSampler, clamp(p, vec2(0.0), vec2(1.0))).rgb;
            float luma = dot(c, vec3(0.299, 0.587, 0.114));
            float keep = smoothstep(0.42, 0.90, luma);
            float w = (1.0 - f * 0.55) * edgeMask;
            streak = max(streak, c * keep * w);
        }
        float dist = length((uv - center) * vec2(1.0, 1.15));
        float dim = 1.0 - amplitude * 0.18 * smoothstep(0.20, 0.95, dist);
        vec3 tunneled = max(rgba.rgb * dim, streak * (1.0 + amplitude * 0.95));
        float mixAmt = amplitude * (0.28 + pulse * 0.37);
        return vec4(mix(rgba.rgb, tunneled, mixAmt), rgba.a);
    }
    else if (effectType == 26) {
        return vec4(logoShowLook(rgba.rgb, uv, t, amplitude, vec3(0.0)), rgba.a);
    }
    return rgba;
}

void main() {
    vec2 uv = fragTexCoord;
    vec4 rgba = texture(texSampler, uv);
    vec4 finalColor = rgba;

    float intensity = push.intensity;
    float amplitude = clamp(intensity * 2.0, 0.0, 1.0);
    float t = push.time;

    // 按顺序应用所有效果（真正的并行叠加）
    finalColor = applyEffect(finalColor, push.effectType,  amplitude, uv, t);
    finalColor = applyEffect(finalColor, push.effectType2, amplitude, uv, t);
    finalColor = applyEffect(finalColor, push.effectType3, amplitude, uv, t);
    finalColor = applyEffect(finalColor, push.effectType4, amplitude, uv, t);

    // 应用UI层透明度
    outColor = vec4(finalColor.rgb, finalColor.a * push.alpha);
}
