#version 450
// Owned Vulkan1.0 fixture: solid red output for exact UNORM pixel comparisons.
// Generate: glslangValidator -V --target-env vulkan1.0 --vn TriangleFragmentShader tests/vgpu/shaders/triangle.frag -o build/triangle_fragment_generated.h
layout(location = 0) out vec4 output_color;
void main() {
    output_color = vec4(1.0, 0.0, 0.0, 1.0);
}
