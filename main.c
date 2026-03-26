/****************************************************************************
 *
 * MODULE:       r3.to.mem
 * AUTHOR(S):    Generated for GRASS GIS
 * PURPOSE:      Extract 2D raster slices from a 3D raster map into a
 *               RAM-backed in-memory GRASS mapset for fast access.
 *
 * KEY DESIGN DECISIONS:
 *
 *  1) Fast extraction via Rast3d_get_block() + RASTER3D_NO_CACHE
 *     The default voxel API (Rast3d_get_value / Rast3d_get_double) makes
 *     one function call + cache lookup per voxel — O(nrows*ncols) calls per
 *     slice. With RASTER3D_NO_CACHE, Rast3d_get_block() switches to
 *     Rast3d_get_block_nocache() which iterates over tiles, reads each tile
 *     exactly once and bulk-copies the z-plane into the output buffer.
 *     Speedup: tileX * tileY times fewer operations (typically 100-1000x).
 *
 *  2) In-memory storage via /dev/shm (Linux tmpfs)
 *     A GRASS mapset is just a directory. If that directory lives on
 *     /dev/shm (tmpfs), all reads/writes are pure RAM I/O — no rotational
 *     disk or SSD involved. We create the actual storage in /dev/shm and
 *     symlink it into the GRASS location so GRASS finds it by mapset name.
 *
 *  3) Zero changes to r.mapcalc
 *     Maps are written as standard GRASS 2D FCELL/DCELL rasters. The RAM
 *     mapset is added to the persistent SEARCH_PATH so r.mapcalc and any
 *     other module finds the maps by name without @MEMORY qualification.
 *
 *  4) Null-value compatibility
 *     Rast3d_set_null_value() delegates to Rast_set_f/d_null_value(), so
 *     3D and 2D null bit patterns are identical. No conversion is needed
 *     when copying the block buffer directly to Rast_put_*_row().
 *
 * COPYRIGHT:    (C) 2025 by the GRASS Development Team
 *               This program is free software under the GNU General
 *               Public License (>=v2).
 *
 *****************************************************************************/

#define _GNU_SOURCE   /* for symlink() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <grass/gis.h>
#include <grass/raster.h>
#include <grass/raster3d.h>
#include <grass/glocale.h>

/* -------------------------------------------------------------------------
 * Z-range parsing
 * Supports: "all", "5", "0,5,10", "0-9", "0-4,10,15-19"
 * Returns heap-allocated array of Z indices; caller owns the memory.
 * -------------------------------------------------------------------------*/
