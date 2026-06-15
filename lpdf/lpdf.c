/**
 * @file lpdf.c
 * @brief Command-line interface for the lexicon-pdf library.
 *
 * Commands
 * --------
 *   lpdf info    <file.pdf> [options]   Print metadata + page dimensions
 *   lpdf text    <file.pdf> [options]   Extract text (all pages or one page)
 *   lpdf render  <file.pdf> [options]   Render page(s) to PNG/JPEG images
 *   lpdf extract <file.pdf> [options]   Dump a single page's text to a file
 *
 * Every command accepts both short and long forms of its options (e.g.
 * `-p 3` and `--page 3` are equivalent). Run `lpdf <command> --help` for the
 * full option list of a specific command.
 *
 * Build (standalone, no CMake):
 *   cc -std=c11 -Wall -Wextra -Werror -o lpdf lpdf.c \
 *      $(pkg-config --cflags --libs lexicon-pdf)
 *
 * Portability note:
 *   This program uses getopt_long(), which is available on Linux (glibc)
 *   and macOS (libc) out of the box. On Windows, link against a
 *   getopt_long-compatible shim (e.g. the "getopt-win32" or gnulib
 *   implementations) since MSVC's CRT does not provide it.
 */

#include <lexicon/lexicon_pdf.h>

#include <errno.h>   /* errno, strerror                                  */
#include <getopt.h>  /* getopt_long, struct option, optarg, optind       */
#include <stdbool.h> /* bool                                              */
#include <stdio.h>   /* fprintf, printf, fopen, fputs, fclose, stderr    */
#include <stdlib.h>  /* malloc, free, EXIT_SUCCESS, EXIT_FAILURE          */
#include <string.h>  /* strcmp, strrchr, strlen, strerror                 */

/* ===========================================================================
 * Constants
 * ======================================================================== */

#define META_BUF_SIZE 512u /**< Bytes for any single metadata field.            */
#define PATH_BUF_SIZE 512u /**< Bytes for constructed output file paths.        */
#define DEFAULT_SCALE 2.0f /**< 144 DPI - good thumbnail / screen quality.      */
/**
 * Worst-case length (including the terminating NUL) of the "_pageN.ext"
 * suffix appended to output filenames, where N is a page number formatted
 * with %d/%03d.
 */
#define OUT_SUFFIX_LEN 20u
/** Max length of a user- or input-derived output filename prefix/stem. */
#define PREFIX_MAX   (PATH_BUF_SIZE - OUT_SUFFIX_LEN)
#define JPEG_QUALITY 88 /**< 0-100; 85-90 is the sweet spot for text pages.  */

/* ===========================================================================
 * Shared option-parsing helpers
 * ======================================================================== */

/**
 * Parses a 1-based page number from a string and converts it to a 0-based
 * index.
 *
 * @param s String to parse (as passed to `-p`/`--page`).
 * @return The 0-based page index on success, or -1 on parse error or if the
 *         value is less than 1.
 */
static int parse_page(const char* s) {
    if (s == NULL) { return -1; }

    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < 1) {
        fprintf(stderr, "error: page number must be a positive integer (got '%s')\n", s);
        return -1;
    }
    return (int)(v - 1); /* Convert to 0-based. */
}

/**
 * Parses a floating-point scale factor (e.g. for `-s`/`--scale`).
 *
 * @param s String to parse, or NULL to use the default.
 * @return The parsed scale factor, or ::DEFAULT_SCALE if @p s is NULL,
 *         malformed, or non-positive.
 */
static float parse_scale(const char* s) {
    if (s == NULL) { return DEFAULT_SCALE; }

    char* end = NULL;
    float v = strtof(s, &end);
    if (*end != '\0' || v <= 0.0f) {
        fprintf(stderr, "warning: invalid scale '%s', using %.1f\n", s, (double)DEFAULT_SCALE);
        return DEFAULT_SCALE;
    }
    return v;
}

/**
 * Result of parsing an image-format option (`-f`/`--format`).
 */
typedef struct {
    pdf_image_format_t format; /**< Library format enum to pass to pdf_bitmap_write(). */
    const char* extension;     /**< File extension (without leading dot), e.g. "png".  */
} image_format_t;

