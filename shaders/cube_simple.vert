#version 450

// Per-vertex input
layout(location = 0) in vec3 inPosition;

// Per-instance input
layout(location = 1) in vec3 instancePosition;
layout(location = 2) in vec3 instanceScale;
layout(location = 3) in vec4 instanceColor;

// Output
layout(location = 0) out vec4 fragColor;

void main() {
    // Apply instance transform
    vec3 worldPos = inPosition * instanceScale + instancePosition;

    // Simple orthographic projection (no camera matrix needed for testing)
    // Map world space [-10, 10] to clip space [-1, 1]
    gl_Position = vec4(worldPos.x * 0.1, worldPos.y * 0.1, worldPos.z * 0.1, 1.0);

    fragColor = instanceColor;
}
