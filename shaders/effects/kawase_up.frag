#version 450
precision mediump int;
precision highp float;

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    float alpha;
    float intensity;  // > 0.5 表示这是最后一次 pass，应用玻璃效果
    int effectType;
    float time;
    int effectType2;
    int effectType3;
    int effectType4;
    float reserved;
} pc;

// 高质量抖动函数 - 消除色带
highp float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    // ============================================================
    // 标准 Dual Kawase 上采样 (Standard Dual Filter Upsample)
    // 参考: Marius Bjørge - "Band宽度-Efficient Rendering" (SIGGRAPH 2015)
    // ============================================================

    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 texelSize = 1.0 / texSize;
    float offsetScale = clamp(pc.intensity, 1.0, 2.2);
    vec2 halfPixel = texelSize * 0.5 * offsetScale;

    // 9-Tap 采样模式:
    // 中心 (权重4) + 上下左右 (各权重2) + 四角 (各权重1) = 总权重16
    //
    //     [1] [2] [1]
    //     [2] [4] [2]
    //     [1] [2] [1]

    vec4 sum = texture(u_texture, v_texCoord) * 4.0;

    // 上下左右 (权重2)
    sum += texture(u_texture, v_texCoord + vec2(-halfPixel.x * 2.0, 0.0)) * 2.0;
    sum += texture(u_texture, v_texCoord + vec2(0.0, halfPixel.y * 2.0)) * 2.0;
    sum += texture(u_texture, v_texCoord + vec2(halfPixel.x * 2.0, 0.0)) * 2.0;
    sum += texture(u_texture, v_texCoord + vec2(0.0, -halfPixel.y * 2.0)) * 2.0;

    // 四角 (权重1)
    sum += texture(u_texture, v_texCoord + vec2(-halfPixel.x, -halfPixel.y));
    sum += texture(u_texture, v_texCoord + vec2(halfPixel.x, -halfPixel.y));
    sum += texture(u_texture, v_texCoord + vec2(-halfPixel.x, halfPixel.y));
    sum += texture(u_texture, v_texCoord + vec2(halfPixel.x, halfPixel.y));

    vec4 result = sum / 16.0;
    vec2 farPixel = halfPixel * 3.0;
    vec4 farSum = vec4(0.0);
    farSum += texture(u_texture, v_texCoord + vec2(-farPixel.x, -farPixel.y));
    farSum += texture(u_texture, v_texCoord + vec2(farPixel.x, -farPixel.y));
    farSum += texture(u_texture, v_texCoord + vec2(-farPixel.x, farPixel.y));
    farSum += texture(u_texture, v_texCoord + vec2(farPixel.x, farPixel.y));
    result = mix(result, farSum * 0.25, 0.38);

    // ============================================================
    // iOS 玻璃质感增强 (仅在最后一次 pass 应用)
    // 通过 pc.特效类型 == 1 判断是否为最终 pass
    // ============================================================
    if (pc.effectType == 1) {
        // 1. 轻微提亮 - 产生"发光"的玻璃质感
        result.rgb = result.rgb * 1.03 + 0.015;

        // 2. 轻微饱和度提升 - 增加色彩通透感
        float luma = dot(result.rgb, vec3(0.2126, 0.7152, 0.0722));
        result.rgb = mix(vec3(luma), result.rgb, 1.08);

        // 3. 轻微材质白雾 - 压低背景细节，增强毛玻璃质感
        float veil = clamp((pc.intensity - 1.0) * 0.04, 0.0, 0.055);
        result.rgb = mix(result.rgb, vec3(1.0), veil);

        // 4. 高质量抖动 - 消除8位色彩断层，产生丝滑过渡
        float dither = (hash(v_texCoord * texSize) - 0.5) / 255.0;
        result.rgb += dither;
    }

    // 确保颜色在有效范围内
    result.rgb = clamp(result.rgb, 0.0, 1.0);

    outColor = vec4(result.rgb, result.a * pc.alpha);
}
