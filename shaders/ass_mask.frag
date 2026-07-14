#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

void main() {
    float mask = texture(texSampler, fragTexCoord).r;
    float alpha = fragColor.a * mask;
    if (alpha <= 0.001) {
        discard;
    }
    outColor = vec4(fragColor.rgb, alpha);
}
