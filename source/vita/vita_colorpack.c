#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_set.h"
#include "vita.h"
#include "vita_color.h"
#include "vita_colorpack.h"

#define CP_BUILTIN_DIR "app0:assets/colorpacks"
#define CP_USER_DIR RV_DATA_DIR "/colorpacks"

#define GRID_W 48
#define GRID_H 28
#define GRID_CELLS (GRID_W * GRID_H)
#define CONTEXT_REACH 4
#define MAX_GROUPS 256
#define VARIANT_WAYS 4
#define VARIANT_COUNT (CP_TOTAL_SLOTS - CP_BASE_SLOTS)
#define VARIANT_SETS (VARIANT_COUNT / VARIANT_WAYS)
#define FIGURE_SIZE 128

typedef struct {
    uint64_t w[4];
} CpBits;

typedef struct {
    uint64_t mask;
    uint8_t rgb[64][3];
} CpTile;

typedef struct {
    uint64_t key;
    CpTile tile;
} CpKeyed;

typedef struct {
    uint32_t hash;
    uint16_t cell;
    uint8_t palette;
    uint64_t mask;
    uint64_t keep;
    uint8_t rgb[64][3];
} CpCell;

typedef struct {
    uint32_t hash;
    uint32_t group;
    CpTile tile;
} CpContext;

typedef struct {
    uint32_t hash;
    CpBits bits;
} CpMarker;

typedef struct {
    CpKeyed *tiles;
    uint32_t tile_count;
    CpKeyed *layers;
    uint32_t layer_count;
    CpKeyed *palettes;
    uint32_t palette_count;
    CpCell *cells;
    uint32_t cell_count;
    uint32_t *cell_start;
    uint32_t *cell_hashes;
    uint32_t cell_hash_count;
    uint32_t *layered;
    uint32_t layered_count;
    CpContext *contexts;
    uint32_t context_count;
    CpMarker *markers;
    uint32_t marker_count;
    CpBits layer_bound;
    bool any_layer_bound;
    uint32_t group_count;
    int reference_level;
} CpPack;

typedef struct {
    uint32_t hash;
    const CpTile *base;
    const CpTile *pal[4];
    const CpTile *layer_tile;
    uint8_t layer_world;
    bool layered;
    bool cell_colored;
    bool simple;
    bool marker;
    bool colored;
    bool valid;
    uint32_t ctx_first, ctx_count;
    CpBits marker_bits;
} CpSlot;

typedef struct {
    uint32_t hash;
    const CpCell *cell;
    const CpTile *tile;
    uint32_t used;
    uint8_t fill;
    uint8_t valid;
} CpVariant;

typedef struct {
    CpBits sprite[GRID_CELLS];
    CpBits bg[GRID_CELLS];
    int8_t bg_layer[GRID_CELLS];
} CpGrid;

bool cp_frame_on = false;

static CpPack *pack = NULL;
static bool game_active = false;
static CpSlot slots[CP_BASE_SLOTS];
static CpVariant variants[VARIANT_COUNT];
static uint32_t frame_no = 1;
static CpGrid grids[3];
static int grid_cur = 0;
static CpBits near_cache[GRID_CELLS], close_cache[GRID_CELLS];
static uint32_t near_stamp[GRID_CELLS];

static int world_colors[32];
static uint32_t auto_worlds = 0;
static uint32_t figure_worlds = 0;
static uint32_t ramp_state = 0xFFFFFFFF;
static uint32_t ramp_figures = 0xFFFFFFFF;
static float layer_ramps[32][3][3];
static int cur_colors = 0;
static bool cur_fill = false;
static int auto_max_level = 0;
static CpShading shading;

static const uint8_t layer_ramp_table[4][3][3] = {
    {{36, 20, 66}, {112, 72, 150}, {212, 184, 232}},
    {{102, 34, 20}, {210, 100, 48}, {250, 198, 132}},
    {{116, 62, 18}, {230, 152, 56}, {255, 234, 176}},
    {{70, 72, 40}, {168, 170, 92}, {240, 240, 196}},
};
static const uint8_t sprite_ramp_table[4][3][3] = {
    {{132, 24, 24}, {236, 82, 44}, {255, 226, 128}},
    {{24, 46, 132}, {70, 128, 236}, {206, 232, 255}},
    {{22, 96, 36}, {70, 196, 78}, {210, 252, 176}},
    {{108, 22, 96}, {214, 76, 176}, {255, 206, 242}},
};
static const uint8_t base_table[4][3] = {{6, 6, 14}, {70, 76, 98}, {140, 148, 170}, {226, 230, 240}};

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool bits_any(const CpBits *b) {
    return (b->w[0] | b->w[1] | b->w[2] | b->w[3]) != 0;
}

static bool bits_test(const CpBits *b, uint32_t g) {
    return (b->w[g >> 6] >> (g & 63)) & 1;
}

