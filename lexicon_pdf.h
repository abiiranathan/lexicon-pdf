#ifndef LEXICON_PDF_H
#define LEXICON_PDF_H

#include <stdbool.h> /* bool              */
#include <stddef.h>  /* size_t            */
#include <stdint.h>  /* uint8_t           */

/* -------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------- */

/** Status codes returned by all pdf_* functions. */
typedef enum {
    PDF_OK = 0,           /**< Success.                                  */
    PDF_ERR_PARAM = 1,    /**< NULL or invalid parameter.                */
    PDF_ERR_NOMEM = 2,    /**< Memory allocation failed.                 */
    PDF_ERR_FILE = 3,     /**< File not found or unreadable.             */
    PDF_ERR_FORMAT = 4,   /**< Not a PDF or corrupted.                   */
    PDF_ERR_PASSWORD = 5, /**< Document is password-protected.           */
    PDF_ERR_PAGE = 6,     /**< Invalid page index or content error.      */
    PDF_ERR_RENDER = 7,   /**< Bitmap creation or rendering failed.      */
    PDF_ERR_TEXT = 8,     /**< Text page load failed.                    */
    PDF_ERR_TRUNC = 9,    /**< Output truncated; buffer too small.       */
    PDF_ERR_UNKNOWN = 10, /**< PDFium reported an unclassified error.    */
} pdf_status_t;

/**
 * Returns a human-readable string for a pdf_status_t value.
 * The returned pointer is a string literal; do not free.
 *
 * @param status  Status code to describe.
 * @return        NUL-terminated description string.
 */
const char* pdf_strerror(pdf_status_t status);

/* -------------------------------------------------------------------------
 * RGBA bitmap (page render output)
 * ---------------------------------------------------------------------- */

/**
 * Raw RGBA pixel buffer produced by pdf_page_render().
 * Caller must free pixels with pdf_bitmap_free().
 */
typedef struct {
    uint8_t* pixels; /**< Row-major RGBA bytes, top-to-bottom.            */
    int width;       /**< Width in pixels.                                */
    int height;      /**< Height in pixels.                               */
    int stride;      /**< Row stride in bytes (>= width * 4).             */
} pdf_bitmap_t;

/**
 * Frees pixel memory allocated by pdf_page_render().
 * Safe to call with a zeroed struct (pixels == NULL is a no-op).
 *
 * @param bmp  Bitmap whose pixels member will be freed.
 */
void pdf_bitmap_free(pdf_bitmap_t* bmp);

/**
 * PDF document handle.
 *
 * May be stack-allocated.  Initialise with pdf_document_open,
 * release resources with pdf_document_destroy().
 *
 */
typedef struct pdf_document {
    void* _handle; /**< Internal PDFium handle. Do not access directly.  */
} pdf_document_t;

/**
 * PDF page handle.
 *
 * May be stack-allocated.  Initialise with pdf_page_open,
 * release resources with pdf_page_close().
 */
typedef struct pdf_page {
    void* _handle; /**< Internal PDFium handle. Do not access directly.  */
} pdf_page_t;

/**
 * Text-extraction handle.
 *
 * May be stack-allocated.  Initialise with pdf_text_init(),
 * release resources with pdf_text_destroy().
 *
 * For backward compatibility, pdf_text_open() heap-allocates and returns
 * a pointer; release with pdf_text_close().
 */
typedef struct pdf_text {
    void* _handle; /**< Internal PDFium handle. Do not access directly.  */
    int count;     /**< Cached character count.                          */
} pdf_text_t;

/* -------------------------------------------------------------------------
 * Library lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Initialises the PDFium library.
 * Must be called exactly once before any other pdf_* function.
 */
void pdf_library_init(void);

/**
 * Tears down the PDFium library and releases all internal resources.
 * Must be called exactly once after all pdf_* handles have been closed.
 */
void pdf_library_destroy(void);

/* -------------------------------------------------------------------------
 * Document — zero-allocation (stack-friendly) API
 * ---------------------------------------------------------------------- */

/**
 * Releases resources held by a document handle.
 * Does NOT free the pdf_document_t struct itself (use for stack handles).
 * Safe to call on a zeroed struct.
 *
 * @param doc  Document handle initialised by pdf_document_init*().
 */
void pdf_document_destroy(pdf_document_t* doc);

