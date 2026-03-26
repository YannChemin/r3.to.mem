# r3.to.mem

GRASS GIS addon — extract 2D raster slices from a 3D raster (voxel) map
into a RAM-backed in-memory mapset for zero-disk-I/O access by `r.mapcalc`
and any other GRASS module.

## The problem

When processing 3D raster data (e.g. hyperspectral cubes) slice by slice,
the standard GRASS approach calls `Rast3d_get_value()` once per cell. For a
1000×1000 image that is **one million function calls per band**, each
triggering a tile-cache lookup. Extracting 200 bands means 200 million calls.
The second bottleneck is writing intermediate 2D maps to disk and reading them
back for every `r.mapcalc` expression.

## The solution

`r3.to.mem` solves both bottlenecks:

1. **Tile-bulk extraction** — opens the 3D map with `RASTER3D_NO_CACHE` and
   calls `Rast3d_get_block()`, which reads each tile at the target Z level
   exactly once and bulk-copies the plane into a flat buffer. For a 32×32×32
   tile: ~1 000 tile reads instead of ~1 000 000 voxel calls. Typical speedup:
   **100–1000×**.

2. **RAM-backed mapset** — writes output 2D maps to a GRASS mapset whose
   directory lives in `/dev/shm` (Linux tmpfs). `r.mapcalc` reads from
   `/dev/shm` — pure RAM I/O, no disk involved. The mapset is added to
   `SEARCH_PATH` automatically so maps are usable without `@MEMORY`.

## Quick start

```sh
# Align 2D region to the cube
g.region raster3d=hypercube

# Extract band 42 into RAM
r3.to.mem input=hypercube zrange=42 output=band_42

# Use it immediately — no @MEMORY needed
r.mapcalc "reflectance = band_42 * 0.0001"

# Clean up when done
r3.to.mem -c
```

## Usage

### Extraction

```sh
# Single slice
r3.to.mem input=CUBE zrange=Z output=NAME [mapset=MAPSET]

# Range of slices — creates NAME.Z0, NAME.Z1, …
r3.to.mem input=CUBE zrange=Z0-ZN output=NAME

# Comma-separated list
r3.to.mem input=CUBE zrange=0,5,10,15 output=NAME

# All slices (default)
r3.to.mem input=CUBE output=NAME

# Show cube dimensions and exit
r3.to.mem input=CUBE -l
```

### Cleanup

Two modes let you balance RAM use against setup overhead:

```sh
# Full cleanup — removes entire /dev/shm tree, symlink, SEARCH_PATH entry
r3.to.mem -c [mapset=MEMORY]

# Per-map cleanup — removes named maps, keeps the mapset structure
r3.to.mem remove=NAME [mapset=MEMORY]
r3.to.mem remove=NAME.10,NAME.11,NAME.12 [mapset=MEMORY]
```

### Options summary

| Option / flag | Default | Description |
|---|---|---|
| `input=` | — | Source 3D raster map (optional for `-c` and `remove=`) |
| `output=` | same as input | Base name for output 2D maps |
| `zrange=` | `all` | Z indices: `all`, `5`, `0-9`, `0,5,10`, `0-4,10,15-19` |
| `mapset=` | `MEMORY` | Name of the in-memory GRASS mapset |
| `remove=` | — | Map name(s) to delete from the RAM mapset |
| `-l` | | Print cube dimensions and exit |
| `-c` | | Full cleanup: remove mapset, `/dev/shm` dir, `SEARCH_PATH` entry |

## Iterative workflow (memory-bounded)

For large cubes where loading all bands at once would exhaust RAM:

```sh
eval $(r3.to.mem input=hypercube -l | tr ' ' '\n' | grep depths)

for z in $(seq 0 $((depths - 1))); do
    r3.to.mem input=hypercube zrange=${z} output=band
    r.mapcalc "result_${z} = band * coeff_${z}"
    r3.to.mem remove=band          # free before next iteration
done

r3.to.mem -c                       # tear down the mapset when finished
```

## Multiple cubes in parallel

Use distinct `mapset=` names to keep sources isolated:

```sh
r3.to.mem input=cube_jan output=jan mapset=JAN_MEM
r3.to.mem input=cube_feb output=feb mapset=FEB_MEM
r.mapcalc "anomaly = jan.15@JAN_MEM - feb.15@FEB_MEM"
r3.to.mem -c mapset=JAN_MEM
r3.to.mem -c mapset=FEB_MEM
```

## For C module authors

If you are writing a C module that reads from a 3D raster (e.g. an
`i.hyper.*` module), use the same pattern directly instead of calling
`r3.to.mem` as a subprocess:

```c
/* Open with NO_CACHE — mandatory for the tile-bulk path */
RASTER3D_Map *map = Rast3d_open_cell_old(name, mapset, &region,
                                          RASTER3D_TILE_SAME_AS_FILE,
                                          RASTER3D_NO_CACHE);

/* Allocate once, reuse per iteration */
FCELL *buf = G_malloc((size_t)nrows * ncols * sizeof(FCELL));

for (int z = 0; z < ndepths; z++) {
    /* Each tile at this Z level is read exactly once */
    Rast3d_get_block(map, 0, 0, z, ncols, nrows, 1, buf, FCELL_TYPE);

    /* buf[row * ncols + col] — write to 2D raster row by row */
    int fd = Rast_open_new(outname, FCELL_TYPE);
    for (int row = 0; row < nrows; row++)
        Rast_put_f_row(fd, buf + row * ncols);
    Rast_close(fd);
}
```

> **Never** use `Rast3d_get_value()`, `Rast3d_get_double()`, or
> `Rast3d_get_float()` in a pixel loop — this is the slow path that
> `r3.to.mem` was designed to replace.

## How the RAM mapset works

```
/dev/shm/grass_r3mem_<location>_<mapset>/   ← tmpfs, OS clears on reboot
        WIND
        cell/band_42
        cell_misc/band_42/{range, f_format, …}
        hist/band_42
        …

$GISDBASE/$LOCATION/MEMORY  →  (symlink)  →  /dev/shm/grass_r3mem_…/
```

`r.mapcalc` finds `band_42` via the normal mapset search path. It calls
`open()` on the cell file, which the kernel serves from RAM pages. No disk
I/O occurs after the initial 3D raster read.

## Requirements

- GRASS GIS 7.x or 8.x
- Linux (or any system with a tmpfs mount; falls back to `$TMPDIR`/`/tmp`)
- C99 compiler

See [INSTALL.md](INSTALL.md) for build instructions.
