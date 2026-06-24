#include <emacs-module.h>
#include <tree_sitter/api.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
// What is built here (PLAN build-order steps 2 + the buffer-only half of 3):
//   * the **Workspace** per project (`treejure-init`) holding N `FileNode`s;
//   * the per-active-file parser/tree/state lifecycle + the diff-at-debounce
//     **Parse model** (incremental reparse from a prefix/suffix text diff);
//   * the **category** and **diagnostic-id** int<->keyword contracts
//     (`treejure-category-names' / `-diagnostic-ids');
//   * `treejure-check-buffer' -- incremental reparse + grammar-level
//     diagnostics (Tier 0) + a **scope & resolution pass** producing
//     buffer-only `:local'/unused-greyout faces and the `unused-binding'
//     diagnostic (Tier 1, locals only) + an **ns-form require pass**
//     flattening the require/use specs and emitting the buffer-only require
//     diagnostics `duplicate-require'/`refer-all'/`namespace-name-mismatch'
//     (Tier 1); the flattened requires seed the cross-file require graph next;
//   * `treejure-semantic-faces' (flat [s e cat ...] read of the face map) and
//     `treejure-close-buffer'.
//
// The cross-file index (NsIndex/require graph/jars) and the `:global-var' /
// `:unresolved' faces it enables are deliberately NOT here -- they need the
// dependency closure and would be false-positive-prone buffer-locally.  They
// are the next slice (PLAN build-order step 4).  All positions are 0-based byte
// offsets; Elisp converts to buffer positions on apply.
// ---------------------------------------------------------------------------

// ===========================================================================
// Enums -- the int<->keyword contract with Elisp.
//
// These orderings ARE the wire format: `treejure-semantic-faces' returns
// category ints, diagnostics carry id ints, and Elisp maps them back through
// `treejure-category-names' / `-diagnostic-ids'.  Never reorder without
// rebuilding; always append.
// ===========================================================================

// Semantic face categories.  Only the categories treesit cannot express live
// here.  Buffer-local analysis emits the local-* categories; the rest arrive
// with the cross-file tier (next slice) and are listed now so the contract is
// stable.
enum {
    CAT_LOCAL,             // a resolved local binding occurrence / usage
    CAT_LOCAL_UNUSED,      // a local binding never used in its scope (greyout)
    CAT_GLOBAL_VAR,        // resolved current-ns / referred var      (Tier 2)
    CAT_SPECIAL_FORM,      // resolved special form                   (Tier 2)
    CAT_MACRO_INVOCATION,  // resolved macro head                     (Tier 2)
    CAT_UNRESOLVED,        // symbol resolving to nothing             (Tier 2)
    CAT__COUNT
};
static const char *const CATEGORY_NAMES[CAT__COUNT] = {
    ":local", ":local-unused", ":global-var",
    ":special-form", ":macro-invocation", ":unresolved"
};

// Diagnostic ids.  Tier 0 = grammar-emitted; the rest are analysis findings.
enum {
    DIAG_SYNTAX_ERROR,            // an ERROR node (Tier 0)
    DIAG_MISSING_FORM,           // a MISSING node (Tier 0)
    DIAG_INVALID_NUMBER,         // (Tier 0)
    DIAG_INVALID_STRING,         // (Tier 0)
    DIAG_INVALID_CHARACTER,      // (Tier 0)
    DIAG_INVALID_SYMBOLIC_VALUE, // (Tier 0)
    DIAG_UNUSED_BINDING,         // a local never used (Tier 1)
    DIAG_DUPLICATE_REQUIRE,      // ns required more than once (Tier 1)
    DIAG_REFER_ALL,              // :refer :all / :use without :only (Tier 1)
    DIAG_NAMESPACE_NAME_MISMATCH,// ns name disagrees with file path (Tier 1)
    DIAG__COUNT
};
static const char *const DIAG_IDS[DIAG__COUNT] = {
    ":syntax-error", ":missing-form", ":invalid-number", ":invalid-string",
    ":invalid-character", ":invalid-symbolic-value", ":unused-binding",
    ":duplicate-require", ":refer-all", ":namespace-name-mismatch"
};

// Severity.  The keyword is what Flymake gets; the level configured for an id
// (`treejure-set-levels', later) can downgrade/suppress it -- for now fixed.
enum { SEV_WARNING, SEV_ERROR };
static const char *const SEVERITY_NAMES[] = { ":warning", ":error" };

// ===========================================================================
// Small growable arrays.
// ===========================================================================

typedef struct {
    uint32_t start, end;
    int category;
} SemanticSpan;

typedef struct {
    uint32_t start, end;
    int severity;
    int id;
    char *message;   // malloc'd, owned by the FileNode
} Diagnostic;

// A lexical binding in flight on the scope stack.
typedef struct {
    const char *name;   // points into the FileNode text (not NUL-terminated)
    size_t name_len;
    uint32_t start, end; // byte span of the binding occurrence (for the face)
    int used;
    int underscore;      // name begins with '_' -> intentionally ignored
} Local;

// ===========================================================================
// Per-file node: the parser/tree/text state + the analysis outputs.
//
// Carries the Parse model (tree + last full text, replaced wholesale each
// cycle so it cannot desync).  `face_map' and `diagnostics' are recomputed on
// each check.  This is the seed of PLAN's FileNode; the cross-file fields
// (NsIndex, requires[], mtime, kind) arrive with the workspace-model slice.
// ===========================================================================

typedef struct {
    char     *path;      // owned key (absolute file path)
    TSTree   *tree;      // NULL until first parse
    char     *text;      // last-parsed bytes (NUL-terminated copy), or NULL
    size_t    len;       // byte length of `text` (excluding the NUL)
    uint32_t  version;   // monotonic, bumped per successful reparse

    SemanticSpan *spans; // sorted by start, recomputed each check
    size_t        nspans, cap_spans;
    Diagnostic   *diags; // recomputed each check
    size_t        ndiags, cap_diags;
} FileNode;

// ===========================================================================
// Workspace: one per project, holds N FileNodes + the shared parser.
//
// `files' is a flat array keyed by path (linear scan -- file counts per
// session are small; swap for a hash if it ever matters).  classpath/roots are
// stored for the cross-file slice; unused here.
// ===========================================================================

