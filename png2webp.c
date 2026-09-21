/* png2webp.c - PNG -> WebP converter with ComfyUI metadata (EXIF)
 *
 * - Drag & drop a .png file/folder onto the .exe icon: silent conversion.
 * - Double-click (no args): console with help; drag & drop files/folders
 *   onto the console window. Enter = continue, Esc = exit.
 * - Output: <source>\webp\YYYY_MM_DD\*.webp
 * - tEXt chunks (prompt, workflow, extra_pnginfo) -> WebP EXIF.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <conio.h>

#include "stb_image.h"
#include "webp/encode.h"
#include "webp/types.h"

#define MAXW 32768

/* ---------------- basic helpers ---------------- */

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

static char *w2u(const wchar_t *w)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = xmalloc((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL);
    return s;
}

static void set32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* join a + "\\" + b into dst (bounded) */
static void wjoin(wchar_t *dst, size_t cap, const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    if (la + 2 + lb > cap) la = cap - 2 - lb;
    if (la > cap - 2) la = cap - 2;
    memcpy(dst, a, la * sizeof(wchar_t));
    dst[la] = L'\\';
    memcpy(dst + la + 1, b, lb * sizeof(wchar_t));
    dst[la + 1 + lb] = 0;
}

static void mkdirp_w(const wchar_t *path)
{
    wchar_t *tmp = xmalloc((wcslen(path) + 1) * sizeof(wchar_t));
    wcscpy(tmp, path);
    for (wchar_t *p = tmp + 1; *p; ++p) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, NULL);
    free(tmp);
}

static int is_png_name(const wchar_t *name)
{
    size_t L = wcslen(name);
    return L >= 4 && _wcsicmp(name + L - 4, L".png") == 0;
}

static void file_date(const wchar_t *path, wchar_t *out, size_t cap)
{
    SYSTEMTIME st, utc, local;
    GetLocalTime(&st);
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        if (GetFileTime(h, &ft, NULL, NULL)) {
            FileTimeToSystemTime(&ft, &utc);
            if (SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) st = local;
        }
        CloseHandle(h);
    }
    swprintf(out, cap, L"%04d_%02d_%02d", st.wYear, st.wMonth, st.wDay);
}

/* ---------------- PNG tEXt metadata ---------------- */

typedef struct {
    const uint8_t *prompt;   size_t prompt_len;
    const uint8_t *workflow; size_t workflow_len;
    int extra_n;
    struct {
        const uint8_t *key; size_t key_len;
        const uint8_t *val; size_t val_len;
    } *extra;
} PngMeta;

static void skip_ws(const uint8_t *p, size_t n, size_t *i)
{
    while (*i < n && (p[*i] == ' ' || p[*i] == '\t' ||
                      p[*i] == '\n' || p[*i] == '\r')) (*i)++;
}

/* skip a JSON value starting at *i; returns 1 on success */
static int skip_json_value(const uint8_t *p, size_t n, size_t *i)
{
    skip_ws(p, n, i);
    if (*i >= n) return 0;
    uint8_t c = p[*i];
    if (c == '{' || c == '[') {
        int depth = 0, in_str = 0;
        for (; *i < n; (*i)++) {
            uint8_t ch = p[*i];
            if (in_str) {
                if (ch == '\\' && *i + 1 < n) (*i)++;
                else if (ch == '"') in_str = 0;
            } else {
                if (ch == '"') in_str = 1;
                else if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']') {
                    depth--;
                    if (depth == 0) { (*i)++; return 1; }
                }
            }
        }
        return 0;
    }
    if (c == '"') {
        (*i)++;
        for (; *i < n; (*i)++) {
            if (p[*i] == '\\') { if (*i + 1 < n) (*i)++; }
            else if (p[*i] == '"') { (*i)++; return 1; }
        }
        return 0;
    }
    for (; *i < n; (*i)++) {
        uint8_t ch = p[*i];
        if (ch == ',' || ch == ']' || ch == '}' || ch == ' ' ||
            ch == '\t' || ch == '\n' || ch == '\r') break;
    }
    return 1;
}

