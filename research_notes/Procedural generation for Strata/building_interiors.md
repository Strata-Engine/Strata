# Procedural Building Interiors: Floor Plans, Rooms, Furnishing, and Simulation Data (for a voxel engine targeting Shadows-of-Doubt-style games)

Scope note: Everything below is from primary sources where I could reach them (papers read in full: Merrell et al. 2010, Yu et al. 2011, PCG Book ch. 3; Shadows of Doubt devblogs #13, #14, #15, #21, #22; Boris the Brave's Gungeon, Diablo 1 and WFC articles; Caves of Qud modding wiki; Edgar docs). The Lopes et al. 2010 PDF and the Marson & Musse 2010 squarified-treemap paper could not be fetched (connection reset / 403); findings on them come from abstracts and citing works and are marked as such.

---

## Key Question 1: Floor plan generation — algorithms for dividing a floor into apartments and rooms, placing doors/corridors, and the constraints that matter

### Takeaway
Four families dominate: (a) **greedy scored placement on a tile grid** (what Shadows of Doubt actually ships: rooms placed in priority order into the best-scoring free region, with a hallway forced when the entrance is far from a corner); (b) **recursive subdivision** (BSP, squarified treemaps) which is trivial on a grid but needs a post-pass for corridors/connectivity; (c) **constrained growth** (Lopes et al. 2010) which grows rooms from seeds on a grid subject to adjacency/size constraints and is cited as faster than constraint solving; and (d) **stochastic optimisation over a room graph** (Merrell et al. 2010: Bayesian net emits the "program", Metropolis search realises walls) which gives the most realistic houses but costs seconds to minutes per building. For a whole city generated at world creation, the shipped-game evidence favours (a)/(b)/(c) with explicit connectivity rules, not (d).

### Cited Findings

**Shadows of Doubt's actual floor-plan algorithm (DevBlog #13, Feb 2019)**
- Hierarchy is City > District > Block > Building > Floor > Address > Room > Tile; "the floor is the point where designed and procedural elements meet". — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Each tile is 1.8 m × 1.8 m; the tile grid is used for pathfinding, culling, and generation. Building floors are 15 × 15 tiles. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- A designer-made floorplan tool divides each floor into addresses ("similar to zoning in SimCity"); a "generate interior" button then creates the dividing walls. For an apartment it places a bathroom, then bedrooms, a kitchen, and on larger floors a study, shower room or dining room. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Hallways: the system checks whether the address entrance is too far from a corner of the floor; if so it draws a hallway toward a tile closer to that corner. "Hallways are essential" so rooms aren't reachable only through other rooms. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Room placement algorithm: (1) rooms processed in order of importance (living room first, then bathroom, ...); (2) for each room, simulate placement in every free space and score each option; (3) store each location+score, sort, place in the best spot; (4) repeat until all rooms placed or no space remains. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Three scoring criteria: floor space (weighted more for large rooms like living rooms); uniform shape measured by corner count (matters more for larger rooms); windows (living rooms and bedrooms need them more than studies). — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Special conditions: connection rules limit which rooms may join which — every room can reach a hallway, and kitchens can also connect directly to living rooms; some rooms are limited to one door (bathrooms); some rooms cannot contain the address entrance; an **override system** lets a room take floor space from an already-placed room that has excess area. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Scoring weights, the full room list and rules, and exact details are not stated in the post. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)

**Merrell, Schkufza, Koltun 2010 — "Computer-Generated Residential Building Layouts" (SIGGRAPH Asia / ToG 29)**
- Pipeline: a Bayesian network trained on real-world data expands sparse requirements (number of bedrooms/bathrooms, total sq. ft.) into a full architectural program (room list, adjacencies, desired areas); floor plans realising it are obtained by stochastic optimisation; floor plans are turned into 3D models. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Training data: 120 architectural programs manually encoded from a catalogue of residential layouts (Wood 2007). Features recorded: total square footage, footprint, per-room type, per-room area, per-room aspect ratio, room-to-room adjacency (bool), adjacency type {open-wall, door}. Continuous attributes quantised. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Separate networks per storey count: single-storey (42 nodes, 33 edges, 1452 s training), two-storey (82 nodes, 61 edges, 4407 s), three-storey (120 nodes, 92 edges, 8293 s), trained on 40/50/30 programs respectively, on an Intel Pentium D 2.8 GHz using Intel's open-source PNL. Structure learning via local search on the Bayesian score (BIC-approximated). Aspect-ratio nodes omitted from structure learning and attached in a post-process. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Floor-plan optimisation: Metropolis algorithm with Boltzmann objective f(x)=exp(−βC(x)); starts from a "canonical high-cost configuration" of equal-sized rectangular rooms packed on a grid on each floor. Reversible-jump MCMC was tried but found too complex; CMA-ES unusable because dimensionality varies. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Proposal moves: **sliding a wall** (choose a wall and split point; move one part a distance d ~ N(0, σ²)); walls snap to an existing wall if within ε, with ε = 1 ft and σ = 2ε; walls also align with walls on neighbouring floors; **swapping rooms** (two rooms' labels interchanged). — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Cost C = k_a·C_a (accessibility: missing required door/open connections + enclosed patios/entrances/garages; a connection needs a shared wall segment long enough for a door) + k_d·C_d (dimensions: negative log-likelihood of each room's area and aspect ratio under the network's distributions) + k_f·C_f (floors: unsupported area of each floor over the one below, normalised) + k_s·C_s (shapes: convexity measure M_c = (A(hull)−A)/A + edge-count regulariser, applied to non-hallway rooms, to open-wall groups, and to each floor outline). — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Runtime: the illustrated optimisation took 35 s on an Intel Core i7 3.2 GHz; figure shows plans at 200 / 2,000 / 20,000 / 100,000 iterations. "All models shown in this paper took a few seconds to 7 minutes to generate by our single-threaded implementation." — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Failure rate: optimisation sometimes terminates before accessibility is zero (e.g. staircase blocked on second storey); "virtually absent for single-story buildings, arise in roughly 1 out of 20 optimization instances for two-story buildings, and in 1 out of 5 three-story optimizations"; the cause is multi-storey spaces (staircases, two-storey living rooms) coupling floors. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Door placement rule: doors are placed to minimise the sum of distances from the door to the nearest corner in each adjacent room (citing Alexander et al.: a door mid-wall "destroys the center, and leaves no single area which is large enough to use"). Ordinary doors in private rooms, French doors in semi-private rooms, wide passageways between public rooms. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Windows: laid out along exterior wall segments at regular intervals within each room; sizes/styles from a style template; if obstructed by a lower roof, a smaller window type is used or none. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Architectural rules of thumb the paper cites: privacy gradient (common areas near the entrance, private rooms far), near-convex room shapes, hallways/stairways exempt from convexity. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Related prior work the paper identifies: Hahn, Bose & Whitehead 2006 "Persistent real-time building interior generation" (Sandbox '06) generates grid-like layouts by random splitting with axis-aligned planes; Martin 2005/2006 "Procedural house generation" (6- to 9-room layouts); Galle 1981 exhaustive enumeration of rectangle arrangements (exponential); Michalek et al. 2002 evolutionary search for single-storey apartments. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Discussion: authors suggest globally coupled optimisation across all storeys "is not necessary" for apartment complexes and office buildings due to regularity between floors. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)

**Lopes, Tutenel, Smelik, de Kraker, Bidarra 2010 — "A Constrained Growth Method for Procedural Floor Plan Generation" (GAME-ON 2010, Leicester)** — PDF unreachable; abstract-level only
- Proposes "a novel and flexible technique for generating procedural floor plans subject to user-defined constraints"; demonstrated for different classes of buildings, "some of which consist of several connected floors"; motivated by earlier methods not adhering to "reachability and connectivity" constraints. — [TU Delft publication entry](https://publications.graphics.tudelft.nl/papers/584); [PDF (unreachable at time of research)](https://graphics.tudelft.nl/~rafa/myPapers/bidarra.GAMEON10.pdf)
- A citing abstract says Lopes et al. "generates more flexible results, and does that faster than other constraint solving techniques". — [arXiv 1211.5842 (suburban houses)](https://arxiv.org/pdf/1211.5842v1.pdf)
- A 2019 Minecraft (GDMC) paper says Lopes' method "generated fully-connected rooms for structures, which heavily influenced" it; that paper combines constrained growth with cellular automata to produce organic buildings with rooms, windows and connecting doors inside a given 3D voxel volume. — [arXiv 1906.05094](https://arxiv.org/abs/1906.05094)

**Squarified treemaps (Marson & Musse 2010, "Automatic Real-Time Generation of Floor Plans Based on Squarified Treemaps Algorithm", Int. J. Computer Games Technology)** — paper itself 403; abstract/secondary only
- Uses the squarified treemap algorithm to split a building's space into rooms, "ensuring optimal use of available area without long narrow rooms"; then connects rooms and places doors based on a connection graph; floors are built top-down so generation can be lazy; "different and realistic floor plans can be created by adjusting a few parameters". — [Marson & Musse abstract listing](https://library.kab.ac.ug/Record/doaj-art-5d41cd5786c04764877690fe15272ffd/Description)

**BSP / space partitioning (PCG Book ch. 3, Shaker, Liapis, Togelius, Lopes, Bidarra)**
- Textbook BSP dungeon: root = whole area w×h; repeatedly choose a random leaf and split along a random vertical/horizontal line; stop splitting when below a minimum (example: w/4 × h/4); each leaf gets one room with corners chosen stochastically inside the cell; corridors connect children of the same parent, bottom-up, until the root's children are joined. Guarantees non-overlap and full connectivity. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf)
- The BSP hierarchy doubles as a semantic hierarchy: non-leaf nodes can theme their subtrees (treasure vs monsters, lock-and-key gating), since subtrees connect through a single corridor. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf)
- Agent-based "digger" alternative (blind: 5 % chance to turn / 5 % to add a 3–7 tile room, each rising by 5 % per step; look-ahead variant checks intersections) is described as unpredictable and can overlap rooms or terminate early — not recommended for interiors. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf)
- Graph-grammar approaches (Adams; Dormans' mission→space grammars; van der Linden's gameplay grammars) generate a topology graph first and embed geometry second. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf)

