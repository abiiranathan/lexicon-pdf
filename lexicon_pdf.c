/**
 * @file lexicon_pdf.c
 * @brief PDFium-backed PDF processing library — optimised implementation.
 *
 * Thread-safety: PDFium internals are serialised with a global mutex.
 * iconv is used outside the lock (safe — each thread has its own cd).
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <errno.h>     /* errno, EILSEQ, E2BIG               */
#include <fpdf_doc.h>  /* FPDF_GetMetaText                   */
#include <fpdf_text.h> /* FPDFText_LoadPage, FPDFText_GetText */
#include <fpdfview.h>  /* FPDF_InitLibrary, FPDF_LoadDocument */
#include <iconv.h>     /* iconv_t, iconv_open, iconv, iconv_close */
#include <limits.h>    /* INT_MAX                             */
#include <pthread.h>   /* pthread_mutex_t                     */
#include <stdint.h>    /* uint8_t, uint16_t                   */
#include <stdio.h>     /* fprintf, stderr                     */
#include <stdlib.h>    /* malloc, free                        */
#include <string.h>    /* memset, memcpy                      */

#include "lexicon_pdf.h"

/* -------------------------------------------------------------------------
 * PDFium global mutex
 * ---------------------------------------------------------------------- */

static pthread_mutex_t g_pdf_mutex = PTHREAD_MUTEX_INITIALIZER;

#define PDF_LOCK()   pthread_mutex_lock(&g_pdf_mutex)
#define PDF_UNLOCK() pthread_mutex_unlock(&g_pdf_mutex)

/* -------------------------------------------------------------------------
 * Thread-local iconv descriptor (UTF-16LE → UTF-8)
 *
 * Opened lazily on first use per thread; never explicitly closed (the OS
 * reclaims it on thread exit).  This eliminates one iconv_open/iconv_close
 * pair per page across all worker threads.
 * ---------------------------------------------------------------------- */

static _Thread_local iconv_t tl_iconv_cd = (iconv_t)-1;

/** Returns the thread-local iconv descriptor, opening it if necessary. */
static iconv_t get_iconv_cd(void) {
    if (tl_iconv_cd == (iconv_t)-1) {
        tl_iconv_cd = iconv_open("UTF-8", "UTF-16LE");
        /* Failure here is catastrophic; we can only propagate NULL upward. */
    }
    return tl_iconv_cd;
}

/* -------------------------------------------------------------------------
 * Threshold for stack vs. heap UTF-16 scratch buffer.
 *
 * A page with <= MAX_UTF16_STACK_CHARS characters uses an 8 KB stack
 * buffer.  Beyond that, a heap allocation is made and freed immediately.
 * Most PDF pages have < 3000 characters.
 * ---------------------------------------------------------------------- */

#define MAX_UTF16_STACK_CHARS 4096

/* -------------------------------------------------------------------------
 * Error helpers
 * ---------------------------------------------------------------------- */

static pdf_status_t map_pdfium_error(unsigned long err) {
    switch (err) {
        case FPDF_ERR_SUCCESS:
            return PDF_OK;
        case FPDF_ERR_FILE:
            return PDF_ERR_FILE;
        case FPDF_ERR_FORMAT:
            return PDF_ERR_FORMAT;
        case FPDF_ERR_PASSWORD:
            return PDF_ERR_PASSWORD;
        case FPDF_ERR_PAGE:
            return PDF_ERR_PAGE;
        default:
            return PDF_ERR_UNKNOWN;
    }
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
            return "unknown PDFium error";
        default:
            return "unrecognised error code";
    }
}

/* -------------------------------------------------------------------------
 * Library lifecycle
 * ---------------------------------------------------------------------- */

void pdf_library_init(void) {
    PDF_LOCK();
    FPDF_InitLibrary();
    PDF_UNLOCK();
}