typedef struct {
    char      *project_dir;
    char     **classpath; size_t n_classpath;
    char     **roots;     size_t n_roots;
    char     **def_forms; size_t n_def_forms; // user macros analysed like `defn`
    TSParser  *parser;    // one parser, language set once (not reentrant)
    FileNode **files;     size_t n_files, cap_files;
} Workspace;

// --- FileNode lifecycle ---------------------------------------------------

static void filenode_clear_outputs(FileNode *f) {
    for (size_t i = 0; i < f->ndiags; i++) free(f->diags[i].message);
    f->ndiags = 0;
    f->nspans = 0;
}

static void filenode_free(FileNode *f) {
    if (!f) return;
    filenode_clear_outputs(f);
    if (f->tree) ts_tree_delete(f->tree);
    free(f->spans);
    free(f->diags);
    free(f->text);
    free(f->path);
    free(f);
}

static FileNode *ws_find_file(Workspace *ws, const char *path) {
    for (size_t i = 0; i < ws->n_files; i++)
        if (strcmp(ws->files[i]->path, path) == 0) return ws->files[i];
    return NULL;
}

static FileNode *ws_intern_file(Workspace *ws, const char *path) {
    FileNode *f = ws_find_file(ws, path);
    if (f) return f;
    f = calloc(1, sizeof(FileNode));
    f->path = strdup(path);
    if (ws->n_files == ws->cap_files) {
        ws->cap_files = ws->cap_files ? ws->cap_files * 2 : 8;
        ws->files = realloc(ws->files, ws->cap_files * sizeof(FileNode *));
    }
    ws->files[ws->n_files++] = f;
    return f;
}

static void ws_drop_file(Workspace *ws, const char *path) {
    for (size_t i = 0; i < ws->n_files; i++) {
        if (strcmp(ws->files[i]->path, path) == 0) {
            filenode_free(ws->files[i]);
            ws->files[i] = ws->files[--ws->n_files];
            return;
        }
    }
}

static void finalizer_workspace(void *ptr) {
    if (!ptr) return;
    Workspace *ws = (Workspace *)ptr;
    for (size_t i = 0; i < ws->n_files; i++) filenode_free(ws->files[i]);
    free(ws->files);
    if (ws->parser) ts_parser_delete(ws->parser);
    for (size_t i = 0; i < ws->n_classpath; i++) free(ws->classpath[i]);
    for (size_t i = 0; i < ws->n_roots; i++) free(ws->roots[i]);
    for (size_t i = 0; i < ws->n_def_forms; i++) free(ws->def_forms[i]);
    free(ws->classpath);
    free(ws->roots);
    free(ws->def_forms);
    free(ws->project_dir);
    free(ws);
}

// ===========================================================================
// Parse model -- incremental reparse via prefix/suffix diff.
// ===========================================================================

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
// text (two linear scans; prefix and suffix never overlap).  A correct
// prefix/suffix edit always yields a correct tree; multi-spot edits coalesce
// into one wider span (less reuse, never wrong).  The boundaries fall between
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

// Reparse FILE with NEW (length LEN, owned by the caller -> taken over here).
// First call full-parses; later calls reparse incrementally against the stored
// last text (Parse model).
static void filenode_reparse(Workspace *ws, FileNode *f, char *new, size_t len) {
    TSTree *new_tree;
    if (f->tree && f->text) {
        TSInputEdit edit = compute_edit(f->text, f->len, new, len);
        ts_tree_edit(f->tree, &edit);
        new_tree = ts_parser_parse_string(ws->parser, f->tree, new, (uint32_t)len);
        ts_tree_delete(f->tree);
    } else {
        new_tree = ts_parser_parse_string(ws->parser, NULL, new, (uint32_t)len);
    }
    free(f->text);
    f->text = new;
    f->len  = len;
    f->tree = new_tree;
    f->version++;
}

// ===========================================================================
// Tree-walk helpers.
// ===========================================================================

static int type_is(TSNode n, const char *t) {
    return strcmp(ts_node_type(n), t) == 0;
}

// The k-th named child of N that is a real form (skipping comment/discard).
// Returns a null node (ts_node_is_null) when there is none.
static TSNode nth_form(TSNode n, uint32_t k) {
    uint32_t cnt = ts_node_named_child_count(n), seen = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode c = ts_node_named_child(n, i);
        const char *t = ts_node_type(c);
        if (strcmp(t, "comment") == 0 || strcmp(t, "discard") == 0) continue;
        if (seen == k) return c;
        seen++;
    }
    return ts_node_named_child(n, cnt); // null
}

// Peel `with_metadata' wrappers down to the form they decorate.
static TSNode unwrap_meta(TSNode n) {
    while (!ts_node_is_null(n) && type_is(n, "with_metadata"))
        n = ts_node_child_by_field_name(n, "target", 6);
    return n;
}

static TSNode field_name_node(TSNode sym) {
    return ts_node_child_by_field_name(sym, "name", 4);
}

static int sym_has_namespace(TSNode sym) {
    return !ts_node_is_null(ts_node_child_by_field_name(sym, "namespace", 9));
}

// Compare a symbol's NAME field bytes against a C string.
static int sym_name_eq(const char *text, TSNode sym, const char *s) {
    TSNode nm = field_name_node(sym);
    if (ts_node_is_null(nm)) return 0;
    uint32_t a = ts_node_start_byte(nm), b = ts_node_end_byte(nm);
    size_t len = b - a, sl = strlen(s);
    return len == sl && memcmp(text + a, s, len) == 0;
}

// Keyword name field bytes equal a C string (the marker `:` is excluded).
static int kw_name_eq(const char *text, TSNode kw, const char *s) {
    TSNode nm = field_name_node(kw);
    if (ts_node_is_null(nm)) return 0;
    uint32_t a = ts_node_start_byte(nm), b = ts_node_end_byte(nm);
    size_t len = b - a, sl = strlen(s);
    return len == sl && memcmp(text + a, s, len) == 0;
}

// ===========================================================================
// Scope & resolution pass (buffer-only: locals).
//
// One recursive walk builds lexical scopes and resolves every *reference*
// symbol to a local or leaves it for the cross-file tier.  Binding forms are
// handled specially (their binding positions are consumed here, not treated as
// references); everything else is walked generically.
// ===========================================================================

typedef struct {
    Workspace *ws;
    FileNode  *f;
    const char *text;
    Local     *locals; size_t nlocals, cap_locals;
} Analyzer;

