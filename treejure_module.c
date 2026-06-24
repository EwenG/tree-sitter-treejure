#include <emacs-module.h>
#include <tree_sitter/api.h>
#include <string.h>
#include <stdlib.h>

// This tells C about the generated grammar function in parser.c.
extern const TSLanguage *tree_sitter_treejure();

int plugin_is_GPL_compatible;

// ---------------------------------------------------------------------------
// treejure semantic module
//
// Emacs' built-in `treesit' owns the per-buffer *syntax* layer for Clojure --
// grammar-level highlighting and cljfmt indentation (see
// replique-clojure-mode.el).  This C dynamic module owns the *semantic* layer:
// project-wide scope and resolution, diagnostics, cross-file navigation, and
// the semantic faces layered on top of treesit's.
//
// The module runs its **own** parser on the **same** grammar.  A TSTree cannot
// cross the emacs-module.h boundary (treesit's tree is an opaque Lisp object),
// so treesit and the module parse the same source independently -- one grammar,
// redundant CPU, never divergent logic.  See PLAN.md, "Core architectural
// facts".
//
// This file is the foundation the semantic layer is built on: the
// per-active-file parser/tree/state lifecycle and the diff-at-debounce **Parse
// model** (`treejure-buffer-parse'), an incremental reparse derived from a
// prefix/suffix text diff.  The wider surface in PLAN.md's "Module API" and
// "Data model (C-side sketch)" -- a Workspace per project (files hash, ns index,
// require graph), the scope/resolution pass, `treejure-check-buffer',
// `treejure-semantic-faces', `treejure-definition'/`-references', and the
// int<->keyword category and diagnostic-id contracts -- builds on it: the
// per-active-file struct below is the seed of a FileNode, and
// `treejure-buffer-parse' is the active-file path of `treejure-check-buffer'.
// ---------------------------------------------------------------------------

// --- Per-active-file parse state & diff-at-debounce incremental reparse ---
//
// C owns the parser/tree for the active file AND the last full text it parsed.
// On reparse with new full text we derive a single TSInputEdit from the
// prefix/suffix diff of last-vs-new text, edit the old tree, and re-parse
// reusing it -> incremental subtree reuse with no dependence on Emacs change
// events.  The last-parsed text is replaced wholesale each cycle, so it cannot
// desync or compound.

typedef struct {
    TSParser *parser;   // one parser, language set once (not reentrant)
    TSTree   *tree;     // NULL until first parse
    char     *text;     // last-parsed bytes (NUL-terminated copy), or NULL
    size_t    len;      // byte length of `text` (excluding the NUL)
    uint32_t  version;  // monotonic, bumped per successful reparse
} treejure_buffer;

static void finalizer_buffer(void *ptr) {
    if (!ptr) return;
    treejure_buffer *b = (treejure_buffer *)ptr;
    if (b->tree) ts_tree_delete(b->tree);
    if (b->parser) ts_parser_delete(b->parser);
    free(b->text);
    free(b);
}

// Row/column (both in bytes) of byte offset OFF within text T.
static TSPoint point_at(const char *t, size_t off) {
    uint32_t row = 0, col = 0;
    for (size_t i = 0; i < off; i++) {
        if (t[i] == '\n') { row++; col = 0; } else { col++; }
    }
    TSPoint p = { row, col };
    return p;
}

// Derive one TSInputEdit from the longest common prefix/suffix of old vs new
// text (two linear scans; prefix and suffix never overlap). A correct
// prefix/suffix edit always yields a correct tree; multi-spot edits coalesce
// into one wider span (less reuse, never wrong). The boundaries fall between
// byte-identical runs, so no UTF-8 codepoint is split.
static TSInputEdit compute_edit(const char *o, size_t lo, const char *n, size_t ln) {
    size_t minlen = lo < ln ? lo : ln;
    size_t p = 0;
    while (p < minlen && o[p] == n[p]) p++;
    size_t s = 0;
    while (s < minlen - p && o[lo - 1 - s] == n[ln - 1 - s]) s++;
    TSInputEdit e;
    e.start_byte     = (uint32_t)p;
    e.old_end_byte   = (uint32_t)(lo - s);
    e.new_end_byte   = (uint32_t)(ln - s);
    e.start_point    = point_at(o, p);
    e.old_end_point  = point_at(o, lo - s);
    e.new_end_point  = point_at(n, ln - s);
    return e;
}

// (treejure-buffer-new) -> buffer user-ptr (owns parser + tree + last text).
static emacs_value f_buffer_new(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
    treejure_buffer *b = calloc(1, sizeof(treejure_buffer));
    b->parser = ts_parser_new();
    ts_parser_set_language(b->parser, tree_sitter_treejure());
    return env->make_user_ptr(env, finalizer_buffer, b);
}

// (treejure-buffer-parse BUF TEXT) -> version int.
// First call (no prior tree): full parse. Later calls: diff-at-debounce
// incremental reparse against the stored last text (Parse model).
static emacs_value f_buffer_parse(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data) {
    treejure_buffer *b = env->get_user_ptr(env, args[0]);

    ptrdiff_t size = 0;
    env->copy_string_contents(env, args[1], NULL, &size); // size includes the NUL
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return)
        return env->intern(env, "nil");
    char *text = malloc((size_t)size);
    env->copy_string_contents(env, args[1], text, &size);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
        free(text);
        return env->intern(env, "nil");
    }
    size_t len = (size_t)size - 1; // byte length excluding the NUL

    TSTree *new_tree;
    if (b->tree && b->text) {
        TSInputEdit edit = compute_edit(b->text, b->len, text, len);
        ts_tree_edit(b->tree, &edit);
        new_tree = ts_parser_parse_string(b->parser, b->tree, text, (uint32_t)len);
        ts_tree_delete(b->tree);
    } else {
        new_tree = ts_parser_parse_string(b->parser, NULL, text, (uint32_t)len);
    }

    free(b->text);
    b->text = text;
    b->len = len;
    b->tree = new_tree;
    b->version++;

    return env->make_integer(env, b->version);
}

// --- Helper to bind C functions to Emacs Lisp symbols ---

static void bind_fn(emacs_env *env, const char *name, ptrdiff_t min, ptrdiff_t max,
                    emacs_value (*fn)(emacs_env *, ptrdiff_t, emacs_value[], void *)) {
    emacs_value func = env->make_function(env, min, max, fn, NULL, NULL);
    emacs_value symbol = env->intern(env, name);
    emacs_value args[] = {symbol, func};
    env->funcall(env, env->intern(env, "defalias"), 2, args);
}

// --- Initialization ---

int emacs_module_init(struct emacs_runtime *ert) {
    if (ert->size < sizeof (*ert)) return 1;

    emacs_env *env = ert->get_environment(ert);

    bind_fn(env, "treejure-buffer-new", 0, 0, f_buffer_new);
    bind_fn(env, "treejure-buffer-parse", 2, 2, f_buffer_parse);

    return 0;
}
