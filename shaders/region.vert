#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec2 fragGridPos;
layout(location = 2) out vec2 fragScreenPos;
layout(location = 3) out vec2 fragDeformedPos;  // 变形后的位置（用于热点和高亮）


layout(std140, set = 0, binding = 1) uniform CaveParams {
    mat4 view;          // 偏移 0
    mat4 proj;          // 偏移 64
    vec4 corners[4];    // 偏移 128
    vec4 logicalOutputRect; // 偏移 192 （移到此处以绕过 128 字节限制）
    vec4 projParams;    // 偏移 208: [offsetX, offsetY, scaleX, scaleY]
    vec4 projParams2;   // 偏移 224: [rotate, keystoneX, keystoneY, projFlags]
    vec4 edgeSlope;     // 偏移 240: [slopeL, slopeR, slopeT, slopeB]
    vec4 stripStartEndH;
    vec4 stripStartEndV;
    vec4 edgeAnchor;
    vec4 outputSize;
    vec4 selectedPoints;
    vec4 blendBrightR;
    vec4 blendBrightG;
    vec4 blendBrightB;
    vec4 maskMeta;
    vec4 maskBounds;
    vec4 maskPolygon[64];
} cave;

layout(push_constant) uniform PushConstants {
    vec4 regionRect;       // 偏移 0
    vec4 outputRect;       // 偏移 16
    vec4 lumContSatRot;    // 偏移 32: [luminance, contrast, saturation, rotationRadians]
    uint regionIdx;        // 偏移 48
    uint gridFlags;        // 偏移 52
    uint maskFlags;        // 偏移 56
    uint showLicenseWatermark; // 偏移 60
    vec4 blendParams;      // 偏移 64
    vec4 intensityGamma;   // 偏移 80: [强度, γL, γR, γT]
    float gammaBottom;     // 偏移 96
    uint activeRegionId, _pad98, _pad99;
    vec4 dmxParams;        // 偏移 112: [dmxR, dmxG, dmxB, enabled]
} pc;

#define pc_luminance (pc.lumContSatRot.x)
#define pc_contrast (pc.lumContSatRot.y)
#define pc_saturation (pc.lumContSatRot.z)
#define pc_rotationRadians (pc.lumContSatRot.w)

#define pc_projOffsetX (cave.projParams.x)
#define pc_projOffsetY (cave.projParams.y)
#define pc_projScaleX (cave.projParams.z)
#define pc_projScaleY (cave.projParams.w)
#define pc_projRotate (cave.projParams2.x)
#define pc_projKeystoneX (cave.projParams2.y)
#define pc_projKeystoneY (cave.projParams2.z)
#define pc_projFlags (uint(cave.projParams2.w))

#define pc_logicalOutputRect (cave.logicalOutputRect)
#define pc_dmxR (pc.dmxParams.x)
#define pc_dmxG (pc.dmxParams.y)
#define pc_dmxB (pc.dmxParams.z)
#define pc_dmxEnabled (uint(pc.dmxParams.w))

const uint REGION_AA_LEFT = 0x1u;
const uint REGION_AA_RIGHT = 0x2u;
const uint REGION_AA_TOP = 0x4u;
const uint REGION_AA_BOTTOM = 0x8u;

// 顶点位置为变形后的 (pu,pv)，纹理坐标保持规则 (fu,fv) 用于视频/融合/遮罩采样。
void main() {
    vec2 outSize = pc.outputRect.zw;
    vec2 outPos = pc.outputRect.xy;

    vec2 pos01 = inPos * 0.5 + 0.5;  // 变形位置 (pu,pv)，几何/遮罩一致
    vec2 uvCoord = inUV;             // 规则 (fu,fv) 用于采样

    // 单采样光栅化无法覆盖像素中心刚好落在
    // outside a polygon. Expand 仅 visible outside edges by about one pixel
    // 再由 region.frag 的覆盖渐变把真实边缘拉回 uv=0/1。
    vec2 outputPixels = max(cave.outputSize.zw, vec2(1.0));
    bool visibleLeft = (pc._pad99 & REGION_AA_LEFT) != 0u;
    bool visibleRight = (pc._pad99 & REGION_AA_RIGHT) != 0u;
    bool visibleTop = (pc._pad99 & REGION_AA_TOP) != 0u;
    bool visibleBottom = (pc._pad99 & REGION_AA_BOTTOM) != 0u;
    vec2 pxLocal = 1.0 / max(abs(outSize) * outputPixels, vec2(1.0));
    const float aaPixels = 1.0;
    const float edgeVertexEps = 1e-6;
    if (visibleLeft && inUV.x <= edgeVertexEps) {
        pos01.x -= pxLocal.x * aaPixels;
        uvCoord.x -= pxLocal.x * aaPixels;
    }
    if (visibleRight && inUV.x >= 1.0 - edgeVertexEps) {
        pos01.x += pxLocal.x * aaPixels;
        uvCoord.x += pxLocal.x * aaPixels;
    }
    if (visibleTop && inUV.y <= edgeVertexEps) {
        pos01.y -= pxLocal.y * aaPixels;
        uvCoord.y -= pxLocal.y * aaPixels;
    }
    if (visibleBottom && inUV.y >= 1.0 - edgeVertexEps) {
        pos01.y += pxLocal.y * aaPixels;
        uvCoord.y += pxLocal.y * aaPixels;
    }

    vec2 finalPos = outPos + pos01 * outSize;
    vec2 posNDC = finalPos * 2.0 - 1.0;

    if ((pc_projFlags & 2u) != 0u) {
        // CAVE 离轴投影：用 pos01 (pu,pv) 作 u,v 参与 3D 插值
        float u = pos01.x, v = pos01.y;
        vec3 ll = cave.corners[0].xyz;
        vec3 lr = cave.corners[1].xyz;
        vec3 ul = cave.corners[2].xyz;
        vec3 ur = cave.corners[3].xyz;
        vec3 pos3D = (1.0 - v) * ((1.0 - u) * ll + u * lr) + v * ((1.0 - u) * ul + u * ur);
        vec4 clipPos = cave.proj * cave.view * vec4(pos3D, 1.0);
        gl_Position = clipPos;
        fragScreenPos = clipPos.xy / clipPos.w * 0.5 + 0.5;
    } else {
        if ((pc_projFlags & 1u) != 0u) {
            // 使用透视除法模拟旧项目中的投影校正效果
            float c = cos(pc_projRotate);
            float s = sin(pc_projRotate);
            vec2 p = posNDC;

            // 模拟 3D 透视投影
            float wk = 1.0 + pc_projKeystoneX * p.y + pc_projKeystoneY * p.x;
            p /= wk;

            p *= vec2(pc_projScaleX, pc_projScaleY);
            p = vec2(p.x * c - p.y * s, p.x * s + p.y * c);
            p += vec2(pc_projOffsetX, pc_projOffsetY);
            posNDC = p;
        }
        // 融合带与 contentLocal 使用统一的输出矩形 0-1 空间，避免内容区被误判为融合带
        fragScreenPos = finalPos;
        gl_Position = vec4(posNDC, 0.0, 1.0);
    }

    fragUV = uvCoord;
    fragGridPos = uvCoord;  // 使用逻辑坐标绘制网格，使网格随顶点变形同步
    fragDeformedPos = pos01;  // 热点位置仍使用物理 0-1
}