/**
 * Parses an image format name into an ::image_format_t.
 *
 * Recognised values are "png" (the default), "jpg", and "jpeg". Any other
 * non-NULL value triggers a warning and falls back to PNG.
 *
 * @param s Format name, or NULL to select the default (PNG).
 * @return The resolved format and its associated file extension.
 */
static image_format_t parse_image_format(const char* s) {
    if (s != NULL && (strcmp(s, "jpg") == 0 || strcmp(s, "jpeg") == 0)) {
        return (image_format_t){.format = PDF_IMAGE_JPEG, .extension = "jpg"};
    }
    if (s != NULL && strcmp(s, "png") != 0) { fprintf(stderr, "warning: unknown format '%s', using 'png'\n", s); }
    return (image_format_t){.format = PDF_IMAGE_PNG, .extension = "png"};
}

/**
 * Derives an output filename stem from an input PDF path by stripping the
 * directory component and a trailing ".pdf" extension (if present).
 *
 * Examples: "/docs/report.pdf" -> "report", "notes.PDF" -> "notes.PDF"
 * (extension matching is case-sensitive, matching the original file naming).
 *
 * @param pdf_path Input path, e.g. "/docs/report.pdf".
 * @param out      Destination buffer for the derived stem. Always
 *                 NUL-terminated on return.
 * @param out_size Size of @p out in bytes. Must be greater than zero.
 */
static void derive_basename_stem(const char* pdf_path, char* out, size_t out_size) {
    const char* base = strrchr(pdf_path, '/');
#if defined(_WIN32)
    /* Windows accepts both '/' and '\\' as path separators. */
    const char* base_win = strrchr(pdf_path, '\\');
    if (base_win != NULL && (base == NULL || base_win > base)) { base = base_win; }
#endif
    base = (base != NULL) ? base + 1 : pdf_path;

    size_t len = strlen(base);
    static const char pdf_ext[] = ".pdf";
    const size_t pdf_ext_len = sizeof(pdf_ext) - 1u;
    if (len > pdf_ext_len && strcmp(base + len - pdf_ext_len, pdf_ext) == 0) { len -= pdf_ext_len; }

    snprintf(out, out_size, "%.*s", (int)len, base);
}

/**
 * Opens a PDF document and reports a uniform error message on failure.
 *
 * @param filename Path to the PDF file.
 * @param password Optional password, or NULL if the document is not
 *                  encrypted.
 * @param out_doc  Receives the opened document handle on success. Left
 *                  unmodified on failure.
 * @return ::PDF_OK on success, or the error status returned by
 *         pdf_document_open() on failure.
 */
static pdf_status_t open_document_or_warn(const char* filename, const char* password, pdf_document_t* doc) {
    pdf_status_t st = pdf_document_open(doc, filename, password);
    if (st != PDF_OK) { fprintf(stderr, "error: cannot open '%s': %s\n", filename, pdf_strerror(st)); }
    return st;
}

/**
 * Validates a 0-based page index against the document's page count and
 * reports a uniform error message if it is out of range.
 *
 * @param page_index 0-based page index to validate.
 * @param page_count Total number of pages in the document.
 * @return true if @p page_index is within range, false otherwise.
 */
static bool page_index_in_range(int page_index, int page_count) {
    if (page_index < 0 || page_index >= page_count) {
        fprintf(stderr, "error: page %d is out of range (document has %d page%s)\n", page_index + 1, page_count,
                (page_count == 1) ? "" : "s");
        return false;
    }
    return true;
}

/* ===========================================================================
 * Command: info
 * ======================================================================== */

/** Prints usage information for the `info` command to @p out. */
static void print_info_usage(FILE* out) {
    fprintf(out,
            "Usage: lpdf info <file.pdf> [options]\n"
            "\n"
            "Print document metadata and per-page dimensions.\n"
            "\n"
            "Options:\n"
            "  -w, --password <pw>   Password for encrypted documents\n"
            "  -h, --help             Show this help message\n");
}

