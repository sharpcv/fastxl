#ifndef FASTXL_C_H
#define FASTXL_C_H

// fastxl C ABI: one shared library plus this header, no C++ runtime, no
// third-party headers and no Python ABI. Every entry point is exception-safe
// (errors are returned as status codes and UTF-8 messages) and every buffer
// returned by the library is released with fastxl_buffer_free().
//
// Status codes: 0 is success. Any other value is 1 + the ordinal of the
// internal fastxl::ErrorCode (FileNotFound = 1, NotAZip = 2, ...); use
// fastxl_error_name() for the stable machine-readable name. The message in
// out_err is diagnostic text and is not a stable contract.
//
// Options are always optional UTF-8 JSON objects (NULL means defaults). Unknown
// keys are ignored so newer wrappers can pass fields to older libraries.
// Supported keys per entry point are listed on the function.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(FASTXL_C_EXPORTS)
#define FASTXL_C_API __declspec(dllexport)
#elif defined(FASTXL_C_STATIC)
#define FASTXL_C_API
#else
#define FASTXL_C_API __declspec(dllimport)
#endif
#else
#define FASTXL_C_API __attribute__((visibility("default")))
#endif

#define FASTXL_C_ABI_VERSION 1u

typedef int32_t fastxl_status_t;

typedef struct fastxl_session fastxl_session_t;
typedef struct fastxl_iter fastxl_iter_t;
typedef struct fastxl_writer fastxl_writer_t;

typedef struct fastxl_buffer {
  char* data;  /* UTF-8 payload (JSON / JSONL / error message), NUL-terminated */
  size_t size; /* byte length, excluding the terminator */
} fastxl_buffer_t;

/* Output buffers: every entry point overwrites the structs it returns through
 * and never inspects their previous contents, so passing an uninitialised
 * stack struct is safe. Reusing a struct for a second call without calling
 * fastxl_buffer_free() on its previous payload leaks that payload. */

/* Version and memory management. */
FASTXL_C_API uint32_t fastxl_abi_version(void);
FASTXL_C_API const char* fastxl_version_string(void);
/* Stable ErrorCode name for a status ("OK", "InvalidSheetRef", ...). */
FASTXL_C_API const char* fastxl_error_name(fastxl_status_t status);
/* Releases data and zeroes the struct. Safe on a zeroed buffer and NULL. */
FASTXL_C_API void fastxl_buffer_free(fastxl_buffer_t* buf);

/* Session: optional cache for repeated probes. Thread-safe; pass NULL to any
 * probe for a one-shot call without caching.
 *
 * options_json keys: capacity (default 8, info payloads), preview_capacity
 * (default 4, preview payloads), profile_capacity (default 64,
 * schema/probe payloads), search_capacity (default 16, search payloads).
 * Entries are keyed by the file's size and mtime, so a rewritten workbook
 * misses automatically.
 */
FASTXL_C_API fastxl_status_t fastxl_session_create(const char* options_json,
                                                   fastxl_session_t** out_session,
                                                   fastxl_buffer_t* out_err);
FASTXL_C_API void fastxl_session_clear(fastxl_session_t* session);
FASTXL_C_API void fastxl_session_destroy(fastxl_session_t* session);

/* Probe and conversion entry points. `file_utf8` is UTF-8 everywhere, also on
 * Windows. On failure out_json is zeroed and out_err holds the message; on
 * success out_err is zeroed. max_bytes in options folds the payload exactly
 * like the CLI --max-bytes flag. */

/* options: format ("xlsx"), max_bytes. */
FASTXL_C_API fastxl_status_t fastxl_info_json(fastxl_session_t* session,
                                              const char* file_utf8,
                                              const char* options_json,
                                              fastxl_buffer_t* out_json,
                                              fastxl_buffer_t* out_err);

/* options: sheet, header_row (number or "auto"), format, rows, range,
 * columns, max_cell_bytes, jobs, max_bytes, infer, delimiter, fill_gaps. */
