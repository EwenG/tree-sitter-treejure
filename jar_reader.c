// jar_reader.c -- the vendored-miniz jar entry reader.
//
// The first real TU split off treejure_module.c (PLAN build-order note): a
// self-contained I/O unit with a narrow interface (`read_jar_entry`) and no tie
// to the data model or the Emacs boundary.  miniz is compiled separately
// (miniz.o) and linked in; only its zip *reader* APIs are used (the vendored
// header ships with the writing APIs disabled).

#include "jar_reader.h"
#include "miniz.h"

#include <stdlib.h>
#include <string.h>

char *read_jar_entry(const char *jar_path, const char *entry, size_t *len) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    // Reads the central directory (at the file's tail) and validates it; cheap
    // relative to a full scan.  The FILE is closed by mz_zip_reader_end below.
    if (!mz_zip_reader_init_file(&zip, jar_path, 0)) return NULL;

    // Case-sensitive: zip entry names are case-sensitive by spec and the JVM
    // resolves jar resources that way, so a require for `foo` must not match an
    // entry `Foo.clj` (miniz's default locate is case-insensitive).
    int idx = mz_zip_reader_locate_file(&zip, entry, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (idx < 0) { mz_zip_reader_end(&zip); return NULL; }

    size_t sz = 0;
    void *p = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &sz, 0);
    mz_zip_reader_end(&zip);
    if (!p) return NULL;

    // miniz allocates with its own allocator and does not NUL-terminate; copy
    // into a plain-malloc'd, NUL-terminated buffer so callers free it with
    // `free` and may parse it as a C string (the tree-sitter parser wants a
    // length, but the module's text invariant keeps the trailing NUL).
    char *buf = malloc(sz + 1);
    if (buf) {
        memcpy(buf, p, sz);
        buf[sz] = '\0';
        if (len) *len = sz;
    }
    mz_free(p);
    return buf;
}

char **jar_list_entries(const char *jar_path, size_t *n_entries) {
    *n_entries = 0;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    // Reads + validates the central directory only (no entry data extracted).
    if (!mz_zip_reader_init_file(&zip, jar_path, 0)) return NULL;

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    char **names = n ? malloc((size_t)n * sizeof(char *)) : NULL;
    size_t got = 0;
    if (names) {
        for (mz_uint i = 0; i < n; i++) {
            if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            size_t l = strlen(st.m_filename);
            char *dup = malloc(l + 1);
            if (dup) { memcpy(dup, st.m_filename, l + 1); names[got++] = dup; }
        }
    }
    mz_zip_reader_end(&zip);
    *n_entries = got;
    return names;
}