/**
 * Implements `lpdf info`.
 *
 * @param argc Argument count, as passed to main() for this subcommand
 *             (argv[0] is the subcommand name itself, per getopt convention).
 * @param argv Argument vector for this subcommand.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on error.
 */
static int cmd_info(int argc, char* argv[]) {
    static const struct option long_opts[] = {
        {"password", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    const char* password = NULL;

    optind = 1;
    opterr = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "w:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'w':
                password = optarg;
                break;
            case 'h':
                print_info_usage(stdout);
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "error: unrecognized option '%s'\n", argv[optind - 1]);
                print_info_usage(stderr);
                return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing required <file.pdf> argument\n");
        print_info_usage(stderr);
        return EXIT_FAILURE;
    }
    if (optind != argc - 1) {
        fprintf(stderr, "error: unexpected extra argument '%s'\n", argv[optind + 1]);
        print_info_usage(stderr);
        return EXIT_FAILURE;
    }

    const char* filename = argv[optind];

    pdf_document_t doc = {0};
    if (open_document_or_warn(filename, password, &doc) != PDF_OK) { return EXIT_FAILURE; }

    int exit_code = EXIT_SUCCESS;
    char buf[META_BUF_SIZE];

    /* Metadata fields to print, in display order. */
    static const struct {
        pdf_meta_field_t field; /**< Library field identifier.   */
        const char* label;      /**< Human-readable column label. */
    } fields[] = {
        {PDF_META_TITLE, "Title"},       {PDF_META_AUTHOR, "Author"},   {PDF_META_SUBJECT, "Subject"},
        {PDF_META_KEYWORDS, "Keywords"}, {PDF_META_CREATOR, "Creator"}, {PDF_META_PRODUCER, "Producer"},
    };

    printf("File    : %s\n", filename);
    printf("Pages   : %d\n", pdf_document_page_count(&doc));

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        buf[0] = '\0';
        pdf_document_metadata(&doc, fields[i].field, buf, sizeof(buf));
        /* Skip fields the document left empty. */
        if (buf[0] != '\0') { printf("%-9s: %s\n", fields[i].label, buf); }
    }

    /* Per-page dimensions. */
    int page_count = pdf_document_page_count(&doc);
    printf("\nPage dimensions (points, 1 pt = 1/72 in):\n");

    for (int i = 0; i < page_count; i++) {
        pdf_page_t page = {0};
        pdf_status_t st = pdf_page_open(&doc, i, &page);
        if (st != PDF_OK) {
            fprintf(stderr, "  page %d: failed to open: %s\n", i + 1, pdf_strerror(st));
            exit_code = EXIT_FAILURE;
            continue;
        }
        printf("  page %3d: %.1f x %.1f pt  (%.2f x %.2f in)\n", i + 1, (double)pdf_page_width(&page),
               (double)pdf_page_height(&page), (double)pdf_page_width(&page) / 72.0,
               (double)pdf_page_height(&page) / 72.0);
        pdf_page_close(&page);
    }

    pdf_document_close(&doc);
    return exit_code;
}

/* ===========================================================================
 * Command: text
 * ======================================================================== */

/** Prints usage information for the `text` command to @p out. */
static void print_text_usage(FILE* out) {
    fprintf(out,
            "Usage: lpdf text <file.pdf> [options]\n"
            "\n"
            "Extract text from all pages, or a single page with --page.\n"
            "\n"
            "Options:\n"
            "  -p, --page <N>        Extract only page N (1-based); default: all pages\n"
            "  -o, --output <path>   Write output to <path> instead of stdout\n"
            "  -w, --password <pw>   Password for encrypted documents\n"
            "  -h, --help             Show this help message\n");
}

/**
 * Implements `lpdf text`.
 *
 * @param argc Argument count for this subcommand.
 * @param argv Argument vector for this subcommand.
 * @return EXIT_SUCCESS on success (even if some pages had no text layer),
 *         EXIT_FAILURE on error.
 */