FASTXL_C_API fastxl_status_t fastxl_preview_json(fastxl_session_t* session,
                                                 const char* file_utf8,
                                                 const char* options_json,
                                                 fastxl_buffer_t* out_json,
                                                 fastxl_buffer_t* out_err);

/* options: sheet, header_row (number or "auto"), format, infer, delimiter,
 * fill_gaps, max_rows, sample, top, distinct_cap, jobs, regions, max_bytes. */
FASTXL_C_API fastxl_status_t fastxl_schema_json(fastxl_session_t* session,
                                                const char* file_utf8,
                                                const char* options_json,
                                                fastxl_buffer_t* out_json,
                                                fastxl_buffer_t* out_err);

/* Same options as fastxl_schema_json. */
FASTXL_C_API fastxl_status_t fastxl_probe_json(fastxl_session_t* session,
                                               const char* file_utf8,
                                               const char* options_json,
                                               fastxl_buffer_t* out_json,
                                               fastxl_buffer_t* out_err);

/* options: regex, ignore_case, context, max_hits, max_cell_bytes, sheet,
 * format, max_bytes. */
FASTXL_C_API fastxl_status_t fastxl_search_json(fastxl_session_t* session,
                                                const char* file_utf8,
                                                const char* query_utf8,
                                                const char* options_json,
                                                fastxl_buffer_t* out_json,
                                                fastxl_buffer_t* out_err);

/* options: from ("xlsx"/"csv"/"jsonl"), to, sheet, header_row, infer,
 * delimiter, fill_gaps, limit, normalize, columns, crlf, bom, sheet_name,
 * compression_level. */
FASTXL_C_API fastxl_status_t fastxl_convert(const char* src_utf8, const char* dst_utf8,
                                            const char* options_json,
                                            fastxl_buffer_t* out_err);

/* Streaming row reader. fastxl_iter_open() returns a meta JSON payload with
 * file, format, sheet, header_row, header_auto and columns, then each
 * fastxl_iter_next_batch() appends up to max_rows rows as JSONL.
 *
 * options: sheet, header_row (number or "auto"), format, infer, delimiter,
 * fill_gaps, max_columns, limit, offset, shape ("arrays" default, or
 * "objects"). One JSON line per row. A batch size of 0 means 4096;
 * *out_rows_read == 0 means the stream is exhausted and the reader closed. */
FASTXL_C_API fastxl_status_t fastxl_iter_open(const char* file_utf8, const char* options_json,
                                              fastxl_iter_t** out_iter,
                                              fastxl_buffer_t* out_meta_json,
                                              fastxl_buffer_t* out_err);
FASTXL_C_API fastxl_status_t fastxl_iter_next_batch(fastxl_iter_t* iter, size_t max_rows,
                                                    fastxl_buffer_t* out_chunk,
                                                    size_t* out_rows_read,
                                                    fastxl_buffer_t* out_err);
FASTXL_C_API void fastxl_iter_close(fastxl_iter_t* iter);
FASTXL_C_API void fastxl_iter_destroy(fastxl_iter_t* iter);

/* Streaming writer. Batches are JSONL: one JSON object or array per line.
 * options: columns (required for array rows, optional for objects), format,
 * sheet_name, header, delimiter, crlf, bom, compression_level, fsync. */
FASTXL_C_API fastxl_status_t fastxl_writer_open(const char* dst_utf8,
                                                const char* options_json,
                                                fastxl_writer_t** out_writer,
                                                fastxl_buffer_t* out_err);
/* jsonl_len == 0 means strlen(jsonl_data). */
FASTXL_C_API fastxl_status_t fastxl_writer_write_jsonl(fastxl_writer_t* writer,
                                                       const char* jsonl_data,
                                                       size_t jsonl_len,
                                                       fastxl_buffer_t* out_err);
FASTXL_C_API fastxl_status_t fastxl_writer_finish(fastxl_writer_t* writer,
                                                  fastxl_buffer_t* out_err);
FASTXL_C_API void fastxl_writer_destroy(fastxl_writer_t* writer);

#ifdef __cplusplus
}
#endif

#endif /* FASTXL_C_H */
