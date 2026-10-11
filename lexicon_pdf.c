/**
 * @file lexicon_pdf.c
 * @brief Poppler-backed PDF processing library — optimised implementation.
 *
 * Thread-safety: poppler object access is serialised with a global mutex.
 * Cached text extraction results are read-only after pdf_text_init(), so
 * pdf_text_utf8_buf(), pdf_text_utf8(), pdf_text_char_count() and
 * pdf_text_char_boxes() are lock-free.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cairo.h>
#include <glib.h>
#include <poppler.h>
#include <pthread.h> /* pthread_mutex_t                     */
#include <stdbool.h> /* bool                                */
#include <stdint.h>  /* uint8_t                             */
#include <stdio.h>   /* fprintf, stderr                     */
#include <stdlib.h>  /* malloc, free                        */
#include <string.h>  /* memcpy, strlen                      */

#include "lexicon_pdf.h"

/* -------------------------------------------------------------------------
 * Global mutex (serialises poppler object access)
 * ---------------------------------------------------------------------- */

static pthread_mutex_t g_pdf_mutex = PTHREAD_MUTEX_INITIALIZER;

#define PDF_LOCK()   pthread_mutex_lock(&g_pdf_mutex)
#define PDF_UNLOCK() pthread_mutex_unlock(&g_pdf_mutex)

/* -------------------------------------------------------------------------
 * Internal handle wrappers
 * ---------------------------------------------------------------------- */

/**
 * Document internals.
 *
 * PopplerDocument is a GObject (refcounted). For memory-backed documents
 * the GBytes references the caller's buffer without copying
 * (g_bytes_new_static), so it must stay alive until the document is
 * destroyed — matching the public API contract that the buffer must remain
 * valid until pdf_document_close().
 */
typedef struct {
    PopplerDocument* doc;
    GBytes* bytes; /* non-NULL only for pdf_document_open_mem(); else NULL */
} doc_inner_t;

/**
 * Cached text-extraction results.
 *
 * Poppler has no persistent text-page object: poppler_page_get_text() and
 * poppler_page_get_text_layout() each allocate fresh results. We snapshot
 * both once in pdf_text_init() so the hot-path accessors are pure memcpy
 * (and lock-free). n_rects == nchars is guaranteed by construction; if the
 * backend ever disagrees we fall back to degenerate boxes.
 */
typedef struct {
    char* utf8;            /* NUL-terminated UTF-8 page text (owned).      */
    pdf_char_box_t* boxes; /* count entries in bottom-left-origin points.  */
    int count;             /* Unicode character count (g_utf8_strlen).     */
} text_inner_t;

/* -------------------------------------------------------------------------
 * Error helpers
 * ---------------------------------------------------------------------- */

static pdf_status_t map_gerror(const GError* err) {
    if (err == NULL) return PDF_ERR_UNKNOWN;
    if (g_error_matches(err, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED)) return PDF_ERR_PASSWORD;
    if (err->domain == POPPLER_ERROR) {
        /* INVALID, BAD_CATALOG, DAMAGED, SIGNING → format problems. */
        if (err->code == POPPLER_ERROR_OPEN_FILE) return PDF_ERR_FILE;
        return PDF_ERR_FORMAT;
    }
    /* G_FILE_ERROR / G_IO_ERROR (e.g. ENOENT from new_from_file). */
    return PDF_ERR_FILE;
}

const char* pdf_strerror(pdf_status_t status) {
    switch (status) {
        case PDF_OK:
            return "success";
        case PDF_ERR_PARAM:
            return "invalid parameter (NULL or out-of-range)";
        case PDF_ERR_NOMEM:
            return "memory allocation failed";
        case PDF_ERR_FILE:
            return "file not found or unreadable";
        case PDF_ERR_FORMAT:
            return "not a PDF or file is corrupted";
        case PDF_ERR_PASSWORD:
            return "document is password-protected";
        case PDF_ERR_PAGE:
            return "invalid page index or page content error";
        case PDF_ERR_RENDER:
            return "bitmap creation or rendering failed";
        case PDF_ERR_TEXT:
            return "text page load failed";
        case PDF_ERR_TRUNC:
            return "output truncated; buffer too small";
        case PDF_ERR_UNKNOWN:
            return "unknown PDF backend error";
        default:
            return "unrecognised error code";
    }
}