static void push_span(FileNode *f, uint32_t start, uint32_t end, int cat) {
    if (start >= end) return;
    if (f->nspans == f->cap_spans) {
        f->cap_spans = f->cap_spans ? f->cap_spans * 2 : 64;
        f->spans = realloc(f->spans, f->cap_spans * sizeof(SemanticSpan));
    }
    f->spans[f->nspans++] = (SemanticSpan){ start, end, cat };
}

static void push_diag(FileNode *f, uint32_t start, uint32_t end,
                      int sev, int id, char *msg) {
    if (f->ndiags == f->cap_diags) {
        f->cap_diags = f->cap_diags ? f->cap_diags * 2 : 32;
        f->diags = realloc(f->diags, f->cap_diags * sizeof(Diagnostic));
    }
    f->diags[f->ndiags++] = (Diagnostic){ start, end, sev, id, msg };
}

static void push_local(Analyzer *a, TSNode sym) {
    TSNode nm = field_name_node(sym);
    if (ts_node_is_null(nm)) return;
    uint32_t a0 = ts_node_start_byte(sym), b0 = ts_node_end_byte(sym);
    uint32_t na = ts_node_start_byte(nm),  nb = ts_node_end_byte(nm);
    if (a->nlocals == a->cap_locals) {
        a->cap_locals = a->cap_locals ? a->cap_locals * 2 : 32;
        a->locals = realloc(a->locals, a->cap_locals * sizeof(Local));
    }
    const char *name = a->text + na;
    size_t name_len = nb - na;
    a->locals[a->nlocals++] = (Local){
        .name = name, .name_len = name_len,
        .start = a0, .end = b0, .used = 0,
        .underscore = (name_len >= 1 && name[0] == '_')
    };
}

// Close the scope frame opened at MARK: emit each binding's face (greyed when
// unused) and an `unused-binding' diagnostic for genuinely-unused, non-`_'
// bindings.
static void pop_scope(Analyzer *a, size_t mark) {
    for (size_t i = mark; i < a->nlocals; i++) {
        Local *l = &a->locals[i];
        push_span(a->f, l->start, l->end,
                  l->used ? CAT_LOCAL : CAT_LOCAL_UNUSED);
        if (!l->used && !l->underscore) {
            char buf[256];
            int n = snprintf(buf, sizeof buf, "unused binding %.*s",
                             (int)l->name_len, l->name);
            char *msg = malloc((n < 0 ? 0 : n) + 1);
            memcpy(msg, buf, (n < 0 ? 0 : n));
            msg[n < 0 ? 0 : n] = '\0';
            push_diag(a->f, l->start, l->end, SEV_WARNING, DIAG_UNUSED_BINDING, msg);
        }
    }
    a->nlocals = mark;
}

// Resolve a reference symbol against the scope stack (innermost first).
static void resolve_ref(Analyzer *a, TSNode sym) {
    if (sym_has_namespace(sym)) return;       // qualified -> never a local
    TSNode nm = field_name_node(sym);
    if (ts_node_is_null(nm)) return;
    uint32_t na = ts_node_start_byte(nm), nb = ts_node_end_byte(nm);
    size_t len = nb - na;
    const char *name = a->text + na;
    for (size_t i = a->nlocals; i-- > 0; ) {
        Local *l = &a->locals[i];
        if (l->name_len == len && memcmp(l->name, name, len) == 0) {
            l->used = 1;
            push_span(a->f, ts_node_start_byte(sym), ts_node_end_byte(sym), CAT_LOCAL);
            return;
        }
    }
    // Not a local: current-ns var / referred / core / unresolved -- left to the
    // cross-file tier (treesit already faces builtins/def-names meanwhile).
}

// Forward declarations for the mutually-recursive walk.
static void analyze_node(Analyzer *a, TSNode node);
static void bind_pattern(Analyzer *a, TSNode pat);

// Bind every symbol introduced by a destructuring pattern.
static void bind_pattern(Analyzer *a, TSNode pat) {
    pat = unwrap_meta(pat);
    if (ts_node_is_null(pat)) return;
    if (type_is(pat, "symbol")) {
        if (!sym_name_eq(a->text, pat, "&"))     // `&` is a marker, not a name
            push_local(a, pat);
        return;
    }
    if (type_is(pat, "vector_literal")) {
        // [a b & rest :as all]
        uint32_t k = 0; TSNode c;
        while (!ts_node_is_null(c = nth_form(pat, k))) {
            TSNode u = unwrap_meta(c);
            if (type_is(u, "keyword") && kw_name_eq(a->text, u, "as")) {
                TSNode whole = nth_form(pat, k + 1);
                if (!ts_node_is_null(whole)) bind_pattern(a, whole);
                k += 2; continue;
            }
            if (type_is(u, "symbol") && sym_name_eq(a->text, u, "&")) { k++; continue; }
            bind_pattern(a, c);
            k++;
        }
        return;
    }
    if (type_is(pat, "map_literal")) {
        // {:keys [a b] :as x :or {..} pat lookup ...}
        uint32_t cnt = ts_node_named_child_count(pat);
        for (uint32_t i = 0; i < cnt; i++) {
            TSNode pair = ts_node_named_child(pat, i);
            if (!type_is(pair, "pair")) continue;
            TSNode key = unwrap_meta(ts_node_child_by_field_name(pair, "key", 3));
            TSNode val = ts_node_child_by_field_name(pair, "value", 5);
            if (ts_node_is_null(key) || ts_node_is_null(val)) continue;
            if (type_is(key, "keyword")) {
                if (kw_name_eq(a->text, key, "keys") ||
                    kw_name_eq(a->text, key, "syms") ||
                    kw_name_eq(a->text, key, "strs")) {
                    TSNode vec = unwrap_meta(val);
                    if (type_is(vec, "vector_literal")) {
                        uint32_t k = 0; TSNode s;
                        while (!ts_node_is_null(s = nth_form(vec, k))) {
                            TSNode su = unwrap_meta(s);
                            if (type_is(su, "symbol")) push_local(a, su);
                            k++;
                        }
                    }
                } else if (kw_name_eq(a->text, key, "as")) {
                    bind_pattern(a, val);
                } else if (kw_name_eq(a->text, key, "or")) {
                    analyze_node(a, val);   // defaults are expressions
                }
                // other namespaced/keyword directives: ignore
            } else {
                // {pattern lookup-key}: the key is the binding, value is data.
                bind_pattern(a, key);
            }
        }
        return;
    }
    // Anything else in a binding position: ignore.
}