static int cmd_text(int argc, char* argv[]) {
    static const struct option long_opts[] = {
        {"page", required_argument, NULL, 'p'},
        {"output", required_argument, NULL, 'o'},
        {"password", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    const char* page_str = NULL;
    const char* out_path = NULL;
    const char* password = NULL;

    optind = 1;
    opterr = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:o:w:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                page_str = optarg;
                break;
            case 'o':
                out_path = optarg;
                break;
            case 'w':
                password = optarg;
                break;
            case 'h':
                print_text_usage(stdout);
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "error: unrecognized option '%s'\n", argv[optind - 1]);
                print_text_usage(stderr);
                return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing required <file.pdf> argument\n");
        print_text_usage(stderr);
        return EXIT_FAILURE;
    }
    if (optind != argc - 1) {
        fprintf(stderr, "error: unexpected extra argument '%s'\n", argv[optind + 1]);
        print_text_usage(stderr);
        return EXIT_FAILURE;
    }

    const char* filename = argv[optind];

    /* -1 means "all pages". A malformed --page value is fatal. */
    int single_page = -1;
    if (page_str != NULL) {
        single_page = parse_page(page_str);
        if (single_page < 0) { return EXIT_FAILURE; }
    }

    pdf_document_t doc = {0};
    if (open_document_or_warn(filename, password, &doc) != PDF_OK) { return EXIT_FAILURE; }

    int page_count = pdf_document_page_count(&doc);
    int first = (single_page >= 0) ? single_page : 0;
    int last = (single_page >= 0) ? single_page : page_count - 1;

    if (single_page >= 0 && !page_index_in_range(single_page, page_count)) {
        pdf_document_close(&doc);
        return EXIT_FAILURE;
    }

    /* Open the output file, if any; otherwise write to stdout. */
    FILE* out = stdout;
    if (out_path != NULL) {
        out = fopen(out_path, "w");
        if (out == NULL) {
            fprintf(stderr, "error: cannot open output file '%s': %s\n", out_path, strerror(errno));
            pdf_document_close(&doc);
            return EXIT_FAILURE;
        }
    }

    int exit_code = EXIT_SUCCESS;
    for (int i = first; i <= last; i++) {
        pdf_page_t page = {0};
        pdf_status_t st = pdf_page_open(&doc, i, &page);
        if (st != PDF_OK) {
            fprintf(stderr, "warning: skipping page %d: %s\n", i + 1, pdf_strerror(st));
            exit_code = EXIT_FAILURE;
            continue;
        }

        pdf_text_t* text = NULL;
        st = pdf_text_open(&page, &text);
        if (st != PDF_OK) {
            fprintf(stderr, "warning: no text layer on page %d: %s\n", i + 1, pdf_strerror(st));
            pdf_page_close(&page);
            exit_code = EXIT_FAILURE;
            continue;
        }

        char* utf8 = NULL;
        st = pdf_text_utf8(text, &utf8);
        if (st != PDF_OK) {
            fprintf(stderr, "warning: text extraction failed on page %d: %s\n", i + 1, pdf_strerror(st));
            exit_code = EXIT_FAILURE;
        } else {
            /* Print a page separator when dumping multiple pages. */
            if (first != last) { fprintf(out, "\n--- Page %d ---\n\n", i + 1); }
            fputs(utf8, out);
            fputc('\n', out);
            free(utf8);
        }

        pdf_text_close(text);
        pdf_page_close(&page);
    }

    if (out != stdout) {
        fclose(out);
        printf("Text written to '%s'\n", out_path);
    }

    pdf_document_close(&doc);
    return exit_code;
}

/* ===========================================================================
 * Command: render
 * ======================================================================== */

/** Prints usage information for the `render` command to @p out. */
static void print_render_usage(FILE* out) {
    fprintf(out,
            "Usage: lpdf render <file.pdf> [options]\n"
            "\n"
            "Render pages to image files. Output filenames are derived from\n"
            "the input name (or --output): <prefix>_pageNNN.<ext>\n"
            "\n"
            "Options:\n"
            "  -p, --page <N>        Render only page N (1-based); default: all pages\n"
            "  -s, --scale <X>       Scale factor (default: %.1f, i.e. %d DPI)\n"
            "  -o, --output <prefix> Output filename prefix; default: derived from input\n"
            "  -f, --format <fmt>    Image format: 'png' (default) or 'jpg'/'jpeg'\n"
            "  -w, --password <pw>   Password for encrypted documents\n"
            "  -h, --help             Show this help message\n",
            (double)DEFAULT_SCALE, (int)(72.0f * DEFAULT_SCALE));
}

