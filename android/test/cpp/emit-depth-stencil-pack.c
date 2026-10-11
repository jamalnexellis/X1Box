/* Emit the production D24 integer-pack shader for generated pixel fixtures. */
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/depth-stencil-pack-glsl.h"

int main(void)
{
    puts("#version 450\nlayout(local_size_x = 256) in;");
    fputs(xemu_vk_pack_d24_glsl, stdout);
    return ferror(stdout) ? 1 : 0;
}