// Bind a parameter vector (fn/defn arity): like a sequential vector pattern.
static void bind_params(Analyzer *a, TSNode vec) {
    if (ts_node_is_null(vec) || !type_is(vec, "vector_literal")) return;
    uint32_t k = 0; TSNode c;
    while (!ts_node_is_null(c = nth_form(vec, k))) {
        TSNode u = unwrap_meta(c);
        if (type_is(u, "symbol") && sym_name_eq(a->text, u, "&")) { k++; continue; }
        bind_pattern(a, c);
        k++;
    }
}

// Analyze the body forms of LIST starting at child index FROM as references.
static void analyze_body(Analyzer *a, TSNode list, uint32_t from) {
    TSNode c;
    for (uint32_t k = from; !ts_node_is_null(c = nth_form(list, k)); k++)
        analyze_node(a, c);
}

// fn/defn tail starting at child index IDX: one `[params] body...` arity, or
// several `([params] body...)` arity lists.
static void analyze_fn_tail(Analyzer *a, TSNode list, uint32_t idx) {
    TSNode first = nth_form(list, idx);
    if (ts_node_is_null(first)) return;
    TSNode u = unwrap_meta(first);
    if (type_is(u, "vector_literal")) {           // single arity
        size_t mark = a->nlocals;
        bind_params(a, u);
        analyze_body(a, list, idx + 1);
        pop_scope(a, mark);
    } else if (type_is(u, "list_literal")) {       // multi arity
        TSNode arity; uint32_t k = idx;
        while (!ts_node_is_null(arity = nth_form(list, k))) {
            TSNode au = unwrap_meta(arity);
            if (type_is(au, "list_literal")) {
                size_t mark = a->nlocals;
                bind_params(a, unwrap_meta(nth_form(au, 0)));
                analyze_body(a, au, 1);
                pop_scope(a, mark);
            }
            k++;
        }
    } else {
        analyze_body(a, list, idx);
    }
}

// Sequential binding vector `[pat init pat init ...]` then body -- the
// let/loop/binding/for/doseq family.  WITH_MODIFIERS handles for/doseq's
// `:let`/`:when`/`:while`.  Bindings accumulate into the current frame; the
// caller pops.
static void analyze_bindings_vec(Analyzer *a, TSNode vec, int with_modifiers) {
    if (ts_node_is_null(vec) || !type_is(vec, "vector_literal")) return;
    uint32_t k = 0; TSNode pat;
    while (!ts_node_is_null(pat = nth_form(vec, k))) {
        TSNode init = nth_form(vec, k + 1);
        TSNode pu = unwrap_meta(pat);
        if (with_modifiers && type_is(pu, "keyword")) {
            if (kw_name_eq(a->text, pu, "let")) {
                analyze_bindings_vec(a, unwrap_meta(init), 0);  // nested pairs
            } else if (!ts_node_is_null(init)) {
                analyze_node(a, init);                          // :when / :while
            }
            k += 2; continue;
        }
        if (!ts_node_is_null(init)) analyze_node(a, init);      // init sees prior
        bind_pattern(a, pat);                                   // then bind
        k += 2;
    }
}

// --- Binding-form dispatch ------------------------------------------------

// Head-symbol name sets.  Matched on the unqualified name only (a `foo/let`
// would falsely match, but that is vanishingly rare and harmless here).
static int name_in(const char *text, TSNode sym, const char *const *set) {
    for (size_t i = 0; set[i]; i++)
        if (sym_name_eq(text, sym, set[i])) return 1;
    return 0;
}

// User-declared def macros (`replique-clojure-extra-def-forms`, pushed via
// `treejure-set-def-forms`) are analysed exactly like `defn`: the name is
// skipped and any param vector(s) bind locals.  Degrades gracefully for
// non-defn-shaped forms (their tail is just walked as references).
static int is_extra_def_form(Workspace *ws, const char *text, TSNode sym) {
    for (size_t i = 0; i < ws->n_def_forms; i++)
        if (sym_name_eq(text, sym, ws->def_forms[i])) return 1;
    return 0;
}

static const char *const LET_FORMS[] = {
    "let", "let*", "if-let", "when-let", "if-some", "when-some",
    "loop", "binding", "with-open", "with-local-vars", "with-redefs",
    "dotimes", "when-first", 0
};
static const char *const SEQ_FORMS[] = { "for", "doseq", 0 }; // with modifiers
static const char *const FN_FORMS[]  = { "fn", "fn*", 0 };
static const char *const DEFN_FORMS[] = { "defn", "defn-", "defmacro", 0 };
static const char *const DEF_FORMS[]  = { "def", "defonce", 0 };