static int *parse_zrange(const char *str, int ndepths, int *count_out)
{
    int *zlist, cap, n, a, b, z;
    char *buf, *tok, *saveptr, *dash;

    if (!str || strcmp(str, "all") == 0) {
        zlist = G_malloc(ndepths * sizeof(int));
        for (int i = 0; i < ndepths; i++)
            zlist[i] = i;
        *count_out = ndepths;
        return zlist;
    }

    cap   = 32;
    n     = 0;
    zlist = G_malloc(cap * sizeof(int));
    buf   = G_store(str);

    tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        /* skip leading whitespace */
        while (*tok == ' ' || *tok == '\t')
            tok++;

        /* Look for range separator '-' after first character to avoid
         * confusing a leading minus sign with a range dash. Z indices are
         * always non-negative so this is safe. */
        dash = strchr(tok + 1, '-');
        if (dash) {
            *dash = '\0';
            a = atoi(tok);
            b = atoi(dash + 1);
            if (a > b) {
                int tmp = a;
                a       = b;
                b       = tmp;
            }
            for (z = a; z <= b; z++) {
                if (z < 0 || z >= ndepths)
                    G_fatal_error(_("Z index %d out of range 0..%d"),
                                  z, ndepths - 1);
                if (n >= cap) {
                    cap  *= 2;
                    zlist = G_realloc(zlist, cap * sizeof(int));
                }
                zlist[n++] = z;
            }
        }
        else {
            z = atoi(tok);
            if (z < 0 || z >= ndepths)
                G_fatal_error(_("Z index %d out of range 0..%d"),
                              z, ndepths - 1);
            if (n >= cap) {
                cap  *= 2;
                zlist = G_realloc(zlist, cap * sizeof(int));
            }
            zlist[n++] = z;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    G_free(buf);
    *count_out = n;
    if (n == 0)
        G_fatal_error(_("No valid Z indices parsed from zrange='%s'"), str);
    return zlist;
}

/* -------------------------------------------------------------------------
 * Write WIND file into the RAM mapset.
 * Mirrors the G_make_mapset logic (after G_mkdir) without recreating the
 * directory — the symlink is already in place.
 * -------------------------------------------------------------------------*/
static void init_shm_mapset_wind(const char *mapset_name)
{
    struct Cell_head default_window;

    G_create_alt_env();

    G_setenv_nogisrc("GISDBASE", G_gisdbase());
    G_setenv_nogisrc("LOCATION_NAME", G_location());
    /* Read default window from PERMANENT */
    G_setenv_nogisrc("MAPSET", "PERMANENT");
    G_get_default_window(&default_window);

    /* Switch to the new mapset and write its WIND */
    G_setenv_nogisrc("MAPSET", mapset_name);
    G_put_element_window(&default_window, "", "WIND");

    G_switch_env();
}

/* -------------------------------------------------------------------------
 * Add mapset_name to the persistent SEARCH_PATH file of the current mapset.
 * This is what makes r.mapcalc (a separate process) find maps without
 * @MEMORY qualification. G_add_mapset_to_search_path() is in-memory only;
 * we need to write the file so subsequent GRASS processes pick it up.
 * -------------------------------------------------------------------------*/
static void persist_in_search_path(const char *mapset_name)
{
    FILE *fp;
    char line[GNAME_MAX];
    char **names = NULL;
    int n = 0, cap = 32, found = 0, i;

    /* Read current SEARCH_PATH */
    fp = G_fopen_old("", "SEARCH_PATH", G_mapset());
    if (fp) {
        names = G_malloc(cap * sizeof(char *));
        while (fscanf(fp, "%63s", line) == 1) {
            if (n >= cap) {
                cap  *= 2;
                names = G_realloc(names, cap * sizeof(char *));
            }
            names[n++] = G_store(line);
            if (strcmp(line, mapset_name) == 0)
                found = 1;
        }
        fclose(fp);
    }

    if (!found) {
        fp = G_fopen_new("", "SEARCH_PATH");
        if (fp) {
            for (i = 0; i < n; i++)
                fprintf(fp, "%s\n", names[i]);
            fprintf(fp, "%s\n", mapset_name);
            fclose(fp);
        }
        else {
            G_warning(_("Cannot update SEARCH_PATH — "
                        "run: g.mapsets mapset=%s operation=add"),
                      mapset_name);
        }
    }

    for (i = 0; i < n; i++)
        G_free(names[i]);
    G_free(names);
}

/* -------------------------------------------------------------------------
 * Remove mapset_name from the persistent SEARCH_PATH file.
 * -------------------------------------------------------------------------*/
static void remove_from_search_path(const char *mapset_name)
{
    FILE *fp;
    char line[GNAME_MAX];
    char **names = NULL;
    int n = 0, cap = 32, i;

    fp = G_fopen_old("", "SEARCH_PATH", G_mapset());
    if (!fp)
        return;

    names = G_malloc(cap * sizeof(char *));
    while (fscanf(fp, "%63s", line) == 1) {
        if (strcmp(line, mapset_name) == 0)
            continue;   /* skip the one we're removing */
        if (n >= cap) {
            cap  *= 2;
            names = G_realloc(names, cap * sizeof(char *));
        }
        names[n++] = G_store(line);
    }
    fclose(fp);

    fp = G_fopen_new("", "SEARCH_PATH");
    if (fp) {
        for (i = 0; i < n; i++)
            fprintf(fp, "%s\n", names[i]);
        fclose(fp);
    }

    for (i = 0; i < n; i++)
        G_free(names[i]);
    G_free(names);
}

/* -------------------------------------------------------------------------
 * Determine the /dev/shm (or fallback tmpfs) backing path for the mapset.
 * Path is stable: same location + mapset_name always maps to the same dir.
 * -------------------------------------------------------------------------*/
static void get_shm_dir(const char *mapset_name, char *shm_dir, size_t size)
{
    struct stat st;
    const char *base = "/dev/shm";

    if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) {
        base = getenv("TMPDIR");
        if (!base || !*base)
            base = "/tmp";
        G_verbose_message(_("/dev/shm not available, using %s"), base);
    }

    snprintf(shm_dir, size, "%s/grass_r3mem_%s_%s",
             base, G_location(), mapset_name);
}