/* -------------------------------------------------------------------------
 * Document — heap-allocation (legacy / pointer) API
 * ---------------------------------------------------------------------- */

/**
 * Opens a PDF document from a file path.
 * Heap-allocates the handle; caller must release with pdf_document_close().
 *
 * @param filename   NUL-terminated path to the PDF file.
 * @param password   Optional password string, or NULL.
 * @param[out] out   Receives the heap-allocated handle on success.
 * @return           PDF_OK on success, or an error code.
 */
pdf_status_t pdf_document_open(pdf_document_t* doc, const char* filename, const char* password);

/**
 * Opens a PDF document from a memory buffer.
 * Heap-allocates the handle; caller must release with pdf_document_close().
 * The buffer is NOT copied; it must remain valid until pdf_document_close().
 *
 * @param data       Pointer to PDF bytes.
 * @param size       Number of bytes at data.
 * @param password   Optional password string, or NULL.
 * @param[out] out   Receives the heap-allocated handle on success.
 * @return           PDF_OK on success, or an error code.
 */
pdf_status_t pdf_document_open_mem(pdf_document_t* out, const void* data, size_t size, const char* password);

/**
 * Closes and frees a heap-allocated document handle.
 * Safe to call with NULL.
 *
 * @param doc  Handle returned by pdf_document_open*().
 */
void pdf_document_close(pdf_document_t* doc);

/**
 * Returns the number of pages in the document.
 *
 * @param doc  Initialised document handle (stack or heap).
 * @return     Page count (>= 1), or -1 if doc is NULL.
 */
int pdf_document_page_count(const pdf_document_t* doc);

/* -------------------------------------------------------------------------
 * Metadata
 * ---------------------------------------------------------------------- */

/** Metadata field selector. */
typedef enum {
    PDF_META_TITLE,    /**< Document title.   */
    PDF_META_AUTHOR,   /**< Author.           */
    PDF_META_SUBJECT,  /**< Subject.          */
    PDF_META_KEYWORDS, /**< Keywords.         */
    PDF_META_CREATOR,  /**< Creating app.     */
    PDF_META_PRODUCER, /**< PDF producer.     */
} pdf_meta_field_t;

/**
 * Reads a metadata field from the document into a caller-supplied buffer.
 *
 * @param doc    Initialised document handle.
 * @param field  Which metadata field to read.
 * @param buf    Output buffer (UTF-8).
 * @param bufsz  Capacity of buf in bytes.
 * @return       PDF_OK on success, PDF_ERR_TRUNC if output was truncated,
 *               PDF_ERR_PARAM if doc/buf is NULL.
 */
pdf_status_t pdf_document_metadata(const pdf_document_t* doc, pdf_meta_field_t field, char* buf, size_t bufsz);

/* -------------------------------------------------------------------------
 * Page — heap-allocation (legacy / pointer) API
 * ---------------------------------------------------------------------- */

/**
 * Loads a single page from an open document.
 * Heap-allocates the inner page handle; caller must release with pdf_page_close().
 *
 * @param doc        Initialised document handle.
 * @param page_index Zero-based page index.
 * @param[out] out   Receives the heap-allocated handle on success.
 * @return           PDF_OK on success, or an error code.
 */
pdf_status_t pdf_page_open(pdf_document_t* doc, int page_index, pdf_page_t* page);

/**
 * Closes and frees a heap-allocated page handle.
 * Safe to call with NULL.
 */
void pdf_page_close(pdf_page_t* page);

/** Returns the width of the page in PDF points. */
float pdf_page_width(const pdf_page_t* page);

/** Returns the height of the page in PDF points. */
float pdf_page_height(const pdf_page_t* page);

/**
 * Renders a page to an RGBA bitmap.
 *
 * @param page       Initialised page handle.
 * @param scale      Pixels per PDF point (must be > 0).
 * @param[out] bmp   Receives the rendered bitmap; free with pdf_bitmap_free().
 * @return           PDF_OK on success, or an error code.
 */
pdf_status_t pdf_page_render(const pdf_page_t* page, float scale, pdf_bitmap_t* bmp);

/* -------------------------------------------------------------------------
 * Text extraction — zero-allocation (stack-friendly) API
 * ---------------------------------------------------------------------- */

