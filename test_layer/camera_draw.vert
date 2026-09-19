#version 450
layout(set=0,binding=29,std140) uniform Camera {
    mat4 objectTransform;
    mat4 view;
    mat4 projection;
    mat4 inverseView;
    vec4 origin;
    mat4 previousObject;
    mat4 previousView;
    mat4 previousProjection;
} camera;
void main() {
    // This owned test shader actually reads the candidate uniform binding.
    gl_Position = vec4(float(gl_VertexIndex & 1), float(gl_VertexIndex & 2), 0, 1);
    gl_Position.x += camera.objectTransform[0][0] * 0.00001;
}
