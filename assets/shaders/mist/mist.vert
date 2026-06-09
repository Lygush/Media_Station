#version 440 core

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 coord;

// Единственный UBO — Qt сам кладёт сюда qt_Matrix и qt_Opacity,
// наши переменные добавляются следом в тот же блок.
layout(std140, binding = 0) uniform buf {
    mat4  qt_Matrix;
    float qt_Opacity;
    float time;
    float scale;
    float speed;
};

void main() {
    coord       = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
}