/* -------------------------------------------------------------------------
 * Set up the RAM-backed GRASS mapset:
 *   1. Create /dev/shm/grass_r3mem_{loc}_{mapset}/ directory
 *   2. Symlink $GISDBASE/$LOCATION/$mapset  ->  shm_dir
 *   3. Write WIND file if the mapset is new
 *   4. Persist in SEARCH_PATH
 * -------------------------------------------------------------------------*/
static void setup_ram_mapset(const char *mapset_name)
{
    char shm_dir[GPATH_MAX];
    char link_path[GPATH_MAX];
    char wind_path[GPATH_MAX];
    struct stat st;

    get_shm_dir(mapset_name, shm_dir, sizeof(shm_dir));

    snprintf(link_path, sizeof(link_path), "%s/%s/%s",
             G_gisdbase(), G_location(), mapset_name);

    /* Create RAM backing directory */
    if (G_mkdir(shm_dir) != 0 && errno != EEXIST)
        G_fatal_error(_("Cannot create RAM directory <%s>: %s"),
                      shm_dir, strerror(errno));

    /* Create symlink from GRASS location into /dev/shm */
    if (lstat(link_path, &st) != 0) {
        if (symlink(shm_dir, link_path) != 0)
            G_fatal_error(_("Cannot create symlink %s -> %s: %s"),
                          link_path, shm_dir, strerror(errno));
        G_verbose_message(_("Linked RAM mapset: %s -> %s"),
                          mapset_name, shm_dir);
    }
    else if (!S_ISLNK(st.st_mode) && !S_ISDIR(st.st_mode)) {
        G_fatal_error(_("Path <%s> exists and is neither a directory nor "
                        "a symlink"), link_path);
    }

    /* Initialize WIND file if this is a fresh mapset */
    snprintf(wind_path, sizeof(wind_path), "%s/WIND", shm_dir);
    if (access(wind_path, F_OK) != 0) {
        init_shm_mapset_wind(mapset_name);
        G_verbose_message(_("Initialized RAM mapset <%s>"), mapset_name);
    }

    /* In-memory search path for this process */
    G_add_mapset_to_search_path(mapset_name);

    /* Persistent SEARCH_PATH so r.mapcalc and other processes find maps */
    persist_in_search_path(mapset_name);
}

/* -------------------------------------------------------------------------
 * Extract one Z-slice from the already-open 3D raster map and write it
 * as a 2D GRASS raster in the RAM mapset.
 *
 * The slice is read with Rast3d_get_block().  Because map was opened with
 * RASTER3D_NO_CACHE (map->useCache == 0), Rast3d_get_block() calls
 * Rast3d_get_block_nocache() — the tile-bulk path that reads each tile at
 * the target Z level exactly once.
 *
 * Block layout: buf[row * ncols + col]  (nz=1 collapses the z dimension)
 * This maps directly to Rast_put_{f,d}_row(fd, buf + row*ncols).
 *
 * Null values are written to the RAM mapset by temporarily redirecting the
 * GRASS environment (G_create_alt_env / G_switch_env) so that Rast_open_new
 * and related functions target the RAM mapset directory.
 * -------------------------------------------------------------------------*/
static void extract_slice(RASTER3D_Map *map, int z,
                          const char *outname, const char *ram_mapset,
                          int nrows, int ncols, int type)
{
    void *buf;
    int fd, row, nbytes;

    nbytes = (type == FCELL_TYPE) ? (int)sizeof(FCELL) : (int)sizeof(DCELL);
    buf    = G_malloc((size_t)nrows * (size_t)ncols * (size_t)nbytes);

    /* ---- Bulk read: tile-based, each tile at this Z level read once ---- */
    Rast3d_get_block(map, 0, 0, z, ncols, nrows, 1, buf, type);

    /* ---- Write into the RAM mapset ---- */
    /* Temporarily redirect GRASS file operations to the RAM mapset.
     * G_create_alt_env() deep-copies the current env; G_setenv_nogisrc()
     * modifies the copy; G_switch_env() restores the original afterward. */
    G_create_alt_env();
    G_setenv_nogisrc("MAPSET", ram_mapset);

    fd = Rast_open_new(outname, type);
    if (fd < 0)
        G_fatal_error(_("Cannot create output raster <%s@%s>"),
                      outname, ram_mapset);

    for (row = 0; row < nrows; row++) {
        if (type == FCELL_TYPE)
            Rast_put_f_row(fd, (FCELL *)buf + (size_t)row * ncols);
        else
            Rast_put_d_row(fd, (DCELL *)buf + (size_t)row * ncols);
    }

    Rast_close(fd);

    /* Restore original mapset environment */
    G_switch_env();

    G_free(buf);
}

