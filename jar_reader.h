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

// List the file entries of the zip/jar at JAR_PATH: returns a freshly malloc'd
// array of *N_ENTRIES freshly malloc'd, NUL-terminated entry names (directories
// excluded), or NULL (with *N_ENTRIES = 0) if the jar cannot be opened.  Reads
// only the central directory, no entry data.  The caller owns the result and
// frees each name and the array with plain `free`.  Lets the module cache a
// jar's directory once so repeated namespace probes need no further jar I/O.
char **jar_list_entries(const char *jar_path, size_t *n_entries);

#endif // TREEJURE_JAR_READER_H