static void bits_or(CpBits *d, const CpBits *s) {
    for (int i = 0; i < 4; i++) d->w[i] |= s->w[i];
}

static void bits_and(CpBits *d, const CpBits *s) {
    for (int i = 0; i < 4; i++) d->w[i] &= s->w[i];
}

static void bits_andnot(CpBits *d, const CpBits *s) {
    for (int i = 0; i < 4; i++) d->w[i] &= ~s->w[i];
}

static uint32_t hash_rows(const uint16_t rows[8]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) {
        h = (h ^ (rows[i] & 0xFF)) * 16777619u;
        h = (h ^ (rows[i] >> 8)) * 16777619u;
    }
    return h;
}

static void free_pack(CpPack *p) {
    if (!p) return;
    free(p->tiles);
    free(p->layers);
    free(p->palettes);
    free(p->cells);
    free(p->cell_start);
    free(p->cell_hashes);
    free(p->layered);
    free(p->contexts);
    free(p->markers);
    free(p);
}

static int cmp_keyed(const void *a, const void *b) {
    uint64_t x = ((const CpKeyed *)a)->key, y = ((const CpKeyed *)b)->key;
    return x < y ? -1 : x > y;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int cmp_cell(const void *a, const void *b) {
    const CpCell *x = a, *y = b;
    if (x->cell != y->cell) return x->cell < y->cell ? -1 : 1;
    if (x->palette != y->palette) return x->palette < y->palette ? -1 : 1;
    return x->hash < y->hash ? -1 : x->hash > y->hash;
}

static int cmp_context(const void *a, const void *b) {
    const CpContext *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->group < y->group ? -1 : x->group > y->group;
}

static int cmp_marker(const void *a, const void *b) {
    uint32_t x = ((const CpMarker *)a)->hash, y = ((const CpMarker *)b)->hash;
    return x < y ? -1 : x > y;
}

static void sort_keyed(CpKeyed *k, uint32_t n) {
    for (uint32_t i = 1; i < n; i++)
        if (k[i - 1].key > k[i].key) {
            qsort(k, n, sizeof(*k), cmp_keyed);
            return;
        }
}

static uint32_t unique_u32(uint32_t *v, uint32_t n) {
    if (!n) return 0;
    qsort(v, n, sizeof(*v), cmp_u32);
    uint32_t m = 1;
    for (uint32_t i = 1; i < n; i++)
        if (v[i] != v[m - 1]) v[m++] = v[i];
    return m;
}

static const CpTile *find_keyed(const CpKeyed *k, uint32_t n, uint64_t key) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (k[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && k[lo].key == key ? &k[lo].tile : NULL;
}

static bool has_u32(const uint32_t *v, uint32_t n, uint32_t x) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (v[mid] < x) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && v[lo] == x;
}

static void read_tile(CpTile *t, const uint8_t *p) {
    t->mask = rd64(p);
    memcpy(t->rgb, p + 8, sizeof(t->rgb));
}

static bool read_variants(const uint8_t *b, size_t len, size_t *off, unsigned bits, CpKeyed **out, uint32_t *count) {
    if (*off + 4 > len) return false;
    uint32_t n = rd32(b + *off);
    *off += 4;
    if ((uint64_t)n * 208 > len - *off) return false;
    CpKeyed *k = n ? malloc(sizeof(CpKeyed) * n) : NULL;
    if (n && !k) return false;
    for (uint32_t i = 0; i < n; i++) {
        k[i].key = ((uint64_t)rd32(b + *off) << bits) | rd32(b + *off + 4);
        read_tile(&k[i].tile, b + *off + 8);
        *off += 208;
    }
    sort_keyed(k, n);
    *out = k;
    *count = n;
    return true;
}

static CpPack *parse_pack(const uint8_t *b, size_t len) {
    static const char magic1[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '1'};
    static const char magic2[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '2'};
    static const char magic3[8] = {'V', 'B', 'G', 'O', 'C', 'P', '0', '3'};
    if (len < 12) return NULL;
    int version = !memcmp(b, magic3, 8) ? 3 : !memcmp(b, magic2, 8) ? 2 : !memcmp(b, magic1, 8) ? 1 : 0;
    if (!version) return NULL;
    CpPack *p = calloc(1, sizeof(CpPack));
    if (!p) return NULL;
    p->reference_level = 63;
    size_t off = 8;
    uint32_t n = rd32(b + off);
    off += 4;
    if ((uint64_t)n * 204 > len - off) goto fail;
    p->tiles = n ? malloc(sizeof(CpKeyed) * n) : NULL;
    if (n && !p->tiles) goto fail;
    for (uint32_t i = 0; i < n; i++) {
        p->tiles[i].key = rd32(b + off);
        read_tile(&p->tiles[i].tile, b + off + 4);
        off += 204;
    }
    p->tile_count = n;
    sort_keyed(p->tiles, n);
    if (version >= 2 && off + 4 <= len) {
        if (!read_variants(b, len, &off, 5, &p->layers, &p->layer_count)) goto fail;
        if (off + 4 <= len) {
            uint32_t left = rd32(b + off);
            off += 4;
            if ((uint64_t)left * 12 > len - off) goto fail;
            off += (size_t)left * 12;
        }
    }
    if (version >= 3) {
        if (!read_variants(b, len, &off, 2, &p->palettes, &p->palette_count)) goto fail;
        if (off + 4 > len) goto fail;
        uint32_t cells = rd32(b + off);
        off += 4;
        if ((uint64_t)cells * 216 > len - off) goto fail;
        p->cells = cells ? malloc(sizeof(CpCell) * cells) : NULL;
        if (cells && !p->cells) goto fail;
        for (uint32_t i = 0; i < cells; i++) {
            const uint8_t *q = b + off;
            CpCell *c = &p->cells[i];
            c->cell = (uint16_t)(q[0] | (q[1] << 8));
            c->palette = q[2] & 3;
            c->hash = rd32(q + 4);
            c->mask = rd64(q + 8);
            c->keep = rd64(q + 16);
            memcpy(c->rgb, q + 24, sizeof(c->rgb));
            off += 216;
        }
        p->cell_count = cells;
        if (off + 4 <= len) {
            uint32_t level = rd32(b + off);
            off += 4;
            p->reference_level = level < 1 ? 1 : level > 63 ? 63 : (int)level;
        }
        if (cells) qsort(p->cells, cells, sizeof(CpCell), cmp_cell);
        if (off + 12 <= len && !memcmp(b + off, "VBGOCTX1", 8)) {
            off += 8;
            uint32_t groups = rd32(b + off);
            off += 4;
            bool ok = groups <= MAX_GROUPS;
            uint32_t **group_markers = calloc(groups ? groups : 1, sizeof(uint32_t *));
            uint32_t *group_sizes = calloc(groups ? groups : 1, sizeof(uint32_t));
            ok = ok && group_markers && group_sizes;
            uint32_t total = 0;
            for (uint32_t g = 0; ok && g < groups; g++) {
                ok = off + 4 <= len;
                if (!ok) break;
                uint32_t m = rd32(b + off);
                off += 4;
                ok = (uint64_t)m * 4 <= len - off;
                if (!ok) break;
                group_markers[g] = malloc(sizeof(uint32_t) * (m ? m : 1));
                ok = group_markers[g] != NULL;
                for (uint32_t i = 0; ok && i < m; i++) group_markers[g][i] = rd32(b + off + i * 4);
                group_sizes[g] = m;
                total += m;
                off += (size_t)m * 4;
            }
            uint32_t ctx = 0;
            if (ok) {
                ok = off + 4 <= len;
                if (ok) {
                    ctx = rd32(b + off);
                    off += 4;
                    ok = (uint64_t)ctx * 208 <= len - off;
                }
            }
            CpContext *contexts = NULL;
            if (ok && ctx) {
                contexts = malloc(sizeof(CpContext) * ctx);
                ok = contexts != NULL;
            }
            for (uint32_t i = 0; ok && i < ctx; i++) {
                contexts[i].hash = rd32(b + off);
                contexts[i].group = rd32(b + off + 4);
                read_tile(&contexts[i].tile, b + off + 8);
                ok = contexts[i].group < groups;
                off += 208;
            }
            CpMarker *markers = NULL;
            if (ok && total) {
                markers = calloc(total, sizeof(CpMarker));
                ok = markers != NULL;
            }
            if (ok) {
                uint32_t k = 0;
                for (uint32_t g = 0; g < groups; g++)
                    for (uint32_t i = 0; i < group_sizes[g]; i++) {
                        markers[k].hash = group_markers[g][i];
                        markers[k].bits.w[g >> 6] |= 1ull << (g & 63);
                        k++;
                    }
                if (total) qsort(markers, total, sizeof(CpMarker), cmp_marker);
                uint32_t m = 0;
                for (uint32_t i = 0; i < total; i++) {
                    if (m && markers[m - 1].hash == markers[i].hash) bits_or(&markers[m - 1].bits, &markers[i].bits);
                    else markers[m++] = markers[i];
                }
                if (ctx) qsort(contexts, ctx, sizeof(CpContext), cmp_context);
                p->contexts = contexts;
                p->context_count = ctx;
                p->markers = markers;
                p->marker_count = m;
                p->group_count = groups;
                contexts = NULL;
                markers = NULL;
                if (off + 16 <= len && !memcmp(b + off, "VBGOCTXL", 8)) {
                    uint64_t v = rd64(b + off + 8);
                    off += 16;
                    p->layer_bound.w[0] = groups >= 64 ? v : v & ((1ull << groups) - 1);
                } else if (off + 40 <= len && !memcmp(b + off, "VBGOCTXW", 8)) {
                    for (int i = 0; i < 4; i++) p->layer_bound.w[i] = rd64(b + off + 8 + i * 8);
                    off += 40;
                    for (uint32_t g = groups; g < MAX_GROUPS; g++) p->layer_bound.w[g >> 6] &= ~(1ull << (g & 63));
                }
                p->any_layer_bound = bits_any(&p->layer_bound);
            }
            free(contexts);
            free(markers);
            if (group_markers)
                for (uint32_t g = 0; g < groups; g++) free(group_markers[g]);
            free(group_markers);
            free(group_sizes);
        }
    }
    if (p->cell_count) {
        p->cell_start = malloc(sizeof(uint32_t) * 65537);
        p->cell_hashes = malloc(sizeof(uint32_t) * p->cell_count);
        if (!p->cell_start || !p->cell_hashes) goto fail;
        uint32_t i = 0;
        for (uint32_t c = 0; c <= 65536; c++) {
            while (i < p->cell_count && p->cells[i].cell < c) i++;
            p->cell_start[c] = i;
        }
        for (i = 0; i < p->cell_count; i++) p->cell_hashes[i] = p->cells[i].hash;
        p->cell_hash_count = unique_u32(p->cell_hashes, p->cell_count);
    }
    if (p->layer_count) {
        p->layered = malloc(sizeof(uint32_t) * p->layer_count);
        if (!p->layered) goto fail;
        for (uint32_t i = 0; i < p->layer_count; i++) p->layered[i] = (uint32_t)(p->layers[i].key >> 5);
        p->layered_count = unique_u32(p->layered, p->layer_count);
    }
    return p;
fail:
    free_pack(p);
    return NULL;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static CpPack *load_file(const char *path) {
    size_t len = 0;
    uint8_t *b = read_file(path, &len);
    if (!b) return NULL;
    CpPack *p = parse_pack(b, len);
    free(b);
    return p;
}

static bool footer_matches(const char *path, uint32_t crc, uint32_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t tail[16];
    bool ok = fseek(f, -16, SEEK_END) == 0 && fread(tail, 1, 16, f) == 16;
    fclose(f);
    return ok && !memcmp(tail, "VBGOROM1", 8) && rd32(tail + 8) == crc && (!size || rd32(tail + 12) == size);
}

static void name_key(char *out, size_t len, const char *name, bool base_only) {
    size_t n = 0;
    int depth = 0;
    bool sep = false;
    for (const char *p = name; *p && n + 1 < len; p++) {
        unsigned char c = (unsigned char)*p;
        if (base_only && (c == '(' || c == '[')) {
            depth++;
            continue;
        }
        if (base_only && (c == ')' || c == ']')) {
            if (depth) depth--;
            continue;
        }
        if (depth) continue;
        if (isalnum(c)) {
            if (sep && n > 0 && n + 2 < len) out[n++] = '_';
            out[n++] = (char)tolower(c);
            sep = false;
        } else {
            sep = true;
        }
    }
    out[n] = 0;
}

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static CpPack *scan_user_dir(const char *key, uint32_t crc, uint32_t size) {
    DIR *d = opendir(CP_USER_DIR);
    if (!d) return NULL;
    char path[768];
    char found[768] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_ext(e->d_name, ".vbcp")) continue;
        char stem[256], k[256];
        snprintf(stem, sizeof(stem), "%s", e->d_name);
        stem[strlen(stem) - 5] = 0;
        name_key(k, sizeof(k), stem, false);
        snprintf(path, sizeof(path), CP_USER_DIR "/%s", e->d_name);
        if (!strcmp(k, key)) {
            snprintf(found, sizeof(found), "%s", path);
            break;
        }
        if (!found[0] && footer_matches(path, crc, size)) snprintf(found, sizeof(found), "%s", path);
    }
    closedir(d);
    return found[0] ? load_file(found) : NULL;
}

