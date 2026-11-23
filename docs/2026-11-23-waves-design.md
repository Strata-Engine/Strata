# Wave refraction around islands — design

Date: 2026-11-23 · Status: built (Strata.Waves; first client Ur)

## Intent

The big swell bands should behave like the real ocean around Ur's islands:
slow and bend in shallow water until their crests run along the coast,
focus on headlands and spread in bays, wrap round an island's flanks into
its lee, and (as a follow-up) cross behind it. The model lives in **Strata**
as a reusable, documented module, because a future game may be built around
water; Ur is its first client.

## The model (Strata.Waves, pure March)

- **Input:** a depth grid (metres, `w × h` cells of `cell` metres, low corner
  `(x0, z0)`; land is depth ≤ 0), the swell's deep-water direction (unit) and
  wavelength `λ0`.
- **Dispersion:** fixed angular frequency `ω² = g k0` (k0 = 2π/λ0). In depth
  `d` the wavenumber solves `ω² = g k tanh(k d)`, so waves shorten and slow
  in the shallows.
- **Phase (eikonal):** solve `|∇Φ| = k(d)` by fast marching. Water cells on
  the grid's upwind edges start as the incoming plane wave,
  `Φ = k0 · (x · dir)`; land blocks the march. First arrivals bend toward
  shallow water, swing parallel to the shore and wrap round islands. Crests
  are where `Φ − ωt` is constant.
- **Amplitude:**
  - **shoaling:** `sqrt(cg0 / cg)`;
  - **refraction:** the rays' spreading, from the divergence of `∇Φ/|∇Φ|`,
    with converging rays (headlands) larger and diverging ones (bays, lees)
    smaller, clamped;
  - **lee:** an attenuation behind land along the incoming direction,
    softened by the distance travelled round it.
- **Output:** per cell `Φ` (radians, unwrapped) and an amplitude factor (0..2).
  Pure and deterministic, so it runs on a worker and can be tested.

## Rendering (Strata engine)

- `Vk.set_sea_field(phase, amp, w, h, x0, z0, cell, omega, dx, dz)` (or `Strata.Render.set_sea_field(swell)`) uploads the field,
  like the sea map.
- `water.frag` replaces the swell layer's plane waves inside the field with
  the refracted swell, `A(x) · shaped(cos(Φ(x) − ωt))`, sampled bilinearly
  at the column centres. Outside the field it falls back to the plane wave.
  The short chop and ripples stay on the height tile. Without a field,
  nothing changes.

## Ur

- On a wind-sector change or a coast change, a worker builds the depth grid
  (2 m cells over the map and its ring) from `Ur.Ocean` and runs
  `Strata.Waves.solve`. Startup is asynchronous too.
- `λ0` comes from the sea state (Pierson–Moskowitz, as now).

## Approaches considered

- Shore-following rings in the shader: rejected, a look rather than
  behaviour.
- **Eikonal refraction field on a worker: chosen.** Physically grounded,
  reusable, and the hook for breakwaters and reefs in the gameplay phase.
- Full wave simulation (mild-slope or Boussinesq): too costly per frame;
  possible later in Strata for a water-centric game.

## Follow-up (not in this step)

- **Crossing seas in the lee:** a second arrival around each island's other
  flank (fast marching keeps only the first), for the lattice behind
  islands.
- A viewer scene for experimenting with the model.