/* -------------------------------------------------------------------------
 * Library lifecycle (poppler needs no init; kept as no-ops for API compat)
 * ---------------------------------------------------------------------- */

void pdf_library_init(void) { /* poppler/glib require no global init since glib 2.36. */ }

void pdf_library_destroy(void) { /* No-op. */ }

/* -------------------------------------------------------------------------
 * Bitmap
 * ---------------------------------------------------------------------- */

void pdf_bitmap_free(pdf_bitmap_t* bmp) {
    if (bmp == NULL || bmp->pixels == NULL) return;
    free(bmp->pixels);
    bmp->pixels = NULL;
    bmp->width = bmp->height = bmp->stride = 0;
}

/* -------------------------------------------------------------------------
 * Document
 * ---------------------------------------------------------------------- */

static pdf_status_t pdf_document_init_file(pdf_document_t* doc, const char* filename,
                                           const char* password) {
    char* uri = g_filename_to_uri(filename, NULL, NULL);
    if (uri == NULL) {
        fprintf(stderr, "pdf_document_init: cannot convert filename '%s' to URI\n", filename);
        return PDF_ERR_FILE;
    }

    PDF_LOCK();
    GError* err = NULL;
    PopplerDocument* raw = poppler_document_new_from_file(uri, password, &err);
    PDF_UNLOCK();
    g_free(uri);

    if (raw == NULL) {
        pdf_status_t st = map_gerror(err);
        fprintf(stderr, "pdf_document_init: failed to open '%s': %s\n", filename, pdf_strerror(st));
        if (err) {
            fprintf(stderr, "pdf_document_init: backend said: %s\n", err->message);
            g_error_free(err);
        }
        return st;
    }

    doc_inner_t* inner = malloc(sizeof(*inner));
    if (inner == NULL) {
        g_object_unref(raw);
        return PDF_ERR_NOMEM;
    }
    inner->doc = raw;
    inner->bytes = NULL;
    doc->_handle = inner;
    return PDF_OK;
}

static pdf_status_t pdf_document_init_mem(pdf_document_t* doc, const void* data, size_t size,
                                          const char* password) {
    if (doc == NULL || data == NULL || size == 0) {
        fprintf(stderr, "pdf_document_init_mem: invalid parameter\n");
        return PDF_ERR_PARAM;
    }

    /* Static bytes: no copy, no free — caller buffer must outlive the doc. */
    GBytes* bytes = g_bytes_new_static(data, size);
    if (bytes == NULL) return PDF_ERR_NOMEM;

    PDF_LOCK();
    GError* err = NULL;
    PopplerDocument* raw = poppler_document_new_from_bytes(bytes, password, &err);
    PDF_UNLOCK();

    if (raw == NULL) {
        pdf_status_t st = map_gerror(err);
        fprintf(stderr, "pdf_document_init_mem: %s\n", pdf_strerror(st));
        if (err) {
            fprintf(stderr, "pdf_document_init_mem: backend said: %s\n", err->message);
            g_error_free(err);
        }
        g_bytes_unref(bytes);
        return st;
    }

    doc_inner_t* inner = malloc(sizeof(*inner));
    if (inner == NULL) {
        g_object_unref(raw);
        g_bytes_unref(bytes);
        return PDF_ERR_NOMEM;
    }
    inner->doc = raw;
    inner->bytes = bytes; /* keep alive until destroy */
    doc->_handle = inner;
    return PDF_OK;
}

void pdf_document_destroy(pdf_document_t* doc) {
    if (doc == NULL || doc->_handle == NULL) return;
    doc_inner_t* inner = (doc_inner_t*)doc->_handle;
    /* Unref under lock to serialise against concurrent opens. */
    PDF_LOCK();
    g_object_unref(inner->doc);
    PDF_UNLOCK();
    if (inner->bytes) g_bytes_unref(inner->bytes);
    free(inner);
    doc->_handle = NULL;
}