/**
 * Implements `lpdf render`.
 *
 * @param argc Argument count for this subcommand.
 * @param argv Argument vector for this subcommand.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE if any page failed to
 *         render or write.
 */
static int cmd_render(int argc, char* argv[]) {
    static const struct option long_opts[] = {
        {"page", required_argument, NULL, 'p'},
        {"scale", required_argument, NULL, 's'},
        {"output", required_argument, NULL, 'o'},
        {"format", required_argument, NULL, 'f'},
        {"password", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    const char* page_str = NULL;
    const char* scale_str = NULL;
    const char* prefix = NULL;
    const char* format_str = NULL;
    const char* password = NULL;

    optind = 1;
    opterr = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:s:o:f:w:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                page_str = optarg;
                break;
            case 's':
                scale_str = optarg;
                break;
            case 'o':
                prefix = optarg;
                break;
            case 'f':
                format_str = optarg;
                break;
            case 'w':
                password = optarg;
                break;
            case 'h':
                print_render_usage(stdout);
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "error: unrecognized option '%s'\n", argv[optind - 1]);
                print_render_usage(stderr);
                return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing required <file.pdf> argument\n");
        print_render_usage(stderr);
        return EXIT_FAILURE;
    }
    if (optind != argc - 1) {
        fprintf(stderr, "error: unexpected extra argument '%s'\n", argv[optind + 1]);
        print_render_usage(stderr);
        return EXIT_FAILURE;
    }

    const char* filename = argv[optind];

    float scale = parse_scale(scale_str);

    int single = -1;
    if (page_str != NULL) {
        single = parse_page(page_str);
        if (single < 0) { return EXIT_FAILURE; }
    }

    image_format_t img = parse_image_format(format_str);
    /* JPEG benefits from an explicit quality setting; PNG ignores it. */
    int write_quality = (img.format == PDF_IMAGE_JPEG) ? JPEG_QUALITY : 0;

    /* Derive the output prefix from the input filename if not supplied. */
    char default_prefix[PATH_BUF_SIZE];
    if (prefix == NULL) {
        derive_basename_stem(filename, default_prefix, sizeof(default_prefix));
        prefix = default_prefix;
    }
    if (strlen(prefix) > PREFIX_MAX) {
        fprintf(stderr, "error: output prefix is too long (max %u chars)\n", (unsigned)PREFIX_MAX);
        return EXIT_FAILURE;
    }

    pdf_document_t doc = {0};
    if (open_document_or_warn(filename, password, &doc) != PDF_OK) { return EXIT_FAILURE; }

    int page_count = pdf_document_page_count(&doc);
    int first = (single >= 0) ? single : 0;
    int last = (single >= 0) ? single : page_count - 1;

    if (single >= 0 && !page_index_in_range(single, page_count)) {
        pdf_document_close(&doc);
        return EXIT_FAILURE;
    }

    printf("Rendering %d page(s) at scale %.1fx (%d DPI) as %s...\n", last - first + 1, (double)scale,
           (int)(72.0f * scale), img.extension);

    int exit_code = EXIT_SUCCESS;
    for (int i = first; i <= last; i++) {
        pdf_page_t page = {0};
        pdf_status_t st = pdf_page_open(&doc, i, &page);
        if (st != PDF_OK) {
            fprintf(stderr, "warning: skipping page %d: %s\n", i + 1, pdf_strerror(st));
            exit_code = EXIT_FAILURE;
            continue;
        }

        pdf_bitmap_t bmp = {0};
        st = pdf_page_render(&page, scale, &bmp);
        if (st != PDF_OK) {
            fprintf(stderr, "warning: render failed on page %d: %s\n", i + 1, pdf_strerror(st));
            pdf_page_close(&page);
            exit_code = EXIT_FAILURE;
            continue;
        }

        /* PREFIX_MAX guarantees this fits within PATH_BUF_SIZE. */
        char out_path[PATH_BUF_SIZE] = {0};
        snprintf(out_path, sizeof(out_path), "%.*s_page%03d.%s", (int)PREFIX_MAX, prefix, i + 1, img.extension);

        pdf_status_t wst = pdf_bitmap_write(&bmp, out_path, img.format, write_quality);
        if (wst == PDF_OK) {
            printf("  page %3d -> %s  (%dx%d)\n", i + 1, out_path, bmp.width, bmp.height);
        } else {
            fprintf(stderr, "warning: failed to write '%s': %s\n", out_path, pdf_strerror(wst));
            exit_code = EXIT_FAILURE;
        }

        pdf_bitmap_free(&bmp);
        pdf_page_close(&page);
    }

    pdf_document_close(&doc);
    return exit_code;
}

