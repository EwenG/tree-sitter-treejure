#ifndef TREEJURE_JAR_READER_H
#define TREEJURE_JAR_READER_H

#include <stddef.h>

// Read a single ENTRY (e.g. "clojure/string.clj") from the zip/jar at JAR_PATH
// into a freshly malloc'd, NUL-terminated buffer; *LEN gets the byte length
// excluding the NUL.  Returns NULL if the jar cannot be opened or the entry is
// absent.  The caller owns the buffer and frees it with plain `free`.
//
// This is the module's one window onto jar contents (PLAN: "expose just
// read_jar_entry(path, entry) -> bytes"): it wraps the vendored miniz zip
// reader (STORED + DEFLATE + zip64) and has no dependency on the tree walks or
// the Emacs boundary, so it can later run off the main thread.
char *read_jar_entry(const char *jar_path, const char *entry, size_t *len);

#endif // TREEJURE_JAR_READER_H