pdf_status_t pdf_document_open(pdf_document_t* doc, const char* filename, const char* password) {
    if (filename == NULL || doc == NULL) {
        fprintf(stderr, "pdf_document_open: NULL parameter\n");
        return PDF_ERR_PARAM;
    }
    return pdf_document_init_file(doc, filename, password);
}

pdf_status_t pdf_document_open_mem(pdf_document_t* doc, const void* data, size_t size,
                                   const char* password) {
    if (data == NULL || size == 0 || doc == NULL) {
        fprintf(stderr, "pdf_document_open_mem: invalid parameter\n");
        return PDF_ERR_PARAM;
    }
    return pdf_document_init_mem(doc, data, size, password);
}

void pdf_document_close(pdf_document_t* doc) {
    if (doc == NULL) return;
    pdf_document_destroy(doc);
}

int pdf_document_page_count(const pdf_document_t* doc) {
    if (doc == NULL || doc->_handle == NULL) return -1;
    const doc_inner_t* inner = (const doc_inner_t*)doc->_handle;
    PDF_LOCK();
    int count = poppler_document_get_n_pages(inner->doc);
    PDF_UNLOCK();
    return count;
}

/* -------------------------------------------------------------------------
 * Metadata (poppler returns UTF-8 directly — no iconv needed)
 * ---------------------------------------------------------------------- */

static pdf_status_t copy_meta_string(const char* src, char* buf, size_t bufsz) {
    if (src == NULL) {
        buf[0] = '\0';
        return PDF_OK;
    }
    size_t len = strlen(src);
    if (len + 1 <= bufsz) {
        memcpy(buf, src, len + 1);
        return PDF_OK;
    }
    /* Truncate at a UTF-8 character boundary. */
    size_t n = bufsz - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    memcpy(buf, src, n);
    buf[n] = '\0';
    return PDF_ERR_TRUNC;
}

pdf_status_t pdf_document_metadata(const pdf_document_t* doc, pdf_meta_field_t field, char* buf,
                                   size_t bufsz) {
    if (doc == NULL || doc->_handle == NULL || buf == NULL || bufsz == 0) return PDF_ERR_PARAM;

    const doc_inner_t* inner = (const doc_inner_t*)doc->_handle;
    gchar* val = NULL;

    PDF_LOCK();
    switch (field) {
        case PDF_META_TITLE:
            val = poppler_document_get_title(inner->doc);
            break;
        case PDF_META_AUTHOR:
            val = poppler_document_get_author(inner->doc);
            break;
        case PDF_META_SUBJECT:
            val = poppler_document_get_subject(inner->doc);
            break;
        case PDF_META_KEYWORDS:
            val = poppler_document_get_keywords(inner->doc);
            break;
        case PDF_META_CREATOR:
            val = poppler_document_get_creator(inner->doc);
            break;
        case PDF_META_PRODUCER:
            val = poppler_document_get_producer(inner->doc);
            break;
        default:
            val = poppler_document_get_title(inner->doc);
            break;
    }
    PDF_UNLOCK();

    pdf_status_t ret = copy_meta_string(val, buf, bufsz);
    g_free(val);
    return ret;
}