**Graph-first embedding (Edgar)**
- Edgar: the designer describes a level as a graph of rooms and connections; the generator produces a layout matching it using handmade room templates; rooms connect directly by doors or via short corridors. "Usually works best for levels with less than 30 rooms"; some inputs are "too hard" and need guideline-following. — [Edgar-Unity docs](https://ondrejnepozitek.github.io/Edgar-Unity/docs/introduction)

**WFC for floor plans (Boris the Brave)**
- A four-tile set (one empty) easily produces square rooms; tile weights change room size; adding door and corridor tiles yields varied man-made floor plans; fixed tiles let you pin entrances/exits or draw a boundary and let WFC fill the interior. — [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)
- Connectivity: a global "path constraint" forces annotated tiles into one connected component; "without it, rooms often end up disconnected. With it, doors are inserted between rooms automatically"; give T-junction tiles very low weight. — [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)
- Caves of Qud subdivides the map into regions and runs WFC with different settings/tilesets per region, which fixes WFC's tendency toward disconnected, samey output. — [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)
- Later academic use: PlanNet (TVCG 2023) uses WFC to create large-scale wireframe patterns as early forms of floor plans. — [PlanNet TVCG 2023](https://hongbofu.people.ust.hk/doc/PlanNet_TVCG_2023.pdf)

### Inferences
- SoD's algorithm is essentially greedy best-first placement with a hand-ordered room list and a three-term score, plus an "override" repair step; on a 15×15 grid with ~5–8 rooms per address, "simulate placement in every free space" is cheap enough to run for an entire city at creation. It is the most directly transferable approach for Strata, since it is already grid/tile-based.
- The hallway rule in SoD (hallway drawn when entrance is far from a corner) is a heuristic stand-in for Merrell's accessibility term; combining greedy placement with an explicit room-graph check (every room reachable from the entrance via allowed adjacencies) would give SoD-like speed with Merrell-like guarantees.
- Merrell's door-near-corner rule and regular-interval exterior windows are cheap, grid-friendly rules worth copying regardless of which plan generator is used.
- Merrell-style MCMC (seconds to minutes per house, 5–20 % failure on multi-storey) is unsuitable for whole-city generation but could be used offline to author a library of plan templates.
- BSP on a grid produces plans with no corridors unless corridors are added, and leaf rooms have no windows guarantee; treemaps avoid long thin rooms but likewise need a connectivity pass. Both are fine for office floors (regular) and weaker for apartments.

### Gaps
- Lopes et al. 2010 algorithm details (grid resolution, growth phases, L-shape handling, runtime) — PDF fetch failed twice; only abstract and citations available.
- Marson & Musse 2010 zone grouping/corridor insertion details — Wiley returned 403.
- SoD scoring weights, exact room list per address type, and whether the algorithm changed between 2019 and the 2025 1.0 release — not published.
- Hahn et al. 2006 "Persistent real-time building interior generation" — only known via Merrell's related-work summary.

---

## Key Question 2: Multi-storey consistency — stairwells, elevators, structural walls aligned across floors; lobbies, roof access, basements

### Takeaway
The literature handles vertical alignment either by coupling floors inside the optimiser (Merrell: floor-support cost + wall alignment to neighbouring floors + staircase spaces fixed in the program) or by making the floor the unit of design and stamping a shared "shell" (SoD: designed per-floor plans on fixed 15×15 footprints, with stairs/elevators as part of the designed floor rather than the generated address interior). Shipped games overwhelmingly do the latter.

### Cited Findings
- Merrell 2010: each floor must be contained within the envelope of the floor beneath; cost C_f = Σ A(F^i − F^{i−1}) / Σ A(F^i); the ground floor must fit the lot. Walls are snapped to align with walls on neighbouring floors "to increase consistency across floors". Multi-storey spaces (stairwells) "are indicated as such" in the architectural program. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Merrell 2010 staircase rules: staircase space is specified in the layout; narrow rectangular → linear stairs; wide enough for two flights → U-shaped; L-shaped → 90° turn; steps at least 36 in wide, treads 9–11 in, risers at most 8¼ in (AIA Architectural Graphic Standards). — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Merrell 2010 failure mode is explicitly vertical: optimisation terminated with "the staircase blocked on the second story" in ~1/20 two-storey and ~1/5 three-storey runs. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- SoD: buildings are split into floors; floors are designer-laid-out and divided into addresses; floor plans are 15×15 tiles; the generator only fills interiors of addresses. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- SoD: the developer has said buildings like the hotel and city hall keep the same layout each time but appear at a random map position; a fan guide says there is "a degree of random generation to the buildings, not just in name but in layout". — [Steam discussion (seeds)](https://steamcommunity.com/app/986130/discussions/0/3825289852108687122); [gameplay.tips building guide](https://gameplay.tips/guides/shadows-of-doubt-building-type-guide.html)
- SoD DevBlog #16/#17: ventilation shafts (an inter-floor traversal system) were "left to procedural generation, with gameplay prioritized over realism". — [itch.io DevBlog #17 Ventilation Shafts](https://itch.io/devlog/98636/shadows-of-doubt-devblog-17-ventilation-shafts.amp)
- Diablo 1: up/down stairs are placed after tile conversion; "if they cannot be placed, the level is retried" — i.e. vertical connection is a validation gate with regeneration. — [Boris the Brave, Diablo 1](https://boristhebrave.com/2019/07/14/dungeon-generation-in-diablo-1)
- WFC for stacking: tiles whose bases are at least as wide as their tops prevent unsupported stacking, so multi-storey WFC buildings get solid foundations. — [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)

### Inferences
- For a voxel city generated whole, the SoD pattern (fixed footprint; a designed or generated "core" column containing stairwell, elevator shaft, lobby, and corridor spine that is identical on every floor; addresses fill the remainder) sidesteps every multi-storey failure mode Merrell reports.
- Roof access and basements fall out naturally if the core column is extended one floor above/below the occupied floors; Merrell's "each floor within the envelope of the one below" rule is irrelevant for box-shaped towers but matters for houses.

### Gaps
- No source describes elevator-shaft or lobby generation explicitly; SoD devblogs on this topic (if any) were not found.
- No source found giving SoD's rule for how many stairwells per building or how they are placed on the 15×15 grid.

---

## Key Question 3: Furnishing — constraint/optimisation placement vs slot/anchor systems vs templates; grid collision/walkability

### Takeaway
Research systems (Yu 2011, Merrell 2011) treat furnishing as continuous optimisation of a cost/density function with terms for accessibility, visibility, pathways, wall-distance priors and pairwise relations; they produce good rooms but cost tens of seconds to minutes per room. Shipped games (SoD) use preset-driven placement where the *owner's* traits drive selection, with props stored as configuration files; the grid naturally supports a cheaper "anchor + filler" discretisation of the same cost terms.

### Cited Findings

**Yu et al. 2011, "Make it Home" (SIGGRAPH 2011)**
- Two stages: (1) extract spatial (distance d̄_i and orientation θ̄_i to nearest wall), hierarchical (first-tier on floor/wall; second-tier on first-tier surfaces), and pairwise relationships from a few positive example rooms; (2) optimise a random initial layout by simulated annealing with Metropolis-Hastings. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)
- Object representation: bounding box with six surfaces; "back" surface = nearest wall; each surface has an accessible-space rectangle sized from examples or an average adult; visibility-requiring objects (TV, painting) get a viewing frustum approximated by rectangles. Translation is in (x,y) only; z fixed by parent surface. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)
- Cost C = w_a·C_a (accessibility: penalise overlap of any object centre with another's accessible space) + w_v·C_v (visibility frustum overlap) + w_path·C_path (free space along a cubic-Bézier pathway between doors, approximated by rectangles; path control points are also movable) + w_pr^d·C_pr^d + w_pr^θ·C_pr^θ (priors on wall distance/orientation) + w_pair^d·C_pair^d + w_pair^θ·C_pair^θ (pairwise). Weights used: w_a = 0.1, w_v = 0.01, w_path = 0.1, w_pr^d = w_pair^d = [1.0, 5.0], w_pr^θ = w_pair^θ = 10.0. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)
- Moves: translate (δp ~ N(0, σ_p²)) and rotate (δθ ~ N(0, σ_θ²)) with variances proportional to temperature; swap two objects of the same tier; move pathway control points. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)
- Runtimes (3.33 GHz Intel Xeon): Living Room 20 objects / 20,000 iterations / 22 s; Bedroom 24 / 20,000 / 48 s; Restaurant 54 / 25,000 / 219 s; Resort 30 / 42,000 / 126 s; Factory 51 / 42,000 / 262 s; Flower Shop 64 / 22,000 / 376 s; Gallery 35 / 18,000 / 88 s. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)
- Perceptual study (25 participants, 70 paired comparisons each): for 10 of 21 synthesised rooms no significant preference for human-designed layouts; dropping orientation constraints was more detectable than dropping distance constraints, "which suggests that a greater weight can be applied to penalizing orientation deviation". — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)
- A later survey describes this method as "effective but takes several minutes". — [Microsoft Research layout optimisation paper](https://microsoft.com/en-us/research/wp-content/uploads/2014/01/layoutopt-1.pdf)

**Merrell et al. 2011, "Interactive Furniture Layout Using Interior Design Guidelines" (SIGGRAPH 2011)**
- Encodes interior-design guidelines "as terms in a density function" and generates suggestions by sampling it with a "hardware-accelerated Monte Carlo sampler"; user study: 18 participants, five rooms; professional designers preferred suggestion-assisted layouts in four of five rooms at p < 0.01. No runtime numbers on the project page. — [Stanford project page](https://graphics.stanford.edu/projects/furniture/)
- The method is patented (US 9,355,470 "Method and system for interactive layout"). — [Google Patents US9355470](https://patents.google.com/patent/US9355470)

**Shadows of Doubt furnishing**
- "For an apartment, the choice of furniture and decoration is driven by the owner's preferences." — [ColePowered DevBlog #12](https://colepowered.com/?p=34480)
- Colour palettes: five colours each (two complementary primaries, a secondary pair, a neutral), stored as ScriptableObjects; each palette has four attributes — Modernity (richer citizens get more modern), Cleanness (bathrooms, corporate), Loudness (extroverts), Emotive (warm vs cold, emotive citizens) — matched to the assigned citizen's personality; base materials matched similarly (floral for emotive). A dirt map depicts neglected apartments. — [ColePowered DevBlog #14](https://colepowered.com/?p=34717)
- A custom shader uses a mask texture per prop so regions (e.g. a bed's pillows and duvet) take colours from the five-colour palette generated for each room; most basic props take under an hour to model in Qubicle. — [ColePowered DevBlog #21](https://colepowered.itch.io/shadows/devlog/133150/shadows-of-doubt-devblog-21-how-voxels-saved-the-project)
- "The city generation process relies on stored configuration files for furniture and items"; adding a piece of furniture changes generation so the same seed can yield a different city across versions. — [ColePowered DevBlog #36](https://colepowered.com/?p=35528)
- Purchased player apartments start empty; furniture can be bought and placed freely; a "start with apartment" option begins with a furnished one. Community mod "PlacementPlus" adds precision placement, snaps ceiling lights flush, and pushes furniture out of walls — implying the shipped placement is continuous, not grid-snapped, for player placement. — [gamepressure apartment guide](https://www.gamepressure.com/newsroom/apartment-in-shadows-of-doubt-everything-you-should-know/z35536); [PlacementPlus on Thunderstore](https://thunderstore.io/c/shadows-of-doubt/p/kropath/PlacementPlus/)
- By DevBlog #15 (2019) "the first furnishings are appearing in apartments"; plan was furnished rooms with simulated citizens by early May 2019. — [ColePowered DevBlog #15](https://colepowered.com/?p=34835)

**Grid / WFC / anchor approaches**
- A graph-based WFC write-up suggests recursion: furniture added to rooms, objects placed on furniture, with constraints at both room and object level. — [Graph Based WFC](https://logan169.github.io/2025/01/18/Graph-Based-WFC.html)
- Boris the Brave recommends keeping WFC for local tile rules and handling global structure separately (path constraints, fixed tiles, per-region tilesets). — [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)
- Earlier "template" furnishing: Kjølaas 2000 represents a room as a nested hierarchy of rectangular templates swapped by eight mutation functions, with empty boxes in front of doors/windows; limited to rectangular rooms (summarised in Yu et al.'s related work). Germer & Schwarz 2009 treat each furniture object as an agent attaching to a parent object. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)

### Inferences
- Yu's cost terms discretise cleanly onto a voxel grid: "accessible space" becomes a set of cells in front of an object that must stay walkable; "visibility frustum" becomes a cone of cells; "pathway" becomes a BFS corridor between door cells that must remain unblocked; wall-distance priors become "must touch wall / must be within N cells of wall / centre". Evaluating these as hard constraints over candidate cells (SoD-style "score every free slot") replaces the annealing loop.
- Placing "anchor" objects first (bed, sofa, desk — the ones with strong wall priors and pairwise partners) and then "fillers" mirrors both SoD's importance-ordered room placement and Yu's finding that orientation errors are the most noticeable.
- SoD's palette-attribute matching (modernity/cleanness/loudness/emotive vs citizen wealth/personality) is a cheap and well-received way to tie furnishing to simulation data without changing geometry.

### Gaps
- Merrell 2011's actual guideline terms (clearance, circulation, conversation, balance, alignment, emphasis) and runtime: project page lacks them; full PDF not fetched.
- SoD's furniture placement algorithm (slot system? presets? cost?) is not described in any devblog found; only that owner preferences and config files drive it. A Reddit snippet mentioning "anchors", "fillers" and "edge/corner/center" slot types was not attributable to SoD and was excluded.

---

## Key Question 4: Shadows of Doubt in detail — interior generation, address types, building types, lighting, generation time, and the data model linking rooms to citizens and routines

### Takeaway
SoD's interiors are grid-based (1.8 m tiles, 15×15 floors), with designer-zoned floors split into addresses whose rooms are placed greedily by score; rooms are the fundamental world unit (streets are rooms too) and doors are both the connectivity graph and the rendering/culling portals; citizens are assigned to addresses and their personality/wealth drives décor; routines were rewritten from pre-computed schedules to a goal-based, needs-driven AI with proximity-scaled update rates. Generation time is reported by players as long (especially on Steam Deck) but no official number exists.

### Cited Findings

**Generation hierarchy and interiors** — see KQ1 for the full algorithm. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)

**Rooms, doors, and the world graph**
- "The world is divided into rooms connected by doors. Streets are treated as rooms too." Doors serve as culling portals: groups of connected rooms are determined by doors, so only what the player can see is rendered; little is rendered behind closed doors. "The game combines procedural generation with hand-crafted design so that every room of every building can be explored." — [ColePowered DevBlog #22 (Doors)](https://colepowered.itch.io/shadows/devlog/164492/shadows-of-doubt-devblog-22-doors-galore)
- Door gameplay semantics attached: doors open away from the player; most can be knocked on (sleeping occupants need louder repeated knocks); many lockable, lockpickable (paperclips/hairclips wear out), spare keys under mats or in plants; gap under door for peeking/passing notes; shoulder-barge with strength-based retries. — [ColePowered DevBlog #22 (Doors)](https://colepowered.itch.io/shadows/devlog/164492/shadows-of-doubt-devblog-22-doors-galore)
- Every room of every building can be entered without loading, supported by culling work. — [ColePowered DevBlog #15](https://colepowered.com/?p=34835)

**Citizens and routines**
- AI rewritten to a goal-based system; citizens have "a sims-like stat system" driving eating, drinking, sleeping. Previously routines were "almost entirely pre-computed", so spontaneous actions (investigating a noise/sighting) had to be scripted and forced recalculation. AI paths computed dynamically. Update rate is high only near the player or during awareness-critical actions, keeping cost for ~95 % of citizens "relatively insignificant". — [ColePowered DevBlog #15](https://colepowered.com/?p=34835)
- When a citizen is assigned an apartment, palettes are matched to their personality. — [ColePowered DevBlog #14](https://colepowered.com/?p=34717)
- Modding ecosystem confirms the data model is ScriptableObject-based: Piepieonline's library mod creates ScriptableObjects from raw JSON and AssetBundles; CitizenImporter creates citizens from a citizens.csv at city generation; SOD.Common exposes "Street & District edit, Advanced Building Edit" and citizen relational data. — [Piepieonline mods](https://thunderstore.io/c/shadows-of-doubt/p/Piepieonline/); [SOD.Common](https://thunderstore.io/c/shadows-of-doubt/p/Venomaus/SODCommon/dependants/)
- Official mod documentation exists as a Notion page but returned no readable content to the fetcher. — [SoD Mod Documentation (Notion)](https://shadowsofdoubt.notion.site/Shadows-of-Doubt-Mod-Documentation-33744ac558304d89997e88f2051280f7); [In-Game City Editor (Notion)](https://shadowsofdoubt.notion.site/In-Game-City-Editor-9ac9c55aedec4a4388234e4ae0488b8b)

**Seeds, scale, and generation time**
- Seed format: `CityName.CitySize.Index.Seed` with a 16-character alphanumeric seed string regardless of city size; seeds produce different cities across game versions. — [Steam discussion (seeds)](https://steamcommunity.com/app/986130/discussions/0/3825289852108687122)
- Players report populations of 600 to 900+ in large cities. — [Steam discussions](https://steamcommunity.com/app/986130/discussions/0/3829791550530471806)
- A review states "the city generation itself takes a long while, especially if you're loading in on a Steam Deck or similar handheld PC" — no number given. — [StartMenu review](https://www.startmenu.co.uk/home/review-shadows-of-doubt)
- Scale in 2019: "a couple of city blocks" of skyscrapers was the room count being handled. — [ColePowered DevBlog #15](https://colepowered.com/?p=34835)

**Art / voxel pipeline**
- Props modelled in Qubicle (chosen over MagicaVoxel because it voxelises existing meshes and outputs UV-mapped meshes); colour map → greyscale heightmap → Unity-generated normal map; HDRP renderer. — [ColePowered DevBlog #21](https://colepowered.itch.io/shadows/devlog/133150/shadows-of-doubt-devblog-21-how-voxels-saved-the-project)
- Procedural generation made baked shadows unavailable (2018). — [ColePowered DevBlog #6](https://colepowered.com/?p=34059)

**Building types**
- A fan guide lists building types and notes random generation of layouts; the fetch of the page failed (404), so the list is not reproduced here. — [gameplay.tips building type guide (404 at research time)](https://gameplay.tips/guides/shadows-of-doubt-building-type-guide.html)

### Inferences
- SoD's "room" is the universal container: it is simultaneously the floor-plan output, the culling unit, the navigation region, the ownership unit (via address), and the décor unit (palette per room). Strata should make the room ID the primary key that everything else (door graph, address, owner, lighting portal) hangs off.
- Because SoD stores addresses as the unit of ownership and rooms inside them, "address type" (apartment, office, shop, lobby) is where the room list and importance order live; the devblog's apartment recipe (bathroom → bedrooms → kitchen → study/dining if room) is one such preset.

### Gaps
- No official generation-time figure; no official address-type list; no public description of SoD's furniture placement, lighting placement, or how floors differ by building type beyond "designed floorplans per building".
- The Notion mod docs (which presumably name the ScriptableObject types such as presets for rooms/furniture) could not be read by the fetcher.
- No 2024–2026 GDC talk or long-form interview on SoD's generation was found; coverage (Edge, PC Gamer, Wireframe) is conceptual.

---

## Key Question 5: Dungeon-style techniques that transfer (room-and-corridor, BSP, Spelunky/Gungeon templates, Diablo tiles, Caves of Qud, WFC)

### Takeaway
The transferable pattern from the best-documented shipped generators is a **two-layer split**: an abstract graph/flow (Gungeon flows, Spelunky's 4×4 path, Diablo's "predungeon" boolean plan, Qud's builder sequence) authored or sampled first, then a geometry/tile pass that stitches hand-made templates or runs marching-squares/pattern matching to emit walls. Retries on failed validation (connectivity, stairs, minimum floor area) are universal.

### Cited Findings
- **Enter the Gungeon**: a pre-authored "flow" graph (rooms, types, connections, no positions) is chosen — 4 flows in the Hollow, 8 in Gungeon Proper; transformations add chains, alternate paths, and data-driven node injection (e.g. secret rooms prefer dead ends, 1/5 chance to attach to any room, 90 % spawn chance); ~300 candidate rooms for stage 1; loops are cut into "loop composites", trees laid out depth-first joining exit pairs, reroll+backtrack up to 3 times; loop closure first tries a small rectangular room then pathfinds a corridor (4–30 units, 50 in the Mines). Separating abstract graph from positioning "makes pacing and scope easier to control". — [Boris the Brave, Gungeon](https://www.boristhebrave.com/2019/07/28/dungeon-generation-in-enter-the-gungeon)
- **Diablo 1**: all levels on a 40×40 tile grid; two stages — "predungeon" boolean floor plan, then conversion to tiles via marching squares (cathedral: wall tiles sit on the back edge of each tile, so some orientations have no tile and a fixup adds them — making room separators thinner than normal); cathedral spine of up to three 10×10 rooms with recursive "budding" of 2/4/6-tile rooms off parent edges; dividing walls run corner-to-wall (25 % arches, 25 % barred arches, rest solid, with a random door so the area stays walkable); flood-fill finds enclosed spaces for theme rooms; catacombs: 3×3-neighbourhood pattern matching assigns wall/floor/door, rooms 4–9 tiles, corridors width 1–3, doors skipped if adjacent to another door, ≥700 floor tiles or restart. — [Boris the Brave, Diablo 1](https://boristhebrave.com/2019/07/14/dungeon-generation-in-diablo-1)
- **Spelunky**: 4×4 grid of 16 rooms; a guaranteed solution path is carved top row to bottom; path rooms connect left/right, drop rooms get bottom exits, landing rooms get top exits; each room draws from hand-built templates per role with "probabilistic tiles" inside chunks. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf); [tinysubversions Spelunky generator lessons](https://tinysubversions.com/spelunkyGen2/)
- **Caves of Qud**: zones are built by sequenced zone-builder classes (Strata, MoonStair, RiverBuilder) declared in Worlds.xml; FastNoise and NoiseMap set broad terrain; WFC takes a PNG colormap template from `wavetemplates/` and produces a locally-similar layout, with colours mapped to wall/terrain types; WFC is the main method for buildings (huts, crypts, lairs), used for Tomb of the Eaters crypts, historic sites, initial village structures; repair steps such as `EnsureAllVoidsConnected` keep open areas reachable. — [Caves of Qud Modding: Zone Procedural Generation](https://wiki.cavesofqud.com/wiki/Modding:Zone_Procedural_Generation)
- Qud's GDC sessions: "Tile-Based Map Generation using Wave Function Collapse in Caves of Qud" and a ruin-generation talk working "in increasingly fine levels of detail". — [GDC Vault listing](https://gdcvault.com/play/1025913/Math-for-Game-Developers-Tile)
- **Unexplored** (Boris): biomes keep independent systems making consistent choices; non-terminal symbols are placeholders resolved later — i.e. a grammar with deferred expansion. — [Boris the Brave, Unexplored](https://www.boristhebrave.com/2021/04/10/dungeon-generation-in-unexplored/)
- **Binding of Isaac**: layout has no loops (tree on a grid). — [Boris the Brave, Isaac](https://boristhebrave.com/2020/09/12/dungeon-generation-in-binding-of-isaac)
- **BSP and lock-and-key theming** — see KQ1. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf)

### Inferences
- A building floor is the natural analogue of a Gungeon "flow": an address-type preset is a graph (entrance → hallway → {living, kitchen, bedrooms…}) with required/optional nodes and injection rules (e.g. "study spawns with 40 % chance if area > N"), embedded onto the 15×15-style grid by greedy placement.
- Diablo's marching-squares/pattern-matching step is exactly the "plan grid → voxel walls" conversion Strata needs; its lesson that wall tiles on the back edge produce thin separators maps directly onto the one-voxel-wall question in KQ6.
- Spelunky-style hand-made room templates with probabilistic sub-chunks are a cheap way to furnish *special* rooms (lobbies, shop floors, hotel reception) while generic apartments use the rule-based path.

### Gaps
- The official Gungeon GDC talk was not found; all details are from Boris the Brave's decompilation-based write-up.
- Qud's actual WFC parameters and the ruin-generation details are only in GDC Vault videos not fetched.

---

## Key Question 6: Voxel-specific concerns — wall thickness, floor/ceiling thickness, door frames, windows as cutouts, stairs, shell alignment, lighting

### Takeaway
Direct published guidance is thin. What exists: SoD uses a 1.8 m gameplay tile (coarser than its art voxels), models props as voxel meshes with UVs, and uses doors as light/visibility portals; Diablo shows that putting walls on one edge of each cell yields thin partitions; Boris's WFC notes give a rule for structurally sound stacking; the GDMC Minecraft work shows constrained growth + CA works inside a given 3D voxel volume. Specific voxel wall-thickness and door-frame conventions had to be inferred.

### Cited Findings
- SoD's gameplay/generation grid is 1.8 m × 1.8 m tiles while its art is voxel-modelled in Qubicle at finer resolution; props use a mask texture for palette recolouring. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642); [ColePowered DevBlog #21](https://colepowered.itch.io/shadows/devlog/133150/shadows-of-doubt-devblog-21-how-voxels-saved-the-project)
- SoD: doors are culling portals; closed doors cull what's behind them; baked shadows unavailable because of procedural generation. — [ColePowered DevBlog #22](https://colepowered.itch.io/shadows/devlog/164492/shadows-of-doubt-devblog-22-doors-galore); [ColePowered DevBlog #6](https://colepowered.com/?p=34059)
- SoD chose voxels "mainly because art assets could be turned around much faster", accepting a cartoonish read as a trade-off. — [ColePowered DevBlog #21](https://colepowered.itch.io/shadows/devlog/133150/shadows-of-doubt-devblog-21-how-voxels-saved-the-project)
- Diablo 1 cathedral: wall tiles sit on the back edge of each tile; separators between rooms are thinner than normal as a result; each 40×40 tile is split into four render pieces for walkability; rooms must stay ≥1 tile from the map edge "reserved for walls". — [Boris the Brave, Diablo 1](https://boristhebrave.com/2019/07/14/dungeon-generation-in-diablo-1)
- Merrell: windows at regular intervals along exterior wall segments per room; door placed near a corner; stair dimensions (≥36 in wide, 9–11 in tread, ≤8¼ in riser) and shape chosen by staircase footprint. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- WFC multi-storey: tiles with bases at least as wide as their tops prevent unsupported stacking. — [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)
- GDMC Minecraft paper: floor plans generated "within a given 3D space" by constrained growth + cellular automata, producing rooms, windows, and connecting doors. — [arXiv 1906.05094](https://arxiv.org/abs/1906.05094)
- SoD window scoring: rooms are scored on windows (living rooms/bedrooms need them, studies less) — i.e. daylight access is a plan-time constraint, not a post-process. — [ColePowered DevBlog #13](https://colepowered.com/?p=34642)
- Note on "Interior Mapping": this is a shader technique (fake parallax interiors behind windows) unrelated to actual interior generation; it was out of scope and not researched further — no source fetched.

### Inferences
- With a 1.8 m gameplay tile and finer art voxels, SoD effectively has two grids: a coarse plan grid and a fine voxel grid. For Strata, where the world *is* voxels, the plan grid should be an integer multiple of the voxel size with walls occupying whole voxel columns; Diablo's "wall on the back edge" suggests assigning each wall to exactly one of the two adjacent cells so partitions are one voxel thick and both rooms keep full interior cells.
- Daylight: because SoD scores windows at plan time, it never has to retrofit windows; Strata should mark exterior-adjacent cells per room during placement and emit window cutouts at Merrell-style regular intervals.
- Door frames need a 1-voxel-wide, ~2-voxel-high cutout plus (optionally) frame voxels; doors as portals implies the door cell must be recorded in the room graph, not just carved.

### Gaps
- No source found that states a recommended voxel wall thickness (1 vs 2), floor slab thickness, or door-frame convention for interiors; no voxel-game devblog on stairs-in-voxels was found.
- GDMC paper details (wall thickness, door placement rules) are in the PDF, which was not fetched.

---

## Key Question 7: Libraries and open-source references (floor plans, furniture placement, Unity assets, Rust crates)

### Takeaway
Mature, permissively licensed *floor-plan* or *furniture-placement* libraries are scarce; the strongest off-the-shelf options are dungeon/level generators (Edgar, Dungeon Architect) whose graph→template→layout pipeline can be repurposed, plus a few small research-grade repos.

### Cited Findings
- **Edgar** (Ondřej Nepožitek): Unity plugin on top of Edgar for .NET; graph-based with handmade room templates; third-party listings report the GitHub version as MIT-licensed (verify against the repo's LICENSE); the docs' terms say the plugin can be used commercially but "cannot be redistributed or resold"; PRO version sold on the Asset Store with coroutine-based generation; works best under ~30 rooms. Also ported to Godot (Edgar.Godot). — [Edgar-Unity docs](https://ondrejnepozitek.github.io/Edgar-Unity/docs/introduction); [Edgar-Unity GitHub](https://github.com/OndrejNepozitek/Edgar-Unity); [edgar-dotnet](https://github.com/ondrejnepozitek/edgar-dotnet); [Edgar.Godot asset](https://godotengine.org/asset-library/asset/edit/22743)
- **Dungeon Architect** (commercial, Unity/Unreal): multiple builders including Grid, GridFlow (flow graph controls layout and item placement), and SnapGridFlow (stitch pre-made rooms via a layout graph editor); marker/theme-based prop placement; runtime navmesh generation. — [Dungeon Architect Asset Store](https://assetstore-fallback.unity.com/packages/tools/utilities/dungeon-architect-53895); [GridFlow docs](https://raw.githubusercontent.com/coderespawn/docs.dungeonarchitect.dev/main/unity/gridflow-create-dungeon.html)
- **FloorPlan6** (poolpet): open-source apartment layout generator, AGPL-3.0, with plot subdivision and volumetric modules still placeholders at time of posting. — [Graphisoft community post](https://community.graphisoft.com/t5/Parametric-design/FloorPlan6-open-source-apartment-layout-generator-looking-for/td-p/701647)
- **proceduralBuildingGenerator** (wojtryb): "classical building generation technique allowing to recursively divide a rectangular building plan into rooms". — [uithub mirror](https://uithub.com/wojtryb/proceduralBuildingGenerator)
- **watabou's Mansion** generator: browser-based procedural mansion floor plans (reference for look and room naming, not a library). — [watabou mansion](https://watabou.github.io/mansion)
- **Magnetizing Floor Plan Generator** (Grasshopper, Bauhaus student project): exhaustive search, "works best for plans with up to 6/7 rooms". — [Food4Rhino](https://www2.food4rhino.com/en/app/magnetizing-floor-plan-generator)
- **vernacular** (drmrd): React/Three.js floor planner for historic house styles (tool, not generator). — [awesome.ecosyste.ms entry](https://awesome.ecosyste.ms/projects/github.com%2Fdrmrd%2Fvernacular)
- **mxgmn/WaveFunctionCollapse**: the reference WFC implementation (MIT). — [awesome.ecosyste.ms entry](https://awesome.ecosyste.ms/projects/github.com%2Fmxgmn%2Fwavefunctioncollapse)
- **Rust**: the "Roguelike Tutorial in Rust" has a WFC chapter (bracket-lib ecosystem). — [bfnightly WFC chapter](https://bfnightly.bracketproductions.com/chapter_33.html)
- **MGAIA-Minecraft**: WFC building generation with a separate interior-decoration step. — [GitHub MGAIA-Minecraft](https://github.com/ScholliYT/MGAIA-Minecraft)
- Merrell 2010 used Intel's open-source Probabilistic Networks Library (PNL) for the Bayesian net. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)

### Inferences
- Nothing found is a drop-in for "apartment floor plans + furnishing on a grid"; the realistic path is to implement the SoD-style greedy placer (small) and borrow Edgar's graph-with-templates idea for special floors.

### Gaps
- "BuildingGen" returned no results in search. No Rust crate specifically for floor-plan or furniture placement was found. Boris the Brave's DeBroglie (C# WFC with path constraints) was not fetched/verified for license. Edgar's MIT status is from third-party listings only.

---

## Key Question 8: Semantic data to emit for gameplay (room types, door graph, ownership/address, line-of-sight, nav)

### Takeaway
Every strong source converges on a room graph with typed doors as the core artefact: SoD uses it for culling, navigation, ownership and door gameplay; Merrell's program distinguishes door vs open-wall adjacencies; Gungeon/Edgar start from the graph; Qud validates connectivity after generation. Rooms should carry type, address/owner, exterior-adjacency (windows), and a door list with lock/key semantics.

### Cited Findings
- SoD: world = rooms connected by doors; streets are rooms; door-connected room groups drive rendering/culling; the tile grid drives pathfinding and culling; doors carry lock/key/knock/peek semantics; palettes per room are derived from the assigned citizen. — [ColePowered DevBlog #22](https://colepowered.itch.io/shadows/devlog/164492/shadows-of-doubt-devblog-22-doors-galore); [ColePowered DevBlog #13](https://colepowered.com/?p=34642); [ColePowered DevBlog #14](https://colepowered.com/?p=34717)
- SoD: citizens' routines are goal/needs-driven with dynamic pathing; AI tick rate scales with distance to the player. — [ColePowered DevBlog #15](https://colepowered.com/?p=34835)
- Merrell: architectural program = room list, per-room area/aspect targets, adjacency pairs typed {open-wall, door}; accessibility cost counts missing required connections, which demands a shared wall long enough for a door. — [Merrell et al. 2010 PDF](https://www.cs.princeton.edu/courses/archive/spring11/cos598A/pdfs/Merrell10a.pdf)
- Gungeon: flow nodes carry room type (combat, shop, secret, connector, boss) and directed connections; combat rooms lock doors on first entry and spawn enemies — gameplay semantics attached to graph node type. — [Boris the Brave, Gungeon](https://www.boristhebrave.com/2019/07/28/dungeon-generation-in-enter-the-gungeon); [Edgar Gungeon example](https://ondrejnepozitek.github.io/Edgar-Unity/docs/examples/enter-the-gungeon)
- PCG Book: BSP subtree nodes used to theme content and gate with lock-and-key because each subtree has a single corridor entry. — [PCG Book ch. 3 PDF](https://www.pcgbook.com/chapter03.pdf)
- Qud: `EnsureAllVoidsConnected` repair pass; WFC path constraint (Boris) inserts doors to guarantee one connected component. — [Caves of Qud Modding wiki](https://wiki.cavesofqud.com/wiki/Modding:Zone_Procedural_Generation); [Boris the Brave, WFC tips](https://boristhebrave.com/2020/02/08/wave-function-collapse-tips-and-tricks)
- Yu 2011 furnishing needs doors and windows as fixed inputs (their positions "are fixed and not updated during the optimization") and emits per-object accessible-space and pathway rectangles — i.e. furnishing both consumes the door graph and produces walkability data. — [Yu et al. 2011 PDF](https://ww3.math.ucla.edu/camreport/cam11-49.pdf)

### Inferences
- Minimum emitted schema suggested by the sources: Building{floors[], core/stair cells} → Floor{addresses[]} → Address{type, owner/company, rooms[], entrance door} → Room{type, cells, exterior-edge cells, windows[], doors[], palette/décor tags, furniture[] with blocked cells and "accessible" cells} → Door{roomA, roomB, cell, lockState, keyId, isPortal}. Nav regions = rooms; portals = doors; LOS = door-open state plus window cells.
- Because SoD's address is the ownership unit, routines can be stored as (citizen → address slots: home, work) plus needs; the room graph gives path targets (bed cell, desk cell) without a separate schedule table.

### Gaps
- No source gives SoD's actual serialized schema (field names, how routines reference rooms); the Notion mod docs likely do but were unreadable to the fetcher.