// Returns 1 if NODE (a list) was handled as a binding form.
static int analyze_binding_form(Analyzer *a, TSNode list, TSNode head) {
    const char *text = a->text;

    if (name_in(text, head, LET_FORMS) || name_in(text, head, SEQ_FORMS)) {
        int mods = name_in(text, head, SEQ_FORMS);
        size_t mark = a->nlocals;
        analyze_bindings_vec(a, unwrap_meta(nth_form(list, 1)), mods);
        analyze_body(a, list, 2);
        pop_scope(a, mark);
        return 1;
    }
    if (name_in(text, head, FN_FORMS)) {
        TSNode after = nth_form(list, 1);
        size_t mark = a->nlocals;
        uint32_t idx = 1;
        if (!ts_node_is_null(after) && type_is(unwrap_meta(after), "symbol")) {
            push_local(a, unwrap_meta(after));   // self-name visible in body
            idx = 2;
        }
        analyze_fn_tail(a, list, idx);
        pop_scope(a, mark);
        return 1;
    }
    if (name_in(text, head, DEFN_FORMS) || is_extra_def_form(a->ws, text, head)) {
        // name [meta|docstring|attr-map]* (arity | arities) [attr-map]
        uint32_t k = 2;
        TSNode c;
        while (!ts_node_is_null(c = nth_form(list, k))) {
            TSNode u = unwrap_meta(c);
            if (type_is(u, "string") || type_is(u, "map_literal")) { k++; continue; }
            break;
        }
        analyze_fn_tail(a, list, k);
        return 1;
    }
    if (sym_name_eq(text, head, "defmethod")) {
        // (defmethod multifn dispatch-val [params] body...)
        TSNode dispatch = nth_form(list, 2);
        if (!ts_node_is_null(dispatch)) analyze_node(a, dispatch);
        analyze_fn_tail(a, list, 3);
        return 1;
    }
    if (sym_name_eq(text, head, "letfn")) {
        // (letfn [(name [params] body)...] body...)
        TSNode vec = unwrap_meta(nth_form(list, 1));
        size_t mark = a->nlocals;
        if (!ts_node_is_null(vec) && type_is(vec, "vector_literal")) {
            uint32_t k = 0; TSNode spec;
            while (!ts_node_is_null(spec = nth_form(vec, k))) {   // bind names
                TSNode su = unwrap_meta(spec);
                if (type_is(su, "list_literal")) {
                    TSNode nm = unwrap_meta(nth_form(su, 0));
                    if (!ts_node_is_null(nm) && type_is(nm, "symbol"))
                        push_local(a, nm);
                }
                k++;
            }
            k = 0;
            while (!ts_node_is_null(spec = nth_form(vec, k))) {   // analyze fns
                TSNode su = unwrap_meta(spec);
                if (type_is(su, "list_literal")) analyze_fn_tail(a, su, 1);
                k++;
            }
        }
        analyze_body(a, list, 2);
        pop_scope(a, mark);
        return 1;
    }
    if (sym_name_eq(text, head, "catch")) {
        // (catch Class e body...)
        TSNode cls = nth_form(list, 1);
        if (!ts_node_is_null(cls)) analyze_node(a, cls);
        TSNode bind = unwrap_meta(nth_form(list, 2));
        size_t mark = a->nlocals;
        if (!ts_node_is_null(bind) && type_is(bind, "symbol")) push_local(a, bind);
        analyze_body(a, list, 3);
        pop_scope(a, mark);
        return 1;
    }
    if (sym_name_eq(text, head, "as->")) {
        // (as-> expr name body...)
        TSNode expr = nth_form(list, 1);
        if (!ts_node_is_null(expr)) analyze_node(a, expr);
        TSNode bind = unwrap_meta(nth_form(list, 2));
        size_t mark = a->nlocals;
        if (!ts_node_is_null(bind) && type_is(bind, "symbol")) push_local(a, bind);
        analyze_body(a, list, 3);
        pop_scope(a, mark);
        return 1;
    }
    if (name_in(text, head, DEF_FORMS)) {
        // (def name docstring? init?) -- skip the name, refs follow.
        analyze_body(a, list, 2);
        return 1;
    }
    return 0;
}

// Generic recursive walk.
static void analyze_node(Analyzer *a, TSNode node) {
    if (ts_node_is_null(node)) return;
    const char *t = ts_node_type(node);

    if (strcmp(t, "symbol") == 0) { resolve_ref(a, node); return; }
    if (strcmp(t, "with_metadata") == 0) {
        analyze_node(a, ts_node_child_by_field_name(node, "target", 6));
        return;
    }
    // Quoted / discarded / eval-literal subtrees are data, not code -- opaque
    // for local resolution (syntax-quote unquoting is a later refinement).
    if (strcmp(t, "quote") == 0 || strcmp(t, "syntax_quote") == 0 ||
        strcmp(t, "var_quote") == 0 || strcmp(t, "discard") == 0 ||
        strcmp(t, "eval_literal") == 0)
        return;

    if (strcmp(t, "list_literal") == 0) {
        TSNode head = unwrap_meta(nth_form(node, 0));
        if (!ts_node_is_null(head) && type_is(head, "symbol") &&
            !sym_has_namespace(head) && analyze_binding_form(a, node, head))
            return;
        analyze_body(a, node, 0);   // generic: every child is a reference
        return;
    }

    // Other collections / reader macros: walk their form children.
    if (strcmp(t, "vector_literal") == 0 || strcmp(t, "map_literal") == 0 ||
        strcmp(t, "set_literal") == 0 || strcmp(t, "namespaced_map_literal") == 0 ||
        strcmp(t, "fn_literal") == 0 || strcmp(t, "pair") == 0 ||
        strcmp(t, "reader_conditional") == 0 || strcmp(t, "tagged_literal") == 0 ||
        strcmp(t, "deref") == 0 || strcmp(t, "unquote") == 0 ||
        strcmp(t, "unquote_splicing") == 0) {
        analyze_body(a, node, 0);
        return;
    }
    // Atoms (number/string/keyword/...): nothing to resolve.
}

// ===========================================================================
// Grammar-level diagnostics (Tier 0): ERROR / MISSING / invalid_*.
//
// A separate full walk over every child (named and unnamed -- MISSING nodes
// are zero-width and may be unnamed).
// ===========================================================================

static void collect_grammar_diags(FileNode *f, TSNode node) {
    if (ts_node_is_missing(node)) {
        const char *tp = ts_node_type(node);
        char buf[128];
        int n = snprintf(buf, sizeof buf, "missing %s", tp);
        char *msg = malloc(n + 1); memcpy(msg, buf, n); msg[n] = '\0';
        push_diag(f, ts_node_start_byte(node), ts_node_end_byte(node),
                  SEV_ERROR, DIAG_MISSING_FORM, msg);
    } else if (ts_node_is_error(node)) {
        push_diag(f, ts_node_start_byte(node), ts_node_end_byte(node),
                  SEV_ERROR, DIAG_SYNTAX_ERROR, strdup("syntax error"));
    } else {
        const char *tp = ts_node_type(node);
        int id = -1; const char *msg = NULL;
        if (strcmp(tp, "invalid_number") == 0) { id = DIAG_INVALID_NUMBER; msg = "invalid number"; }
        else if (strcmp(tp, "invalid_string") == 0) { id = DIAG_INVALID_STRING; msg = "invalid string"; }
        else if (strcmp(tp, "invalid_character") == 0) { id = DIAG_INVALID_CHARACTER; msg = "invalid character"; }
        else if (strcmp(tp, "erroneous_symbolic_value") == 0) { id = DIAG_INVALID_SYMBOLIC_VALUE; msg = "invalid symbolic value"; }
        if (id >= 0)
            push_diag(f, ts_node_start_byte(node), ts_node_end_byte(node),
                      SEV_ERROR, id, strdup(msg));
    }
    uint32_t cnt = ts_node_child_count(node);
    for (uint32_t i = 0; i < cnt; i++)
        collect_grammar_diags(f, ts_node_child(node, i));
}

