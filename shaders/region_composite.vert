#version 450

/**
 * 区域合成顶点着色器
 * 
 * 功能：渲染全屏四边形，用于从 canvas buffer 直接采样并合成到输出布局
 * 零拷贝方案：不需要中间纹理，直接从 canvas buffer 采样
 */

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec2 fragScreenPos;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
    // 传递屏幕坐标 (0-1 范围)
    fragScreenPos = inTexCoord;
}

