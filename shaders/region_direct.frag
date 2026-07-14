#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragScreenPos;
layout(location = 2) in vec2 fragLocalPos;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D canvasTexture;

layout(push_constant) uniform PushConstants {
    vec4 regionRect;
    vec4 outputRect;
    vec4 lumContSatRot;
    vec4 rotSinQrSize;
    vec4 dmxParams;
    uint regionIdx;
    uint showLicenseWatermark;
    uint aaEdgeFlags;
    uint _pad0;
} pc;

#define pc_luminance (pc.lumContSatRot.x)
#define pc_contrast (pc.lumContSatRot.y)
#define pc_saturation (pc.lumContSatRot.z)

void main() {
    vec3 rgb = texture(canvasTexture, clamp(fragUV, vec2(0.0), vec2(1.0))).rgb;
    rgb = (rgb - 0.5) * pc_contrast + 0.5;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luma), rgb, pc_saturation);
    outColor = vec4(rgb * pc_luminance, 1.0);
}