/* -------------------------------------------------------------------------
 * Page
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_page_open(pdf_document_t* doc, int page_index, pdf_page_t* page) {
    if (doc == NULL || doc->_handle == NULL || page == NULL || page_index < 0) return PDF_ERR_PARAM;

    doc_inner_t* inner = (doc_inner_t*)doc->_handle;
    PDF_LOCK();
    int n = poppler_document_get_n_pages(inner->doc);
    if (page_index >= n) {
        PDF_UNLOCK();
        fprintf(stderr, "pdf_page_open: page %d out of range (n=%d)\n", page_index, n);
        return PDF_ERR_PAGE;
    }
    PopplerPage* raw = poppler_document_get_page(inner->doc, page_index);
    PDF_UNLOCK();

    if (raw == NULL) {
        fprintf(stderr, "pdf_page_open: failed to load page %d\n", page_index);
        return PDF_ERR_PAGE;
    }
    page->_handle = raw;
    return PDF_OK;
}

void pdf_page_close(pdf_page_t* page) {
    if (page == NULL || page->_handle == NULL) return;
    PDF_LOCK();
    g_object_unref((PopplerPage*)page->_handle);
    PDF_UNLOCK();
    page->_handle = NULL;
}

static double page_width_locked(PopplerPage* p) {
    double w = 0.0, h = 0.0;
    poppler_page_get_size(p, &w, &h);
    return w;
}

static double page_height_locked(PopplerPage* p) {
    double w = 0.0, h = 0.0;
    poppler_page_get_size(p, &w, &h);
    return h;
}

float pdf_page_width(const pdf_page_t* page) {
    if (page == NULL || page->_handle == NULL) return 0.0f;
    PDF_LOCK();
    double v = page_width_locked((PopplerPage*)page->_handle);
    PDF_UNLOCK();
    return (float)v;
}

float pdf_page_height(const pdf_page_t* page) {
    if (page == NULL || page->_handle == NULL) return 0.0f;
    PDF_LOCK();
    double v = page_height_locked((PopplerPage*)page->_handle);
    PDF_UNLOCK();
    return (float)v;
}

/* -------------------------------------------------------------------------
 * Rendering (poppler → cairo ARGB32 → RGBA)
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_page_render(const pdf_page_t* page, float scale, pdf_bitmap_t* bmp) {
    if (page == NULL || page->_handle == NULL || bmp == NULL || scale <= 0.0f) return PDF_ERR_PARAM;

    PDF_LOCK();
    double pw = 0.0, ph = 0.0;
    poppler_page_get_size((PopplerPage*)page->_handle, &pw, &ph);
    int w = (int)(pw * (double)scale);
    int h = (int)(ph * (double)scale);

    if (w <= 0 || h <= 0) {
        PDF_UNLOCK();
        fprintf(stderr, "pdf_page_render: degenerate page dimensions %dx%d\n", w, h);
        return PDF_ERR_RENDER;
    }

    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        PDF_UNLOCK();
        cairo_surface_destroy(surf);
        return PDF_ERR_RENDER;
    }

    cairo_t* cr = cairo_create(surf);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        PDF_UNLOCK();
        return PDF_ERR_RENDER;
    }

    /* White background . */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_scale(cr, (double)scale, (double)scale);
    poppler_page_render((PopplerPage*)page->_handle, cr);

    cairo_status_t cs = cairo_status(cr);
    cairo_destroy(cr);
    if (cs != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        PDF_UNLOCK();
        return PDF_ERR_RENDER;
    }

    cairo_surface_flush(surf);
    int stride = cairo_image_surface_get_stride(surf);
    unsigned char* src = cairo_image_surface_get_data(surf);
    if (src == NULL) {
        cairo_surface_destroy(surf);
        PDF_UNLOCK();
        return PDF_ERR_RENDER;
    }

    size_t total = (size_t)stride * (size_t)h;
    uint8_t* rgba = malloc(total);
    if (rgba == NULL) {
        cairo_surface_destroy(surf);
        PDF_UNLOCK();
        return PDF_ERR_NOMEM;
    }

    /*
     * cairo ARGB32 on little-endian is stored BGRA (premultiplied).
     * Unpremultiply to straight RGBA. Pages are painted opaque white
     * first, so alpha is 255 almost everywhere; the generic path below
     * handles any translucent annotation pixels correctly.
     */
    for (int row = 0; row < h; row++) {
        const unsigned char* s = src + (size_t)row * (size_t)stride;
        uint8_t* d = rgba + (size_t)row * (size_t)stride;
        for (int col = 0; col < w; col++, s += 4, d += 4) {
            unsigned b = s[0], g = s[1], r = s[2], a = s[3];
            if (a == 255) {
                d[0] = (uint8_t)r;
                d[1] = (uint8_t)g;
                d[2] = (uint8_t)b;
                d[3] = 255;
            } else if (a == 0) {
                d[0] = d[1] = d[2] = 255;
                d[3] = 255;
            } else {
                /* Unpremultiply with rounding; composite over white. */
                unsigned rr = (r * 255 + a / 2) / a;
                unsigned gg = (g * 255 + a / 2) / a;
                unsigned bb = (b * 255 + a / 2) / a;
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb > 255) bb = 255;
                d[0] = (uint8_t)rr;
                d[1] = (uint8_t)gg;
                d[2] = (uint8_t)bb;
                d[3] = 255;
            }
        }
    }

    cairo_surface_destroy(surf);
    PDF_UNLOCK();

    *bmp = (pdf_bitmap_t){.pixels = rgba, .width = w, .height = h, .stride = stride};
    return PDF_OK;
}

