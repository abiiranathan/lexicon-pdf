# lexicon-pdf

`lexicon-pdf` is a pure C library that provides a lightweight abstraction layer over the PDFium engine. It is designed to handle common PDF tasks such as document metadata retrieval, text extraction, and page rendering without requiring heavy UI dependencies.

---

## Features

* **Document Parsing:** Load PDF documents from file paths, memory buffers.
* **Metadata Extraction:** Access document metadata fields (Title, Author, Subject, Keywords, Creator, Producer) converted to UTF-8.
* **Text Extraction:** Extract text content from pages, handled internally in UTF-16LE by PDFium and exposed as UTF-8 via standard `iconv` conversion.
* **Page Rendering:** Render PDF pages into RGBA bitmaps at user-defined scales.
* **Image Generation:** Export rendered bitmaps directly to PNG or JPEG files using an embedded `stb_image_write` implementation.
* **CLI Tool:** Includes `lpdf`, a command-line interface for inspecting, extracting, and rendering PDFs.

---

## Dependencies

Before compiling, ensure you have the following dependencies installed on your system:

1. **PDFium:** The library expects PDFium headers (`fpdfview.h`, `fpdf_doc.h`, `fpdf_text.h`) and compiled binaries to be present in standard search paths (e.g., `/usr/local` or `/usr`).
2. **iconv:** Used to translate PDFium's internal UTF-16LE text to UTF-8 (typically part of POSIX standard libraries or `libc`).

---

## Installation & Build

### Building the Library

The build is managed using CMake. 

To configure and compile a static library:

```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

### Build Options

You can customize the build using the following CMake variables:

* `BUILD_SHARED_LIBS` (Default: `OFF`): Compile `lexicon-pdf` as a shared library instead of a static one.
* `LEXICON_PDF_INSTALL` (Default: `ON`): Generate install targets and a `pkg-config` (`lexicon-pdf.pc`) configuration file.

### Installing

To install the library, headers, and package configuration files:

```bash
sudo make install
```

---

## Integrating with Your Project

### Using CMake

If you install the library, you can consume it in your downstream `CMakeLists.txt`:

```cmake
find_package(lexicon-pdf REQUIRED)

target_link_libraries(my_application PRIVATE lexicon::pdf)
```

### Using pkg-config

Alternatively, compile with standard build tools via `pkg-config`:

```bash
cc $(pkg-config --cflags lexicon-pdf) -o main main.c $(pkg-config --libs lexicon-pdf)
```

---

## API Usage Example

Below is a simple example demonstrating how to initialize the library, open a document, extract text, render a page, and release resources.

```c
#include <stdio.h>
#include <stdlib.h>
#include <lexicon/lexicon_pdf.h>

int main(void) {
    // 1. Initialize the PDFium library
    pdf_library_init();

    pdf_document_t* doc = NULL;
    pdf_status_t status = pdf_document_open("sample.pdf", NULL, &doc);
    if (status != PDF_OK) {
        fprintf(stderr, "Failed to open document: %s\n", pdf_strerror(status));
        pdf_library_destroy();
        return EXIT_FAILURE;
    }

    int page_count = pdf_document_page_count(doc);
    printf("Document loaded. Total pages: %d\n", page_count);

    if (page_count > 0) {
        pdf_page_t* page = NULL;
        // Open the first page (0-indexed)
        if (pdf_page_open(doc, 0, &page) == PDF_OK) {
            
            // --- Text Extraction ---
            pdf_text_t* text_handle = NULL;
            if (pdf_text_open(page, &text_handle) == PDF_OK) {
                char* text_utf8 = NULL;
                if (pdf_text_utf8(text_handle, &text_utf8) == PDF_OK) {
                    printf("--- Page 1 Text ---\n%s\n", text_utf8);
                    free(text_utf8);
                }
                pdf_text_close(text_handle);
            }

            // --- Rendering to Image ---
            pdf_bitmap_t bmp = {0};
            float scale = 2.0f; // Render at 144 DPI (2 * 72 points per inch)
            if (pdf_page_render(page, scale, &bmp) == PDF_OK) {
                // Save the rendered page to a PNG file
                pdf_bitmap_write(&bmp, "page_0.png", PDF_IMAGE_PNG, 0);
                pdf_bitmap_free(&bmp);
            }

            pdf_page_close(page);
        }
    }

    // Clean up
    pdf_document_close(doc);
    pdf_library_destroy();

    return EXIT_SUCCESS;
}
```

---

## Thread Safety

The handle structures (`pdf_document_t`, `pdf_page_t`, and `pdf_text_t`) are **not** safe for concurrent use across multiple threads. You must ensure that operations on a single handle are confined to a single thread, or guarded by external synchronization primitives.

---

## Command Line Utility (`lpdf`)

The repository includes a companion CLI utility, `lpdf`, located in the `lpdf/` directory.

### Building the CLI

If `lexicon-pdf` is installed on your system, you can build the CLI utility using the provided `Makefile`:

```bash
cd lpdf
make
```

### Usage Commands

The tool provides subcommands to interact with PDF files directly:

* **Retrieve Metadata and Page Info:**
  ```bash
  ./lpdf info document.pdf
  ```
* **Extract Text to Standard Output or File:**
  ```bash
  ./lpdf text document.pdf -p 1 -o page1.txt
  ```
* **Render Pages to PNG or JPEG:**
  ```bash
  ./lpdf render document.pdf --scale 2.0 --format png
  ```
* **Dump Text Layer for a Single Page:**
  ```bash
  ./lpdf extract document.pdf --page 5 -o page5_text.txt
  ```

For more options, pass `--help` to any of the subcommands (e.g., `./lpdf render --help`).