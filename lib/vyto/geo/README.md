# vyto/geo — geodesy & spatial primitives

Coordinates, reference ellipsoids, 3D Cartesian conversion, spherical
measurement, shapes, map projections, slippy-map tiles, GeoJSON and encoded
polylines — in pure Vyto, with no dependency beyond `vyto/math` and
`vyto/geom` for the core.

The core is **projection-independent on purpose**. Web Mercator and raster
tiles are one layer built on top, not the foundation, so a 3D globe, a
planetarium or a non-Mercator map uses `vyto/geo` directly and never touches
the tile maths.

```vyto
import { LatLon, distanceM, initialBearing, destination,
         datumWgs84, toEcef, fromEcef, rayHitsEllipsoid } from "vyto/geo";
import { mercatorProject, OrthographicProjection } from "vyto/geo/project";
import { lonLatToTile, tileKey, metersPerPixel } from "vyto/geo/tiles";
import { parseGeoJson } from "vyto/geo/geojson";
import { decodePolyline } from "vyto/geo/polyline";

let sf = LatLon(-122.4194, 37.7749);          // longitude first, like GeoJSON
let ny = LatLon(-74.0060, 40.7128);

distanceM(sf, ny)                              // 4129092 m
initialBearing(sf, ny)                         // 69.908 degrees
destination(sf, initialBearing(sf, ny), distanceM(sf, ny))   // back at NY

let wgs = datumWgs84();
let p = toEcef(sf.withAlt(0.0), wgs);          // Earth-centred Cartesian
fromEcef(p, wgs)                               // round-trips, altitude included

mercatorProject(sf)                            // EPSG:3857 metres
tileKey(lonLatToTile(sf, 12))                  // "12/655/1583"
metersPerPixel(0.0, 0)                         // 156543.034 m/px

let globe = new OrthographicProjection(LatLon(-30.0, 20.0));
globe.isVisible(sf)                            // true — near hemisphere
globe.isVisible(LatLon(139.7, 35.7))           // false — Tokyo is round the back

parseGeoJson(text)                             // GeoFeature[], soft-fails to []
decodePolyline("_p~iF~ps|U_ulLnnqC_mqNvxq`@", 5)
```

## Modules

| Module | What it gives you | Depends on |
|--------|-------------------|------------|
| `vyto/geo` | Coordinates, `Datum`, angle housekeeping, spherical measures, 3D/ECEF/ENU, `BBox`, `Geometry`, bulk algorithms. | `vyto/math`, `vyto/geom` |
| `vyto/geo/project` | `mercator*`, `equirect*`, `orthographic*`, `gnomonic*`, and the `Projection` class hierarchy. | core |
| `vyto/geo/tiles` | The slippy-map scheme: `TileXY`, `WorldPx`, `TileRange`, zoom/resolution maths. | core |
| `vyto/geo/geojson` | RFC 7946 reader → `GeoFeature[]`. | core, `vyto/util/json` |
| `vyto/geo/polyline` | Google encoded-polyline codec. | core, `vyto/util/text` |

Conventions that hold everywhere:

- **Longitude before latitude** in every constructor, matching GeoJSON's `x, y`
  order.
- **Degrees at the API surface** (that is what data files carry), radians
  internally.
- **Metres** for every distance, **square metres** for every area.
- Bulk functions take `(lons, lats, start, count)` so one ring of a multi-part
  shape can be processed without allocating a slice for it.

---

## Design decisions

### Why the core is projection-independent

An earlier draft put Web Mercator and slippy tiles at the centre, with
`LatLon`/`WorldPx`/`TileXY` as the core types. That quietly makes 2D raster
tiles the only first-class use: anyone building a globe, a planetarium, or a
map on another projection would work *around* the module rather than with it.
So the centre of gravity is geodesy, and tiles are a leaf nobody else imports.

### Two Earth radii, deliberately

| Constant | Value | Used for |
|---|---|---|
| `EARTH_RADIUS_M` | 6378137.0 | Semi-major axis. Mercator and tile scale — it is what puts zoom 0 on the universal 156543.034 m/px every scale bar agrees on. |
| `EARTH_MEAN_RADIUS_M` | 6371008.8 | IUGG mean radius R₁. Distances and areas. Using the semi-major axis for these biases them +0.11%. |

### Ellipsoid for conversion, sphere for measurement

Split by operation, not by accident:

- **Conversion** (`toEcef` / `fromEcef`) uses the ellipsoid, where a closed
  form is exact. There is no cost to being exact, so it is.
- **Measurement** (`distanceM`, `polygonAreaM2`, …) uses the sphere. That is
  ~0.5% off a true ellipsoidal geodesic, which needs an iterative solver
  (Vincenty) that fails to converge near-antipodal. The spherical form is
  fast, always terminates, and its error sits below what map data carries.

### `Datum` is a parameter, not a constant

`Datum` is a value struct of semi-major axis and flattening, so the module
serves Mars, a unit sphere or a game world without forking. `f == 0.0` is a
sphere and every formula degrades correctly. Datums are **functions**, not
consts, because a Vyto const initializer cannot be a struct.

---

## Reference — `vyto/geo`

### Constants

`DEG2RAD` · `RAD2DEG` · `EARTH_RADIUS_M` · `EARTH_MEAN_RADIUS_M` ·
`EARTH_CIRCUMFERENCE_M` · `MAX_MERCATOR_LAT` (85.05112877980659 — the latitude
where a square Mercator world runs out).

### Coordinates

```vyto
struct LatLon    { lon: float; lat: float; }
struct LatLonAlt { lon: float; lat: float; alt: float; }
```

Value structs: passing one costs two or three doubles and the refcount never
sees it. `alt` is metres **above the ellipsoid**, not above mean sea level —
that would need a geoid model this module does not carry.

| Method | Returns |
|---|---|
| `LatLon.equals(o)` | Exact component equality. |
| `LatLon.withAlt(alt)` | `LatLonAlt`. |
| `LatLonAlt.flat()` | `LatLon`, dropping altitude. |

### Datums

| Function | What |
|---|---|
| `datumWgs84()` | WGS84 — the one GPS and GeoJSON mean. |
| `datumSphere(radius)` | A sphere of any radius (Mars, the Moon, a game world). |
| `datumUnitSphere()` | Radius 1. |
| `datumEarthSphere()` | Sphere at Earth's mean radius. |

`Datum` methods: `b()` semi-minor axis · `e2()` first eccentricity squared ·
`meanRadius()` R₁ = (2a + b)/3 · `isSphere()`.

### Angle housekeeping

| Function | Range / behaviour |
|---|---|
| `normalizeLon(lon)` | → `[-180, 180)`. **Opt-in, never automatic** — a map panning past the antimeridian needs unwrapped longitude to keep world-pixel x monotonic. |
| `normalizeBearing(deg)` | → `[0, 360)`. |
| `lonDelta(a, b)` | Signed shortest difference, `[-180, 180)`. Exactly half a turn reports −180; the sign is arbitrary there. |
| `clampMercatorLat(lat)` | Clamps to ±`MAX_MERCATOR_LAT`. Applied *inside* the projection: `tan(π/2)` is infinite and an `as int` on an infinite float is undefined behaviour in C. |
| `clampLat(lat)` | Clamps to ±90. |

### Spherical measures

| Function | Notes |
|---|---|
| `angularSeparation(a, b)` | Central angle in **radians** — the radius-free primitive. Multiply by any body's radius, or use directly as an astronomical separation. Haversine in its `atan2` form, not the `asin` form that loses precision near-antipodal. |
| `distanceM(a, b)` | Great-circle metres on Earth. |
| `distanceOnM(a, b, radius)` | Great-circle metres on any sphere. |
| `distanceEquirectM(a, b)` | One `cos` and one `hypot` instead of haversine's four trig calls. ~0.002% over 150 km at mid latitudes, worse as the pair grows or moves poleward. For sorting, hit-testing and clustering — not for anything the user reads. |
| `initialBearing(a, b)` | Degrees clockwise from north, `[0, 360)`. *Initial*: on a great circle other than a meridian or the equator the bearing changes along the way. |
| `finalBearing(a, b)` | Bearing on arrival. |
| `destination(from, bearingDeg, distM)` | Walk a great circle on Earth. |
| `destinationOn(from, bearingDeg, distM, radius)` | Same, any sphere. |
| `interpolateLonLat(a, b, t)` | Spherical interpolation — the path a plane actually flies, correct across the antimeridian. Not linear in lon/lat. |
| `midpoint(a, b)` | `interpolateLonLat(a, b, 0.5)`. |

> **Landing on a pole leaves longitude undefined.** There is no meridian
> there, and the returned value is whatever the `atan2` of two near-zero terms
> produced. Latitude stays exact. Callers that care must special-case it.

### 3D — Cartesian space

Axes are the standard Earth-centred, Earth-fixed frame: **+x** through (0, 0),
**+y** through (90E, 0), **+z** through the north pole. Unit vectors and ECEF
positions agree on orientation, so a renderer can mix them freely.

| Function | What |
|---|---|
| `toUnitVector(ll)` | Position as a point on the unit sphere. For geodetic latitude this is **also the surface normal** on any ellipsoid — which is why it needs no datum. |
| `fromUnitVector(v)` | Inverse. Reads direction only; magnitude is ignored. |
| `surfaceNormal(ll)` | Identical to `toUnitVector`. Exists under this name because that is what a renderer looks for. |
| `toEcef(p, d)` | Geodetic → ECEF metres. Exact, no iteration. |
| `fromEcef(v, d)` | ECEF → geodetic, by Bowring's closed form. See accuracy below. |
| `enuBasis(ll)` | `[east, north, up]` unit vectors. Stays orthonormal at the poles, where east and north are arbitrary but must not collapse. Right-handed: east × north = up. |
| `enuOffsetToEcef(ll, e, n, u)` | A local offset in metres as an ECEF displacement. |
| `slerp(a, b, t)` | Great-circle interpolation between directions. Inputs normalized, so the result is always a unit vector; `t` outside `[0,1]` extrapolates. |
| `rayHitsSphere(origin, dir, radius)` | Ray parameter `t` of the first hit, or `-1.0` on a miss. If `dir` is normalized, `t` is a distance. |
| `rayHitsEllipsoid(origin, dir, d)` | Same against an ellipsoid. Scaling each axis by its radius reduces to a unit sphere; because that scaling is linear, `t` survives unchanged, so the hit point is still `origin + dir * t` in the original frame. |
| `geodeticToGeocentricLat(lat, d)` | Angle at the centre, rather than the angle of the surface normal. They differ by up to 0.192° on WGS84 and agree on a sphere. |
| `geocentricToGeodeticLat(lat, d)` | Inverse. |

`rayHits*` is a globe's click handler: a camera ray through a pixel → surface
point → `fromEcef` → lat/lon. A tangent ray returns its grazing hit rather
than a miss, and a ray starting inside returns the exit — the two cases
hand-rolled versions get wrong.

### BBox

```vyto
struct BBox { west: float; south: float; east: float; north: float; }
```

GeoJSON field order. **`west > east` means the box crosses the
antimeridian** — a real extent, not an error. Emptiness is therefore keyed on
latitude alone: `south > north`.

| Method | Notes |
|---|---|
| `isEmpty()` / `crossesAntimeridian()` | |
| `lonSpan()` / `latSpan()` | `lonSpan` accounts for wrapping. |
| `center()` / `southWest()` / `northEast()` | |
| `containsLonLat(lon, lat)` / `containsPoint(p)` | Handles a wrapping box. |
| `contains(o)` | True when `o` lies wholly inside. |
| `intersects(o)` | |
| `union(o)` | Smallest box holding both. **Cannot resolve a wrapping input** (two candidate hulls, neither always right), so it widens to full span rather than guessing. |
| `grow(lonDeg, latDeg)` | Expands, clamping latitude. |

Constructors: `bboxEmpty()` (the identity for `union`) · `bboxWorld()` ·
`bboxOfPoint(p)` · `bboxOfCoords(lons, lats, start, count)`.

### Geometry

A class, not a struct, because it holds arrays. Coordinates are flat and
parallel with a separate table of part offsets — nested arrays are awkward in
Vyto, a draw loop wants a flat run, and one ring can be measured without
allocating a slice.

```vyto
class Geometry {
    kind: int;                 // GEOM_POINT | GEOM_LINE | GEOM_POLYGON
    lons: float[]; lats: float[]; alts: float[];
    partStart: int[]; partLen: int[]; partOuter: bool[];
}
```

Two fields a naive flat representation drops:

- **`partOuter`** — marks exterior rings against holes. Start/length alone
  cannot tell a MultiPolygon's second *exterior* ring from a *hole* in its
  first, which is exactly how holes end up drawn as outlines.
- **`alts`** — one altitude per point, always the same length as `lons`.
  Terrain, flight paths and satellite tracks are the obvious spatial apps and
  altitude is the whole point of them.

| Method | Notes |
|---|---|
| `partCount()` / `pointCount()` / `isEmpty()` | |
| `addPart(lons, lats, outer)` | Altitudes default to zero. |
| `addPart3(lons, lats, alts, outer)` | Truncates to the shortest input rather than reading past the end. |
| `partPoint(part, i)` / `partPointAlt(part, i)` | |
| `partBBox(part)` / `partLengthM(part)` / `partAreaM2(part)` | |
| `bbox()` / `lengthM()` | Over every part. |
| `areaM2()` | Exterior rings less holes; never negative. |

### Bulk algorithms

| Function | Notes |
|---|---|
| `polylineLengthM(lons, lats, start, count)` | Open path — no closing edge, so a ring must repeat its first point to be measured whole. |
| `polygonAreaM2(lons, lats, start, count)` | **Spherical excess**, not a planar shoelace; a planar formula in degrees is wrong by whatever `cos(lat)` does across the shape. Always positive. Ring closed implicitly. |
| `ringIsClockwise(lons, lats, start, count)` | Planar shoelace sign. GeoJSON's right-hand rule wants exteriors counter-clockwise. |
| `pointInRing(lon, lat, lons, lats, start, count)` | Crossing-number ray cast. A point exactly on an edge may land either way — inherent to the test. |
| `pointInGeometry(g, lon, lat)` | Inside an exterior ring **and** inside no hole. Exact for well-formed data; not a general boolean over arbitrary overlapping rings, which would need ring-to-polygon grouping GeoJSON does not carry. Non-polygons are always outside. |
| `simplifyIndicesM(lons, lats, start, count, toleranceM)` | Ramer–Douglas–Peucker, tolerance in **metres**. A degree tolerance is unpickable — it means a different distance at every latitude — so longitude is scaled by `cos(midLat)` into a locally metric plane. Explicit stack, not recursion: worst-case depth is O(n) and Vyto has no tail calls. Returns **absolute** ascending indices, always including both endpoints. |

---

## Reference — `vyto/geo/project`

Every projection maps a `LatLon` to a `Vec2` in **metres on the projection
plane**, using the semi-major axis as the sphere radius. That is the EPSG:3857
convention and it is what makes the tile layer's scale land on 156543.034 m/px
at zoom 0.

Two ways in, on purpose: **free functions** for hot paths, since a per-vertex
loop should not pay for virtual dispatch, and a **`Projection` class** so
library code can accept *a* projection instead of hard-coding one.

| Projection | Free functions | Good for |
|---|---|---|
| Web Mercator | `mercatorProject` / `mercatorUnproject` | Slippy maps. Conformal, so local shapes survive; area does not, which is why Greenland looks the way it does. |
| Equirectangular | `equirectProject(At)` / `equirectUnproject(At)` | The native grid of most whole-world raster datasets, and the cheapest projection there is. `standardLat` is the parallel along which scale is true; 0 gives plate carrée. |
| Orthographic | `orthographicProjectAt` / `orthographicUnprojectAt` / `orthographicVisible` | The globe view. |
| Gnomonic | `gnomonicProjectAt` / `gnomonicUnprojectAt` / `gnomonicVisible` | **Every great circle maps to a straight line** — route planning and visibility become straight-edge geometry. Less than a hemisphere is representable; a tool, not a basemap. |

Classes: `Projection` (a concrete identity base, so a partial subclass has
something defined to fall back on) · `MercatorProjection` ·
`EquirectProjection(standardLat)` · `OrthographicProjection(centre)` ·
`GnomonicProjection(centre)`. The last two carry `setCentre()` — spinning a
globe is moving one point, nothing else rebuilds.

`isVisible(ll)` lives on the **base**, not on the two azimuthal subclasses that
can answer `false`. Asking is then always valid, which is what lets a renderer
treat projections uniformly. For orthographic it is the near/far hemisphere
cull — the single thing every hand-rolled globe gets wrong, since the far
hemisphere projects onto the same disc as the near one and draws over it.

`orthographicUnprojectAt` clamps a point outside the disc to the rim rather
than returning a NaN that would propagate silently. `gnomonicProjectAt`
returns the zero vector beyond the horizon rather than an infinity.

## Reference — `vyto/geo/tiles`

The usual scheme: the world is a 256 px square at zoom 0, doubling each level,
x increasing east from −180 and y increasing **south** from the top. Row 0 at
the top is the part people trip over — the TMS convention numbers it the other
way.

`TILE_SIZE` · `struct TileXY {x, y, z}` · `struct WorldPx {x, y, z}` ·
`struct TileRange {minX, minY, maxX, maxY, z}` with `width()` / `height()` /
`count()` / `contains(t)`.

| Function | Notes |
|---|---|
| `tilesAcross(z)` / `worldSize(z)` | 2^z tiles; 256·2^z pixels. |
| `lonLatToWorldPx(ll, z)` / `worldPxToLonLat(p)` | Fractional, so it answers for a camera position, not only a tile corner. **Longitude is not wrapped** — a map panning east past 180 needs x to keep increasing. |
| `tileForWorldPx(p)` / `lonLatToTile(ll, z)` | `floor`, not truncation: at negative x (a westward pan past the antimeridian) truncation rounds toward zero and picks the tile next door. |
| `tileNW(t)` / `tileBounds(t)` / `tileCentre(t)` | Adjacent tiles share an edge exactly. |
| `wrapTileX(x, z)` | `((x % n) + n) % n`. Vyto's `%` is C's — the sign follows the dividend — so the double modulo is required, not redundant. |
| `tileYValid(t)` | y does not wrap; there is nothing above the north edge. |
| `bboxToTileRange(b, z)` | Inclusive. A wrapping extent stays a simple interval: `minX..maxX` runs past the edge and the caller wraps each x as it fetches. The span is capped at the width of the world. |
| `metersPerPixel(lat, z)` / `zoomResolution(z)` | 156543.034 at zoom 0. |
| `zoomForBBox(b, widthPx, heightPx)` | **Fractional and unclamped** — min/max zoom is app policy, and a layer's maxZoom is a property of the tile server, not of the maths. A degenerate point extent returns the 22.0 cap, not an infinity. |
| `tileParent(t)` / `tileChildren(t)` | Children in `[NW, NE, SW, SE]` order. Zoom 0 is its own parent. |
| `tileKey(t)` | `"z/x/y"` — cheaper to compare than three ints, and what a disk cache path wants anyway. |

## Reference — `vyto/geo/geojson`

```vyto
class GeoFeature { geom: Geometry; props: Map<string, string>; id: string; }
```

| Function | Notes |
|---|---|
| `parseGeoJson(text)` | → `GeoFeature[]`. **Soft-fails to an empty array** — a bad overlay must not take an app down. |
| `parseGeoJsonValue(root)` | From an already-parsed `JsonValue`, so a caller reading exotic properties from its own tree does not parse twice. |
| `geometryFromJson(v)` | One geometry object → `Geometry`, or `null`. |
| `featureCollectionBBox(features)` | Empty collection → empty box, not the whole world. |

`GeoFeature`: `prop(k)` · `hasProp(k)` · `name()` (tries `name`, `NAME`,
`title`) · `bbox()`.

Handles `Point`, `MultiPoint`, `LineString`, `MultiLineString`, `Polygon`,
`MultiPolygon`, `GeometryCollection`, `Feature` and `FeatureCollection`. A
bare geometry and a bare `Feature` are both accepted alongside a
`FeatureCollection`, since all three turn up in the wild and rejecting the
first two would put the same three-way branch in every caller.

Three decisions worth knowing:

- **Properties flatten to `Map<string, string>`.** Exposing `JsonValue` would
  leak `vyto/util/json` into the types of every consumer, including ones that
  only want to draw shapes; keeping just the name is not enough, since styling
  by `class`/`highway`/`admin_level` is ordinary work. Integral numbers keep
  an integer spelling (`"7"`, not `"7.000000"`). Nested objects and arrays are
  skipped — they have no useful flat form.
- **`prop()` returns `""` for a missing key.** Required, not a convenience:
  `Map.get` panics on a missing key and real data is full of absent
  properties.
- **A `GeometryCollection` becomes one feature per member**, each carrying a
  copy of the parent's properties, which keeps `Geometry`'s invariant that a
  shape has exactly one kind.

Reader only. Writing GeoJSON is a different job with different questions
(precision, key order, whether to emit a bbox) and nothing needs it yet.

## Reference — `vyto/geo/polyline`

| Function | Notes |
|---|---|
| `decodePolyline(s, precision)` | → single-part `GEOM_LINE` geometry. Malformed input decodes as far as it can rather than failing the whole route. |
| `encodePolyline(lons, lats, precision)` | |
| `encodePolylineRange(lons, lats, start, count, precision)` | |
| `encodeGeometryPart(g, part, precision)` | |

`precision` is the number of decimal places and has **no default**: 5 is
Google's original, 6 is what OSRM and Valhalla emit, and getting it wrong
scales every coordinate by ten — which looks like data corruption rather than
a parameter mismatch, so it should not be guessable.

Encoding rounds rather than truncates: truncation biases every coordinate
toward zero and shows up as a route slowly drifting off the road.

---

## Accuracy

| Operation | Error |
|---|---|
| `toEcef` | Exact (closed form). |
| `fromEcef` at ground level | Exact to double precision. |
| `fromEcef` at 1 km | 9 nm |
| `fromEcef` at low orbit (400 km) | 1.3 mm |
| `fromEcef` at geostationary | 0.26 m |
| `fromEcef` at lunar distance | 0.35 m |
| `distanceM` vs a true ellipsoidal geodesic | ~0.5% |
| `distanceEquirectM` vs `distanceM`, 150 km at mid latitude | ~0.002% |
| `polygonAreaM2` | Spherical; same ~0.5% class as distance. |

Anything needing better than `fromEcef` gives at orbital altitude wants an
iterative refinement — but the closed form terminates in fixed time, while the
iterative one is the usual source of a hang on a degenerate input.

## The antimeridian

The likeliest silent bug in any geo library, so it is handled case by case
rather than uniformly:

| Operation | Behaviour |
|---|---|
| `angularSeparation`, `distanceM`, `polylineLengthM` | **Correct automatically.** `sin²(Δλ/2)` is symmetric under Δλ → Δλ − 360°, so a straddling pair already takes the short way. Do not "fix" this. |
| `polygonAreaM2` | Each Δλ is wrapped into `[-π, π]` before accumulating. Without that a ring crossing 180° reports a near-whole-earth area. |
| `pointInRing` | **Breaks** — it is a planar ray cast. Callers pre-shift the ring and query point by +360°. Not fixable inside, because only the caller knows which side of the world the query means. |
| `BBox` | `west > east` *means* crossing. Handled in `contains` / `intersects`; `union` cannot and widens instead. |
| `normalizeLon` | Opt-in only, never applied automatically. |
| `lonLatToWorldPx` | Deliberately unwrapped, so a pan stays continuous. |
| `wrapTileX` | Where wrapping actually happens, at fetch time. |

## Deliberately out of scope

Ellipsoidal geodesics (Vincenty/Karney — iterative, non-convergent
near-antipodal, ~0.5% better for this workload; the ellipsoid is still used for
ECEF, where the closed form is exact) · CRS registries, proj strings, EPSG
lookup · UTM/MGRS/geohash/S2/H3 · topology (buffer, clip, union, hull,
triangulation — a GEOS-class engine; polygon clipping for rendering belongs in
`vyto/geom`) · KML/GPX/Shapefile/WKT · camera and matrix maths (that is
`vyto/geom` plus a renderer) · spatial **indexes** (quadtree/R-tree/k-d tree —
a separate concern, generic over any extent rather than geographic) ·
**anything with I/O** — no tile fetching, caching or geocoding, which would
break the maths-only dependency rule the module exists for · and no rendering.

## Dependencies

The core needs `vyto/math` and `vyto/geom` only — no libc, allocator or JSON.
It is **not** freestanding-safe, though: `vyto/math` is an `extern "C"` binding
to libm, so `sin cos tan asin acos atan atan2 sqrt hypot exp log log2 floor
fmod fmin fmax fabs round pow` must resolve at link time. Newlib provides
them; a genuinely libm-less target does not, and `--no-float` can never use
this module.

`geojson` adds `vyto/util/json` (and therefore an allocator); `polyline` adds
`vyto/util/text` for `StringBuilder`, which is a C shim. Both are separate
files precisely so an embedded target can project coordinates without either,
and a globe can use the 3D maths without pulling in tiles.

## Tests

`tests/fixtures/geo_math.vt`, `geo_3d.vt`, `geo_shape.vt`, `geo_tiles.vt` and
`geo_format.vt`, plus `examples/71_geo.vt`. All pure computation — no I/O, no
gfx — so they run on every host.

Golden files print **ints and bools only, never a bare float**: the runtime
prints `%g` at six significant digits and the freestanding path uses a
hand-rolled `%g` that is explicitly not shortest-round-trip. Absolutes go
through `round()` rather than truncation, which flips on a 1-ULP wobble and
truncates the wrong way for negative latitudes.

Anchors worth keeping: zoom 0 is 156543.034 m/px · the Golden Gate Bridge is
tile `12/654/1582` · a ring along the equator encloses 255032940 km² (exactly
2πR²) · the Mercator world edge is 20037508 m · and the canonical polyline
``_p~iF~ps|U_ulLnnqC_mqNvxq`@`` decodes to (38.5, −120.2), (40.7, −120.95),
(43.252, −126.453) and re-encodes byte for byte.
