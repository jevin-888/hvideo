#version 450

layout(location = 0) in float fragTexCoord;
layout(location = 1) in flat int fragLineRow;
layout(location = 2) in flat int fragLineCol;
layout(location = 3) in vec2 fragScreenPos;
layout(location = 4) in vec2 fragLocalPos;
layout(location = 5) in flat int fragVertexKind;

#define GRID_HOTSPOT_OUTLINE_SCALE 1.4166667
#define GRID_HOTSPOT_OUTLINE_RADIUS uintBitsToFloat(pc._pad98)
#define GRID_HOTSPOT_RADIUS (GRID_HOTSPOT_OUTLINE_RADIUS / GRID_HOTSPOT_OUTLINE_SCALE)
#define SELECTED_GRID_LINE_COLOR vec3(0.0, 1.0, 0.0)
#define SELECTED_MASK_GRID_LINE_COLOR vec3(1.0, 0.0, 1.0)

layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 1) uniform CaveParams {
    mat4 view;
    mat4 proj;
    vec4 corners[4];
    vec4 logicalOutputRect;
    vec4 projParams;
    vec4 projParams2;
    vec4 edgeSlope;
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
    vec4 regionRect;
    vec4 outputRect;
    vec4 lumContSatRot;
    uint regionIdx;
    uint gridFlags;
    uint maskFlags;
    uint showLicenseWatermark;
    vec4 blendParams;
    vec4 intensityGamma;
    float gammaBottom;
    uint activeRegionId, _pad98, _pad99;
    vec4 dmxParams;
} pc;

#define pc_logicalOutputRect (cave.logicalOutputRect)
#define pc_selectedPointScreenPos (cave.selectedPoints.xy)
#define pc_selectedMaskPointScreenPos (cave.selectedPoints.zw)

int getSelectedRow() {
    return int((pc.gridFlags >> 13) & 0xFFu) - 128;
}

int getSelectedCol() {
    return int((pc.gridFlags >> 21) & 0xFFu) - 128;
}

int getSelectedMaskRow() {
    return int((pc.maskFlags >> 14) & 0xFFu) - 128;
}

int getSelectedMaskCol() {
    return int((pc.maskFlags >> 22) & 0xFFu) - 128;
}

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

float sdDigit(vec2 p, int digit) {
    float d = 1.0;
    if (digit == 0 || digit == 2 || digit == 3 || digit == 5 || digit == 6 || digit == 7 || digit == 8 || digit == 9) d = min(d, sdSegment(p, vec2(0.22, 0.86), vec2(0.78, 0.86)));
    if (digit == 0 || digit == 4 || digit == 5 || digit == 6 || digit == 8 || digit == 9) d = min(d, sdSegment(p, vec2(0.20, 0.50), vec2(0.20, 0.82)));
    if (digit == 0 || digit == 1 || digit == 2 || digit == 3 || digit == 4 || digit == 7 || digit == 8 || digit == 9) d = min(d, sdSegment(p, vec2(0.80, 0.50), vec2(0.80, 0.82)));
    if (digit == 2 || digit == 3 || digit == 4 || digit == 5 || digit == 6 || digit == 8 || digit == 9) d = min(d, sdSegment(p, vec2(0.24, 0.50), vec2(0.76, 0.50)));
    if (digit == 0 || digit == 2 || digit == 6 || digit == 8) d = min(d, sdSegment(p, vec2(0.20, 0.18), vec2(0.20, 0.50)));
    if (digit == 0 || digit == 1 || digit == 3 || digit == 4 || digit == 5 || digit == 6 || digit == 7 || digit == 8 || digit == 9) d = min(d, sdSegment(p, vec2(0.80, 0.18), vec2(0.80, 0.50)));
    if (digit == 0 || digit == 2 || digit == 3 || digit == 5 || digit == 6 || digit == 8 || digit == 9) d = min(d, sdSegment(p, vec2(0.22, 0.14), vec2(0.78, 0.14)));
    return d;
}