void pdf_library_destroy(void) {
    PDF_LOCK();
    FPDF_DestroyLibrary();
    PDF_UNLOCK();
}

/* -------------------------------------------------------------------------
 * Bitmap
 * ---------------------------------------------------------------------- */

void pdf_bitmap_free(pdf_bitmap_t* bmp) {
    if (bmp == NULL || bmp->pixels == NULL) return;
    free(bmp->pixels);
    bmp->pixels = NULL;
    bmp->width = bmp->height = bmp->stride = 0;
}

static pdf_status_t pdf_document_init_mem(pdf_document_t* doc, const void* data, size_t size, const char* password) {
    if (doc == NULL || data == NULL || size == 0) {
        fprintf(stderr, "pdf_document_init_mem: invalid parameter\n");
        return PDF_ERR_PARAM;
    }
    if (size > (size_t)INT_MAX) {
        fprintf(stderr, "pdf_document_init_mem: buffer too large (%zu bytes)\n", size);
        return PDF_ERR_PARAM;
    }

    PDF_LOCK();
    FPDF_DOCUMENT raw = FPDF_LoadMemDocument(data, (int)size, password);
    if (raw == NULL) {
        pdf_status_t st = map_pdfium_error(FPDF_GetLastError());
        PDF_UNLOCK();
        fprintf(stderr, "pdf_document_init_mem: %s\n", pdf_strerror(st));
        return st;
    }
    PDF_UNLOCK();

    doc->_handle = raw;
    return PDF_OK;
}

static pdf_status_t pdf_document_init(pdf_document_t* doc, const char* filename, const char* password) {
    PDF_LOCK();
    FPDF_DOCUMENT raw = FPDF_LoadDocument(filename, password);
    if (raw == NULL) {
        pdf_status_t st = map_pdfium_error(FPDF_GetLastError());
        PDF_UNLOCK();
        fprintf(stderr, "pdf_document_init: failed to open '%s': %s\n", filename, pdf_strerror(st));
        return st;
    }
    PDF_UNLOCK();

    doc->_handle = raw;
    return PDF_OK;
}

void pdf_document_destroy(pdf_document_t* doc) {
    if (doc == NULL || doc->_handle == NULL) return;
    PDF_LOCK();
    FPDF_CloseDocument((FPDF_DOCUMENT)doc->_handle);
    PDF_UNLOCK();
    doc->_handle = NULL;
}

/* -------------------------------------------------------------------------
 * Document — heap-allocation (legacy / pointer) API
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_document_open(pdf_document_t* doc, const char* filename, const char* password) {
    if (filename == NULL || doc == NULL) {
        fprintf(stderr, "pdf_document_open: NULL parameter\n");
        return PDF_ERR_PARAM;
    }

    return pdf_document_init(doc, filename, password);
}

pdf_status_t pdf_document_open_mem(pdf_document_t* doc, const void* data, size_t size, const char* password) {
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
    PDF_LOCK();
    int count = FPDF_GetPageCount((FPDF_DOCUMENT)doc->_handle);
    PDF_UNLOCK();
    return count;
}

/* -------------------------------------------------------------------------
 * Metadata
 * ---------------------------------------------------------------------- */

static const char* meta_key(pdf_meta_field_t field) {
    switch (field) {
        case PDF_META_TITLE:
            return "Title";
        case PDF_META_AUTHOR:
            return "Author";
        case PDF_META_SUBJECT:
            return "Subject";
        case PDF_META_KEYWORDS:
            return "Keywords";
        case PDF_META_CREATOR:
            return "Creator";
        case PDF_META_PRODUCER:
            return "Producer";
        default:
            return "Title";
    }
}

