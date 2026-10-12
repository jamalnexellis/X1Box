#version 450
layout(location=0) out vec4 outputColor;
void main() {
    vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],0,1);
    outputColor=vec4(0,1,0,1);
}