static CpPack *scan_builtin(const char *key, const char *base, uint32_t crc, uint32_t size) {
    FILE *f = fopen(CP_BUILTIN_DIR "/index.txt", "r");
    if (!f) return NULL;
    char line[512], best[160] = "";
    int best_rank = 0;
    while (fgets(line, sizeof(line), f)) {
        char k[160], b[160];
        unsigned int c = 0, s = 0;
        if (sscanf(line, "%159s %x %u %159s", k, &c, &s, b) != 4) continue;
        int rank = 0;
        if (!strcmp(k, key)) rank = 4;
        else if (c == crc && (!s || s == size)) rank = 3;
        else if (strcmp(b, "-") && !strcmp(b, base)) rank = 1;
        if (rank > best_rank) {
            best_rank = rank;
            snprintf(best, sizeof(best), "%s", k);
        }
    }
    fclose(f);
    if (!best_rank) return NULL;
    char path[512];
    snprintf(path, sizeof(path), CP_BUILTIN_DIR "/%s.vbcp", best);
    return load_file(path);
}

static void reset_runtime(void) {
    memset(slots, 0, sizeof(slots));
    memset(variants, 0, sizeof(variants));
    memset(grids, 0, sizeof(grids));
    memset(near_stamp, 0, sizeof(near_stamp));
    for (int i = 0; i < 3; i++) memset(grids[i].bg_layer, -1, sizeof(grids[i].bg_layer));
    frame_no = 1;
    auto_worlds = 0;
    figure_worlds = 0;
    ramp_state = ramp_figures = 0xFFFFFFFF;
    auto_max_level = 0;
}

