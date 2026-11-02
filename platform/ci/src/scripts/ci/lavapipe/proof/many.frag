#version 450
#extension GL_EXT_nonuniform_qualifier : enable
layout(set = 0, binding = 0) buffer S { uint v; } bufs[64];
layout(set = 0, binding = 1, r32ui) uniform uimage2D imgs[64];
void main() {
  for (int i = 0; i < 64; i++) {
    bufs[i].v = uint(i + 1);
    imageStore(imgs[i], ivec2(0, 0), uvec4(uint(i + 100)));
  }
}
