// The game's block textures: one layer per texture id, repeat addressing, so
// a merged quad tiles its texture once per block.
layout(set = 0, binding = 6) uniform sampler2DArray tiles;
