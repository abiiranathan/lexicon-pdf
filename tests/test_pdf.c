/**
 * @file test_pdf.c
 * @brief Self-contained regression suite for lexicon-pdf.
 *
 * Generates its own fixture PDFs at runtime (pure C, no third-party tools),
 * then exercises the full public API: open (file + memory), page count,
 * metadata, page dimensions, text extraction (stack + heap API), character
 * boxes, rendering, bitmap write/encode, and every documented error path.
 *
 * Exit code is 0 when all checks pass, 1 otherwise.
 *
 * Build & run via CMake/CTest (see top-level CMakeLists.txt):
 *   cmake -B build && cmake --build build && ctest --test-dir build
 *   # or: make -C build test
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../lexicon_pdf.h"

/* -------------------------------------------------------------------------
 * Tiny test harness
 * ---------------------------------------------------------------------- */

static int g_fails = 0;
static int g_passes = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (cond) {                                                \
            g_passes++;                                            \
            printf("PASS: %s\n", msg);                             \
        } else {                                                   \
            g_fails++;                                             \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                          \
    } while (0)

/* -------------------------------------------------------------------------
 * Minimal in-memory PDF writer (enough for a valid 3-page fixture)
 * ---------------------------------------------------------------------- */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} sbuf_t;

static void sb_reserve(sbuf_t* sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap * 2 : 1024;
    while (cap < sb->len + extra + 1) cap *= 2;
    char* next = realloc(sb->data, cap);
    if (next == NULL) {
        fprintf(stderr, "test: out of memory\n");
        exit(1);
    }
    sb->data = next;
    sb->cap = cap;
}

static void sb_putn(sbuf_t* sb, const void* p, size_t n) {
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, p, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_puts(sbuf_t* sb, const char* s) { sb_putn(sb, s, strlen(s)); }

static void sb_printf(sbuf_t* sb, const char* fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) {
        fprintf(stderr, "test: vsnprintf failed\n");
        exit(1);
    }
    if ((size_t)n >= sizeof tmp) {
        fprintf(stderr, "test: formatted string too long\n");
        exit(1);
    }
    sb_putn(sb, tmp, (size_t)n);
}

/** Appends a PDF literal string with (\( \) \\) escaping. */
static void sb_pdf_string(sbuf_t* sb, const char* s) {
    sb_putn(sb, "(", 1);
    for (; *s; s++) {
        if (*s == '(' || *s == ')' || *s == '\\') sb_putn(sb, "\\", 1);
        sb_putn(sb, s, 1);
    }
    sb_putn(sb, ")", 1);
}

/**
 * Writes a minimal 3-page PDF to @p path.
 * Page 1: "Hello World", page 2: "Second page caf\xE9" (WinAnsi -> U+00E9),
 * page 3: empty. Info dict carries Title/Author only (Producer absent).
 */