static void json_top_level(const uint8_t *p, size_t n, PngMeta *m)
{
    size_t i = 0;
    skip_ws(p, n, &i);
    if (i >= n || p[i] != '{') return;
    i++;
    for (;;) {
        skip_ws(p, n, &i);
        if (i >= n || p[i] == '}') return;
        if (p[i] != '"') return;
        size_t kstart = ++i;
        for (; i < n; i++) {
            if (p[i] == '\\' && i + 1 < n) i++;
            else if (p[i] == '"') break;
        }
        if (i >= n) return;
        size_t klen = i - kstart;
        i++;
        skip_ws(p, n, &i);
        if (i >= n || p[i] != ':') return;
        i++;
        skip_ws(p, n, &i);
        size_t vstart = i;
        if (!skip_json_value(p, n, &i)) return;
        m->extra = realloc(m->extra, (size_t)(m->extra_n + 1) * sizeof(m->extra[0]));
        m->extra[m->extra_n].key = p + kstart;
        m->extra[m->extra_n].key_len = klen;
        m->extra[m->extra_n].val = p + vstart;
        m->extra[m->extra_n].val_len = i - vstart;
        m->extra_n++;
        skip_ws(p, n, &i);
        if (i >= n) return;
        if (p[i] == ',') { i++; continue; }
        return;
    }
}

static void parse_png_meta(const uint8_t *data, size_t size, PngMeta *m)
{
    memset(m, 0, sizeof(*m));
    if (size < 8) return;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(data, sig, 8) != 0) return;
    size_t off = 8;
    while (off + 12 <= size) {
        uint32_t len = rd32be(data + off);
        const uint8_t *type = data + off + 4;
        if (memcmp(type, "IEND", 4) == 0) break;
        if (len > 0 && off + 8 + (size_t)len + 4 <= size) {
            const uint8_t *payload = data + off + 8;
            if (memcmp(type, "tEXt", 4) == 0) {
                const uint8_t *nul = memchr(payload, 0, len);
                if (nul) {
                    size_t klen = (size_t)(nul - payload);
                    const uint8_t *val = nul + 1;
                    size_t vlen = len - klen - 1;
                    if (klen == 6 && memcmp(payload, "prompt", 6) == 0) {
                        m->prompt = val; m->prompt_len = vlen;
                    } else if (klen == 8 && memcmp(payload, "workflow", 8) == 0) {
                        m->workflow = val; m->workflow_len = vlen;
                    } else if (klen == 13 && memcmp(payload, "extra_pnginfo", 13) == 0) {
                        json_top_level(val, vlen, m);
                    }
                }
            }
        }
        off += 8 + len + 4;
    }
}

/* ---------------- EXIF (TIFF IFD0, ASCII values) ---------------- */

typedef struct {
    uint16_t tag;
    uint8_t *data; /* malloc'd, includes no terminator */
    uint32_t len;
} ExifEntry;

static int cmp_tag(const void *a, const void *b)
{
    uint16_t ta = ((const ExifEntry *)a)->tag;
    uint16_t tb = ((const ExifEntry *)b)->tag;
    return (int)ta - (int)tb;
}

static void add_entry(ExifEntry **entries, int *n, int *cap,
                      uint16_t tag, const uint8_t *src, uint32_t len)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *entries = realloc(*entries, (size_t)(*cap) * sizeof(ExifEntry));
    }
    (*entries)[*n].tag = tag;
    (*entries)[*n].data = xmalloc(len);
    memcpy((*entries)[*n].data, src, len);
    (*entries)[*n].len = len;
    (*n)++;
}