/* ===========================================================================
 * Command: extract
 * ======================================================================== */

/** Prints usage information for the `extract` command to @p out. */
static void print_extract_usage(FILE* out) {
    fprintf(out,
            "Usage: lpdf extract <file.pdf> --page N [options]\n"
            "\n"
            "Extract a single page's text to a file. This dumps the page's text\n"
            "layer rather than producing a standalone PDF; for a true PDF page\n"
            "split, use a tool such as pdftk or qpdf.\n"
            "\n"
            "Options:\n"
            "  -p, --page <N>        Page to extract (1-based). Required.\n"
            "  -o, --output <path>   Output file path; default: derived from input\n"
            "  -w, --password <pw>   Password for encrypted documents\n"
            "  -h, --help             Show this help message\n");
}

/**
 * Implements `lpdf extract`.
 *
 * @param argc Argument count for this subcommand.
 * @param argv Argument vector for this subcommand.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on error.
 */
static int cmd_extract(int argc, char* argv[]) {
    static const struct option long_opts[] = {
        {"page", required_argument, NULL, 'p'},
        {"output", required_argument, NULL, 'o'},
        {"password", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    const char* page_str = NULL;
    const char* out_path = NULL;
    const char* password = NULL;

    optind = 1;
    opterr = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:o:w:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                page_str = optarg;
                break;
            case 'o':
                out_path = optarg;
                break;
            case 'w':
                password = optarg;
                break;
            case 'h':
                print_extract_usage(stdout);
                return EXIT_SUCCESS;
            default:
                fprintf(stderr, "error: unrecognized option '%s'\n", argv[optind - 1]);
                print_extract_usage(stderr);
                return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing required <file.pdf> argument\n");
        print_extract_usage(stderr);
        return EXIT_FAILURE;
    }
    if (optind != argc - 1) {
        fprintf(stderr, "error: unexpected extra argument '%s'\n", argv[optind + 1]);
        print_extract_usage(stderr);
        return EXIT_FAILURE;
    }
    if (page_str == NULL) {
        fprintf(stderr, "error: -p/--page <N> is required for 'extract'\n");
        print_extract_usage(stderr);
        return EXIT_FAILURE;
    }

    const char* filename = argv[optind];

    int page_idx = parse_page(page_str);
    if (page_idx < 0) { return EXIT_FAILURE; }

    pdf_document_t doc = {0};
    if (open_document_or_warn(filename, password, &doc) != PDF_OK) { return EXIT_FAILURE; }

    int exit_code = EXIT_SUCCESS;
    pdf_page_t page = {0};
    pdf_text_t* text = NULL;
    char* utf8 = NULL;
    char auto_path[PATH_BUF_SIZE];

    if (!page_index_in_range(page_idx, pdf_document_page_count(&doc))) {
        exit_code = EXIT_FAILURE;
        goto out_close_doc;
    }

    pdf_status_t st = pdf_page_open(&doc, page_idx, &page);
    if (st != PDF_OK) {
        fprintf(stderr, "error: cannot open page %d: %s\n", page_idx + 1, pdf_strerror(st));
        exit_code = EXIT_FAILURE;
        goto out_close_doc;
    }

    st = pdf_text_open(&page, &text);
    if (st != PDF_OK) {
        fprintf(stderr, "error: no text layer on page %d: %s\n", page_idx + 1, pdf_strerror(st));
        exit_code = EXIT_FAILURE;
        goto out_close_page;
    }

    st = pdf_text_utf8(text, &utf8);
    if (st != PDF_OK) {
        fprintf(stderr, "error: text extraction failed: %s\n", pdf_strerror(st));
        exit_code = EXIT_FAILURE;
        goto out_close_text;
    }

    /* Determine the output path if not given: <basename>_pageN.txt
     * stem is sized to PREFIX_MAX so that, combined with OUT_SUFFIX_LEN,
     * auto_path (PATH_BUF_SIZE bytes) is provably large enough. */
    if (out_path == NULL) {
        char stem[PREFIX_MAX];
        derive_basename_stem(filename, stem, sizeof(stem));
        snprintf(auto_path, sizeof(auto_path), "%s_page%d.txt", stem, page_idx + 1);
        out_path = auto_path;
    }

    FILE* f = fopen(out_path, "w");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open '%s': %s\n", out_path, strerror(errno));
        exit_code = EXIT_FAILURE;
        goto out_free_utf8;
    }

    fputs(utf8, f);
    fclose(f);
    printf("Page %d text extracted to '%s' (%d chars)\n", page_idx + 1, out_path, pdf_text_char_count(text));

out_free_utf8:
    free(utf8);
out_close_text:
    pdf_text_close(text);
out_close_page:
    pdf_page_close(&page);
out_close_doc:
    pdf_document_close(&doc);
    return exit_code;
}

