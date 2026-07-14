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
#define pc_dmxR (pc.dmxParams.x)
#define pc_dmxG (pc.dmxParams.y)
#define pc_dmxB (pc.dmxParams.z)
#define pc_dmxEnabled (uint(pc.dmxParams.w))

const uint REGION_AA_LEFT = 0x1u;
const uint REGION_AA_RIGHT = 0x2u;
const uint REGION_AA_TOP = 0x4u;
const uint REGION_AA_BOTTOM = 0x8u;

float regionEdgeCoverage(vec2 localPos) {
    vec2 pxLocal = 1.0 / max(pc.rotSinQrSize.zw, vec2(1.0));
    float cov = 1.0;
    if ((pc.aaEdgeFlags & REGION_AA_LEFT) != 0u) {
        cov *= smoothstep(-pxLocal.x, 0.0, localPos.x);
    }
    if ((pc.aaEdgeFlags & REGION_AA_RIGHT) != 0u) {
        cov *= 1.0 - smoothstep(1.0, 1.0 + pxLocal.x, localPos.x);
    }
    if ((pc.aaEdgeFlags & REGION_AA_TOP) != 0u) {
        cov *= smoothstep(-pxLocal.y, 0.0, localPos.y);
    }
    if ((pc.aaEdgeFlags & REGION_AA_BOTTOM) != 0u) {
        cov *= 1.0 - smoothstep(1.0, 1.0 + pxLocal.y, localPos.y);
    }
    return clamp(cov, 0.0, 1.0);
}

void main() {
    vec2 regionSize = pc.regionRect.zw - pc.regionRect.xy;
    vec2 uv01 = clamp(fragUV, vec2(0.0), vec2(1.0));
    vec2 canvasUV = clamp(pc.regionRect.xy + uv01 * regionSize,
                          vec2(0.0), vec2(1.0));
    vec3 rgb = texture(canvasTexture, canvasUV).rgb;

    rgb = (rgb - 0.5) * pc_contrast + 0.5;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luma), rgb, pc_saturation);
    vec3 result = rgb * pc_luminance;

    if (pc_dmxEnabled != 0u) {
        result *= vec3(pc_dmxR, pc_dmxG, pc_dmxB);
    }

    result *= regionEdgeCoverage(fragLocalPos);
    outColor = vec4(result, 1.0);
}
