#ifndef XEMU_VK_DEPTH_STENCIL_PACK_GLSL_H
#define XEMU_VK_DEPTH_STENCIL_PACK_GLSL_H
/* Pack the raw D24 integer representation, preserving all 24 depth bits. */
static const char *const xemu_vk_pack_d24_glsl =
    "layout(push_constant) uniform PushConstants { uint width_in, width_out; };\n"
    "layout(set = 0, binding = 0) readonly buffer DepthIn { uint depth_in[]; };\n"
    "layout(set = 0, binding = 1) readonly buffer StencilIn { uint stencil_in[]; };\n"
    "layout(set = 0, binding = 2) writeonly buffer DepthStencilOut { uint depth_stencil_out[]; };\n"
    "uint get_input_idx(uint idx_out) {\n"
    "    uint scale = width_in / width_out;\n"
    "    uint y = (idx_out / width_out) * scale;\n"
    "    uint x = (idx_out % width_out) * scale;\n"
    "    return y * width_in + x;\n"
    "}\n"
    "void main() {\n"
    "    uint idx_out = gl_GlobalInvocationID.x;\n"
    "    uint idx_in = get_input_idx(idx_out);\n"
    "    uint depth_value = depth_in[idx_in];\n"
    "    uint stencil_value = (stencil_in[idx_in / 4] >> ((idx_in % 4) * 8)) & 0xff;\n"
    "    depth_stencil_out[idx_out] = depth_value << 8 | stencil_value;\n"
    "}\n";
#endif