bool cp_load_for_game(const char *rom_path, const char *game_name, uint32_t crc, uint32_t size) {
    cp_unload();
    game_active = true;
    reset_runtime();
    char key[256], base[256], path[768];
    name_key(key, sizeof(key), game_name, false);
    name_key(base, sizeof(base), game_name, true);
    snprintf(path, sizeof(path), "%s", rom_path);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash)) {
        snprintf(dot, sizeof(path) - (size_t)(dot - path), ".vbcp");
        pack = load_file(path);
    }
    if (!pack) pack = scan_user_dir(key, crc, size);
    if (!pack) pack = scan_builtin(key, base, crc, size);
    cp_invalidate();
    return pack != NULL;
}

void cp_unload(void) {
    free_pack(pack);
    pack = NULL;
    game_active = false;
    reset_runtime();
}

bool cp_has_pack(void) {
    return pack != NULL;
}

bool cp_display_on(void) {
    if (!game_active) return false;
    if (settings.color_mode == COLOR_MODE_AUTO) return true;
    return settings.color_mode == COLOR_MODE_PACK && pack;
}

void cp_invalidate(void) {
    for (int i = 0; i < CP_BASE_SLOTS; i++) {
        slots[i].valid = false;
        slots[i].colored = true;
    }
    memset(variants, 0, sizeof(variants));
}