static void write_fixture_pdf(const char* path) {
    static const char* contents[] = {
        "BT /F1 12 Tf 100 750 Td (Hello World) Tj ET",
        "BT /F1 12 Tf 100 750 Td (Second page caf\351) Tj ET",
        "",
    };
    const int npages = 3;

    sbuf_t sb = {0};
    sb_puts(&sb, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");

    /* Object numbers: 1 catalog, 2 pages, 3..5 page, 6 font, 7..9 contents,
     * 10 info. */
    long offs[11] = {0};

#define BEGIN_OBJ(n)                     \
    do {                                 \
        offs[n] = (long)sb.len;          \
        sb_printf(&sb, "%d 0 obj\n", n); \
    } while (0)

    BEGIN_OBJ(1);
    sb_puts(&sb, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    BEGIN_OBJ(2);
    sb_puts(&sb, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n");

    for (int i = 0; i < npages; i++) {
        BEGIN_OBJ(3 + i);
        sb_printf(&sb,
                  "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 841] "
                  "/Resources << /Font << /F1 6 0 R >> >> /Contents %d 0 R >>\nendobj\n",
                  7 + i);
    }

    BEGIN_OBJ(6);
    sb_puts(&sb, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");

    for (int i = 0; i < npages; i++) {
        BEGIN_OBJ(7 + i);
        sb_printf(&sb, "<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n", strlen(contents[i]),
                  contents[i]);
    }

    BEGIN_OBJ(10);
    sb_puts(&sb, "<< ");
    sb_puts(&sb, "/Title ");
    sb_pdf_string(&sb, "Hello Title");
    sb_puts(&sb, " /Author ");
    sb_pdf_string(&sb, "Test Author");
    sb_puts(&sb, " >>\nendobj\n");

#undef BEGIN_OBJ

    long xref = (long)sb.len;
    sb_printf(&sb, "xref\n0 11\n0000000000 65535 f \n");
    for (int i = 1; i <= 10; i++) sb_printf(&sb, "%010ld 00000 n \n", offs[i]);
    sb_puts(&sb, "trailer\n<< /Size 11 /Root 1 0 R /Info 10 0 R >>\nstartxref\n");
    sb_printf(&sb, "%ld\n%%%%EOF\n", xref);

    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "test: cannot write %s\n", path);
        exit(1);
    }
    if (fwrite(sb.data, 1, sb.len, f) != sb.len) {
        fprintf(stderr, "test: short write to %s\n", path);
        exit(1);
    }
    fclose(f);
    free(sb.data);
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static bool valid_utf8(const char* s) {
    while (*s) {
        unsigned char c = (unsigned char)*s;
        int need;
        if (c < 0x80)
            need = 0;
        else if ((c & 0xE0) == 0xC0)
            need = 1;
        else if ((c & 0xF0) == 0xE0)
            need = 2;
        else if ((c & 0xF8) == 0xF0)
            need = 3;
        else
            return false;
        for (int i = 1; i <= need; i++)
            if (((unsigned char)s[i] & 0xC0) != 0x80) return false;
        s += 1 + need;
    }
    return true;
}

static char* read_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    char* data = malloc((size_t)sz);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return data;
}

/* -------------------------------------------------------------------------
 * Suite
 * ---------------------------------------------------------------------- */

int main(void) {
    const char* tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
    char dir[512];
    snprintf(dir, sizeof dir, "%s/lexicon-pdf-test-%d", tmp, (int)getpid());
    mkdir(dir, 0700);

    char pdf_path[600], png_path[600], jpg_path[600];
    snprintf(pdf_path, sizeof pdf_path, "%s/fixture.pdf", dir);
    snprintf(png_path, sizeof png_path, "%s/page.png", dir);
    snprintf(jpg_path, sizeof jpg_path, "%s/page.jpg", dir);
    write_fixture_pdf(pdf_path);

    pdf_library_init();

    /* -- document: file open + page count ------------------------------ */
    pdf_document_t doc = {0};
    CHECK(pdf_document_open(&doc, pdf_path, NULL) == PDF_OK, "open fixture file");
    CHECK(pdf_document_page_count(&doc) == 3, "page count == 3");
    CHECK(pdf_document_open(NULL, pdf_path, NULL) == PDF_ERR_PARAM, "open NULL doc rejected");
    CHECK(pdf_document_page_count(NULL) == -1, "page_count NULL -> -1");

    /* -- metadata ------------------------------------------------------ */
    char buf[512];
    CHECK(pdf_document_metadata(&doc, PDF_META_TITLE, buf, sizeof buf) == PDF_OK &&
              strcmp(buf, "Hello Title") == 0,
          "metadata title");
    CHECK(pdf_document_metadata(&doc, PDF_META_AUTHOR, buf, sizeof buf) == PDF_OK &&
              strcmp(buf, "Test Author") == 0,
          "metadata author");
    CHECK(
        pdf_document_metadata(&doc, PDF_META_PRODUCER, buf, sizeof buf) == PDF_OK && buf[0] == '\0',
        "missing metadata -> empty string");
    char tiny[4];
    CHECK(pdf_document_metadata(&doc, PDF_META_TITLE, tiny, sizeof tiny) == PDF_ERR_TRUNC,
          "metadata truncation reported");
    CHECK(pdf_document_metadata(NULL, PDF_META_TITLE, buf, sizeof buf) == PDF_ERR_PARAM,
          "metadata NULL doc rejected");

    /* -- page dimensions + bad index ----------------------------------- */
    pdf_page_t page = {0};
    CHECK(pdf_page_open(&doc, 0, &page) == PDF_OK, "page open 0");
    float w = pdf_page_width(&page), h = pdf_page_height(&page);
    CHECK(w > 593.0f && w < 597.0f && h > 839.0f && h < 843.0f, "page dims ~595x841pt");
    pdf_page_t badpage = {0};
    CHECK(pdf_page_open(&doc, 99, &badpage) == PDF_ERR_PAGE, "out-of-range page rejected");

    /* -- text: stack API on ASCII page --------------------------------- */
    pdf_text_t txt = {0};
    CHECK(pdf_text_init(&page, &txt) == PDF_OK, "text init page 1");
    CHECK(pdf_text_char_count(&txt) == 11, "page 1 char count 11");
    char big[64];
    size_t written = 0;
    CHECK(pdf_text_utf8_buf(&txt, big, sizeof big, &written) == PDF_OK &&
              strcmp(big, "Hello World") == 0 && written == 11,
          "utf8_buf exact content");
    char small[6];
    CHECK(pdf_text_utf8_buf(&txt, small, sizeof small, &written) == PDF_ERR_TRUNC &&
              strcmp(small, "Hello") == 0 && written == 5,
          "utf8_buf truncation");
    char* heap = NULL;
    CHECK(pdf_text_utf8(&txt, &heap) == PDF_OK && strcmp(heap, "Hello World") == 0,
          "utf8 heap content");
    free(heap);

    pdf_char_box_t boxes[11];
    CHECK(pdf_text_char_boxes(&txt, 0, 11, boxes) == PDF_OK, "char boxes full range");
    CHECK(boxes[0].codepoint == (uint32_t)'H', "box[0] codepoint 'H'");
    CHECK(boxes[0].left > 98.0f && boxes[0].left < 102.0f, "box[0] left edge near x=100");
    CHECK(boxes[0].left < boxes[0].right && boxes[0].bottom < boxes[0].top,
          "box[0] sane bottom-left-origin box");
    CHECK(pdf_text_char_boxes(&txt, 10, 2, boxes) == PDF_ERR_PARAM, "char boxes OOB rejected");
    pdf_text_destroy(&txt);
    pdf_page_close(&page);

    /* -- text: heap API on WinAnsi unicode page ------------------------ */
    CHECK(pdf_page_open(&doc, 1, &page) == PDF_OK, "page open 1");
    pdf_text_t* ht = NULL;
    CHECK(pdf_text_open(&page, &ht) == PDF_OK, "heap text open");
    CHECK(pdf_text_char_count(ht) == 16, "page 2 char count 16 (é is one char)");
    CHECK(pdf_text_utf8(ht, &heap) == PDF_OK && strlen(heap) == 17, "page 2 utf8 is 17 bytes");
    free(heap);
    /* Cut inside é's 2-byte sequence: must back off, stay valid UTF-8. */
    char mbuf[17];
    CHECK(pdf_text_utf8_buf(ht, mbuf, sizeof mbuf, &written) == PDF_ERR_TRUNC && valid_utf8(mbuf) &&
              written == strlen(mbuf),
          "utf8_buf never splits a multibyte char");
    pdf_text_close(ht);
    pdf_page_close(&page);

    /* -- text: empty page ---------------------------------------------- */
    CHECK(pdf_page_open(&doc, 2, &page) == PDF_OK, "page open 2 (empty)");
    CHECK(pdf_text_init(&page, &txt) == PDF_OK, "text init empty page");
    CHECK(pdf_text_char_count(&txt) == 0, "empty page char count 0");
    CHECK(pdf_text_utf8(&txt, &heap) == PDF_OK && heap[0] == '\0', "empty page utf8 empty");
    free(heap);
    pdf_text_destroy(&txt);

    /* -- render + encode ----------------------------------------------- */
    pdf_bitmap_t bmp = {0};
    CHECK(pdf_page_render(&page, 2.0f, &bmp) == PDF_OK, "render scale 2");
    CHECK(bmp.width == 1190 && bmp.height == 1682 && bmp.pixels != NULL, "bitmap 1190x1682");
    CHECK(bmp.pixels[0] > 200 && bmp.pixels[1] > 200 && bmp.pixels[2] > 200, "background is white");
    CHECK(pdf_bitmap_write(&bmp, png_path, PDF_IMAGE_PNG, 0) == PDF_OK, "write PNG file");
    CHECK(pdf_bitmap_write(&bmp, jpg_path, PDF_IMAGE_JPEG, 88) == PDF_OK, "write JPEG file");
    size_t fsz = 0;
    char* fdata = read_file(png_path, &fsz);
    CHECK(fdata != NULL && fsz > 1000, "PNG file non-trivial size");
    free(fdata);
    uint8_t* enc = NULL;
    size_t encsz = 0;
    CHECK(pdf_bitmap_encode(&bmp, PDF_IMAGE_PNG, 0, &enc, &encsz) == PDF_OK && encsz > 1000,
          "encode PNG to memory");
    free(enc);
    CHECK(pdf_bitmap_encode(&bmp, PDF_IMAGE_JPEG, 88, &enc, &encsz) == PDF_OK && encsz > 100,
          "encode JPEG to memory");
    free(enc);
    pdf_bitmap_t dummy = {0};
    CHECK(pdf_page_render(&page, 0.0f, &dummy) == PDF_ERR_PARAM, "render zero scale rejected");
    pdf_bitmap_free(&bmp);
    pdf_page_close(&page);

    /* -- document: memory open ----------------------------------------- */
    size_t pdf_size = 0;
    char* pdf_data = read_file(pdf_path, &pdf_size);
    CHECK(pdf_data != NULL, "read fixture for mem test");
    pdf_document_t memdoc = {0};
    CHECK(pdf_document_open_mem(&memdoc, pdf_data, pdf_size, NULL) == PDF_OK, "open_mem");
    CHECK(pdf_document_page_count(&memdoc) == 3, "mem doc page count");
    pdf_document_close(&memdoc);
    free(pdf_data);

    /* -- error paths ---------------------------------------------------- */
    pdf_document_t errdoc = {0};
    CHECK(pdf_document_open(&errdoc, "/nonexistent-lexicon-pdf-xyz.pdf", NULL) == PDF_ERR_FILE,
          "missing file -> PDF_ERR_FILE");
    static const char garbage[] = "definitely not a pdf";
    CHECK(pdf_document_open_mem(&errdoc, garbage, sizeof garbage, NULL) == PDF_ERR_FORMAT,
          "garbage bytes -> PDF_ERR_FORMAT");
    CHECK(strcmp(pdf_strerror(PDF_OK), "success") == 0, "strerror PDF_OK");

    pdf_document_close(&doc);
    CHECK(pdf_document_page_count(&doc) == -1, "page_count after close -> -1");

    pdf_library_destroy();

    remove(png_path);
    remove(jpg_path);
    remove(pdf_path);
    rmdir(dir);

    printf("\nRESULT: %d passed, %d failed\n", g_passes, g_fails);
    return g_fails ? 1 : 0;
}