vec4 renderRegionNumberOverlay() {
    int regionNumber = int(pc.regionIdx);
    int tens = regionNumber / 10;
    int ones = regionNumber - tens * 10;
    float digitScale = 0.034;
    vec2 contentRect = (pc_logicalOutputRect.z > 1e-6 && pc_logicalOutputRect.w > 1e-6)
        ? pc_logicalOutputRect.xy : pc.outputRect.xy;
    vec2 contentSize = (pc_logicalOutputRect.z > 1e-6 && pc_logicalOutputRect.w > 1e-6)
        ? pc_logicalOutputRect.zw : pc.outputRect.zw;
    vec2 labelFragPos = (fragScreenPos - contentRect) / max(contentSize, vec2(1e-6));
    vec2 labelCenter = clamp(vec2(0.5, 0.5), vec2(0.045), vec2(0.955));
    vec2 labelLocal = (labelFragPos - labelCenter) / digitScale;
    float badgeDist = length(labelLocal) - ((regionNumber >= 10) ? 1.36 : 1.10);
    float digitDist = 1.0;
    if (regionNumber >= 10) {
        vec2 p0 = vec2(labelLocal.x + 1.02, 0.50 - labelLocal.y);
        vec2 p1 = vec2(labelLocal.x + 0.02, 0.50 - labelLocal.y);
        if (p0.x >= -0.2 && p0.x <= 1.2 && p0.y >= -0.2 && p0.y <= 1.2) digitDist = min(digitDist, sdDigit(p0, tens));
        if (p1.x >= -0.2 && p1.x <= 1.2 && p1.y >= -0.2 && p1.y <= 1.2) digitDist = min(digitDist, sdDigit(p1, ones));
    } else {
        float digitCenterX = (ones == 1) ? 0.80 : 0.50;
        vec2 p = vec2(labelLocal.x + digitCenterX, 0.50 - labelLocal.y);
        if (p.x >= -0.2 && p.x <= 1.2 && p.y >= -0.2 && p.y <= 1.2) digitDist = sdDigit(p, ones);
    }
    float badgeAlpha = 1.0 - smoothstep(0.00, 0.16, badgeDist);
    float outlineAlpha = 1.0 - smoothstep(0.115, 0.17, digitDist);
    float digitAlpha = 1.0 - smoothstep(0.055, 0.092, digitDist);
    vec4 overlay = vec4(0.0);
    if (badgeAlpha > 0.001) overlay = vec4(0.02, 0.025, 0.035, badgeAlpha * 0.92);
    if (outlineAlpha > 0.001) overlay = vec4(mix(overlay.rgb, vec3(0.0), outlineAlpha), max(overlay.a, outlineAlpha));
    if (digitAlpha > 0.001) overlay = vec4(vec3(0.92, 0.96, 0.98), digitAlpha);
    return overlay;
}