/* -------------------------------------------------------------------------
 * Text extraction (snapshot at init; lock-free afterwards)
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_text_init(pdf_page_t* page, pdf_text_t* text) {
    if (page == NULL || page->_handle == NULL || text == NULL) return PDF_ERR_PARAM;

    PDF_LOCK();
    PopplerPage* p = (PopplerPage*)page->_handle;
    double w = 0.0, page_h = 0.0;
    poppler_page_get_size(p, &w, &page_h);
    char* raw_text = poppler_page_get_text(p);
    PopplerRectangle* rects = NULL;
    guint n_rects = 0;
    /* Layout failure is non-fatal — we fall back to degenerate boxes. */
    (void)poppler_page_get_text_layout(p, &rects, &n_rects);
    PDF_UNLOCK();

    if (raw_text == NULL) {
        /* Treat as empty page rather than a hard failure. */
        raw_text = g_strdup("");
        if (raw_text == NULL) {
            g_free(rects);
            return PDF_ERR_NOMEM;
        }
    }

    long nchars = g_utf8_strlen(raw_text, -1);
    if (nchars < 0) nchars = 0;

    text_inner_t* inner = malloc(sizeof(*inner));
    if (inner == NULL) {
        g_free(raw_text);
        g_free(rects);
        return PDF_ERR_NOMEM;
    }
    inner->utf8 = raw_text; /* adopt */
    inner->count = (int)nchars;
    inner->boxes = NULL;

    if (nchars > 0) {
        inner->boxes = malloc((size_t)nchars * sizeof(*inner->boxes));
        if (inner->boxes == NULL) {
            g_free(raw_text);
            g_free(rects);
            free(inner);
            return PDF_ERR_NOMEM;
        }
        bool layout_ok = (rects != NULL && n_rects == (guint)nchars);
        /* Walk the UTF-8 sequentially (O(n)); g_utf8_offset_to_pointer()
         * per index would be O(n^2) and dominate on dense pages. */
        const char* ptr = raw_text;
        for (long i = 0; i < nchars; i++) {
            gunichar ch = g_utf8_get_char(ptr);
            if (!layout_ok) {
                inner->boxes[i] = (pdf_char_box_t){.left = 0,
                                                   .right = 0,
                                                   .bottom = 0,
                                                   .top = 0,
                                                   .codepoint = (uint32_t)ch};
            } else {
                /* Poppler rects use a top-left origin; flip Y to the
                 * public bottom-left-origin contract. */
                float left = (float)rects[i].x1;
                float right = (float)rects[i].x2;
                float bottom = (float)(page_h - rects[i].y2);
                float top = (float)(page_h - rects[i].y1);
                inner->boxes[i] = (pdf_char_box_t){.left = left,
                                                   .right = right,
                                                   .bottom = bottom,
                                                   .top = top,
                                                   .codepoint = (uint32_t)ch};
            }
            ptr = g_utf8_next_char(ptr);
        }
    }
    g_free(rects);

    text->_handle = inner;
    text->count = inner->count;
    return PDF_OK;
}

