#version 450

// Per-vertex input (from vertex buffer)
layout(location = 0) in vec3 inPosition;

// Per-instance input (from instance buffer)
layout(location = 1) in vec3 instancePosition;
layout(location = 3) in vec3 instanceScale;
layout(location = 2) in vec3 instanceRotation;
layout(location = 4) in vec4 instanceColor;

// Output to fragment shader
layout(location = 0) out vec4 fragColor;

// Uniform buffer for camera matrices (SDL3 Set 1 = vertex uniform buffers)
layout(set = 1, binding = 0) uniform CameraData {
    mat4 viewProj;
} camera;


mat3 rotationMatrix(vec3 rot) {
    float cx = cos(rot.x);
    float sx = sin(rot.x);
    float cy = cos(rot.y);
    float sy = sin(rot.y);
    float cz = cos(rot.z);
    float sz = sin(rot.z);

    mat3 rx = mat3(1, 0, 0, 0, cx, -sx, 0, sx, cx);
    mat3 ry = mat3(cy, 0, sy, 0, 1, 0, -sy, 0, cy);
    mat3 rz = mat3(cz, -sz, 0, sz, cz, 0, 0, 0, 1);

    return rz * ry * rx;
}
void main() {
    // Apply instance scale and position
    vec3 rotatedPos = rotationMatrix(instanceRotation) * (inPosition * instanceScale);
    vec3 worldPos = rotatedPos + instancePosition;

    // Transform to clip space
    gl_Position = camera.viewProj * vec4(worldPos, 1.0);

    // Pass color to fragment shader
    fragColor = instanceColor;
}