/* -------------------------------------------------------------------------
 * Remove one named 2D raster map from the RAM mapset.
 * Used for iterative workflows: load slice → process → free → next slice.
 * Directly removes the known filesystem elements; no alt-env needed since
 * we operate on the /dev/shm path directly.
 * -------------------------------------------------------------------------*/
static void remove_map_from_ram_mapset(const char *mapname,
                                       const char *ram_mapset)
{
    char shm_dir[GPATH_MAX];
    char path[GPATH_MAX];
    char cmd[GPATH_MAX + 16];

    /* File-based raster elements */
    static const char *file_elems[] = {
        "cell", "hist", "cats", "colr", "fcell", NULL
    };

    get_shm_dir(ram_mapset, shm_dir, sizeof(shm_dir));

    for (int i = 0; file_elems[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s",
                 shm_dir, file_elems[i], mapname);
        unlink(path);   /* silently ignore ENOENT */
    }

    /* cell_misc/<mapname>/ is a directory tree (range, f_format, null, …) */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/cell_misc/%s'", shm_dir, mapname);
    system(cmd);

    G_verbose_message(_("Freed RAM map <%s@%s>"), mapname, ram_mapset);
}

/* -------------------------------------------------------------------------
 * Remove the RAM mapset: delete the symlink, erase the /dev/shm directory
 * tree and remove the mapset from SEARCH_PATH.
 * -------------------------------------------------------------------------*/