// ===========================================================================
// ns-form & require extraction (buffer-only, Tier 1).
//
// A dedicated walk over the top-level `(ns ...)` form distils the require/use
// specs the cross-file tier will resolve, and meanwhile powers the buffer-only
// require linters that need no dependency I/O: `duplicate-require`,
// `refer-all`, and `namespace-name-mismatch`.  Vector specs
// `[lib :as a :refer [..] :refer :all]`, prefix lists `(parent (child :as c)
// child2)`, plain-symbol specs, and `:use` (implicitly refer-all unless
// `:only`/`:refer`) are all flattened to a list of required namespaces.  The
// cross-file slice (PLAN step 4) reuses this to build the require graph; for
// now only the diagnostics it enables are emitted.
// ===========================================================================

// One flattened require/use spec: the resolved namespace plus the location of
// its lib symbol (for the diagnostic), and whether it refers everything.
typedef struct {
    char    *ns;          // resolved required namespace (malloc'd)
    uint32_t start, end;  // byte span of the lib symbol
    int      refer_all;   // :refer :all, or a bare :use spec
    int      from_use;    // came from a (:use ...) clause (message wording)
} ReqSpec;

typedef struct { ReqSpec *specs; size_t n, cap; } ReqList;

static char *node_text_dup(const char *text, TSNode n) {
    uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
    size_t len = b - a;
    char *s = malloc(len + 1);
    memcpy(s, text + a, len);
    s[len] = '\0';
    return s;
}

// Prepend a prefix-list parent (`parent` + "." + NAME); consumes NAME.
static char *with_prefix(const char *prefix, char *name) {
    if (!prefix) return name;
    size_t pl = strlen(prefix), nl = strlen(name);
    char *r = malloc(pl + 1 + nl + 1);
    memcpy(r, prefix, pl);
    r[pl] = '.';
    memcpy(r + pl + 1, name, nl);
    r[pl + 1 + nl] = '\0';
    free(name);
    return r;
}

static void push_req(ReqList *rl, char *ns, uint32_t s, uint32_t e,
                     int refer_all, int from_use) {
    if (rl->n == rl->cap) {
        rl->cap = rl->cap ? rl->cap * 2 : 8;
        rl->specs = realloc(rl->specs, rl->cap * sizeof(ReqSpec));
    }
    rl->specs[rl->n++] = (ReqSpec){ ns, s, e, refer_all, from_use };
}

// Parse one lib spec (symbol | vector | prefix-list) into RL.  PREFIX is the
// enclosing prefix-list parent (or NULL); FROM_USE marks a `:use` clause, whose
// specs refer everything unless narrowed by `:only`/`:refer`.
static void parse_lib_spec(ReqList *rl, const char *text, TSNode spec,
                           const char *prefix, int from_use) {
    spec = unwrap_meta(spec);
    if (ts_node_is_null(spec)) return;

    if (type_is(spec, "symbol")) {
        char *ns = with_prefix(prefix, node_text_dup(text, spec));
        push_req(rl, ns, ts_node_start_byte(spec), ts_node_end_byte(spec),
                 from_use, from_use);
        return;
    }
    if (type_is(spec, "vector_literal")) {
        TSNode lib = unwrap_meta(nth_form(spec, 0));
        if (ts_node_is_null(lib) || !type_is(lib, "symbol")) return;
        char *ns = with_prefix(prefix, node_text_dup(text, lib));
        int refer_all = from_use;
        uint32_t k = 1; TSNode opt;
        while (!ts_node_is_null(opt = nth_form(spec, k))) {
            TSNode ou = unwrap_meta(opt);
            if (type_is(ou, "keyword")) {
                if (kw_name_eq(text, ou, "refer")) {
                    TSNode v = unwrap_meta(nth_form(spec, k + 1));
                    refer_all = (!ts_node_is_null(v) && type_is(v, "keyword")
                                 && kw_name_eq(text, v, "all"));
                    k += 2; continue;
                }
                if (kw_name_eq(text, ou, "only")) { refer_all = 0; k += 2; continue; }
                k += 2; continue;   // :as / :as-alias / :rename / ... + value
            }
            k++;
        }
        push_req(rl, ns, ts_node_start_byte(lib), ts_node_end_byte(lib),
                 refer_all, from_use);
        return;
    }
    if (type_is(spec, "list_literal")) {
        // prefix list: (parent (child ...) child2 ...)
        TSNode par = unwrap_meta(nth_form(spec, 0));
        if (ts_node_is_null(par) || !type_is(par, "symbol")) return;
        char *newpref = with_prefix(prefix, node_text_dup(text, par));
        uint32_t k = 1; TSNode child;
        while (!ts_node_is_null(child = nth_form(spec, k))) {
            parse_lib_spec(rl, text, child, newpref, from_use);
            k++;
        }
        free(newpref);
        return;
    }
}

static int has_clj_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return strcmp(dot, ".clj") == 0 || strcmp(dot, ".cljs") == 0 ||
           strcmp(dot, ".cljc") == 0 || strcmp(dot, ".cljd") == 0 ||
           strcmp(dot, ".bb") == 0;
}

// Does NS (e.g. "foo.bar-baz") munge to PATH's tail ("foo/bar_baz" before the
// extension, on a path-segment boundary)?  clj-kondo's namespace-name-mismatch.
static int ns_path_matches(const char *ns, const char *path) {
    size_t nl = strlen(ns);
    char *m = malloc(nl + 1);
    for (size_t i = 0; i < nl; i++) {
        char c = ns[i];
        m[i] = (c == '.') ? '/' : (c == '-' ? '_' : c);
    }
    m[nl] = '\0';
    const char *dot = strrchr(path, '.');
    size_t plen = dot ? (size_t)(dot - path) : strlen(path);   // path sans ext
    int ok = 0;
    if (plen >= nl && memcmp(path + plen - nl, m, nl) == 0)
        ok = (plen == nl) || path[plen - nl - 1] == '/';
    free(m);
    return ok;
}