static uint32_t pack_rgb(const uint8_t *rgb) {
    return rgb[0] | (rgb[1] << 8) | ((uint32_t)rgb[2] << 16) | 0xFF000000u;
}

static const uint16_t *chr_rows(int chr) {
    return (const uint16_t *)(vb_state->V810_DISPLAY_RAM.off + ((chr & 0x600) << 6) + 0x6000 + (chr & 0x1ff) * 16);
}

static void resolve_slot(CpSlot *s, uint32_t hash) {
    bool colored = s->colored;
    memset(s, 0, sizeof(*s));
    s->hash = hash;
    s->valid = true;
    s->colored = colored;
    s->layer_world = 0xFF;
    if (!pack) {
        s->simple = true;
        return;
    }
    s->base = find_keyed(pack->tiles, pack->tile_count, hash);
    bool same = true;
    for (int p = 0; p < 4; p++) {
        const CpTile *v = pack->palette_count ? find_keyed(pack->palettes, pack->palette_count, ((uint64_t)hash << 2) | p) : NULL;
        s->pal[p] = v ? v : s->base;
        if (s->pal[p] != s->base) same = false;
    }
    s->layered = pack->layered_count && has_u32(pack->layered, pack->layered_count, hash);
    s->cell_colored = pack->cell_hash_count && has_u32(pack->cell_hashes, pack->cell_hash_count, hash);
    if (pack->context_count) {
        uint32_t lo = 0, hi = pack->context_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (pack->contexts[mid].hash < hash) lo = mid + 1;
            else hi = mid;
        }
        uint32_t end = lo;
        while (end < pack->context_count && pack->contexts[end].hash == hash) end++;
        s->ctx_first = lo;
        s->ctx_count = end - lo;
    }
    if (pack->marker_count) {
        uint32_t lo = 0, hi = pack->marker_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (pack->markers[mid].hash < hash) lo = mid + 1;
            else hi = mid;
        }
        if (lo < pack->marker_count && pack->markers[lo].hash == hash) {
            s->marker_bits = pack->markers[lo].bits;
            s->marker = true;
        }
    }
    s->simple = same && !s->layered && !s->cell_colored && !s->ctx_count;
}

void cp_slot_changed(int chr, const uint16_t rows[8]) {
    CpSlot *s = &slots[chr];
    uint32_t hash = hash_rows(rows);
    bool was_colored = s->colored;
    if (!s->valid || s->hash != hash) resolve_slot(s, hash);
    uint32_t colors[64];
    bool any = false;
    for (int i = 0; i < 64; i++) {
        unsigned pv = (rows[i >> 3] >> ((i & 7) * 2)) & 3;
        colors[i] = pv && s->base && ((s->base->mask >> i) & 1) ? pack_rgb(s->base->rgb[i]) : 0;
        if (colors[i]) any = true;
    }
    s->colored = any;
    if (any || was_colored) gpu_slot_colors(chr, colors);
}