/**
 * Initialises a text-extraction handle.
 *
 * @param page       Initialised page handle.
 * @param text       Caller-allocated text struct to initialise.
 * @return           PDF_OK on success, PDF_ERR_TEXT on failure.
 * @note             Release with pdf_text_destroy().
 */
pdf_status_t pdf_text_init(pdf_page_t* page, pdf_text_t* text);

/**
 * Releases resources held by a text handle.
 * Does NOT free the pdf_text_t struct itself.
 * Safe to call on a zeroed struct.
 */
void pdf_text_destroy(pdf_text_t* text);

/* -------------------------------------------------------------------------
 * Text extraction — heap-allocation (legacy / pointer) API
 * ---------------------------------------------------------------------- */

/**
 * Loads the text layer for a page.
 * Heap-allocates the handle; caller must release with pdf_text_close().
 *
 * @param page       Initialised page handle.
 * @param[out] out   Receives the heap-allocated handle on success.
 * @return           PDF_OK on success, PDF_ERR_TEXT on failure.
 */
pdf_status_t pdf_text_open(pdf_page_t* page, pdf_text_t** out);

/**
 * Closes and frees a heap-allocated text handle.
 * Safe to call with NULL.
 */
void pdf_text_close(pdf_text_t* text);

/** Returns the number of Unicode characters on the text page. */
int pdf_text_char_count(const pdf_text_t* text);

/**
 * Extracts all text into a caller-supplied UTF-8 buffer. Zero allocation.
 *
 * This is the preferred hot-path API. Keep one buffer per worker thread
 * and reuse it across every page of every document; the function always
 * NUL-terminates on success or truncation.
 *
 * @param text      Initialised text handle.
 * @param buf       Destination buffer.
 * @param cap       Capacity of buf in bytes (must be >= 1).
 * @param[out] written  If non-NULL, receives the number of UTF-8 bytes
 *                  written (not counting the NUL terminator) on success.
 * @return          PDF_OK on success, PDF_ERR_TRUNC if the output was
 *                  truncated to fit, PDF_ERR_PARAM if text/buf is NULL.
 */
pdf_status_t pdf_text_utf8_buf(const pdf_text_t* text, char* buf, size_t cap, size_t* written);

/**
 * Extracts all text as a heap-allocated NUL-terminated UTF-8 string.
 * Caller must free() the returned buffer.
 *
 * @param text       Initialised text handle.
 * @param[out] out   Receives a malloc'd UTF-8 string on success.
 * @return           PDF_OK on success, PDF_ERR_NOMEM, or PDF_ERR_PARAM.
 */
pdf_status_t pdf_text_utf8(const pdf_text_t* text, char** out);

/* -------------------------------------------------------------------------
 * Image output
 * ---------------------------------------------------------------------- */

/** Image output format. */
typedef enum {
    PDF_IMAGE_PNG,  /**< Lossless RGB PNG.  */
    PDF_IMAGE_JPEG, /**< Lossy RGB JPEG.    */
} pdf_image_format_t;

/**
 * Writes a rendered bitmap to a file as PNG or JPEG.
 *
 * @param bmp     Bitmap produced by pdf_page_render().
 * @param path    Destination file path.
 * @param format  PDF_IMAGE_PNG or PDF_IMAGE_JPEG.
 * @param quality JPEG quality 0–100 (ignored for PNG).
 * @return        PDF_OK on success, PDF_ERR_PARAM or PDF_ERR_RENDER on failure.
 */
pdf_status_t pdf_bitmap_write(const pdf_bitmap_t* bmp, const char* path, pdf_image_format_t format, int quality);

/**
 * Encodes a rendered bitmap to PNG or JPEG in memory.
 * The returned buffer is heap-allocated; caller must free() it.
 *
 * @param bmp           Bitmap produced by pdf_page_render().
 * @param format        PDF_IMAGE_PNG or PDF_IMAGE_JPEG.
 * @param quality       JPEG quality 0–100 (ignored for PNG).
 * @param[out] out_data Receives a pointer to the encoded bytes on success.
 * @param[out] out_size Receives the size of the encoded data in bytes.
 * @return              PDF_OK on success, or an error code.
 */
pdf_status_t pdf_bitmap_encode(const pdf_bitmap_t* bmp, pdf_image_format_t format, int quality, uint8_t** out_data,
                               size_t* out_size);

#endif /* LEXICON_PDF_H */