// Walk the `(ns ...)` form NS_LIST: check the ns/path match, flatten its
// require/use specs, and emit the buffer-only require diagnostics.
static void analyze_ns_form(FileNode *f, const char *text, TSNode ns_list) {
    TSNode name = unwrap_meta(nth_form(ns_list, 1));
    if (!ts_node_is_null(name) && type_is(name, "symbol") && has_clj_ext(f->path)) {
        char *nsname = node_text_dup(text, name);
        if (!ns_path_matches(nsname, f->path))
            push_diag(f, ts_node_start_byte(name), ts_node_end_byte(name),
                      SEV_WARNING, DIAG_NAMESPACE_NAME_MISMATCH,
                      strdup("namespace name does not match file path"));
        free(nsname);
    }

    ReqList rl = { 0 };
    uint32_t k = 2; TSNode clause;
    while (!ts_node_is_null(clause = nth_form(ns_list, k))) {
        TSNode cu = unwrap_meta(clause);
        if (type_is(cu, "list_literal")) {
            TSNode head = unwrap_meta(nth_form(cu, 0));
            if (!ts_node_is_null(head) && type_is(head, "keyword")) {
                int is_req = kw_name_eq(text, head, "require");
                int is_use = kw_name_eq(text, head, "use");
                if (is_req || is_use) {
                    uint32_t j = 1; TSNode spec;
                    while (!ts_node_is_null(spec = nth_form(cu, j))) {
                        // skip trailing flag keywords (:reload, :verbose, ...)
                        if (!type_is(unwrap_meta(spec), "keyword"))
                            parse_lib_spec(&rl, text, spec, NULL, is_use);
                        j++;
                    }
                }
            }
        }
        k++;
    }

    for (size_t i = 0; i < rl.n; i++) {
        ReqSpec *s = &rl.specs[i];
        if (s->refer_all)
            push_diag(f, s->start, s->end, SEV_WARNING, DIAG_REFER_ALL,
                      strdup(s->from_use ? "prefer :require with :refer over :use"
                                         : "avoid :refer :all"));
        for (size_t p = 0; p < i; p++) {
            if (s->ns && rl.specs[p].ns && strcmp(s->ns, rl.specs[p].ns) == 0) {
                char buf[256];
                int n = snprintf(buf, sizeof buf, "duplicate require of %s", s->ns);
                char *msg = malloc((n < 0 ? 0 : n) + 1);
                memcpy(msg, buf, (n < 0 ? 0 : n));
                msg[n < 0 ? 0 : n] = '\0';
                push_diag(f, s->start, s->end, SEV_WARNING,
                          DIAG_DUPLICATE_REQUIRE, msg);
                break;
            }
        }
    }

    for (size_t i = 0; i < rl.n; i++) free(rl.specs[i].ns);
    free(rl.specs);
}

// Find and analyze the file's top-level `(ns ...)` form, if any.
static void analyze_requires(FileNode *f, const char *text, TSNode root) {
    TSNode c;
    for (uint32_t k = 0; !ts_node_is_null(c = nth_form(root, k)); k++) {
        TSNode u = unwrap_meta(c);
        if (!type_is(u, "list_literal")) continue;
        TSNode head = unwrap_meta(nth_form(u, 0));
        if (!ts_node_is_null(head) && type_is(head, "symbol") &&
            !sym_has_namespace(head) && sym_name_eq(text, head, "ns")) {
            analyze_ns_form(f, text, u);
            return;   // only the first ns form governs the file
        }
    }
}

// ===========================================================================
// Span sorting (faces are sliced by [BEG,END) and applied left-to-right).
// ===========================================================================

static int span_cmp(const void *x, const void *y) {
    const SemanticSpan *p = x, *q = y;
    if (p->start != q->start) return p->start < q->start ? -1 : 1;
    if (p->end   != q->end)   return p->end   < q->end   ? -1 : 1;
    return 0;
}

// Run the full buffer-local analysis on F's current tree.
static void analyze_file(Workspace *ws, FileNode *f) {
    filenode_clear_outputs(f);
    if (!f->tree) return;
    TSNode root = ts_tree_root_node(f->tree);

    collect_grammar_diags(f, root);
    analyze_requires(f, f->text, root);

    Analyzer a = { .ws = ws, .f = f, .text = f->text,
                   .locals = NULL, .nlocals = 0, .cap_locals = 0 };
    analyze_body(&a, root, 0);   // top level: every form is a reference context
    free(a.locals);

    if (f->nspans > 1)
        qsort(f->spans, f->nspans, sizeof(SemanticSpan), span_cmp);
}

// ===========================================================================
// Emacs module glue.
// ===========================================================================

static emacs_value Qnil(emacs_env *env) { return env->intern(env, "nil"); }

// Copy a Lisp string arg into a fresh malloc'd buffer; *len gets the byte
// length excluding the NUL.  Returns NULL on a non-local exit.
static char *copy_lisp_string(emacs_env *env, emacs_value v, size_t *len) {
    ptrdiff_t size = 0;
    env->copy_string_contents(env, v, NULL, &size);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) return NULL;
    char *buf = malloc((size_t)size);
    env->copy_string_contents(env, v, buf, &size);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) { free(buf); return NULL; }
    if (len) *len = (size_t)size - 1;
    return buf;
}

// Collect a Lisp list/vector of strings into a malloc'd char* array.
static char **copy_string_seq(emacs_env *env, emacs_value seq, size_t *out_n) {
    emacs_value len_v = env->funcall(env, env->intern(env, "length"), 1, &seq);
    intmax_t n = env->extract_integer(env, len_v);
    if (n <= 0) { *out_n = 0; return NULL; }
    char **arr = calloc((size_t)n, sizeof(char *));
    emacs_value eltf = env->intern(env, "elt");
    size_t got = 0;
    for (intmax_t i = 0; i < n; i++) {
        emacs_value idx = env->make_integer(env, i);
        emacs_value args[] = { seq, idx };
        emacs_value s = env->funcall(env, eltf, 2, args);
        size_t l;
        char *c = copy_lisp_string(env, s, &l);
        if (c) arr[got++] = c;
    }
    *out_n = got;
    return arr;
}

// (treejure-init PROJECT CLASSPATH ROOTS) -> workspace user-ptr.
static emacs_value f_init(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = calloc(1, sizeof(Workspace));
    ws->parser = ts_parser_new();
    ts_parser_set_language(ws->parser, tree_sitter_treejure());

    size_t l;
    ws->project_dir = copy_lisp_string(env, args[0], &l);
    if (nargs > 1) ws->classpath = copy_string_seq(env, args[1], &ws->n_classpath);
    if (nargs > 2) ws->roots = copy_string_seq(env, args[2], &ws->n_roots);

    return env->make_user_ptr(env, finalizer_workspace, ws);
}