/* ===========================================================================
 * Top-level usage and entry point
 * ======================================================================== */

/** Prints top-level usage information (command list) to @p out. */
static void print_usage(FILE* out, const char* prog) {
    fprintf(out,
            "Usage: %s <command> <file.pdf> [options]\n"
            "\n"
            "Commands:\n"
            "  info     Print metadata and page dimensions\n"
            "  text     Extract text from all or one page\n"
            "  render   Render pages to PNG/JPEG images\n"
            "  extract  Extract a single page's text to a file\n"
            "\n"
            "Run '%s <command> --help' for command-specific options.\n"
            "\n"
            "Examples:\n"
            "  %s info    report.pdf\n"
            "  %s text    report.pdf --page 3\n"
            "  %s text    report.pdf -o dump.txt\n"
            "  %s render  report.pdf --scale 3.0\n"
            "  %s render  report.pdf -p 1 -o cover --format jpg\n"
            "  %s extract report.pdf -p 5 -o page5.txt\n",
            prog, prog, prog, prog, prog, prog, prog, prog);
}

/**
 * Program entry point. Dispatches to the appropriate subcommand handler.
 *
 * Each subcommand handler receives an argument vector where index 0 is the
 * subcommand name itself (so that getopt_long's convention of skipping
 * argv[0] works correctly) and the remaining indices are the subcommand's
 * own arguments and options.
 *
 * @param argc Total argument count, including the program name.
 * @param argv Argument vector; argv[0] is the program name, argv[1] is the
 *             subcommand.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on any error.
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    const char* command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }

    /* Subcommand handlers see argv[1] (their own name) as index 0, followed
     * by all remaining arguments and options. */
    int sub_argc = argc - 1;
    char** sub_argv = argv + 1;

    pdf_library_init();

    int rc;
    if (strcmp(command, "info") == 0) {
        rc = cmd_info(sub_argc, sub_argv);
    } else if (strcmp(command, "text") == 0) {
        rc = cmd_text(sub_argc, sub_argv);
    } else if (strcmp(command, "render") == 0) {
        rc = cmd_render(sub_argc, sub_argv);
    } else if (strcmp(command, "extract") == 0) {
        rc = cmd_extract(sub_argc, sub_argv);
    } else {
        fprintf(stderr, "error: unknown command '%s'\n\n", command);
        print_usage(stderr, argv[0]);
        rc = EXIT_FAILURE;
    }

    pdf_library_destroy();
    return rc;
}
