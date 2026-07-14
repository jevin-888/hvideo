#version 450
precision highp float;

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    float alpha;
    float intensity;
    int effectType;
    float time;
    int effectType2;
    int effectType3;
    int effectType4;
    float reserved;
} pc;

void main() {
    // ============================================================
    // 标准 Dual Kawase 下采样 (Standard Dual Filter Downsample)
    // 参考: Marius Bjørge - "Band宽度-Efficient Rendering" (SIGGRAPH 2015)
    // ============================================================

    vec2 texSize = vec2(textureSize(u_texture, 0));
    vec2 texelSize = 1.0 / texSize;

    // 使用固定的半像素偏移 - 这是 Dual Filter 的关键
    // 硬件双线性过滤会自动平均4个texel
    float offsetScale = clamp(pc.intensity, 1.0, 2.2);
    vec2 halfPixel = texelSize * 0.5 * offsetScale;

    // 5-Tap 采样模式:
    // 中心 (权重4) + 四角 (各权重1) = 总权重8
    //
    //     [1]   [1]
    //       . .
    //       示例/字段：[4]  <- center
    //       . .
    //     [1]   [1]

    vec4 sum = texture(u_texture, v_texCoord) * 4.0;

    sum += texture(u_texture, v_texCoord - halfPixel);                    // 左上
    sum += texture(u_texture, v_texCoord + halfPixel);                    // 右下
    sum += texture(u_texture, v_texCoord + vec2(halfPixel.x, -halfPixel.y)); // 右上
    sum += texture(u_texture, v_texCoord + vec2(-halfPixel.x, halfPixel.y)); // 左下

    outColor = sum / 8.0;
}