static const CpTile *layer_tile(CpSlot *s, int world) {
    if (s->layer_world != world) {
        s->layer_world = (uint8_t)world;
        s->layer_tile = find_keyed(pack->layers, pack->layer_count, ((uint64_t)s->hash << 5) | (world & 31));
    }
    return s->layer_tile;
}

static const CpCell *find_cell(int cell, int palette, uint32_t hash) {
    if (cell < 0 || cell > 0xFFFF) return NULL;
    for (uint32_t i = pack->cell_start[cell]; i < pack->cell_start[cell + 1]; i++) {
        const CpCell *c = &pack->cells[i];
        if (c->palette == palette && c->hash == hash) return c;
    }
    return NULL;
}

static int grid_index(int sx, int sy) {
    int gx = (sx + 4) >> 3, gy = (sy + 4) >> 3;
    if (gx < 0) gx = 0;
    if (gx >= GRID_W) gx = GRID_W - 1;
    if (gy < 0) gy = 0;
    if (gy >= GRID_H) gy = GRID_H - 1;
    return gy * GRID_W + gx;
}

static void sprite_near(int idx, CpBits *near, CpBits *close) {
    if (near_stamp[idx] != frame_no) {
        near_stamp[idx] = frame_no;
        CpBits n = {{0}}, c = {{0}};
        int gx = idx % GRID_W, gy = idx / GRID_W;
        for (int y = gy - CONTEXT_REACH; y <= gy + CONTEXT_REACH; y++) {
            if (y < 0 || y >= GRID_H) continue;
            for (int x = gx - CONTEXT_REACH; x <= gx + CONTEXT_REACH; x++) {
                if (x < 0 || x >= GRID_W) continue;
                int i = y * GRID_W + x;
                bool is_close = abs(x - gx) <= 1 && abs(y - gy) <= 1;
                for (int f = 0; f < 3; f++) {
                    bits_or(&n, &grids[f].sprite[i]);
                    if (is_close) bits_or(&c, &grids[f].sprite[i]);
                }
            }
        }
        near_cache[idx] = n;
        close_cache[idx] = c;
    }
    *near = near_cache[idx];
    *close = close_cache[idx];
}

static void bg_near(int idx, int world, CpBits *near, CpBits *close) {
    CpBits n = {{0}}, c = {{0}};
    int gx = idx % GRID_W, gy = idx / GRID_W;
    for (int y = gy - CONTEXT_REACH; y <= gy + CONTEXT_REACH; y++) {
        if (y < 0 || y >= GRID_H) continue;
        for (int x = gx - CONTEXT_REACH; x <= gx + CONTEXT_REACH; x++) {
            if (x < 0 || x >= GRID_W) continue;
            int i = y * GRID_W + x;
            bool is_close = abs(x - gx) <= 1 && abs(y - gy) <= 1;
            for (int f = 0; f < 3; f++) {
                if (grids[f].bg_layer[i] != world) continue;
                bits_or(&n, &grids[f].bg[i]);
                if (is_close) bits_or(&c, &grids[f].bg[i]);
            }
        }
    }
    bits_and(&n, &pack->layer_bound);
    bits_and(&c, &pack->layer_bound);
    *near = n;
    *close = c;
}

static const CpTile *context_tile(const CpSlot *s, bool sprite, int world, int sx, int sy) {
    CpBits near, close;
    int idx = grid_index(sx, sy);
    if (sprite) sprite_near(idx, &near, &close);
    else if (pack->any_layer_bound) bg_near(idx, world, &near, &close);
    else return NULL;
    for (int pass = 0; pass < 2; pass++) {
        const CpBits *b = pass ? &near : &close;
        if (!bits_any(b)) continue;
        for (uint32_t k = 0; k < s->ctx_count; k++) {
            const CpContext *c = &pack->contexts[s->ctx_first + k];
            if (bits_test(b, c->group)) return &c->tile;
        }
    }
    return NULL;
}

