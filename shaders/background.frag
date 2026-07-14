#version 450

// 背景图片片段着色器
// 简单地采样纹理并输出

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D backgroundTexture;

void main() {
    // 正常纹理采样
    outColor = texture(backgroundTexture, fragTexCoord);
}