static ExifEntry *make_entries(const PngMeta *m, int *n_out)
{
    ExifEntry *e = NULL;
    int n = 0, cap = 0;
    if (m->prompt) {
        uint8_t *v = xmalloc(7 + m->prompt_len);
        memcpy(v, "prompt:", 7);
        memcpy(v + 7, m->prompt, m->prompt_len);
        add_entry(&e, &n, &cap, 0x0110, v, (uint32_t)(7 + m->prompt_len));
        free(v);
    }
    if (m->workflow) {
        uint8_t *v = xmalloc(9 + m->workflow_len);
        memcpy(v, "workflow:", 9);
        memcpy(v + 9, m->workflow, m->workflow_len);
        add_entry(&e, &n, &cap, 0x010F, v, (uint32_t)(9 + m->workflow_len));
        free(v);
    }
    for (int i = 0; i < m->extra_n; i++) {
        const uint8_t *k = m->extra[i].key;
        size_t klen = m->extra[i].key_len;
        const uint8_t *v = m->extra[i].val;
        size_t vlen = m->extra[i].val_len;
        uint8_t *buf = xmalloc(klen + 1 + vlen);
        memcpy(buf, k, klen);
        buf[klen] = ':';
        memcpy(buf + klen + 1, v, vlen);
        add_entry(&e, &n, &cap, (uint16_t)(0x010E - i), buf,
                  (uint32_t)(klen + 1 + vlen));
        free(buf);
    }
    *n_out = n;
    return e;
}

static uint8_t *make_tiff(const ExifEntry *entries, int n, uint32_t *out_len)
{
    if (n <= 0) { *out_len = 0; return NULL; }
    ExifEntry *sorted = xmalloc((size_t)n * sizeof(ExifEntry));
    memcpy(sorted, entries, (size_t)n * sizeof(ExifEntry));
    qsort(sorted, (size_t)n, sizeof(ExifEntry), cmp_tag);

    uint16_t count = (uint16_t)n;
    uint32_t value_area = 8 + 2 + 12u * count + 4;
    uint32_t pos = 0;
    uint32_t offsets[512];
    for (int i = 0; i < n; i++) {
        uint32_t vlen = sorted[i].len + 1;
        offsets[i] = vlen > 4 ? value_area + pos : 0;
        if (vlen > 4) pos += vlen;
    }
    uint32_t total = value_area + pos;
    uint8_t *buf = xmalloc(total);
    memset(buf, 0, total);
    buf[0] = 'I'; buf[1] = 'I'; buf[2] = 0x2A; buf[3] = 0;
    set32(buf + 4, 8);
    buf[8] = (uint8_t)(count & 0xFF);
    buf[9] = (uint8_t)(count >> 8);
    uint8_t *p = buf + 10;
    for (int i = 0; i < n; i++) {
        uint32_t vlen = sorted[i].len + 1;
        p[0] = (uint8_t)(sorted[i].tag & 0xFF);
        p[1] = (uint8_t)(sorted[i].tag >> 8);
        p[2] = 2; p[3] = 0; /* TYPE_ASCII */
        set32(p + 4, vlen);
        if (vlen <= 4) {
            memcpy(p + 8, sorted[i].data, sorted[i].len);
        } else {
            set32(p + 8, offsets[i]);
            memcpy(buf + offsets[i], sorted[i].data, sorted[i].len);
        }
        p += 12;
    }
    *out_len = total;
    free(sorted);
    return buf;
}

static void free_entries(ExifEntry *e, int n)
{
    for (int i = 0; i < n; i++) free(e[i].data);
    free(e);
}

/* ---------------- conversion ---------------- */

