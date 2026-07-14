#version 450

layout(binding = 0) uniform sampler2D inputTexture;

// 统一的音频效果 Uniform Buffer 布局
layout(binding = 1) uniform AudioEffectUniform {
    float intensity;
    float time;
    float waveAmplitude;
    float waveFrequency;
    float rotationSpeed;
    float scaleFactor;
    float lowFreq;
    float midFreq;
    float highFreq;
    float reserved;
    float fftData[128];
} ubo;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // 将坐标移到中心
    vec2 center = vec2(0.5, 0.5);
    vec2 coord = fragTexCoord - center;
    
    // 应用基于音频强度的缩放
    float scale = ubo.scaleFactor + (ubo.intensity * 0.5);
    coord = coord / scale;
    
    // 移回原位置
    coord += center;
    
    // 采样纹理（边界外返回透明）
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 0.0);
    } else {
        outColor = texture(inputTexture, coord);
    }
}