pdf_status_t pdf_document_metadata(const pdf_document_t* doc, pdf_meta_field_t field, char* buf, size_t bufsz) {
    if (doc == NULL || buf == NULL || bufsz == 0) return PDF_ERR_PARAM;

    const char* key = meta_key(field);

    PDF_LOCK();
    unsigned long utf16_bytes = FPDF_GetMetaText((FPDF_DOCUMENT)doc->_handle, key, NULL, 0);
    if (utf16_bytes <= 2) {
        buf[0] = '\0';
        PDF_UNLOCK();
        return PDF_OK;
    }

    /*
     * Stack-allocate for typical metadata strings (<= 512 UTF-16 chars).
     * Heap-fallback for unusual cases.
     */
    uint16_t stack_u16[512];
    uint16_t* utf16 = NULL;
    bool heap = (utf16_bytes > sizeof(stack_u16));

    if (heap) {
        utf16 = malloc(utf16_bytes);
        if (utf16 == NULL) {
            PDF_UNLOCK();
            return PDF_ERR_NOMEM;
        }
    } else {
        utf16 = stack_u16;
    }

    FPDF_GetMetaText((FPDF_DOCUMENT)doc->_handle, key, utf16, utf16_bytes);
    PDF_UNLOCK();

    iconv_t cd = get_iconv_cd();
    if (cd == (iconv_t)-1) {
        if (heap) free(utf16);
        return PDF_ERR_UNKNOWN;
    }

    /* Reset iconv state before use. */
    iconv(cd, NULL, NULL, NULL, NULL);

    char* inbuf = (char*)utf16;
    size_t inleft = utf16_bytes - 2; /* exclude UTF-16 NUL */
    char* outbuf = buf;
    size_t outleft = bufsz - 1;

    pdf_status_t ret = PDF_OK;
    size_t rc = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    if (rc == (size_t)-1 && errno == E2BIG) {
        ret = PDF_ERR_TRUNC;
    } else if (rc == (size_t)-1) {
        buf[0] = '\0';
        ret = PDF_ERR_UNKNOWN;
    }

    *outbuf = '\0';
    if (heap) free(utf16);
    return ret;
}

/* -------------------------------------------------------------------------
 * Page — zero-allocation (stack-friendly) API
 * ---------------------------------------------------------------------- */

static pdf_status_t pdf_page_init(pdf_document_t* doc, int page_index, pdf_page_t* page) {
    PDF_LOCK();
    FPDF_PAGE raw = FPDF_LoadPage((FPDF_DOCUMENT)doc->_handle, page_index);
    if (raw == NULL) {
        PDF_UNLOCK();
        fprintf(stderr, "pdf_page_init: failed to load page %d\n", page_index);
        return PDF_ERR_PAGE;
    }
    PDF_UNLOCK();

    page->_handle = raw;
    return PDF_OK;
}

void pdf_page_destroy(pdf_page_t* page) {
    if (page == NULL || page->_handle == NULL) return;
    PDF_LOCK();
    FPDF_ClosePage((FPDF_PAGE)page->_handle);
    PDF_UNLOCK();
    page->_handle = NULL;
}