static void do_cleanup(const char *mapset_name)
{
    char shm_dir[GPATH_MAX];
    char link_path[GPATH_MAX];

    get_shm_dir(mapset_name, shm_dir, sizeof(shm_dir));
    snprintf(link_path, sizeof(link_path), "%s/%s/%s",
             G_gisdbase(), G_location(), mapset_name);

    /* Remove symlink */
    if (unlink(link_path) == 0)
        G_verbose_message(_("Removed symlink <%s>"), link_path);
    else if (errno != ENOENT)
        G_warning(_("Cannot remove symlink <%s>: %s"),
                  link_path, strerror(errno));

    /* Remove /dev/shm tree.  Use the shell since GRASS has no recursive
     * rmdir helper, and the content is throwaway RAM. */
    {
        char cmd[GPATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", shm_dir);
        if (system(cmd) != 0)
            G_warning(_("Could not fully remove <%s>"), shm_dir);
        else
            G_verbose_message(_("Removed RAM storage <%s>"), shm_dir);
    }

    /* Remove from SEARCH_PATH */
    remove_from_search_path(mapset_name);
    G_message(_("RAM mapset <%s> removed."), mapset_name);
}

/* =========================================================================
 * main
 * =========================================================================*/
int main(int argc, char *argv[])
{
    struct GModule *module;
    struct {
        struct Option *input;
        struct Option *output;
        struct Option *zrange;
        struct Option *mapset;
        struct Option *remove;
    } opts;
    struct {
        struct Flag *list;
        struct Flag *cleanup;
    } flags;

    const char *input, *output, *ram_mapset, *found_mapset;
    char in_name[GNAME_MAX];
    char in_mapset_spec[GMAPSET_MAX];
    RASTER3D_Map     *map;
    RASTER3D_Region   region;
    int              *zlist, nz, type, ncols, nrows, ndepths, i;

    G_gisinit(argv[0]);

    /* ------------------------------------------------------------------ */
    module = G_define_module();
    G_add_keyword(_("raster3d"));
    G_add_keyword(_("voxel"));
    G_add_keyword(_("RAM"));
    G_add_keyword(_("in-memory"));
    G_add_keyword(_("performance"));
    module->description =
        _("Extracts 2D raster slices from a 3D raster map into a "
          "RAM-backed in-memory GRASS mapset. Uses tile-bulk reads "
          "(Rast3d_get_block with RASTER3D_NO_CACHE) for fast extraction "
          "and stores results in /dev/shm so r.mapcalc reads them from RAM.");

    opts.input           = G_define_standard_option(G_OPT_R3_INPUT);
    opts.input->required = NO;   /* not needed for -c or remove= */

    opts.output             = G_define_option();
    opts.output->key        = "output";
    opts.output->type       = TYPE_STRING;
    opts.output->required   = NO;
    opts.output->gisprompt  = "new,cell,raster";
    opts.output->description =
        _("Base name for output 2D raster map(s). "
          "Single slice: named exactly <output>. "
          "Multiple slices: named <output>.<z> for each Z index. "
          "Default: same as input map name.");

    opts.zrange             = G_define_option();
    opts.zrange->key        = "zrange";
    opts.zrange->type       = TYPE_STRING;
    opts.zrange->required   = NO;
    opts.zrange->answer     = "all";
    opts.zrange->description =
        _("Z slice indices to extract. "
          "Formats: \"all\" (default), single index \"5\", "
          "comma-separated \"0,5,10\", range \"0-9\", "
          "or combinations \"0-4,10,15-19\".");

    opts.mapset             = G_define_option();
    opts.mapset->key        = "mapset";
    opts.mapset->type       = TYPE_STRING;
    opts.mapset->required   = NO;
    opts.mapset->answer     = "MEMORY";
    opts.mapset->description =
        _("Name for the in-memory GRASS mapset (backed by /dev/shm). "
          "Created once and reused across calls. "
          "Added to SEARCH_PATH so maps are accessible without @MEMORY.");

    opts.remove              = G_define_option();
    opts.remove->key         = "remove";
    opts.remove->type        = TYPE_STRING;
    opts.remove->required    = NO;
    opts.remove->multiple    = YES;
    opts.remove->description =
        _("Name(s) of 2D raster map(s) to remove from the RAM mapset. "
          "Accepts a comma-separated list. "
          "Use during iterative processing to free RAM after each slice "
          "is no longer needed before loading the next one.");

    flags.list              = G_define_flag();
    flags.list->key         = 'l';
    flags.list->description =
        _("Print 3D raster dimensions (depths/rows/cols/type) and exit.");

    flags.cleanup           = G_define_flag();
    flags.cleanup->key      = 'c';
    flags.cleanup->description =
        _("Remove the RAM mapset and its /dev/shm backing storage, "
          "then exit. Cleans symlink and SEARCH_PATH entry.");

    if (G_parser(argc, argv))
        exit(EXIT_FAILURE);

    input      = opts.input->answer;
    ram_mapset = opts.mapset->answer;

    /* ------------------------------------------------------------------ */
    /* Full cleanup mode  (-c)                                            */
    if (flags.cleanup->answer) {
        do_cleanup(ram_mapset);
        exit(EXIT_SUCCESS);
    }

    /* ------------------------------------------------------------------ */
    /* Per-map remove mode  (remove=name[,name,...])                      */
    if (opts.remove->answers) {
        int removed = 0;
        for (int r = 0; opts.remove->answers[r]; r++) {
            remove_map_from_ram_mapset(opts.remove->answers[r], ram_mapset);
            removed++;
        }
        G_message(removed == 1
                  ? _("Freed 1 map from RAM mapset <%s>.")
                  : _("Freed %d maps from RAM mapset <%s>."),
                  removed, ram_mapset);
        exit(EXIT_SUCCESS);
    }

    /* ------------------------------------------------------------------ */
    /* Extraction mode: input= is required                                */
    if (!input)
        G_fatal_error(_("Option <input> is required for slice extraction. "
                        "Use -c to clean up or remove= to free specific maps."));

    /* ------------------------------------------------------------------ */
    /* Parse input map name (may contain @mapset suffix)                   */
    if (G_name_is_fully_qualified(input, in_name, in_mapset_spec)) {
        /* user provided name@mapset */
    }
    else {
        snprintf(in_name, sizeof(in_name), "%s", input);
        in_mapset_spec[0] = '\0';
    }

    output = opts.output->answer ? opts.output->answer : in_name;

    /* ------------------------------------------------------------------ */
    /* Find and open the 3D raster                                         */
    found_mapset = G_find_raster3d(in_name,
                                   in_mapset_spec[0] ? in_mapset_spec : "");
    if (!found_mapset)
        G_fatal_error(_("3D raster map <%s> not found"), input);

    Rast3d_init_defaults();
    Rast3d_get_window(&region);

    /* Open with RASTER3D_NO_CACHE.
     *
     * This is the critical choice for performance: with any cache mode,
     * Rast3d_get_block() falls back to per-voxel Rast3d_get_value_region()
     * calls (see getblock.c:106).  With RASTER3D_NO_CACHE (useCache=0),
     * it calls Rast3d_get_block_nocache() which reads each tile once and
     * bulk-copies the Z-plane — typically 100-1000x fewer operations.    */
    map = Rast3d_open_cell_old(in_name, found_mapset, &region,
                               RASTER3D_TILE_SAME_AS_FILE,
                               RASTER3D_NO_CACHE);
    if (!map)
        G_fatal_error(_("Unable to open 3D raster map <%s>"), input);

    /* Get native map dimensions (not window-resampled) */
    Rast3d_get_region_struct_map(map, &region);
    ncols   = region.cols;
    nrows   = region.rows;
    ndepths = region.depths;
    type    = Rast3d_file_type_map(map);

    /* ------------------------------------------------------------------ */
    /* List mode                                                           */
    if (flags.list->answer) {
        fprintf(stdout,
                "map=%s  depths=%d  rows=%d  cols=%d  type=%s\n",
                in_name, ndepths, nrows, ncols,
                type == FCELL_TYPE ? "FCELL" : "DCELL");
        Rast3d_close(map);
        exit(EXIT_SUCCESS);
    }

    /* ------------------------------------------------------------------ */
    /* Parse Z range                                                       */
    zlist = parse_zrange(opts.zrange->answer, ndepths, &nz);

    /* Sanity-check that the 2D computation window matches the 3D map.    */
    {
        struct Cell_head w2d;
        G_get_window(&w2d);
        if (w2d.rows != nrows || w2d.cols != ncols)
            G_warning(
                _("2D region (%d rows × %d cols) differs from 3D map "
                  "region (%d rows × %d cols). "
                  "Output maps will use 3D map dimensions. "
                  "Consider: g.region raster3d=%s"),
                w2d.rows, w2d.cols, nrows, ncols, in_name);
    }

    /* ------------------------------------------------------------------ */
    /* Set up the RAM-backed mapset                                        */
    setup_ram_mapset(ram_mapset);

    G_message(
        _("Extracting %d slice(s) from <%s> "
          "[%d depths × %d rows × %d cols, %s] into <%s>..."),
        nz, in_name, ndepths, nrows, ncols,
        type == FCELL_TYPE ? "FCELL" : "DCELL",
        ram_mapset);

    /* ------------------------------------------------------------------ */
    /* Extract requested slices                                            */
    for (i = 0; i < nz; i++) {
        char outname[GNAME_MAX];

        /* Single slice → plain name; multiple → name.z */
        if (nz == 1)
            snprintf(outname, sizeof(outname), "%s", output);
        else
            snprintf(outname, sizeof(outname), "%s.%d", output, zlist[i]);

        G_percent(i, nz, 2);

        extract_slice(map, zlist[i], outname, ram_mapset,
                      nrows, ncols, type);

        G_debug(1, "z=%d -> %s@%s", zlist[i], outname, ram_mapset);
    }
    G_percent(nz, nz, 2);

    /* ------------------------------------------------------------------ */
    Rast3d_close(map);

    /* Report */
    if (nz == 1) {
        G_message(_("Created: %s@%s  (z=%d)"),
                  output, ram_mapset, zlist[0]);
    }
    else {
        char first[GNAME_MAX], last[GNAME_MAX];
        snprintf(first, sizeof(first), "%s.%d", output, zlist[0]);
        snprintf(last,  sizeof(last),  "%s.%d", output, zlist[nz - 1]);
        G_message(_("Created %d maps: %s .. %s @%s"),
                  nz, first, last, ram_mapset);
    }
    G_message(
        _("Mapset <%s> is in the search path — "
          "maps are accessible without @%s qualifier."),
        ram_mapset, ram_mapset);

    G_free(zlist);
    exit(EXIT_SUCCESS);
}