void pdf_text_destroy(pdf_text_t* text) {
    if (text == NULL || text->_handle == NULL) return;
    text_inner_t* inner = (text_inner_t*)text->_handle;
    g_free(inner->utf8);
    free(inner->boxes);
    free(inner);
    text->_handle = NULL;
    text->count = 0;
}

pdf_status_t pdf_text_open(pdf_page_t* page, pdf_text_t** out) {
    if (page == NULL || out == NULL) return PDF_ERR_PARAM;

    pdf_text_t* text = malloc(sizeof(*text));
    if (text == NULL) return PDF_ERR_NOMEM;
    text->_handle = NULL;
    text->count = 0;

    pdf_status_t st = pdf_text_init(page, text);
    if (st != PDF_OK) {
        free(text);
        return st;
    }

    *out = text;
    return PDF_OK;
}

void pdf_text_close(pdf_text_t* text) {
    if (text == NULL) return;
    pdf_text_destroy(text);
    free(text);
}

int pdf_text_char_count(const pdf_text_t* text) { return (text != NULL) ? text->count : 0; }

/** UTF-8 continuation-byte test (0x80–0xBF). */
static bool is_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

pdf_status_t pdf_text_utf8_buf(const pdf_text_t* text, char* buf, size_t cap, size_t* written) {
    if (text == NULL || text->_handle == NULL || buf == NULL || cap == 0) return PDF_ERR_PARAM;

    const text_inner_t* inner = (const text_inner_t*)text->_handle;
    size_t len = strlen(inner->utf8);
    if (len + 1 <= cap) {
        memcpy(buf, inner->utf8, len + 1);
        if (written) *written = len;
        return PDF_OK;
    }
    /* Truncate without splitting a UTF-8 sequence. */
    size_t n = cap - 1;
    while (n > 0 && is_utf8_cont((unsigned char)inner->utf8[n])) n--;
    /* If we landed on a lead byte cut off from its continuation, step back. */
    if (n > 0) {
        unsigned char c = (unsigned char)inner->utf8[n - 1];
        size_t need = 0;
        if ((c & 0xE0) == 0xC0)
            need = 1;
        else if ((c & 0xF0) == 0xE0)
            need = 2;
        else if ((c & 0xF8) == 0xF0)
            need = 3;
        if (need > 0 && (cap - 1 - (n - 1) - 1) < need) n--;
    }
    memcpy(buf, inner->utf8, n);
    buf[n] = '\0';
    if (written) *written = n;
    return PDF_ERR_TRUNC;
}

pdf_status_t pdf_text_utf8(const pdf_text_t* text, char** out) {
    if (text == NULL || text->_handle == NULL || out == NULL) return PDF_ERR_PARAM;

    const text_inner_t* inner = (const text_inner_t*)text->_handle;
    size_t len = strlen(inner->utf8);
    char* dup = malloc(len + 1);
    if (dup == NULL) return PDF_ERR_NOMEM;
    memcpy(dup, inner->utf8, len + 1);
    *out = dup;
    return PDF_OK;
}

pdf_status_t pdf_text_char_boxes(const pdf_text_t* text, int start_index, int count,
                                 pdf_char_box_t* boxes) {
    if (text == NULL || text->_handle == NULL || boxes == NULL) return PDF_ERR_PARAM;
    if (start_index < 0 || count <= 0) return PDF_ERR_PARAM;
    const text_inner_t* inner = (const text_inner_t*)text->_handle;
    if (start_index + count > inner->count) return PDF_ERR_PARAM;

    memcpy(boxes, inner->boxes + start_index, (size_t)count * sizeof(*boxes));
    return PDF_OK;
}

/* -------------------------------------------------------------------------
 * Image encoding helpers (unchanged — backend-independent via stb)
 * ---------------------------------------------------------------------- */

