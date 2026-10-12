#version 450
layout(location=0) flat in uvec4 result;
layout(set=0,binding=1,std140) uniform B { uvec4 b; };
layout(location=0) out vec4 outColor;
void main() { outColor=vec4(result+b)/255.0; }
