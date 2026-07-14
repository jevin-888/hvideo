#version 450

layout(binding = 0) uniform sampler2D inputTexture;

// 统一的音频效果 Uniform Buffer 布局
// 必须与 Vulkan渲染器::AudioEffectUniform 匹配
layout(binding = 1) uniform AudioEffectUniform {
    float intensity;      // 音频强度 [0, 1]
    float time;           // 时间（秒）
    float waveAmplitude;  // Wave: 振幅
    float waveFrequency;  // Wave: 频率
    float rotationSpeed;  // Rotate: 旋转速度
    float scaleFactor;    // Scale: 缩放因子
    float lowFreq;        // ColorShift: 低频能量
    float midFreq;        // ColorShift: 中频能量
    float highFreq;       // ColorShift: 高频能量
    float reserved;       // 对齐填充
    float fftData[128];   // Spectrum: FFT 数据
} ubo;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // 应用波形扭曲效果
    float waveOffset = sin(fragTexCoord.y * ubo.waveFrequency * 10.0 + ubo.time * 2.0) 
                     * ubo.waveAmplitude * ubo.intensity;
    vec2 distortedCoord = fragTexCoord + vec2(waveOffset, 0.0);
    
    // 确保坐标在有效范围内
    distortedCoord = clamp(distortedCoord, vec2(0.0), vec2(1.0));
    
    outColor = texture(inputTexture, distortedCoord);
}
