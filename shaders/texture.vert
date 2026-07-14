#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 color;
    vec4 cropInfo;   // x=纹理宽度, y=纹理高度, z=原始高度, w=裁剪 Y 偏移
    vec4 shapeInfo;  // x=形状类型, y=形状参数, z=黑色转透明, w=反转模式
    vec4 userCrop;
} pc;

void main() {
    gl_Position = pc.transform * vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
}
