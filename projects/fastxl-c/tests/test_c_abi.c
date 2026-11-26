/* Pure C11 smoke test for the fastxl C ABI.
 *
 * Build and run:
 *   cc -std=c11 -Iprojects/fastxl-c/include projects/fastxl-c/tests/test_c_abi.c \
 *      -Lbuild/lib -lfastxl -Wl,-rpath,"$PWD/build/lib" -o build/test_c_abi
 *   build/test_c_abi projects/tests/fixtures/basic.xlsx build/c-abi-tmp
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fastxl_c.h"

static void require(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static int file_contains(const char* path, const char* needle) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return 0;
  char buffer[4096];
  const size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  buffer[got] = '\0';
  return strstr(buffer, needle) != NULL;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <fixture.xlsx> <work-dir>\n", argv[0]);
    return 2;
  }
  const char* fixture = argv[1];
  const char* work_dir = argv[2];

  require(fastxl_abi_version() == FASTXL_C_ABI_VERSION, "abi version");
  require(strlen(fastxl_version_string()) != 0, "version string");
  require(strcmp(fastxl_error_name(0), "OK") == 0, "OK name");
  require(strcmp(fastxl_error_name(1), "FileNotFound") == 0, "status offset");

  fastxl_buffer_t out = {0};
  fastxl_buffer_t err = {0};

  /* info (one-shot, no session) */
  require(fastxl_info_json(NULL, fixture, NULL, &out, &err) == 0, "info status");
  require(out.data != NULL && strstr(out.data, "\"sheets\"") != NULL, "info payload");
  require(out.size == strlen(out.data), "buffer size excludes terminator");
  fastxl_buffer_free(&out);
  require(out.data == NULL && out.size == 0, "buffer free zeroes");

  /* session + warm schema */
  fastxl_session_t* session = NULL;
  require(fastxl_session_create("{\"profile_capacity\":4}", &session, &err) == 0,
          "session create");
  require(session != NULL, "session handle");
  require(fastxl_schema_json(session, fixture, "{\"header_row\":\"auto\"}", &out, &err) == 0,
          "schema status");
  require(strstr(out.data, "\"columns\"") != NULL, "schema payload");
  const size_t schema_size = out.size;
  fastxl_buffer_free(&out);
  require(fastxl_schema_json(session, fixture, "{\"header_row\":\"auto\"}", &out, &err) == 0,
          "warm schema status");
  require(out.size == schema_size, "warm schema payload");
  fastxl_buffer_free(&out);

  require(fastxl_probe_json(session, fixture, NULL, &out, &err) == 0, "probe status");
  require(strstr(out.data, "\"rows_scanned\"") != NULL, "probe payload");
  fastxl_buffer_free(&out);

  require(fastxl_preview_json(NULL, fixture, "{\"rows\":2}", &out, &err) == 0,
          "preview status");
  require(strstr(out.data, "\"rows\"") != NULL, "preview payload");
  fastxl_buffer_free(&out);

  require(fastxl_search_json(NULL, fixture, "a", "{\"max_hits\":2}", &out, &err) == 0,
          "search status");
  require(strstr(out.data, "\"match_count\"") != NULL, "search payload");
  fastxl_buffer_free(&out);

  /* error path */
  fastxl_status_t status = fastxl_info_json(NULL, "does-not-exist.xlsx", NULL, &out, &err);
  require(status == 1, "missing file status");
  require(strcmp(fastxl_error_name(status), "FileNotFound") == 0, "missing file name");
  require(err.data != NULL && err.size != 0, "missing file message");
  require(out.data == NULL, "failed call leaves out_json empty");
  fastxl_buffer_free(&err);

  /* streaming iterator */
  fastxl_iter_t* iter = NULL;
  require(fastxl_iter_open(fixture, "{\"limit\":2}", &iter, &out, &err) == 0,
          "iter open");
  require(iter != NULL && strstr(out.data, "\"columns\"") != NULL, "iter meta");
  fastxl_buffer_free(&out);
  size_t rows = 0;
  require(fastxl_iter_next_batch(iter, 1, &out, &rows, &err) == 0, "batch 1");
  require(rows == 1 && out.size != 0, "batch 1 content");
  fastxl_buffer_free(&out);
  require(fastxl_iter_next_batch(iter, 10, &out, &rows, &err) == 0, "batch 2");
  require(rows == 1 && out.size != 0, "batch 2 content");
  fastxl_buffer_free(&out);
  require(fastxl_iter_next_batch(iter, 10, &out, &rows, &err) == 0, "batch end");
  require(rows == 0 && out.size == 0, "batch end empty");
  fastxl_iter_close(iter);
  require(fastxl_iter_next_batch(iter, 10, &out, &rows, &err) == 0, "batch after close");
  require(rows == 0, "closed iterator yields nothing");
  fastxl_iter_destroy(iter);

  /* streaming writer (CSV) */
  char csv_path[1024];
  snprintf(csv_path, sizeof(csv_path), "%s/abi-out.csv", work_dir);
  fastxl_writer_t* writer = NULL;
  require(fastxl_writer_open(csv_path, "{\"columns\":[\"a\",\"b\"],\"format\":\"csv\"}",
                             &writer, &err) == 0,
          "writer open");
  require(fastxl_writer_write_jsonl(writer, "{\"a\":1,\"b\":\"x\"}\n[2,\"y\"]\n", 0,
                                    &err) == 0,
          "writer rows");
  require(fastxl_writer_finish(writer, &err) == 0, "writer finish");
  fastxl_writer_destroy(writer);
  require(file_contains(csv_path, "a,b\n1,x\n2,y\n"), "writer output");

  /* conversion */
  char xlsx_path[1024];
  snprintf(xlsx_path, sizeof(xlsx_path), "%s/abi-out.xlsx", work_dir);
  require(fastxl_convert(csv_path, xlsx_path, "{\"to\":\"xlsx\"}", &err) == 0,
          "convert status");
  require(file_contains(xlsx_path, "PK") == 1, "convert output is a zip");

  fastxl_session_clear(session);
  fastxl_session_destroy(session);
  fastxl_buffer_free(&out);
  fastxl_buffer_free(&err);
  printf("c-abi ok: fastxl %s, abi %u\n", fastxl_version_string(), fastxl_abi_version());
  return 0;
}