static int convert_one(const wchar_t *in, const wchar_t *out)
{
    FILE *f = _wfopen(in, L"rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    __int64 fs = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (fs <= 0) { fclose(f); return 0; }
    uint8_t *data = xmalloc((size_t)fs);
    size_t got = fread(data, 1, (size_t)fs, f);
    fclose(f);
    if (got != (size_t)fs) { free(data); return 0; }

    PngMeta m;
    parse_png_meta(data, got, &m);

    int w = 0, h = 0, comp = 0;
    uint8_t *rgba = stbi_load_from_memory(data, (int)got, &w, &h, &comp, 4);
    if (!rgba) { free(data); return 0; }

    WebPPicture pic;
    WebPPictureInit(&pic);
    pic.width = w;
    pic.height = h;
    WebPPictureImportRGBA(&pic, rgba, (int)((size_t)w * 4));
    free(rgba);

    WebPConfig cfg;
    WebPConfigInit(&cfg);
    cfg.quality = 80;
    cfg.method = 4;
    cfg.lossless = 0;

    WebPMemoryWriter wr;
    WebPMemoryWriterInit(&wr);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &wr;

    if (!WebPEncode(&cfg, &pic)) {
        WebPMemoryWriterClear(&wr);
        WebPPictureFree(&pic);
        free(data);
        return 0;
    }

    uint8_t *pixmap = wr.mem;
    uint32_t riff_size = (uint32_t)wr.size;

    int nent = 0;
    ExifEntry *entries = make_entries(&m, &nent);
    uint32_t exif_len = 0;
    uint8_t *exif = nent ? make_tiff(entries, nent, &exif_len) : NULL;

    uint32_t payload = exif ? (uint32_t)(6 + exif_len) : 0;
    uint32_t pad = payload & 1u;
    uint32_t total = riff_size + (exif ? (uint32_t)(8 + payload + pad) : 0);
    uint8_t *outb = xmalloc(total);
    memcpy(outb, pixmap, riff_size);

    WebPMemoryWriterClear(&wr);
    WebPPictureFree(&pic);
    free(data);
    if (m.extra) free(m.extra);

    if (exif) {
        set32(outb + 4, total - 8);
        memcpy(outb + riff_size, "EXIF", 4);
        set32(outb + riff_size + 4, payload);
        memcpy(outb + riff_size + 8, "Exif\0\0", 6);
        memcpy(outb + riff_size + 14, exif, exif_len);
        if (pad) outb[riff_size + 8 + payload] = 0;
    }
    free(exif);
    free_entries(entries, nent);

    FILE *of = _wfopen(out, L"wb");
    if (!of) { free(outb); return 0; }
    int ok = fwrite(outb, 1, total, of) == (size_t)total;
    fclose(of);
    free(outb);
    return ok;
}

/* ---------------- parallel batch ---------------- */

typedef struct {
    wchar_t *in;
    wchar_t *out;
} Task;

static Task *g_tasks = NULL;
static size_t g_ntasks = 0;
static volatile LONG g_next = 0;
static volatile LONG g_done = 0;
static volatile LONG g_failed = 0;
static CRITICAL_SECTION g_out_cs;

static void add_task(const wchar_t *in, const wchar_t *out)
{
    g_tasks = realloc(g_tasks, (g_ntasks + 1) * sizeof(Task));
    g_tasks[g_ntasks].in = xmalloc((wcslen(in) + 1) * sizeof(wchar_t));
    wcscpy(g_tasks[g_ntasks].in, in);
    g_tasks[g_ntasks].out = xmalloc((wcslen(out) + 1) * sizeof(wchar_t));
    wcscpy(g_tasks[g_ntasks].out, out);
    g_ntasks++;
}

static DWORD WINAPI worker_fn(void *arg)
{
    (void)arg;
    for (;;) {
        LONG r = InterlockedIncrement(&g_next);
        LONG idx = r - 1;
        if (idx >= (LONG)g_ntasks) {
            InterlockedDecrement(&g_next);
            break;
        }
        int ok = convert_one(g_tasks[idx].in, g_tasks[idx].out);
        char *u = w2u(ok ? g_tasks[idx].out : g_tasks[idx].in);
        EnterCriticalSection(&g_out_cs);
        if (ok) printf("   [OK]   %s\n", u);
        else    printf("   [FAIL] %s\n", u);
        LeaveCriticalSection(&g_out_cs);
        free(u);
        if (!ok) InterlockedIncrement(&g_failed);
        InterlockedIncrement(&g_done);
    }
    return 0;
}

static void run_batch(void)
{
    if (!g_ntasks) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD nw = si.dwNumberOfProcessors;
    if (nw > 64) nw = 64;
    if (nw < 1) nw = 1;
    printf("Using %lu CPU cores for parallel conversion...\n",
           (unsigned long)nw);
    g_next = 0;
    g_done = 0;
    g_failed = 0;
    HANDLE h[64];
    for (DWORD i = 0; i < nw; i++)
        h[i] = CreateThread(NULL, 0, worker_fn, NULL, 0, NULL);
    while (g_done < (LONG)g_ntasks) {
        EnterCriticalSection(&g_out_cs);
        printf("\r  progress: %ld / %zu     ", (long)g_done, g_ntasks);
        LeaveCriticalSection(&g_out_cs);
        fflush(stdout);
        Sleep(200);
    }
    printf("\n");
    for (DWORD i = 0; i < nw; i++) CloseHandle(h[i]);
    for (size_t i = 0; i < g_ntasks; i++) {
        free(g_tasks[i].in);
        free(g_tasks[i].out);
    }
    free(g_tasks);
    g_tasks = NULL;
    printf("Done! Converted: %zu, Failed: %ld\n",
           g_ntasks - (size_t)g_failed, (long)g_failed);
    g_ntasks = 0;
}

/* ---------------- path handling ---------------- */

static void scan_dir(const wchar_t *dir, wchar_t ***out, size_t *n)
{
    wchar_t pat[MAXW];
    size_t dl = wcslen(dir);
    if (dl + 2 >= MAXW) return;
    memcpy(pat, dir, dl * sizeof(wchar_t));
    pat[dl] = L'\\';
    pat[dl + 1] = L'*';
    pat[dl + 2] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        wchar_t full[MAXW];
        wjoin(full, MAXW, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(full, out, n);
        } else if (is_png_name(fd.cFileName)) {
            *out = realloc(*out, (*n + 1) * sizeof(wchar_t *));
            (*out)[*n] = xmalloc((wcslen(full) + 1) * sizeof(wchar_t));
            wcscpy((*out)[*n], full);
            (*n)++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void make_task(const wchar_t *file, const wchar_t *base_dir)
{
    wchar_t date[16];
    file_date(file, date, 16);
    wchar_t webp_dir[MAXW];
    wjoin(webp_dir, MAXW, base_dir, L"webp");
    wchar_t out_dir[MAXW];
    wjoin(out_dir, MAXW, webp_dir, date);
    mkdirp_w(out_dir);

    const wchar_t *base = wcsrchr(file, L'\\');
    base = base ? base + 1 : file;
    wchar_t stem[MAXW];
    const wchar_t *dot = NULL;
    for (const wchar_t *q = base; *q; q++)
        if (*q == L'.') dot = q;
    size_t slen = wcslen(base);
    if (dot && dot != base && (base + slen - dot) > 1)
        slen = (size_t)(dot - base);
    if (slen >= MAXW) slen = MAXW - 1;
    memcpy(stem, base, slen * sizeof(wchar_t));
    stem[slen] = 0;

    wchar_t with_ext[MAXW];
    size_t n1 = (size_t)swprintf(with_ext, MAXW, L"%ls.webp", stem);
    (void)n1;
    wchar_t out[MAXW];
    wjoin(out, MAXW, out_dir, with_ext);
    add_task(file, out);
}

static void strip_quotes(wchar_t *s)
{
    size_t L = wcslen(s);
    if (L >= 2 && s[0] == L'"' && s[L - 1] == L'"') {
        memmove(s, s + 1, (L - 2) * sizeof(wchar_t));
        s[L - 2] = 0;
    }
}

static void process_paths(wchar_t **paths, int n)
{
    g_tasks = NULL;
    g_ntasks = 0;
    for (int i = 0; i < n; i++) {
        const wchar_t *p = paths[i];
        DWORD attrs = GetFileAttributesW(p);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            char *u = w2u(p);
            printf("  [ERROR] not found: %s\n", u);
            free(u);
            continue;
        }
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            size_t nn = 0;
            wchar_t **files = NULL;
            scan_dir(p, &files, &nn);
            for (size_t j = 0; j < nn; j++) {
                make_task(files[j], p);
                free(files[j]);
            }
            free(files);
        } else {
            if (!is_png_name(p)) {
                char *u = w2u(p);
                printf("  [WARN] not a .png file, skipped: %s\n", u);
                free(u);
                continue;
            }
            const wchar_t *base = wcsrchr(p, L'\\');
            wchar_t base_dir[MAXW];
            if (base) {
                size_t bl = (size_t)(base - p);
                if (bl >= MAXW) bl = MAXW - 1;
                memcpy(base_dir, p, bl * sizeof(wchar_t));
                base_dir[bl] = 0;
            } else {
                wcscpy(base_dir, p);
            }
            make_task(p, base_dir);
        }
    }
    run_batch();
}

static void wait_key(void)
{
    for (;;) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 13 || c == 27) return;
        }
        Sleep(30);
    }
}

