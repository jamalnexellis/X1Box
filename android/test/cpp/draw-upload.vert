#version 450
layout(location=0) in vec2 position;
layout(location=1) in uvec4 color;
layout(set=0,binding=0,std140) uniform A { uvec4 a; };
layout(location=0) flat out uvec4 result;
void main() { gl_Position=vec4(position,0,1); result=color+a; }
