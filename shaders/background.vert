#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inGridPos;

layout(location = 0) out vec2 fragTexCoord;

layout(push_constant) uniform PushConstants {
    vec4 regionRect;
    vec4 outputRect;
    vec4 lumContSatRot;
    uint regionIdx;
} pc;

void main() {
    vec2 pos01 = inPos * 0.5 + 0.5;
    vec2 posNDC = (pc.outputRect.xy + pos01 * pc.outputRect.zw) * 2.0 - 1.0;
    gl_Position = vec4(posNDC, 0.0, 1.0);
    fragTexCoord = pc.regionRect.xy + inGridPos * pc.regionRect.zw;
}
