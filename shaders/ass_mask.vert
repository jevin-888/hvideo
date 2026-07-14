#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inRect;   // x, y, 宽度, 高度
layout(location = 3) in vec4 inUv;     // u0、v0、u 缩放、v 缩放
layout(location = 4) in vec4 inColor;  // r、g、b、a

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;      // a = 全局透明度
    vec4 cropInfo;   // x=targetW, y=targetH, z=screenRotateRad, w=未使用
    vec4 shapeInfo;
} pc;

void main() {
    vec2 local = inPosition * 0.5 + vec2(0.5);
    vec2 pixelPos = inRect.xy + local * inRect.zw;

    vec2 ndc = vec2(
        -1.0 + (2.0 * pixelPos.x / pc.cropInfo.x),
        -1.0 + (2.0 * pixelPos.y / pc.cropInfo.y)
    );

    if (abs(pc.cropInfo.z) > 0.0001) {
        float c = cos(pc.cropInfo.z);
        float s = sin(pc.cropInfo.z);
        ndc = vec2(c * ndc.x - s * ndc.y, s * ndc.x + c * ndc.y);
    }

    gl_Position = vec4(ndc, 0.0, 1.0);
    fragTexCoord = inUv.xy + local * inUv.zw;
    fragColor = vec4(inColor.rgb, inColor.a * pc.color.a);
}
