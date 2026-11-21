# Rulings made while building map generation
- Task 3: Ruling: flatten eases the margin from the cells' bounding box (Chebyshev distance), not from each cell: footprints are rectangles, and per-cell distance would cost cells x area. Cost: an L-shaped footprint eases from its box.
