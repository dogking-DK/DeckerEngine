# Disk rendering fixture

Original DeckerEngine test data: two indexed quads, a 2x1 RGBA PNG with a transparent left texel,
and a version-1 Project/Scene with a parent and two instances of the masked mesh.
The front mesh has factor alpha 0.5 and cutoff 0.5, so its right texel survives at equality;
the left texel must leave the farther blue mesh visible in both depth and color passes.
Positions use interleaved float32 position/normal/UV; indices are uint32 little-endian.
