# Rulings made while building map generation
- Task 3: Ruling: flatten eases the margin from the cells' bounding box (Chebyshev distance), not from each cell: footprints are rectangles, and per-cell distance would cost cells x area. Cost: an L-shaped footprint eases from its box.
- Task 10: Ruling (slice): MapData holds only what the slice's stages need (heights, water, weirs, sinks, reports); roads, lots, buildings, pieces, trees, lava and sites join it with their tasks. The map is centred on the origin. Cost: none.