void main() {
    bool isMaskGrid = (pc._pad99 & 0x80000000u) != 0u;
    bool isBlendManagerGrid = !isMaskGrid && ((pc.gridFlags & 0x40000000u) != 0u);
    // pc.regionIdx 携带真实的 1 基区域 ID，不要把它当成
    // the 渲染器 array index; matrix remaps can make those differ.
    bool isActiveRegion = pc.regionIdx == pc.activeRegionId;

    bool isPointVertex = fragVertexKind == 0;
    bool isOverlayVertex = fragVertexKind == 2;

    int selectedRow;
    int selectedCol;
    if (isMaskGrid) {
        selectedRow = getSelectedMaskRow();
        selectedCol = getSelectedMaskCol();
    } else {
        selectedRow = getSelectedRow();
        selectedCol = getSelectedCol();
    }

    bool hasSelection = selectedRow >= 0 && selectedCol >= 0;
    // 几何/融合的选中行列只属于当前投影；遮罩是旧项目 ZheZhao 的输入总幕布全局层，
    // 选中行/列必须按整张输入幕布显示，不能按当前 region 裁切后再决定是否高亮。
    bool canShowSelection = isMaskGrid || isActiveRegion;
    bool isSelectedLine = canShowSelection && hasSelection &&
                          ((fragLineRow == selectedRow && fragLineRow >= 0) ||
                           (fragLineCol == selectedCol && fragLineCol >= 0));
    if (isOverlayVertex) {
        bool showRegionNumber = !isMaskGrid && ((pc.gridFlags & 1u) != 0u);
        if (showRegionNumber) {
            vec4 overlay = renderRegionNumberOverlay();
            if (overlay.a > 0.001) {
                outColor = overlay;
                return;
            }
        }
        discard;
    }

    if (isPointVertex) {
        bool isSelectedPoint = canShowSelection &&
                               (fragLineRow == selectedRow && fragLineCol == selectedCol && hasSelection);
        if (!isMaskGrid && (!isActiveRegion || !isSelectedPoint)) {
            discard;
        }
        // fragScreenPos 以热点外圈半径归一化；选中点画外圈，普通遮罩点只保留内圈。
        float dist = length(fragScreenPos);
        float innerRadius = GRID_HOTSPOT_RADIUS / GRID_HOTSPOT_OUTLINE_RADIUS;
        float clipRadius = isSelectedPoint ? 1.0 : innerRadius;
        if (dist > clipRadius) discard;
        // 抗锯齿边缘
        float alpha = 1.0 - smoothstep(clipRadius * 0.85, clipRadius, dist);
        // 旧项目 ZheZhao 选中热点是红色点；选中热点所在行/列线条在下方统一画紫色。
        vec3 selectedPointColor = (isBlendManagerGrid || isMaskGrid) ? vec3(1.0, 0.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 normalPointColor = isMaskGrid ? vec3(0.0, 1.0, 1.0) : vec3(1.0, 1.0, 1.0);
        vec3 color = (isSelectedPoint && dist > innerRadius) ? vec3(1.0) : (isSelectedPoint ? selectedPointColor : normalPointColor);
        outColor = vec4(color, alpha);
        return;
    }

    if (canShowSelection && hasSelection) {
        vec2 selectedPoint = isMaskGrid ? pc_selectedMaskPointScreenPos : pc_selectedPointScreenPos;
        float distToPoint = length(fragLocalPos - selectedPoint);
        if (!isMaskGrid && (!isBlendManagerGrid || isActiveRegion) && distToPoint < GRID_HOTSPOT_RADIUS) {
            outColor = isBlendManagerGrid ? vec4(0.0, 1.0, 1.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
            return;
        }
        if (!isMaskGrid && distToPoint < GRID_HOTSPOT_OUTLINE_RADIUS) {
            outColor = vec4(1.0, 1.0, 1.0, 0.65);
            return;
        }
    }

    float d = abs(fragTexCoord);

    // 使用旧项目 jumu_fusion_player 的简单线性衰减算法（完全去除 f宽度）
    // 这个算法在 OpenGL ES 版本中表现完美，无锯齿
    float alpha = clamp(1.0 - d, 0.0, 1.0);

    vec3 color;
    if (isMaskGrid) {
        color = vec3(0.0, 1.0, 1.0);
        if (isSelectedLine) {
            color = SELECTED_MASK_GRID_LINE_COLOR;
        }
    } else if (isBlendManagerGrid) {
        color = isActiveRegion
            ? vec3(1.0, 1.0, 0.0)
            : vec3(1.0, 1.0, 1.0);
        if (isActiveRegion && isSelectedLine) {
            color = SELECTED_GRID_LINE_COLOR;
        }
    } else {
        color = (isActiveRegion && hasSelection)
            ? vec3(1.0, 1.0, 1.0)
            : vec3(0.74, 0.74, 0.74);
        if (isActiveRegion && isSelectedLine) {
            color = SELECTED_GRID_LINE_COLOR;
        }
    }

    outColor = vec4(color, alpha);
}
