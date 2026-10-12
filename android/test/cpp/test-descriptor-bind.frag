#version 450
layout(set=0, binding=0, std140) uniform A { uvec4 a; };
layout(set=0, binding=1, std140) uniform B { uvec4 b; };
layout(location=0) out vec4 outColor;
void main() { outColor = vec4(a + b) / 255.0; }
