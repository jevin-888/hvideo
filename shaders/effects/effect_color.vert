#version 450

// 效果顶点着色器 - 与标准渲染相同
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    float alpha;
    float intensity;  // 音频强度
    int effectType;   // 效果类型
    float time;       // 时间
} push;

void main() {
    gl_Position = push.transform * vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
}
