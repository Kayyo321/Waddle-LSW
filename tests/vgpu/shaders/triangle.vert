#version 450
// Owned Vulkan1.0 fixture: no vertex input; positions come from gl_VertexIndex.
// Generate: glslangValidator -V --target-env vulkan1.0 --vn TriangleVertexShader tests/vgpu/shaders/triangle.vert -o build/triangle_vertex_generated.h
void main() {
    vec2 positions[3] = vec2[3](vec2(-0.75, -0.75), vec2(0.75, -0.75), vec2(0.0, 0.75));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
