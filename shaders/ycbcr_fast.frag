#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;
    vec4 cropInfo;
    vec4 shapeInfo;
    vec4 userCrop;
    vec4 extEffect;
} pc;

void main() {
    vec2 uv = pc.userCrop.xy + fragTexCoord * pc.userCrop.zw;

    int rawMode = int(pc.shapeInfo.w + 0.5);
    int rotateMode = rawMode & 0xF0;
    int invertMode = rawMode & 0x0F;
    if (rotateMode == 0x10) {
        uv = vec2(1.0 - uv.x, 1.0 - uv.y);
    } else if (rotateMode == 0x20) {
        uv = vec2(uv.y, 1.0 - uv.x);
    } else if (rotateMode == 0x30) {
        uv = vec2(1.0 - uv.y, uv.x);
    }

    if (invertMode == 1) {
        uv.x = 1.0 - uv.x;
    } else if (invertMode == 2) {
        uv.y = 1.0 - uv.y;
    } else if (invertMode == 3) {
        uv = vec2(1.0) - uv;
    }

    outColor = texture(texSampler, uv) * pc.color;
}
