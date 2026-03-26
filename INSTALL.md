# Installation

## Prerequisites

| Requirement | Notes |
|---|---|
| GRASS GIS 7.x or 8.x | Source tree **or** a compiled installation with headers |
| GCC (C99 or later) | Tested with GCC on Linux x86-64 |
| GNU Make | Standard on all Linux systems |
| Linux | `/dev/shm` (tmpfs) is used for RAM storage; falls back to `$TMPDIR`/`/tmp` on other systems |

## Build from source tree

The module is built against a local GRASS source tree.  `MODULE_TOPDIR` in
the Makefile points to `$(HOME)/dev/grass`; adjust if your tree is elsewhere.

```sh
cd ~/dev/r3.to.mem
make
```

The binary is placed in the GRASS build tree:

```
~/dev/grass/dist.x86_64-pc-linux-gnu/bin/r3.to.mem
```

HTML and man-page docs are generated automatically during `make`.

### Changing `MODULE_TOPDIR`

Edit the first line of `Makefile` if your GRASS source is in a different
location:

```makefile
MODULE_TOPDIR = /path/to/your/grass
```

## Install into a GRASS installation

Use the `install` target to copy the binary and docs into a GRASS
installation (default prefix is `/usr/local/grass86`; override with
`INST_DIR`):

```sh
# Install to the default prefix defined by the GRASS build
make install

# Install to a custom prefix
make install INST_DIR=/opt/grass86
```

This copies:
- `bin/r3.to.mem` → `$INST_DIR/bin/`
- `docs/html/r3.to.mem.html` → `$INST_DIR/docs/html/`
- `docs/man/man1/r3.to.mem.1` → `$INST_DIR/docs/man/man1/`

### Using with a GRASS session without a system install

If you work directly from the build tree (common during development), the
binary is already on the PATH inside a GRASS session started from that tree.
No additional steps are needed.

If you start GRASS from a system installation but want to use the addon from
the build tree, add the build-tree `bin/` to `GRASS_ADDON_BASE` or your
`PATH` before starting GRASS:

```sh
export PATH="$HOME/dev/grass/dist.x86_64-pc-linux-gnu/bin:$PATH"
grass
```

Alternatively, copy the binary manually:

```sh
cp ~/dev/grass/dist.x86_64-pc-linux-gnu/bin/r3.to.mem \
   $(grass --config path)/bin/
```

## Verify

Inside a GRASS session:

```sh
r3.to.mem --help
```

Expected output begins with:

```
Extracts 2D raster slices from a 3D raster map into a RAM-backed
in-memory GRASS mapset…

Usage:
 r3.to.mem [-lc] [input=name] [output=string] [zrange=string]
   [mapset=string] [remove=string[,string,...]] …
```

## Clean the build

```sh
make clean
```

Removes compiled objects and the binary from the build tree.