/* ---------------- drag & drop target ---------------- */

static const IID IID_T_DROP =
    { 0x000001e3, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID IID_T_UNKNOWN =
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

struct DropT {
    const struct DropTVtbl *lpVtbl;
    LONG ref;
};

struct DropTVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct DropT *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(struct DropT *);
    ULONG (STDMETHODCALLTYPE *Release)(struct DropT *);
    HRESULT (STDMETHODCALLTYPE *DragEnter)(struct DropT *, IDataObject *,
                                           DWORD, POINTL *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *DragOver)(struct DropT *, POINTL *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *DragLeave)(struct DropT *);
    HRESULT (STDMETHODCALLTYPE *Drop)(struct DropT *, IDataObject *,
                                      POINTL *, DWORD *);
};

static BOOL my_unregister_dragdrop(HWND hwnd)
{
    static BOOL (*fn)(HWND) = NULL;
    if (!fn)
        fn = (BOOL (*)(HWND))GetProcAddress(GetModuleHandleW(L"ole32.dll"),
                                            "UnregisterDragDrop");
    return fn ? fn(hwnd) : FALSE;
}

static volatile int g_drop_done_flag = 0;

static void handle_drop_text(const wchar_t *buf, size_t len)
{
    int cap = 8, n = 0;
    wchar_t **paths = xmalloc((size_t)cap * sizeof(wchar_t *));
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i]) i++;
        if (i > start) {
            size_t L = i - start;
            if (n == cap) { cap *= 2; paths = realloc(paths, (size_t)cap * sizeof(wchar_t *)); }
            paths[n] = xmalloc((L + 1) * sizeof(wchar_t));
            memcpy(paths[n], buf + start, L * sizeof(wchar_t));
            paths[n][L] = 0;
            strip_quotes(paths[n]);
            n++;
        }
    }
    printf("Processing %d item(s)...\n", n);
    process_paths(paths, n);
    for (int j = 0; j < n; j++) free(paths[j]);
    free(paths);
    g_drop_done_flag = 1;
    printf("Press Enter to continue, Esc to exit.\n");
}