/* -------------------------------------------------------------------------
 * Page — heap-allocation (legacy / pointer) API
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_page_open(pdf_document_t* doc, int page_index, pdf_page_t* page) {
    if (doc == NULL || page == NULL || page_index < 0) return PDF_ERR_PARAM;

    return pdf_page_init(doc, page_index, page);
}

void pdf_page_close(pdf_page_t* page) {
    if (page == NULL) return;
    pdf_page_destroy(page);
}

float pdf_page_width(const pdf_page_t* page) {
    if (page == NULL || page->_handle == NULL) return 0.0f;
    PDF_LOCK();
    float v = FPDF_GetPageWidthF((FPDF_PAGE)page->_handle);
    PDF_UNLOCK();
    return v;
}

float pdf_page_height(const pdf_page_t* page) {
    if (page == NULL || page->_handle == NULL) return 0.0f;
    PDF_LOCK();
    float v = FPDF_GetPageHeightF((FPDF_PAGE)page->_handle);
    PDF_UNLOCK();
    return v;
}

/* -------------------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_page_render(const pdf_page_t* page, float scale, pdf_bitmap_t* bmp) {
    if (page == NULL || page->_handle == NULL || bmp == NULL || scale <= 0.0f) return PDF_ERR_PARAM;

    PDF_LOCK();
    int w = (int)(FPDF_GetPageWidthF((FPDF_PAGE)page->_handle) * scale);
    int h = (int)(FPDF_GetPageHeightF((FPDF_PAGE)page->_handle) * scale);

    if (w <= 0 || h <= 0) {
        PDF_UNLOCK();
        fprintf(stderr, "pdf_page_render: degenerate page dimensions %dx%d\n", w, h);
        return PDF_ERR_RENDER;
    }

    FPDF_BITMAP fb = FPDFBitmap_Create(w, h, 1 /* has_alpha */);
    if (fb == NULL) {
        PDF_UNLOCK();
        return PDF_ERR_RENDER;
    }

    FPDFBitmap_FillRect(fb, 0, 0, w, h, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(fb, (FPDF_PAGE)page->_handle, 0, 0, w, h, 0, FPDF_ANNOT);

    int stride = FPDFBitmap_GetStride(fb);
    void* src_pixels = FPDFBitmap_GetBuffer(fb);
    size_t total = (size_t)stride * (size_t)h;

    uint8_t* rgba = malloc(total);
    if (rgba == NULL) {
        FPDFBitmap_Destroy(fb);
        PDF_UNLOCK();
        return PDF_ERR_NOMEM;
    }

    memcpy(rgba, src_pixels, total);
    FPDFBitmap_Destroy(fb);
    PDF_UNLOCK();

    /* Swap B↔R channels (PDFium gives BGRA; we expose RGBA). */
    for (int row = 0; row < h; row++) {
        uint8_t* p = rgba + (size_t)row * (size_t)stride;
        for (int col = 0; col < w; col++, p += 4) {
            uint8_t tmp = p[0];
            p[0] = p[2];
            p[2] = tmp;
        }
    }

    *bmp = (pdf_bitmap_t){.pixels = rgba, .width = w, .height = h, .stride = stride};
    return PDF_OK;
}

/* -------------------------------------------------------------------------
 * Text extraction — zero-allocation (stack-friendly) API
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_text_init(pdf_page_t* page, pdf_text_t* text) {
    if (page == NULL || page->_handle == NULL || text == NULL) return PDF_ERR_PARAM;

    PDF_LOCK();
    FPDF_TEXTPAGE raw = FPDFText_LoadPage((FPDF_PAGE)page->_handle);
    if (raw == NULL) {
        PDF_UNLOCK();
        fprintf(stderr, "pdf_text_init: FPDFText_LoadPage failed\n");
        return PDF_ERR_TEXT;
    }
    text->count = FPDFText_CountChars(raw);
    PDF_UNLOCK();

    text->_handle = raw;
    return PDF_OK;
}

void pdf_text_destroy(pdf_text_t* text) {
    if (text == NULL || text->_handle == NULL) return;
    PDF_LOCK();
    FPDFText_ClosePage((FPDF_TEXTPAGE)text->_handle);
    PDF_UNLOCK();
    text->_handle = NULL;
    text->count = 0;
}

/* -------------------------------------------------------------------------
 * Text extraction — heap-allocation (legacy / pointer) API
 * ---------------------------------------------------------------------- */