static int variant_slot(int chr, uint32_t hash, const CpCell *cell, const CpTile *tile) {
    uint32_t mix = hash * 2654435761u ^ (uint32_t)(uintptr_t)cell * 2246822519u ^ (uint32_t)(uintptr_t)tile * 3266489917u;
    mix ^= mix >> 15;
    int set = (int)(mix % VARIANT_SETS) * VARIANT_WAYS;
    int victim = -1;
    bool victim_free = false;
    uint32_t oldest = 0xFFFFFFFF;
    for (int w = 0; w < VARIANT_WAYS; w++) {
        CpVariant *v = &variants[set + w];
        if (v->valid && v->hash == hash && v->cell == cell && v->tile == tile) {
            v->used = frame_no;
            if (v->fill) cur_fill = true;
            return CP_BASE_SLOTS + set + w;
        }
        if (victim_free) continue;
        if (!v->valid) {
            victim = set + w;
            victim_free = true;
        } else if (frame_no - v->used >= 3 && v->used < oldest) {
            victim = set + w;
            oldest = v->used;
        }
    }
    if (victim < 0) return chr;
    const uint16_t *rows = chr_rows(chr);
    uint32_t colors[64];
    bool fill = false;
    for (int i = 0; i < 64; i++) {
        unsigned pv = (rows[i >> 3] >> ((i & 7) * 2)) & 3;
        uint32_t col = 0;
        if (cell && ((cell->keep >> i) & 1)) col = 0;
        else if (cell && ((cell->mask >> i) & 1)) col = pack_rgb(cell->rgb[i]);
        else if (pv && tile && ((tile->mask >> i) & 1)) col = pack_rgb(tile->rgb[i]);
        if (!pv && col) fill = true;
        colors[i] = col;
    }
    CpVariant *v = &variants[victim];
    v->hash = hash;
    v->cell = cell;
    v->tile = tile;
    v->used = frame_no;
    v->fill = fill;
    v->valid = 1;
    if (fill) cur_fill = true;
    gpu_slot_variant(CP_BASE_SLOTS + victim, chr, colors);
    return CP_BASE_SLOTS + victim;
}

static int resolve(int chr, int palette, int cell, bool sprite, bool use_layer, bool use_ctx, int sx, int sy) {
    if (!cp_frame_on || !pack) return chr;
    CpSlot *s = &slots[chr & 2047];
    if (s->simple) return chr;
    const CpTile *tile = NULL;
    if (s->ctx_count && use_ctx) tile = context_tile(s, sprite, cur_colors, sx, sy);
    if (!tile) {
        tile = s->pal[palette & 3];
        if (s->layered && use_layer && tile == s->base) {
            const CpTile *l = layer_tile(s, cur_colors);
            if (l) tile = l;
        }
    }
    const CpCell *c = !sprite && s->cell_colored ? find_cell(cell, palette & 3, s->hash) : NULL;
    if (!c && tile == s->base) return chr;
    return variant_slot(chr, s->hash, c, tile);
}

int cp_bg_slot(int chr, int palette, int cell, int sx, int sy) {
    return resolve(chr, palette, cell, false, true, true, sx, sy);
}

int cp_obj_slot(int chr, int palette, int sx, int sy) {
    return resolve(chr, palette, -1, true, false, true, sx, sy);
}

int cp_affine_slot(int chr, int palette, int cell) {
    return resolve(chr, palette, cell, false, false, false, 0, 0);
}

void cp_mark_obj(int chr, int sx, int sy) {
    if (!cp_frame_on || !pack) return;
    const CpSlot *s = &slots[chr & 2047];
    if (!s->marker) return;
    CpBits b = s->marker_bits;
    bits_andnot(&b, &pack->layer_bound);
    if (!bits_any(&b)) return;
    if (sx < -8 || sx >= 384 || sy < -8 || sy >= 224) return;
    bits_or(&grids[grid_cur].sprite[grid_index(sx, sy)], &b);
}

void cp_mark_bg(int chr, int sx, int sy) {
    if (!cp_frame_on || !pack || !pack->any_layer_bound) return;
    const CpSlot *s = &slots[chr & 2047];
    if (!s->marker) return;
    CpBits b = s->marker_bits;
    bits_and(&b, &pack->layer_bound);
    if (!bits_any(&b)) return;
    if (sx < -8 || sx >= 384 || sy < -8 || sy >= 224) return;
    int i = grid_index(sx, sy);
    bits_or(&grids[grid_cur].bg[i], &b);
    grids[grid_cur].bg_layer[i] = (int8_t)cur_colors;
}

static void blend_ramp(float out[3][3], float depth) {
    if (depth < 0) depth = 0;
    if (depth > 1) depth = 1;
    float at = depth * 3;
    int i = at >= 3 ? 2 : (int)at;
    float f = at - i;
    for (int s = 0; s < 3; s++)
        for (int c = 0; c < 3; c++)
            out[s][c] = (layer_ramp_table[i][s][c] + (layer_ramp_table[i + 1][s][c] - layer_ramp_table[i][s][c]) * f) / 255.0f;
}

static void update_layer_ramps(void) {
    if (auto_worlds == ramp_state && figure_worlds == ramp_figures) return;
    ramp_state = auto_worlds;
    ramp_figures = figure_worlds;
    uint32_t scenery = auto_worlds & ~figure_worlds;
    int nearest = 0, farthest = 31;
    if (scenery) {
        while (!((scenery >> nearest) & 1)) nearest++;
        while (!((scenery >> farthest) & 1)) farthest--;
    }
    for (int w = 0; w < 32; w++) blend_ramp(layer_ramps[w], farthest > nearest ? (float)(farthest - w) / (farthest - nearest) : 0.5f);
}