/** Converts the RGBA bitmap to a packed RGB buffer. Caller must free(). */
static uint8_t* rgba_to_rgb(const pdf_bitmap_t* bmp) {
    size_t npixels = (size_t)bmp->width * (size_t)bmp->height;
    uint8_t* rgb = malloc(npixels * 3);
    if (rgb == NULL) return NULL;

    const uint8_t* src = bmp->pixels;
    uint8_t* dst = rgb;

    for (int row = 0; row < bmp->height; row++) {
        const uint8_t* s = src + (size_t)row * (size_t)bmp->stride;
        for (int col = 0; col < bmp->width; col++, s += 4, dst += 3) {
            dst[0] = s[0];
            dst[1] = s[1];
            dst[2] = s[2];
        }
    }
    return rgb;
}

pdf_status_t pdf_bitmap_write(const pdf_bitmap_t* bmp, const char* path, pdf_image_format_t format,
                              int quality) {
    if (bmp == NULL || bmp->pixels == NULL || path == NULL) return PDF_ERR_PARAM;

    uint8_t* rgb = rgba_to_rgb(bmp);
    if (rgb == NULL) return PDF_ERR_NOMEM;

    int stride_rgb = bmp->width * 3;
    int rc;

    switch (format) {
        case PDF_IMAGE_PNG:
            rc = stbi_write_png(path, bmp->width, bmp->height, 3, rgb, stride_rgb);
            break;
        case PDF_IMAGE_JPEG:
            quality = (quality <= 0 || quality > 100) ? 88 : quality;
            rc = stbi_write_jpg(path, bmp->width, bmp->height, 3, rgb, quality);
            break;
        default:
            free(rgb);
            return PDF_ERR_PARAM;
    }

    free(rgb);
    if (rc == 0) {
        fprintf(stderr, "pdf_bitmap_write: failed to write '%s'\n", path);
        return PDF_ERR_RENDER;
    }
    return PDF_OK;
}

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
    pdf_status_t status;
} encode_ctx_t;

static void encode_write_cb(void* context, void* data, int size) {
    encode_ctx_t* ctx = (encode_ctx_t*)context;
    if (ctx->status != PDF_OK) return;

    size_t needed = ctx->size + (size_t)size;
    if (needed > ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? 4096 : ctx->capacity * 2;
        while (new_cap < needed) new_cap *= 2;
        uint8_t* next = realloc(ctx->data, new_cap);
        if (next == NULL) {
            ctx->status = PDF_ERR_NOMEM;
            return;
        }
        ctx->data = next;
        ctx->capacity = new_cap;
    }
    memcpy(ctx->data + ctx->size, data, (size_t)size);
    ctx->size = needed;
}

pdf_status_t pdf_bitmap_encode(const pdf_bitmap_t* bmp, pdf_image_format_t format, int quality,
                               uint8_t** out_data, size_t* out_size) {
    if (bmp == NULL || bmp->pixels == NULL || out_data == NULL || out_size == NULL)
        return PDF_ERR_PARAM;

    uint8_t* rgb = rgba_to_rgb(bmp);
    if (rgb == NULL) return PDF_ERR_NOMEM;

    int stride_rgb = bmp->width * 3;
    encode_ctx_t ctx = {.data = NULL, .size = 0, .capacity = 0, .status = PDF_OK};
    int rc = 0;

    switch (format) {
        case PDF_IMAGE_PNG:
            rc = stbi_write_png_to_func(encode_write_cb, &ctx, bmp->width, bmp->height, 3, rgb,
                                        stride_rgb);
            break;
        case PDF_IMAGE_JPEG:
            quality = (quality <= 0 || quality > 100) ? 88 : quality;
            rc = stbi_write_jpg_to_func(encode_write_cb, &ctx, bmp->width, bmp->height, 3, rgb,
                                        quality);
            break;
        default:
            free(rgb);
            return PDF_ERR_PARAM;
    }

    free(rgb);

    if (rc == 0 || ctx.status != PDF_OK) {
        free(ctx.data);
        return ctx.status != PDF_OK ? ctx.status : PDF_ERR_RENDER;
    }

    if (ctx.size < ctx.capacity && ctx.size > 0) {
        uint8_t* exact = realloc(ctx.data, ctx.size);
        if (exact) ctx.data = exact;
    }

    *out_data = ctx.data;
    *out_size = ctx.size;
    return PDF_OK;
}
