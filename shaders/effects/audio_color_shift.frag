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
    vec4 color = texture(inputTexture, fragTexCoord);
    
    // 根据频率能量调整RGB分量
    float r = color.r * (1.0 + ubo.lowFreq * 0.5 * ubo.intensity);
    float g = color.g * (1.0 + ubo.midFreq * 0.5 * ubo.intensity);
    float b = color.b * (1.0 + ubo.highFreq * 0.5 * ubo.intensity);
    
    // 保持亮度平衡
    float maxComponent = max(max(r, g), b);
    if (maxComponent > 1.0) {
        r /= maxComponent;
        g /= maxComponent;
        b /= maxComponent;
    }
    
    outColor = vec4(r, g, b, color.a);
}
