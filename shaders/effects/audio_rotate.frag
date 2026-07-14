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
    // 计算旋转角度
    float angle = ubo.time * ubo.rotationSpeed * ubo.intensity;
    
    // 将坐标移到中心
    vec2 center = vec2(0.5, 0.5);
    vec2 coord = fragTexCoord - center;
    
    // 旋转矩阵
    float cosAngle = cos(angle);
    float sinAngle = sin(angle);
    vec2 rotatedCoord = vec2(
        coord.x * cosAngle - coord.y * sinAngle,
        coord.x * sinAngle + coord.y * cosAngle
    );
    
    // 移回原位置
    rotatedCoord += center;
    
    // 采样纹理（边界外返回透明）
    if (rotatedCoord.x < 0.0 || rotatedCoord.x > 1.0 || 
        rotatedCoord.y < 0.0 || rotatedCoord.y > 1.0) {
        outColor = vec4(0.0);
    } else {
        outColor = texture(inputTexture, rotatedCoord);
    }
}
