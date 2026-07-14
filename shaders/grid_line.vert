#version 450

// 顶点输入：位置 (0-1 纹理空间，已经过几何变形) + 纹理坐标 (1 或 -1) + 行列索引
layout(location = 0) in vec2 inPos;
layout(location = 1) in float inTexCoord;
layout(location = 2) in int inLineRow;
layout(location = 3) in int inLineCol;
layout(location = 4) in vec2 inOffset;

// 输出到 fragment shader
layout(location = 0) out float fragTexCoord;
layout(location = 1) out flat int fragLineRow;
layout(location = 2) out flat int fragLineCol;
layout(location = 3) out vec2 fragScreenPos;  // 屏幕位置 (0-1)
layout(location = 4) out vec2 fragLocalPos;
layout(location = 5) out flat int fragVertexKind; // 字段说明：0=点，1=线，2=叠加层

// UBO: 获取投影参数
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

// 推送常量：与 region.vert 保持严格对齐
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

#define pc_projOffsetX (cave.projParams.x)
#define pc_projOffsetY (cave.projParams.y)
#define pc_projScaleX (cave.projParams.z)
#define pc_projScaleY (cave.projParams.w)
#define pc_projRotate (cave.projParams2.x)
#define pc_projKeystoneX (cave.projParams2.y)
#define pc_projKeystoneY (cave.projParams2.z)
#define pc_projFlags (uint(cave.projParams2.w))

void main() {
    // inPos 已经是变形后的位置（0-1 纹理空间）
    // 直接映射到 outputRect，然后转换到 NDC 空间

    bool isMaskGrid = (pc._pad99 & 0x80000000u) != 0u;
    vec2 outSize = pc.outputRect.zw;
    vec2 outPos = pc.outputRect.xy;

    bool isPointVertex = abs(inTexCoord) < 0.5;
    vec2 sourcePos = inPos + inOffset;
    vec2 localPos = sourcePos;

    if (isMaskGrid) {
        // C++ 端已经把全局 ZheZhao 输入幕布点阵按“当前投影 sourceRect + 几何网格”
        // 采样成当前投影的变形后局部坐标。这里禁止再按输出矩阵或整张幕布重新换算。
        localPos = sourcePos;
    }

    vec2 finalPos = outPos + localPos * outSize;

    // 转换到 NDC 空间 [-1, 1]
    vec2 posNDC = finalPos * 2.0 - 1.0;
    gl_Position = vec4(posNDC, 0.0, 1.0);

    // 应用投影校正 (与 region.vert 严格对齐)
    if ((pc_projFlags & 2u) != 0u) {
        float u = localPos.x;
        float v = localPos.y;
        vec3 ll = cave.corners[0].xyz;
        vec3 lr = cave.corners[1].xyz;
        vec3 ul = cave.corners[2].xyz;
        vec3 ur = cave.corners[3].xyz;
        vec3 pos3D = (1.0 - v) * ((1.0 - u) * ll + u * lr) + v * ((1.0 - u) * ul + u * ur);
        vec4 clipPos = cave.proj * cave.view * vec4(pos3D, 1.0);
        gl_Position = clipPos;
    } else if ((pc_projFlags & 1u) != 0u) {
        float c = cos(pc_projRotate);
        float s = sin(pc_projRotate);
        vec2 p = posNDC;

        // 模拟 3D 透视投影
        float wk = 1.0 + pc_projKeystoneX * p.y + pc_projKeystoneY * p.x;
        p /= wk;

        p *= vec2(pc_projScaleX, pc_projScaleY);
        p = vec2(p.x * c - p.y * s, p.x * s + p.y * c);
        p += vec2(pc_projOffsetX, pc_projOffsetY);
        gl_Position = vec4(p, 0.0, 1.0);
    }

    fragTexCoord = inTexCoord;
    fragLineRow = inLineRow;
    fragLineCol = inLineCol;
    fragLocalPos = localPos;
    fragVertexKind = abs(inTexCoord - 2.0) < 0.5 ? 2 : (abs(inTexCoord) < 0.5 ? 0 : 1);

    // 点顶点(inTexCoord==0): fragScreenPos 存归一化偏移用于圆形裁剪
    // 线段/overlay 顶点: fragScreenPos 存屏幕位置用于 overlay 渲染
    if (abs(inTexCoord) < 0.5) {
        float radiusX = abs(inOffset.x);
        float radiusY = abs(inOffset.y);
        fragScreenPos = vec2(
            radiusX > 1e-7 ? inOffset.x / radiusX : 0.0,
            radiusY > 1e-7 ? inOffset.y / radiusY : 0.0
        );
    } else {
        fragScreenPos = finalPos;
    }
}