// Build a diagnostics plist list from F's diagnostics (encoding "A").
static emacs_value diagnostics_to_lisp(emacs_env *env, FileNode *f) {
    emacs_value listf = env->intern(env, "list");
    emacs_value k_beg = env->intern(env, ":beg");
    emacs_value k_end = env->intern(env, ":end");
    emacs_value k_sev = env->intern(env, ":sev");
    emacs_value k_id  = env->intern(env, ":id");
    emacs_value k_msg = env->intern(env, ":msg");

    if (f->ndiags == 0) return Qnil(env);
    emacs_value *items = malloc(f->ndiags * sizeof(emacs_value));
    for (size_t i = 0; i < f->ndiags; i++) {
        Diagnostic *d = &f->diags[i];
        emacs_value pl[] = {
            k_beg, env->make_integer(env, d->start),
            k_end, env->make_integer(env, d->end),
            k_sev, env->intern(env, SEVERITY_NAMES[d->severity]),
            k_id,  env->intern(env, DIAG_IDS[d->id]),
            k_msg, env->make_string(env, d->message ? d->message : "",
                                    d->message ? (ptrdiff_t)strlen(d->message) : 0)
        };
        items[i] = env->funcall(env, listf, 10, pl);
    }
    emacs_value res = env->funcall(env, listf, (ptrdiff_t)f->ndiags, items);
    free(items);
    return res;
}

// (treejure-check-buffer WS FILE LIVE-TEXT CROSS-FILE-P) -> diagnostics.
// Incremental reparse + grammar diagnostics + buffer-local scope pass.
// CROSS-FILE-P is accepted but not yet acted on (no cross-file tier yet).
static emacs_value f_check_buffer(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    size_t tlen; char *text = copy_lisp_string(env, args[2], &tlen);
    if (!text) { free(path); return Qnil(env); }

    FileNode *f = ws_intern_file(ws, path);
    free(path);
    filenode_reparse(ws, f, text, tlen);   // takes ownership of `text`
    analyze_file(ws, f);
    return diagnostics_to_lisp(env, f);
}

// (treejure-semantic-faces WS FILE BEG END) -> flat [s e cat ...] (encoding B).
static emacs_value f_semantic_faces(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    FileNode *f = ws_find_file(ws, path);
    free(path);
    if (!f) return Qnil(env);

    intmax_t beg = env->extract_integer(env, args[2]);
    intmax_t end = env->extract_integer(env, args[3]);

    // Count overlapping spans first (spans are sorted by start).
    size_t count = 0;
    for (size_t i = 0; i < f->nspans; i++) {
        SemanticSpan *s = &f->spans[i];
        if ((intmax_t)s->start >= end) break;
        if ((intmax_t)s->end > beg) count++;
    }
    if (count == 0) return env->funcall(env, env->intern(env, "vector"), 0, NULL);

    emacs_value *vals = malloc(count * 3 * sizeof(emacs_value));
    size_t j = 0;
    for (size_t i = 0; i < f->nspans; i++) {
        SemanticSpan *s = &f->spans[i];
        if ((intmax_t)s->start >= end) break;
        if ((intmax_t)s->end > beg) {
            vals[j++] = env->make_integer(env, s->start);
            vals[j++] = env->make_integer(env, s->end);
            vals[j++] = env->make_integer(env, s->category);
        }
    }
    emacs_value res = env->funcall(env, env->intern(env, "vector"), (ptrdiff_t)(count * 3), vals);
    free(vals);
    return res;
}

// (treejure-close-buffer WS FILE) -> nil.  Drop the file's transient state.
static emacs_value f_close_buffer(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    ws_drop_file(ws, path);
    free(path);
    return Qnil(env);
}

// (treejure-set-def-forms WS NAMES) -> nil.  NAMES is a list/vector of macro
// name strings analysed like `defn` (the `replique-clojure-extra-def-forms`).
static emacs_value f_set_def_forms(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    for (size_t i = 0; i < ws->n_def_forms; i++) free(ws->def_forms[i]);
    free(ws->def_forms);
    ws->def_forms = copy_string_seq(env, args[1], &ws->n_def_forms);
    return Qnil(env);
}

// (treejure-category-names) -> [:local :local-unused ...]  (int -> keyword).
static emacs_value f_category_names(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    emacs_value vals[CAT__COUNT];
    for (int i = 0; i < CAT__COUNT; i++) vals[i] = env->intern(env, CATEGORY_NAMES[i]);
    return env->funcall(env, env->intern(env, "vector"), CAT__COUNT, vals);
}

// (treejure-diagnostic-ids) -> [:syntax-error ...]  (int -> keyword).
static emacs_value f_diagnostic_ids(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    emacs_value vals[DIAG__COUNT];
    for (int i = 0; i < DIAG__COUNT; i++) vals[i] = env->intern(env, DIAG_IDS[i]);
    return env->funcall(env, env->intern(env, "vector"), DIAG__COUNT, vals);
}

static void bind_fn(emacs_env *env, const char *name, ptrdiff_t min, ptrdiff_t max,
                    emacs_value (*fn)(emacs_env *, ptrdiff_t, emacs_value[], void *)) {
    emacs_value func = env->make_function(env, min, max, fn, NULL, NULL);
    emacs_value symbol = env->intern(env, name);
    emacs_value args[] = { symbol, func };
    env->funcall(env, env->intern(env, "defalias"), 2, args);
}

int emacs_module_init(struct emacs_runtime *ert) {
    if (ert->size < sizeof(*ert)) return 1;
    emacs_env *env = ert->get_environment(ert);

    bind_fn(env, "treejure-init",            1, 3, f_init);
    bind_fn(env, "treejure-check-buffer",    4, 4, f_check_buffer);
    bind_fn(env, "treejure-semantic-faces",  4, 4, f_semantic_faces);
    bind_fn(env, "treejure-close-buffer",    2, 2, f_close_buffer);
    bind_fn(env, "treejure-set-def-forms",   2, 2, f_set_def_forms);
    bind_fn(env, "treejure-category-names",  0, 0, f_category_names);
    bind_fn(env, "treejure-diagnostic-ids",  0, 0, f_diagnostic_ids);

    return 0;
}