pdf_status_t pdf_text_open(pdf_page_t* page, pdf_text_t** out) {
    if (page == NULL || out == NULL) return PDF_ERR_PARAM;

    pdf_text_t* text = malloc(sizeof(*text));
    if (text == NULL) return PDF_ERR_NOMEM;

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

int pdf_text_char_count(const pdf_text_t* text) {
    return (text != NULL) ? text->count : 0;
}

/* -------------------------------------------------------------------------
 * Core UTF-16 → UTF-8 conversion (shared by both text extraction paths).
 *
 * utf16      — input buffer (count UTF-16 code units, no NUL required)
 * char_count — number of code units
 * buf        — destination UTF-8 buffer
 * cap        — capacity of buf (must be >= 1)
 * written    — if non-NULL, receives bytes written (excl. NUL) on PDF_OK
 *
 * Returns PDF_OK, PDF_ERR_TRUNC, or PDF_ERR_UNKNOWN.
 * ---------------------------------------------------------------------- */
static pdf_status_t convert_utf16_to_utf8(const uint16_t* utf16, int char_count, char* buf, size_t cap,
                                          size_t* written) {
    iconv_t cd = get_iconv_cd();
    if (cd == (iconv_t)-1) {
        fprintf(stderr, "pdf: iconv_open failed: %s\n", strerror(errno));
        return PDF_ERR_UNKNOWN;
    }

    /* Reset iconv shift state before every conversion. */
    iconv(cd, NULL, NULL, NULL, NULL);

    char* inbuf = (char*)utf16;
    size_t inleft = (size_t)char_count * sizeof(uint16_t);
    char* outbuf = buf;
    size_t outleft = cap - 1; /* reserve one byte for NUL */

    pdf_status_t ret = PDF_OK;
    size_t rc = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    if (rc == (size_t)-1 && errno == E2BIG) {
        ret = PDF_ERR_TRUNC;
    } else if (rc == (size_t)-1) {
        buf[0] = '\0';
        return PDF_ERR_UNKNOWN;
    }

    *outbuf = '\0';
    if (written) *written = (size_t)(outbuf - buf);
    return ret;
}

pdf_status_t pdf_text_utf8_buf(const pdf_text_t* text, char* buf, size_t cap, size_t* written) {
    if (text == NULL || buf == NULL || cap == 0) return PDF_ERR_PARAM;

    int char_count = text->count;
    if (char_count <= 0) {
        buf[0] = '\0';
        if (written) *written = 0;
        return PDF_OK;
    }

    /*
     * Stack path: char_count <= MAX_UTF16_STACK_CHARS avoids any heap use.
     * Heap fallback: used only for unusually large pages.
     */
    uint16_t stack_buf[MAX_UTF16_STACK_CHARS + 1];
    uint16_t* utf16 = NULL;
    bool heap = (char_count > MAX_UTF16_STACK_CHARS);

    if (heap) {
        utf16 = malloc(((size_t)char_count + 1) * sizeof(uint16_t));
        if (utf16 == NULL) return PDF_ERR_NOMEM;
    } else {
        utf16 = stack_buf;
    }

    PDF_LOCK();
    FPDFText_GetText((FPDF_TEXTPAGE)text->_handle, 0, char_count, utf16);
    PDF_UNLOCK();

    pdf_status_t ret = convert_utf16_to_utf8(utf16, char_count, buf, cap, written);

    if (heap) free(utf16);
    return ret;
}

pdf_status_t pdf_text_utf8(const pdf_text_t* text, char** out) {
    if (text == NULL || out == NULL) return PDF_ERR_PARAM;

    int char_count = text->count;
    if (char_count <= 0) {
        char* empty = malloc(1);
        if (empty == NULL) return PDF_ERR_NOMEM;
        empty[0] = '\0';
        *out = empty;
        return PDF_OK;
    }

    /* Stack path for the UTF-16 source (same threshold as utf8_buf). */
    uint16_t stack_buf[MAX_UTF16_STACK_CHARS + 1];
    uint16_t* utf16 = NULL;
    bool heap = (char_count > MAX_UTF16_STACK_CHARS);

    if (heap) {
        utf16 = malloc(((size_t)char_count + 1) * sizeof(uint16_t));
        if (utf16 == NULL) return PDF_ERR_NOMEM;
    } else {
        utf16 = stack_buf;
    }

    PDF_LOCK();
    FPDFText_GetText((FPDF_TEXTPAGE)text->_handle, 0, char_count, utf16);
    PDF_UNLOCK();

    /* Allocate the UTF-8 output buffer (worst case: 3 bytes per char + NUL). */
    size_t utf8_cap = (size_t)char_count * 3 + 1;
    char* utf8 = malloc(utf8_cap);
    if (utf8 == NULL) {
        if (heap) free(utf16);
        return PDF_ERR_NOMEM;
    }

    size_t written = 0;
    pdf_status_t ret = convert_utf16_to_utf8(utf16, char_count, utf8, utf8_cap, &written);
    if (heap) free(utf16);

    if (ret != PDF_OK && ret != PDF_ERR_TRUNC) {
        free(utf8);
        return ret;
    }

    /*
     * Shrink the allocation to the actual used size.  Ignore realloc
     * failure — the original buffer is still valid and correctly sized.
     */
    if (written + 1 < utf8_cap) {
        char* shrunk = realloc(utf8, written + 1);
        if (shrunk) utf8 = shrunk;
    }

    *out = utf8;
    return PDF_OK; /* callers of the heap API don't need to know about truncation */
}

pdf_status_t pdf_text_char_boxes(const pdf_text_t* text, int start_index, int count, pdf_char_box_t* boxes) {
    if (text == NULL || text->_handle == NULL || boxes == NULL) return PDF_ERR_PARAM;
    if (start_index < 0 || count <= 0) return PDF_ERR_PARAM;
    if (start_index + count > text->count) return PDF_ERR_PARAM;

    FPDF_TEXTPAGE handle = (FPDF_TEXTPAGE)text->_handle;

    PDF_LOCK();
    for (int i = 0; i < count; i++) {
        int idx = start_index + i;
        double left = 0.0, right = 0.0, bottom = 0.0, top = 0.0;

        /* FPDFText_GetCharBox returns 0 on failure; leave a zeroed box
         * for that character rather than aborting the whole batch, so
         * one malformed glyph doesn't cost the caller the rest of the
         * page's layout data. */
        if (!FPDFText_GetCharBox(handle, idx, &left, &right, &bottom, &top)) { left = right = bottom = top = 0.0; }

        boxes[i] = (pdf_char_box_t){
            .left = (float)left,
            .right = (float)right,
            .bottom = (float)bottom,
            .top = (float)top,
            .codepoint = (uint32_t)FPDFText_GetUnicode(handle, idx),
        };
    }
    PDF_UNLOCK();

    return PDF_OK;
}

/* -------------------------------------------------------------------------
 * Image encoding helpers
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

pdf_status_t pdf_bitmap_write(const pdf_bitmap_t* bmp, const char* path, pdf_image_format_t format, int quality) {
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
        while (new_cap < needed)
            new_cap *= 2;
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

pdf_status_t pdf_bitmap_encode(const pdf_bitmap_t* bmp, pdf_image_format_t format, int quality, uint8_t** out_data,
                               size_t* out_size) {
    if (bmp == NULL || bmp->pixels == NULL || out_data == NULL || out_size == NULL) return PDF_ERR_PARAM;

    uint8_t* rgb = rgba_to_rgb(bmp);
    if (rgb == NULL) return PDF_ERR_NOMEM;

    int stride_rgb = bmp->width * 3;
    encode_ctx_t ctx = {.data = NULL, .size = 0, .capacity = 0, .status = PDF_OK};
    int rc = 0;

    switch (format) {
        case PDF_IMAGE_PNG:
            rc = stbi_write_png_to_func(encode_write_cb, &ctx, bmp->width, bmp->height, 3, rgb, stride_rgb);
            break;
        case PDF_IMAGE_JPEG:
            quality = (quality <= 0 || quality > 100) ? 88 : quality;
            rc = stbi_write_jpg_to_func(encode_write_cb, &ctx, bmp->width, bmp->height, 3, rgb, quality);
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