static HRESULT STDMETHODCALLTYPE dt_Drop(struct DropT *t, IDataObject *pdo,
                                        POINTL *pt, DWORD *eff)
{
    (void)t; (void)pt;
    wchar_t *text = NULL;
    size_t text_len = 0;
    FORMATETC fmt;
    ZeroMemory(&fmt, sizeof(fmt));
    fmt.cfFormat = CF_UNICODETEXT;
    fmt.ptd = NULL;
    fmt.dwAspect = 1; /* DVASPECT_CONTENT */
    fmt.lindex = -1;
    fmt.tymed = 0xFFFFFFFF; /* DTIME_FORMAT_ITEM */
    STGMEDIUM med;
    ZeroMemory(&med, sizeof(med));
    HRESULT hr = pdo->lpVtbl->GetData(pdo, &fmt, &med);
    if (SUCCEEDED(hr) && med.pUnkForRelease) {
        DWORD tym = med.tymed;
        if (tym == 3 || tym == 5) { /* IStream / IStream-list */
            IStream *st = med.pstm;
            STATSTG stg;
            ZeroMemory(&stg, sizeof(stg));
            if (SUCCEEDED(st->lpVtbl->Stat(st, &stg, STATFLAG_DEFAULT))) {
                text_len = (size_t)stg.cbSize.QuadPart;
                if (text_len > 0 && text_len < ((size_t)1 << 26)) {
                    text = xmalloc(text_len + 2 * sizeof(wchar_t));
                    ZeroMemory(text, text_len + 2 * sizeof(wchar_t));
                    ULONG got = 0;
                    st->lpVtbl->Read(st, text, (ULONG)text_len, &got);
                    text_len = got / 2;
                    while (text_len > 0 && text[text_len - 1] == 0) text_len--;
                }
                if (stg.pwcsName) CoTaskMemFree(stg.pwcsName);
            }
        } else if (tym == 1) { /* HGLOBAL */
            text = (wchar_t *)med.hGlobal;
            text_len = wcslen(text);
        }
        med.pUnkForRelease->lpVtbl->Release(med.pUnkForRelease);
        if (tym == 1) GlobalFree(med.hGlobal);
    }
    if (text && text_len > 0) {
        handle_drop_text(text, text_len);
        free(text);
    }
    if (eff) *eff = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragEnter(struct DropT *t, IDataObject *pdo,
                                             DWORD ks, POINTL *pt, DWORD *eff)
{
    (void)t; (void)pdo; (void)ks; (void)pt;
    if (eff) *eff = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragOver(struct DropT *t, POINTL *pt, DWORD *eff)
{
    (void)t; (void)pt;
    if (eff) *eff = DROPEFFECT_COPY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragLeave(struct DropT *t)
{
    (void)t;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE dt_AddRef(struct DropT *t)
{
    return (ULONG)InterlockedIncrement(&t->ref);
}

static ULONG STDMETHODCALLTYPE dt_Release(struct DropT *t)
{
    return (ULONG)InterlockedDecrement(&t->ref);
}

static HRESULT STDMETHODCALLTYPE dt_QI(struct DropT *t, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_T_UNKNOWN) || IsEqualIID(riid, &IID_T_DROP)) {
        *ppv = t;
        dt_AddRef(t);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static const struct DropTVtbl g_dt_vtbl = {
    dt_QI, dt_AddRef, dt_Release, dt_DragEnter, dt_DragOver, dt_DragLeave, dt_Drop
};

static struct DropT g_dt = { &g_dt_vtbl, 1 };

/* ---------------- interactive console mode ---------------- */

static int run_interactive(void)
{
    AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"png2webp");
    printf("=====================================\n");
    printf("  PNG -> WEBP converter (ComfyUI)\n");
    printf("=====================================\n");
    printf(" How to use:\n");
    printf("   1) Drag a .png file or folder onto the .exe icon:\n");
    printf("      conversion runs without a console.\n");
    printf("   2) Or drag files/folders onto this window.\n");
    printf("   3) After each conversion: Enter - continue, Esc - exit.\n");
    printf(" Output: <source folder>\\webp\\YYYY_MM_DD\\*.webp\n");
    printf(" Metadata (prompt / workflow / extra_pnginfo)\n");
    printf(" is saved to the WebP EXIF.\n\n");
    printf("Drop a file now (or press Esc to exit).\n");
    fflush(stdout);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HWND hwnd = GetConsoleWindow();
    int registered = 0;
    if (hwnd && RegisterDragDrop(hwnd, (IDropTarget *)&g_dt) == NOERROR) registered = 1;

    if (registered) {
        for (;;) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (_kbhit()) {
                int c = _getch();
                if (c == 27) break;
                if (c == 13 && g_drop_done_flag) {
                    g_drop_done_flag = 0;
                    printf("\nDrop a file or press Esc to exit.\n");
                    fflush(stdout);
                }
            }
            Sleep(30);
        }
        my_unregister_dragdrop(hwnd);
    } else {
        printf("Drag & drop to this window is not available.\n");
        printf("You can still convert: drag files onto the .exe icon.\n");
        printf("Press Enter to exit.\n");
        wait_key();
    }
    return 0;
}

/* ---------------- entry ---------------- */

int wmain(int argc, wchar_t **argv)
{
    InitializeCriticalSection(&g_out_cs);
    if (argc < 2) {
        return run_interactive();
    }
    int any = 0;
    for (int i = 1; i < argc; i++)
        if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) any = 1;
    if (!any) {
        AllocConsole();
        SetConsoleOutputCP(CP_UTF8);
        printf("No valid file or folder was passed.\n");
        printf("Usage: drag a .png file or folder onto this .exe,\n");
        printf("or run it without arguments for an interactive window.\n");
        printf("Press Enter to exit.\n");
        wait_key();
        return 1;
    }
    process_paths(argv + 1, argc - 1);
    return 0;
}
