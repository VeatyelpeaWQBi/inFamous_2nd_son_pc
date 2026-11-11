// SPDX-License-Identifier: GPL-2.0-or-later
#version 460

layout(set = 0, binding = 0, std430) buffer Output {
    uint words[];
};

void main() {
    const uint vertices = 4096 * 3;
    uint word = (uint(gl_InstanceIndex) * vertices + uint(gl_VertexIndex)) * 5;
    words[word + 0] = uint(gl_VertexIndex);
    words[word + 1] = uint(gl_InstanceIndex);
    words[word + 2] = uint(gl_BaseVertex);
    words[word + 3] = uint(gl_BaseInstance);
    words[word + 4] = uint(gl_DrawID);
    gl_Position = vec4(0, 0, 0, 1);
}