void cp_frame_begin(const void *world_data, bool tocpu) {
    static bool last_on = false;
    const WORLD *worlds = world_data;
    cp_frame_on = cp_display_on() && !tocpu;
    if (cp_frame_on != last_on) {
        last_on = cp_frame_on;
        gpu_reset_color_state();
    }
    if (!cp_frame_on) return;
    frame_no++;
    grid_cur = (grid_cur + 1) % 3;
    memset(grids[grid_cur].sprite, 0, sizeof(grids[grid_cur].sprite));
    memset(grids[grid_cur].bg, 0, sizeof(grids[grid_cur].bg));
    memset(grids[grid_cur].bg_layer, -1, sizeof(grids[grid_cur].bg_layer));
    int queue[32], qhead = 0, qtail = 0;
    for (int w = 31; w >= 0; w--) {
        world_colors[w] = w;
        if (worlds[w].end) break;
        if (worlds[w].on == 0) continue;
        if (worlds[w].bgm == 3 || worlds[w].on == 3) {
            qhead = qtail = 0;
            continue;
        }
        if (worlds[w].on == 2) {
            queue[qtail++] = w;
        } else if (worlds[w].on == 1 && qhead < qtail) {
            world_colors[w] = queue[qhead++];
        }
        int c = world_colors[w];
        auto_worlds |= 1u << c;
        int ww = worlds[w].w + 1, hh = worlds[w].h + 1;
        if (ww > 384) ww = 384;
        if (hh > 224) hh = 224;
        int extent = ww > hh ? ww : hh;
        if (extent <= FIGURE_SIZE) figure_worlds |= 1u << c;
        else figure_worlds &= ~(1u << c);
    }
    update_layer_ramps();
}

void cp_world_begin(int wrld) {
    cur_colors = world_colors[wrld & 31];
    cur_fill = false;
    shading.figure = (figure_worlds >> cur_colors) & 1;
    memcpy(shading.world_ramp, layer_ramps[cur_colors], sizeof(shading.world_ramp));
}

bool cp_world_used_fill(void) {
    return cur_fill;
}

const CpShading *cp_shading(void) {
    static bool ready = false;
    if (!ready) {
        ready = true;
        for (int p = 0; p < 4; p++)
            for (int s = 0; s < 3; s++)
                for (int c = 0; c < 3; c++) shading.sprite_ramps[p][s][c] = sprite_ramp_table[p][s][c] / 255.0f;
        for (int s = 0; s < 4; s++)
            for (int c = 0; c < 3; c++) shading.base[s][c] = base_table[s][c] / 255.0f;
    }
    return &shading;
}

static void brightness_cache(int out[4]) {
    int a = vb_state->tVIPREG.BRTA & 0xFF;
    int b = vb_state->tVIPREG.BRTB & 0xFF;
    int c = vb_state->tVIPREG.BRTC & 0xFF;
    int r = vb_state->tVIPREG.REST & 0xFF;
    int repeat = (*(uint16_t *)(vb_state->V810_DISPLAY_RAM.off + 0x3DDFE) >> 8) & 0xFF;
    int cumulative = a + 1 + b + 1 + c + 1 + r + 1 + 1;
    const int max_time = 128;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int i = 0; i < repeat + 1; i++) {
        int base = i * cumulative;
        if (base >= max_time) break;
        int t1 = base + a;
        if (t1 > max_time) t1 = max_time;
        t1 -= base;
        if (t1 < 0) t1 = 0;
        int t2 = base + a + 1 + b;
        if (t2 > max_time) t2 = max_time;
        t2 -= base + a + 1;
        if (t2 < 0) t2 = 0;
        int t3 = base + a + b + c + 1;
        if (t3 > max_time) t3 = max_time;
        t3 -= base + 1;
        if (t3 < 0) t3 = 0;
        out[1] += t1;
        out[2] += t2;
        out[3] += t3;
    }
    for (int i = 0; i < 4; i++) out[i] = 255 * out[i] / max_time;
}

int cp_brightness_level(void) {
    int cache[4];
    brightness_cache(cache);
    int level = cache[3] >> 2;
    return level > 63 ? 63 : level;
}

bool cp_shade_off(int shade) {
    if (shade <= 0) return true;
    int cache[4];
    brightness_cache(cache);
    return cache[shade & 3] == 0;
}

float cp_fade(void) {
    int level = cp_brightness_level();
    int ref;
    if (pack) {
        ref = pack->reference_level;
    } else {
        if (level > auto_max_level) auto_max_level = level;
        ref = auto_max_level;
    }
    if (ref < 1) ref = 63;
    if (level <= 0) return 0.0f;
    float f = powf(level / 63.0f, 1.0f / 2.2f) / powf(ref / 63.0f, 1.0f / 2.2f);
    return f > 1.0f ? 1.0f : f;
}
