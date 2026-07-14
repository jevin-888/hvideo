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
    
    // 根据x坐标获取对应的频段
    int bandIndex = int(fragTexCoord.x * 128.0);
    bandIndex = clamp(bandIndex, 0, 127);
    
    float freqIntensity = ubo.fftData[bandIndex] * ubo.intensity;
    
    // 在底部绘制频谱柱状图（叠加在原始图像上）
    float spectrumHeight = 0.15; // 频谱图高度
    float barHeight = freqIntensity * spectrumHeight;
    
    if (fragTexCoord.y < spectrumHeight && fragTexCoord.y < barHeight) {
        // 绘制频谱条
        float barValue = fragTexCoord.y / max(barHeight, 0.001);
        vec3 spectrumColor = vec3(
            1.0 - barValue,  // 红色在底部
            barValue * 0.5,  // 绿色在中间
            barValue         // 蓝色在顶部
        );
        outColor = vec4(spectrumColor * (freqIntensity + 0.3), 1.0);
    } else {
        // 显示原始颜色，可以根据频率调整亮度
        outColor = color * (1.0 + freqIntensity * 0.2);
    }
}
