#include <emacs-module.h>
#include <tree_sitter/api.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#include "jar_reader.h"   // read_jar_entry: the vendored-miniz jar window

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
//     diagnostic (Tier 1, locals only) + an **NsIndex extraction pass**
//     distilling the file's namespace surface (ns name, flattened require/use
//     specs, defined vars) and emitting the buffer-only `duplicate-require',
//     `refer-all', `namespace-name-mismatch' and `redefined-var' diagnostics
//     (Tier 1); the NsIndex seeds the cross-file require graph + resolution next;
//   * `treejure-semantic-faces' (flat [s e cat ...] read of the face map) and
//     `treejure-close-buffer';
//   * **navigation** -- the scope pass records `NavRef' occurrences (local
//     bindings + their usages by `local_id'; same-ns var defs + their in-file
//     usages by name); `treejure-definition' / `treejure-references' answer a
//     point query by resolved identity (shadowing-correct).  `treejure-definition'
//     also resolves **cross-namespace** targets: an aliased/qualified or
//     `:refer'-ed var -> its def in the dependency `resolve_ns' finds over the
//     classpath (see resolve_cross_ns), read lazily at the query.
//
// There is deliberately NO var face.  A resolved var -- same- or cross-namespace
// -- is left to the syntax layer: treesit already colors a qualified symbol's
// namespace and the def-name forms, so the semantic overlay paints only what
// treesit cannot (locals + form heads).  This keeps the whole scope pass
// buffer-only: it reaches no `resolve_ns' call at either tier and so does NO
// dependency I/O; cross-namespace resolution survives only for the jump-to-def
// point query.  The full tier still emits the `unused-namespace' /
// `unused-referred-var' Tier-2 lints (lint_unused_requires) -- buffer-
// determinable from the require pass's alias/refer maps + the scope pass's usage
// marking, so they too read no dependencies (clj-kondo parity).  The full-tier
// flag now selects only that lint *cadence*, not dependency I/O.
//
// The head of every list form is also classified, buffer-only: a special form
// or core macro gets `:special-form', and a *known* non-core macro (a
// user-declared def-form via `treejure-set-def-forms') gets `:macro-invocation'
// -- the hue treesit cannot assign, since the grammar can't tell a macro call
// from a function call.  An unknown head is left unclaimed (it may be an
// ordinary function); broader macro knowledge (`:lint-as', library macros)
// arrives later.  The CORE_FORMS set is kept in sync with the syntax layer.
//
// Cross-namespace resolution reaches **jars** (the jar slice): `resolve_ns'
// probes jar classpath entries via the vendored miniz reader (read_jar_entry,
// in jar_reader.c), so jump-to-def on an aliased/qualified/`:refer'-ed usage of
// a library or `clojure.core' var lands *inside the jar* -- a jar entry is
// parsed and distilled once and cached as an immutable FileNode.
// `treejure-definition' returns a location whose `:file' is the synthetic
// "<jar>!<entry>" path, and Elisp opens it via `treejure-jar-entry' (a thin
// accessor that re-reads the entry's source on demand).
//
// What is still deferred needs exhaustive knowledge of the whole closure, else
// it false-positives: the `:unresolved' face and the dependency-reading Tier-2
// *warning* diagnostics (undefined var, unresolved-namespace, …), and project-
// wide find-usages -- later slices (PLAN build-order step 4+/5).  All positions
// are 0-based byte offsets; Elisp converts to buffer positions on apply.
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
// here.  Buffer-local analysis emits the local-* categories, `:special-form'
// (special forms + core macros) and `:macro-invocation' (known non-core macros
// -- user def-forms).  There is deliberately NO var face: a resolved var (same-
// or cross-namespace) is left to the syntax layer (treesit already colors a
// qualified symbol's namespace and the def-name forms), so the semantic overlay
// paints only what treesit cannot -- locals, form heads, and (later)
// `:unresolved'.  `:unresolved' arrives with a later cross-file slice (it needs
// exhaustive, jar-inclusive knowledge) and is listed now so the contract is
// stable.
enum {
    CAT_LOCAL,             // a resolved local binding occurrence / usage
    CAT_LOCAL_UNUSED,      // a local binding never used in its scope (greyout)
    CAT_SPECIAL_FORM,      // special form / core macro head (buffer-only)
    CAT_MACRO_INVOCATION,  // known non-core macro head (a user def-form)
    CAT_UNRESOLVED,        // symbol resolving to nothing                 (later)
    CAT__COUNT
};
static const char *const CATEGORY_NAMES[CAT__COUNT] = {
    ":local", ":local-unused",
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
    DIAG_REDEFINED_VAR,          // var defined more than once in-file (Tier 1)
    DIAG_UNUSED_NAMESPACE,       // required ns never used in the file (Tier 2)
    DIAG_UNUSED_REFERRED_VAR,    // :refer-ed/:only var never used (Tier 2)
    DIAG__COUNT
};
static const char *const DIAG_IDS[DIAG__COUNT] = {
    ":syntax-error", ":missing-form", ":invalid-number", ":invalid-string",
    ":invalid-character", ":invalid-symbolic-value", ":unused-binding",
    ":duplicate-require", ":refer-all", ":namespace-name-mismatch",
    ":redefined-var", ":unused-namespace", ":unused-referred-var"
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
    int local_id;        // groups a binding with its usages (navigation)
} Local;

// A navigation occurrence: a resolved symbol (a local binding/usage, or a
// same-namespace var def/usage) recorded so jump-to-definition and find-
// references can answer a point query.  Locals group by `local_id' (one id per
// binding, shared by its usages); vars group by `name'.  Cross-namespace
// targets are NOT recorded here -- they resolve through the cross-file index
// (next slice); a point on one yields no NavRef and the query returns nil.
enum { NAV_LOCAL, NAV_VAR };
typedef struct {
    uint32_t start, end;  // the symbol occurrence's byte span (jump target)
    int      kind;        // NAV_LOCAL | NAV_VAR
    int      local_id;    // NAV_LOCAL grouping (-1 for vars)
    char    *name;        // NAV_VAR grouping (malloc'd; NULL for locals)
    int      is_def;      // 1 = binding/def site, 0 = usage
} NavRef;

// One flattened require/use spec: the resolved namespace plus the location of
// its lib symbol (for diagnostics), and whether it refers everything.
typedef struct {
    char    *ns;          // resolved required namespace (malloc'd)
    uint32_t start, end;  // byte span of the lib symbol
    int      refer_all;   // :refer :all, or a bare :use spec
    int      from_use;    // came from a (:use ...) clause (message wording)
    // `unused-namespace' eligibility + tracking (Tier 2, buffer-determinable).
    int      has_alias;   // saw a real `:as' (loads the ns) -> warn if unused
    int      has_refer_vec; // saw `:refer [..]'/`:only [..]' -> warn if unused
    int      used;        // some usage referenced this ns (alias/fq/refer/`::')
} ReqSpec;

// One var defined in the file (the public/private surface).  Cross-file
// resolution (next slice) reads `name` + `private`; navigation reads the spans.
typedef struct {
    char    *name;             // unqualified var name (malloc'd)
    uint32_t name_start, name_end; // the defined symbol's span (jump target)
    uint32_t def_start, def_end;   // the whole (def... ) form's span
    int      private;          // defn- / ^:private / ^{:private true}
} VarDef;

// An alias -> namespace mapping (`[lib :as a]` / `:as-alias`) and a referred
// var -> namespace mapping (`[lib :refer [foo]]`, or `:use ... :only [foo]`).
// Both feed cross-namespace resolution: a qualified `a/foo` resolves `a` to its
// ns, a bare referred `foo` resolves to the ns that referred it.
typedef struct { char *alias; char *ns; } AliasEntry;
typedef struct {
    char    *name; char *ns;
    uint32_t start, end;  // byte span of the referred symbol (diagnostic site)
    int      used;         // referred var actually used in the file
} ReferEntry;

// The distilled namespace surface of a file: its ns name, the flattened
// requires, the aliases/refers it brings into scope, and the vars it defines.
// Recomputed on each check for the active buffer; the seed of PLAN's NsIndex
// (arities arrive later).
typedef struct {
    char    *ns_name;           // the file's namespace (malloc'd), or NULL
    uint32_t ns_start, ns_end;  // the ns-name symbol's span
    ReqSpec *requires; size_t n_requires, cap_requires;
    VarDef  *vars;     size_t n_vars, cap_vars;
    AliasEntry *aliases; size_t n_aliases, cap_aliases;
    ReferEntry *refers;  size_t n_refers,  cap_refers;
} NsIndex;

// ===========================================================================
// Per-file node: the parser/tree/text state + the analysis outputs.
//
// Carries the Parse model (tree + last full text, replaced wholesale each
// cycle so it cannot desync).  `index', `face_map' and `diagnostics' are
// recomputed on each check.  This is the seed of PLAN's FileNode; the remaining
// cross-file fields (mtime, kind) arrive with the workspace-model slice.
// ===========================================================================

typedef struct {
    char     *path;      // owned key (absolute file path)
    TSTree   *tree;      // NULL until first parse
    char     *text;      // last-parsed bytes (NUL-terminated copy), or NULL
    size_t    len;       // byte length of `text` (excluding the NUL)
    uint32_t  version;   // monotonic, bumped per successful reparse
    time_t    indexed_mtime; // mtime of the disk copy last indexed (0 = never)
    int       live;      // text came from a live buffer -> never clobber from disk
    int       opaque;    // dep that could not be read as UTF-8 -> never resolved
    int       is_jar;    // surface came from a jar entry -> immutable, never re-read

    NsIndex   index;     // distilled ns surface (requires + vars), per check
    SemanticSpan *spans; // sorted by start, recomputed each check
    size_t        nspans, cap_spans;
    Diagnostic   *diags; // recomputed each check
    size_t        ndiags, cap_diags;
    NavRef       *navs;  // navigation occurrences (locals + same-ns vars)
    size_t        nnavs, cap_navs;
} FileNode;

// ===========================================================================
// Workspace: one per project, holds N FileNodes + the shared parser.
//
// `files' is a flat array keyed by path (linear scan -- file counts per
// session are small; swap for a hash if it ever matters).  `classpath' is
// stored for the cross-file slice; unused here.
// ===========================================================================

typedef struct {
    char      *project_dir;
    char     **classpath; size_t n_classpath;
    char     **def_forms; size_t n_def_forms; // user macros analysed like `defn`
    TSParser  *parser;    // one parser, language set once (not reentrant)
    FileNode **files;     size_t n_files, cap_files;
} Workspace;

// --- FileNode lifecycle ---------------------------------------------------

// Free the per-element strings and reset counts, keeping the backing arrays
// (like spans/diags) for reuse across checks.
static void nsindex_clear(NsIndex *ix) {
    free(ix->ns_name);
    ix->ns_name = NULL;
    ix->ns_start = ix->ns_end = 0;
    for (size_t i = 0; i < ix->n_requires; i++) free(ix->requires[i].ns);
    ix->n_requires = 0;
    for (size_t i = 0; i < ix->n_vars; i++) free(ix->vars[i].name);
    ix->n_vars = 0;
    for (size_t i = 0; i < ix->n_aliases; i++) {
        free(ix->aliases[i].alias); free(ix->aliases[i].ns);
    }
    ix->n_aliases = 0;
    for (size_t i = 0; i < ix->n_refers; i++) {
        free(ix->refers[i].name); free(ix->refers[i].ns);
    }
    ix->n_refers = 0;
}

static void filenode_clear_outputs(FileNode *f) {
    for (size_t i = 0; i < f->ndiags; i++) free(f->diags[i].message);
    f->ndiags = 0;
    f->nspans = 0;
    for (size_t i = 0; i < f->nnavs; i++) free(f->navs[i].name);
    f->nnavs = 0;
    nsindex_clear(&f->index);
}

static void filenode_free(FileNode *f) {
    if (!f) return;
    filenode_clear_outputs(f);
    if (f->tree) ts_tree_delete(f->tree);
    free(f->index.requires);
    free(f->index.vars);
    free(f->index.aliases);
    free(f->index.refers);
    free(f->spans);
    free(f->diags);
    free(f->navs);
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

// Revert FILE to disk-backed: drop the transient live tree/text + analysis
// outputs and clear the `live' flag, so the next dependency access re-reads it
// from disk (PLAN close-buffer semantics).  The node itself -- its path key and
// any future cached index -- is kept; only the heavy parse state is freed.
static void filenode_revert_to_disk(FileNode *f) {
    filenode_clear_outputs(f);
    if (f->tree) { ts_tree_delete(f->tree); f->tree = NULL; }
    free(f->text);
    f->text = NULL;
    f->len = 0;
    f->live = 0;
    f->opaque = 0;          // re-decided on the next disk read
    f->indexed_mtime = 0;   // force a fresh disk read on next access
}

static void finalizer_workspace(void *ptr) {
    if (!ptr) return;
    Workspace *ws = (Workspace *)ptr;
    for (size_t i = 0; i < ws->n_files; i++) filenode_free(ws->files[i]);
    free(ws->files);
    if (ws->parser) ts_parser_delete(ws->parser);
    for (size_t i = 0; i < ws->n_classpath; i++) free(ws->classpath[i]);
    for (size_t i = 0; i < ws->n_def_forms; i++) free(ws->def_forms[i]);
    free(ws->classpath);
    free(ws->def_forms);
    free(ws->project_dir);
    free(ws);
}

// ===========================================================================
// Parse model -- incremental reparse via prefix/suffix diff.
// ===========================================================================

// Advance row/column (both in bytes) from point P over T[FROM, TO).
static TSPoint advance_point(TSPoint p, const char *t, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        if (t[i] == '\n') { p.row++; p.column = 0; } else { p.column++; }
    }
    return p;
}

// Derive one TSInputEdit from the longest common prefix/suffix of old vs new
// text (two linear scans; prefix and suffix never overlap).  A correct
// prefix/suffix edit always yields a correct tree; multi-spot edits coalesce
// into one wider span (less reuse, never wrong).  The boundary may fall in the
// middle of a multibyte codepoint (two chars can share a lead byte), but that
// is harmless: tree-sitter's edits and this module's row/column are all counted
// in bytes, so a sub-codepoint byte offset is consistent throughout.
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
    // The [0, p) prefix is byte-identical in O and N, so the point at P is the
    // same in both -- compute it once and continue from it into each tail,
    // rather than rescanning the shared prefix three times.
    TSPoint start = advance_point((TSPoint){ 0, 0 }, o, 0, p);
    e.start_point    = start;
    e.old_end_point  = advance_point(start, o, p, lo - s);
    e.new_end_point  = advance_point(start, n, p, ln - s);
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
    int        next_local_id;   // monotonic per file, assigned at each binding
} Analyzer;

// Record a navigation occurrence.  NAME is duplicated for vars (NULL for
// locals).  The whole-symbol span is the jump target / highlight region.
static void push_nav(FileNode *f, uint32_t start, uint32_t end, int kind,
                     int local_id, const char *name, size_t name_len, int is_def) {
    if (f->nnavs == f->cap_navs) {
        f->cap_navs = f->cap_navs ? f->cap_navs * 2 : 64;
        f->navs = realloc(f->navs, f->cap_navs * sizeof(NavRef));
    }
    char *dup = NULL;
    if (name) { dup = malloc(name_len + 1); memcpy(dup, name, name_len); dup[name_len] = '\0'; }
    f->navs[f->nnavs++] = (NavRef){ start, end, kind, local_id, dup, is_def };
}

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

// Format a diagnostic message into a freshly-malloc'd, exactly-sized string
// (the FileNode owns it; freed in filenode_clear_outputs).  vsnprintf sizes the
// allocation, so there is no fixed buffer to overflow on a long ns/var name --
// the canonical way to build every `push_diag' message.  Returns "" on the
// can't-happen encoding error rather than NULL, so callers never null-check.
static char *msg_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return strdup("");
    char *s = malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
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
    int id = a->next_local_id++;
    a->locals[a->nlocals++] = (Local){
        .name = name, .name_len = name_len,
        .start = a0, .end = b0, .used = 0,
        .underscore = (name_len >= 1 && name[0] == '_'),
        .local_id = id
    };
    push_nav(a->f, a0, b0, NAV_LOCAL, id, NULL, 0, 1);   // the binding site
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
            char *msg = msg_printf("unused binding %.*s",
                                   (int)l->name_len, l->name);
            push_diag(a->f, l->start, l->end, SEV_WARNING, DIAG_UNUSED_BINDING, msg);
        }
    }
    a->nlocals = mark;
}

// --- `unused-namespace' / `unused-referred-var' usage tracking -------------
// Buffer-determinable (no dependency I/O): the scope pass marks each require /
// referred var as it sees a usage, and lint_unused_requires reports the rest at
// the full tier.  A namespace is "used" by an alias-qualified symbol, a fully-
// qualified symbol, a `::alias/kw' auto-resolved keyword, or a syntax-quoted
// qualified symbol -- matching clj-kondo (plain quote / discard / single-colon
// `:alias/kw' do NOT count, and a plain or `:as-alias' require is never flagged).

// Mark every require whose resolved namespace equals NS as used.
static void mark_req_ns_used(NsIndex *ix, const char *ns) {
    for (size_t i = 0; i < ix->n_requires; i++)
        if (ix->requires[i].ns && strcmp(ix->requires[i].ns, ns) == 0)
            ix->requires[i].used = 1;
}

// A namespace qualifier Q[0,QLEN) was used (a symbol's `ns/' part, or a `::'
// keyword's namespace): resolve it through the aliases, else treat it as a
// literal fully-qualified ns; either way mark the matching require(s) used.
static void mark_ns_qualifier_used(NsIndex *ix, const char *q, size_t qlen) {
    for (size_t i = 0; i < ix->n_aliases; i++)
        if (strlen(ix->aliases[i].alias) == qlen &&
            memcmp(ix->aliases[i].alias, q, qlen) == 0) {
            mark_req_ns_used(ix, ix->aliases[i].ns);
            return;
        }
    for (size_t i = 0; i < ix->n_requires; i++)   // literal fully-qualified use
        if (ix->requires[i].ns && strlen(ix->requires[i].ns) == qlen &&
            memcmp(ix->requires[i].ns, q, qlen) == 0)
            ix->requires[i].used = 1;
}

// A bare NAME[0,LEN) was used; if it is a `:refer'-ed/`:only' var, mark that
// referred var (and its providing require) used.
static void mark_refer_used(NsIndex *ix, const char *name, size_t len) {
    for (size_t i = 0; i < ix->n_refers; i++)
        if (strlen(ix->refers[i].name) == len &&
            memcmp(ix->refers[i].name, name, len) == 0) {
            ix->refers[i].used = 1;
            mark_req_ns_used(ix, ix->refers[i].ns);
        }
}

// Resolve a bare NAME[0,LEN) at byte span [SS,SE) that is NOT a local: an
// in-file var (record a NAV_VAR usage so jump-to-def / find-references work) or
// a `:refer'-ed var (mark it + its providing require used).  No face -- treesit
// colors vars.  Shared by the normal resolver (resolve_ref) and the syntax-quote
// walk (scan_syntax_quote): a syntax-quoted bare symbol auto-qualifies to a var
// at read time (never to a local), so it counts as a var usage there too.
static void resolve_bare_var(Analyzer *a, uint32_t ss, uint32_t se,
                             const char *name, size_t len) {
    NsIndex *ix = &a->f->index;
    for (size_t v = 0; v < ix->n_vars; v++) {
        if (strlen(ix->vars[v].name) == len &&
            memcmp(ix->vars[v].name, name, len) == 0) {
            push_nav(a->f, ss, se, NAV_VAR, -1, name, len, 0);
            // An in-file def and a `:refer' of the same name collide in the
            // global ns (ambiguous, unlike a lexical local shadow): count the
            // occurrence as using the refer too, so the require is not falsely
            // flagged unused (matching clj-kondo's leniency here).
            mark_refer_used(ix, name, len);
            return;
        }
    }
    // Not an in-file var: a bare name may be a `:refer'-ed var -- count it as
    // using that referred var + its providing require.
    mark_refer_used(ix, name, len);
}

// Resolve a reference symbol, recording its navigation + (for locals) its face:
//   * a bare name matching a local (innermost first)  -> `:local' face + nav
//   * a bare name matching an in-file var             -> nav only (NAV_VAR)
//   * a qualified `alias/name' / `the.ns/name', or a bare `:refer'-ed name,
//     marks its namespace/referred-var used (for `unused-namespace')
//
// Resolved VARS get NO face: same- and cross-namespace var coloring is the
// syntax layer's job (treesit colors a qualified symbol's namespace + the
// def-name forms).  The semantic overlay paints only locals here.  This keeps
// the scope pass entirely buffer-only -- it reaches NO resolve_ns call at either
// tier, so it never does dependency I/O; cross-namespace resolution survives
// only for the explicit jump-to-def point query (resolve_cross_ns).  In-file var
// usages still get a NAV_VAR occurrence so jump-to-def / find-references work.
// `:unresolved' is deliberately NOT painted: marking a symbol unresolved needs
// exhaustive, jar-inclusive knowledge (else every core/library var
// false-positives), which arrives with a later cross-file slice.
static void resolve_ref(Analyzer *a, TSNode sym) {
    TSNode nm = field_name_node(sym);
    if (ts_node_is_null(nm)) return;
    uint32_t ss = ts_node_start_byte(sym), se = ts_node_end_byte(sym);

    if (!sym_has_namespace(sym)) {            // only a bare name can be a local / in-file var
        uint32_t na = ts_node_start_byte(nm), nb = ts_node_end_byte(nm);
        size_t len = nb - na;
        const char *name = a->text + na;
        for (size_t i = a->nlocals; i-- > 0; ) {
            Local *l = &a->locals[i];
            if (l->name_len == len && memcmp(l->name, name, len) == 0) {
                l->used = 1;
                push_span(a->f, ss, se, CAT_LOCAL);
                push_nav(a->f, ss, se, NAV_LOCAL, l->local_id, NULL, 0, 0);
                return;
            }
        }
        // Not a local: an in-file var usage or a `:refer'-ed var (no face).
        resolve_bare_var(a, ss, se, name, len);
    } else {
        // Qualified `q/name': the qualifier `q' (an alias, or a literal fully-
        // qualified ns) is a namespace usage -- mark it for `unused-namespace'.
        TSNode nsf = ts_node_child_by_field_name(sym, "namespace", 9);
        if (!ts_node_is_null(nsf))
            mark_ns_qualifier_used(&a->f->index, a->text + ts_node_start_byte(nsf),
                                   ts_node_end_byte(nsf) - ts_node_start_byte(nsf));
    }
    // No face for a resolved var (treesit colors it) and none for an unresolved
    // one (that needs jar-inclusive knowledge -- a later slice).  Cross-namespace
    // resolution happens only in jump-to-def (resolve_cross_ns), never here, so
    // the scope pass stays buffer-only at both tiers.
}

// Forward declarations for the mutually-recursive walk.
static void analyze_node(Analyzer *a, TSNode node);
static void bind_pattern(Analyzer *a, TSNode pat);
static void scan_syntax_quote(Analyzer *a, TSNode node, int level);
static TSNode reader_conditional_branch(const char *text, const char *path, TSNode node);

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
        // {:keys [a b] :as x :or {..} pat lookup ...}.  Two passes: bind every
        // name first, THEN analyze the `:or' default values -- a default may
        // reference any of the map's bindings (`{a (inc b)}'), and Clojure binds
        // them all simultaneously, so the defaults must see them regardless of
        // pair order within the map.
        uint32_t cnt = ts_node_named_child_count(pat);
        for (uint32_t i = 0; i < cnt; i++) {           // pass 1: bind names
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
                }
                // `:or' handled in pass 2; other directives: ignore
            } else {
                // {pattern lookup-key}: the key is the binding, value is data.
                bind_pattern(a, key);
            }
        }
        for (uint32_t i = 0; i < cnt; i++) {           // pass 2: `:or' defaults
            TSNode pair = ts_node_named_child(pat, i);
            if (!type_is(pair, "pair")) continue;
            TSNode key = unwrap_meta(ts_node_child_by_field_name(pair, "key", 3));
            if (ts_node_is_null(key) || !type_is(key, "keyword") ||
                !kw_name_eq(a->text, key, "or")) continue;
            // `:or {sym default ...}': only the default *values* are expressions;
            // the keys are the binding names being defaulted, not usages, and are
            // now in scope (pass 1) -- so resolve only the values.
            TSNode m = unwrap_meta(ts_node_child_by_field_name(pair, "value", 5));
            if (!ts_node_is_null(m) && type_is(m, "map_literal")) {
                uint32_t mc = ts_node_named_child_count(m);
                for (uint32_t j = 0; j < mc; j++) {
                    TSNode pr = ts_node_named_child(m, j);
                    if (type_is(pr, "pair"))
                        analyze_node(a, ts_node_child_by_field_name(pr, "value", 5));
                }
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

// Bind a sequential `[pat init ...]` vector then scope it over the whole body.
// `binding`/`with-redefs` are deliberately absent: their left-hand sides are
// *existing var* references (a dynamic-var rebind / a redef target), not new
// locals -- binding them would mis-greyout `*dynvar*` and emit a bogus
// `unused-binding`.  Left out, they fall through to the generic walk, which
// correctly reads both sides as references.  `with-local-vars`, whose names
// *are* genuine locals, stays.
static const char *const LET_FORMS[] = {
    "let", "let*", "when-let", "when-some",
    "loop", "with-open", "with-local-vars",
    "dotimes", "when-first", 0
};
// `if-let`/`if-some` scope the binding over the `then` branch ONLY; the `else`
// branch (and the test init) are evaluated outside it -- handled separately.
static const char *const IF_LET_FORMS[] = { "if-let", "if-some", 0 };
static const char *const SEQ_FORMS[] = { "for", "doseq", 0 }; // with modifiers
static const char *const FN_FORMS[]  = { "fn", "fn*", 0 };
static const char *const DEFN_FORMS[] = { "defn", "defn-", "defmacro", 0 };
static const char *const DEF_FORMS[]  = { "def", "defonce", 0 };
// Other def-like forms whose defined name sits at form index 1.  Like `def`,
// the name is interned by extract_var_defs and must NOT be re-resolved here as a
// reference -- otherwise the generic walk records a second (usage) NavRef at the
// def site, double-counting it in find-references.  We skip the name and walk
// the remainder as references; proper binding of deftype/defrecord fields is
// deferred (PLAN), so those bodies are still walked approximately -- but the
// name is never duplicated.  (`defprotocol`/`definterface` are NOT here: their
// bodies are pure declarations, handled opaquely in analyze_binding_form so the
// interned method-var declaration sites are not re-recorded as usages.)
static const char *const OTHER_DEF_FORMS[] = {
    "defmulti", "definline", "deftest", "deftest-",
    "deftype", "defrecord", "defstruct", 0
};

// Special forms + core macros -- the heads that read as `:special-form'.  Kept
// in sync with the syntax layer's `replique-clojure-builtin-symbols-regexp'
// (replique-clojure-mode.el), so the overlay's special-form hue and treesit's
// keyword fallback agree on the same set.  Matched on the bare (unqualified)
// name only, like the binding-form dispatch above.  A NON-core macro the module
// knows is a macro (a user-declared def-form, `is_extra_def_form') reads as
// `:macro-invocation' instead; unknown heads get neither (they may be ordinary
// function calls -- claiming "macro" would false-positive without macro
// knowledge / `:lint-as', which arrives later).
static const char *const CORE_FORMS[] = {
    "do", "if", "let*", "var", "fn", "fn*", "loop*", "recur",
    "throw", "try", "catch", "finally", "set!", "new",
    "monitor-enter", "monitor-exit", "quote", "->", "->>", "..", ".",
    "amap", "and", "areduce", "as->", "assert", "binding", "bound-fn",
    "case", "comment", "cond", "cond->", "cond->>", "condp",
    "declare", "def", "definline", "definterface", "defmacro", "defmethod",
    "defmulti", "defn", "defn-", "defonce", "defprotocol", "defrecord",
    "defstruct", "deftype", "delay", "doall", "dorun", "doseq", "dosync",
    "dotimes", "doto", "extend-protocol", "extend-type", "extend",
    "for", "future", "gen-class", "gen-interface", "if-let", "if-not",
    "if-some", "import", "in-ns", "io!", "lazy-cat", "lazy-seq", "let",
    "letfn", "locking", "loop", "memfn", "ns", "or", "proxy", "proxy-super",
    "pvalues", "refer-clojure", "reify", "some->", "some->>", "sync",
    "time", "vswap!", "when", "when-first", "when-let", "when-not",
    "when-some", "while", "with-bindings", "with-in-str",
    "with-loading-context", "with-local-vars", "with-open",
    "with-out-str", "with-precision", "with-redefs", "with-redefs-fn",
    "deftest", "deftest-", "is", "are", "testing", 0
};

// Returns 1 if NODE (a list) was handled as a binding form.
static int analyze_binding_form(Analyzer *a, TSNode list, TSNode head) {
    const char *text = a->text;

    if (sym_name_eq(text, head, "ns")) {
        // The `(ns ...)' form is distilled by analyze_requires; its require
        // specs are data (lib names, aliases), not value references -- so the
        // scope pass treats the whole form as opaque (matching scan_var_defs).
        return 1;
    }
    if (sym_name_eq(text, head, "defprotocol") ||
        sym_name_eq(text, head, "definterface")) {
        // Pure declarations: a name, an optional docstring, method signatures
        // (+ `defprotocol' options) -- no evaluated code to resolve.  Opaque
        // here so the method-NAME declaration sites are not re-recorded as
        // usages (record_var_def interns each method as a var with its def-site
        // nav); usages of those methods elsewhere still resolve normally.
        return 1;
    }
    if (name_in(text, head, IF_LET_FORMS)) {
        // (if-let [x test] then else?): x scopes over `then` only.  Bind, walk
        // `then` in scope, pop, then walk `else...` (if any) outside the scope.
        size_t mark = a->nlocals;
        analyze_bindings_vec(a, unwrap_meta(nth_form(list, 1)), 0);
        analyze_node(a, nth_form(list, 2));   // then -- in scope
        pop_scope(a, mark);
        analyze_body(a, list, 3);             // else... -- out of scope
        return 1;
    }
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
            // A named fn need not call itself (the name is often there only for
            // stack traces), so never report it unused -- mark it used up front.
            a->locals[a->nlocals - 1].used = 1;
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
        // (defmethod multifn dispatch-val [params] body...).  `defmethod' does
        // not define the multifn -- it extends an existing one -- so the multifn
        // name at index 1 is a *reference* (like as->'s expr): resolve it for its
        // face/nav and so its alias/`:refer'/fqn counts as a namespace usage.
        TSNode multifn = nth_form(list, 1);
        if (!ts_node_is_null(multifn)) analyze_node(a, multifn);
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
    if (name_in(text, head, DEF_FORMS) || name_in(text, head, OTHER_DEF_FORMS)) {
        // (def name docstring? init?), (deftype Name ...), (defmulti name ...):
        // skip the defined name at index 1 (interned by extract_var_defs, not a
        // reference) and walk the remainder as references.
        analyze_body(a, list, 2);
        return 1;
    }
    return 0;
}

// Within a syntax-quoted template only the `unquote' (~) and `unquote_splicing'
// (~@) targets are evaluated code -- the rest is data (templated,
// namespace-qualified symbols).  Nested `syntax_quote's raise the quoting
// level; an unquote cancels one level and only "escapes" to live code when the
// level returns to 0.  Walk the template tracking that level and hand the
// escaping targets to the normal resolver, so a local used only under a
// backtick is still seen as used.
static void scan_syntax_quote(Analyzer *a, TSNode node, int level) {
    if (ts_node_is_null(node)) return;
    const char *t = ts_node_type(node);
    if (strcmp(t, "syntax_quote") == 0) {
        scan_syntax_quote(a, ts_node_child_by_field_name(node, "target", 6), level + 1);
        return;
    }
    if (strcmp(t, "unquote") == 0 || strcmp(t, "unquote_splicing") == 0) {
        TSNode target = ts_node_child_by_field_name(node, "target", 6);
        if (level <= 1) analyze_node(a, target);          // escapes to live code
        else scan_syntax_quote(a, target, level - 1);     // still templated
        return;
    }
    if (strcmp(t, "symbol") == 0) {
        // A syntax-quoted symbol namespace-resolves at read time (clj-kondo
        // agrees), even though the symbol itself is templated data:
        //   * qualified `q/name'  -> its qualifier counts as a namespace usage;
        //   * bare `name'         -> auto-qualifies to a var (an in-file var or a
        //     `:refer'-ed one), so it counts as that var's usage -- recording a
        //     NAV_VAR and keeping the require from a false `unused' flag.  It
        //     never auto-qualifies to a local, so locals are not consulted here.
        if (sym_has_namespace(node)) {
            TSNode nsf = ts_node_child_by_field_name(node, "namespace", 9);
            if (!ts_node_is_null(nsf))
                mark_ns_qualifier_used(&a->f->index,
                                       a->text + ts_node_start_byte(nsf),
                                       ts_node_end_byte(nsf) - ts_node_start_byte(nsf));
        } else {
            TSNode nm = field_name_node(node);
            if (!ts_node_is_null(nm))
                resolve_bare_var(a, ts_node_start_byte(node), ts_node_end_byte(node),
                                 a->text + ts_node_start_byte(nm),
                                 ts_node_end_byte(nm) - ts_node_start_byte(nm));
        }
        return;
    }
    // An inner plain quote / var-quote / discard / eval-literal stays data.
    if (strcmp(t, "quote") == 0 || strcmp(t, "var_quote") == 0 ||
        strcmp(t, "discard") == 0 || strcmp(t, "eval_literal") == 0)
        return;
    // Otherwise keep descending -- a deeper unquote may live inside.
    uint32_t cnt = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < cnt; i++)
        scan_syntax_quote(a, ts_node_named_child(node, i), level);
}

// Generic recursive walk.
static void analyze_node(Analyzer *a, TSNode node) {
    if (ts_node_is_null(node)) return;
    const char *t = ts_node_type(node);

    if (strcmp(t, "symbol") == 0) { resolve_ref(a, node); return; }
    if (strcmp(t, "keyword") == 0) {
        // `::alias/kw' (auto-resolved -- marker is the 2-byte `::') uses the
        // alias' namespace; a single-colon `:ns/kw' is a literal keyword and
        // does NOT count as a namespace usage (matching clj-kondo).
        TSNode mk = ts_node_child_by_field_name(node, "marker", 6);
        if (!ts_node_is_null(mk) &&
            ts_node_end_byte(mk) - ts_node_start_byte(mk) == 2) {
            TSNode nsf = ts_node_child_by_field_name(node, "namespace", 9);
            if (!ts_node_is_null(nsf))
                mark_ns_qualifier_used(&a->f->index,
                                       a->text + ts_node_start_byte(nsf),
                                       ts_node_end_byte(nsf) - ts_node_start_byte(nsf));
        }
        return;
    }
    if (strcmp(t, "with_metadata") == 0) {
        analyze_node(a, ts_node_child_by_field_name(node, "target", 6));
        return;
    }
    // A tagged literal `#tag form': only the tagged `target' form is code.  The
    // `tag' is a data-reader name, not a var reference -- walking it would, for a
    // qualified tag `#the.ns/x', wrongly count `the.ns' as a namespace usage.
    if (strcmp(t, "tagged_literal") == 0) {
        analyze_node(a, ts_node_child_by_field_name(node, "target", 6));
        return;
    }
    // A syntax-quoted template is data, EXCEPT the ~ / ~@ forms inside it, whose
    // targets are live code -- analyze just those (see scan_syntax_quote).
    if (strcmp(t, "syntax_quote") == 0) {
        scan_syntax_quote(a, ts_node_child_by_field_name(node, "target", 6), 1);
        return;
    }
    // Plain quoted / discarded / eval-literal subtrees are pure data.
    if (strcmp(t, "quote") == 0 ||
        strcmp(t, "var_quote") == 0 || strcmp(t, "discard") == 0 ||
        strcmp(t, "eval_literal") == 0)
        return;

    // Reader conditional: honor only this file's dialect branch (or :default),
    // matching var extraction -- so resolution never sees another platform's
    // code (a `:cljs'-only reference must not resolve against a `.clj' file).
    if (strcmp(t, "reader_conditional") == 0) {
        analyze_node(a, reader_conditional_branch(a->text, a->f->path, node));
        return;
    }

    if (strcmp(t, "list_literal") == 0) {
        TSNode head = unwrap_meta(nth_form(node, 0));
        int bare_head = !ts_node_is_null(head) && type_is(head, "symbol") &&
                        !sym_has_namespace(head);
        if (bare_head) {
            // Paint the head's semantic face before dispatching.  A known
            // non-core macro (a user-declared def-form) reads as
            // `:macro-invocation'; a special form / core macro reads as
            // `:special-form'.  Painting here -- and skipping the head when we
            // walk the body below -- classifies it once, so a core form's head
            // is never re-examined as a plain reference.
            int core = 0;
            if (is_extra_def_form(a->ws, a->text, head))
                push_span(a->f, ts_node_start_byte(head), ts_node_end_byte(head),
                          CAT_MACRO_INVOCATION);
            else if ((core = name_in(a->text, head, CORE_FORMS)))
                push_span(a->f, ts_node_start_byte(head), ts_node_end_byte(head),
                          CAT_SPECIAL_FORM);
            if (analyze_binding_form(a, node, head))
                return;
            if (core) {                 // non-binding core form (if/when/cond/...)
                analyze_body(a, node, 1);   // head already painted -- skip it
                return;
            }
        }
        analyze_body(a, node, 0);   // generic: every child is a reference
        return;
    }

    // Other collections / reader macros: walk their form children.
    if (strcmp(t, "vector_literal") == 0 || strcmp(t, "map_literal") == 0 ||
        strcmp(t, "set_literal") == 0 || strcmp(t, "namespaced_map_literal") == 0 ||
        strcmp(t, "fn_literal") == 0 || strcmp(t, "pair") == 0 ||
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
        push_diag(f, ts_node_start_byte(node), ts_node_end_byte(node),
                  SEV_ERROR, DIAG_MISSING_FORM, msg_printf("missing %s", tp));
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
// ns-form, require & var-definition extraction (buffer-only, Tier 1).
//
// A dedicated walk distils the file's namespace surface into its `NsIndex`:
//   * the top-level `(ns ...)` form's name + flattened require/use specs
//     (vector specs `[lib :as a :refer [..] :refer :all]`, prefix lists
//     `(parent (child :as c) child2)`, plain-symbol specs, `:use` ⇒ implicit
//     refer-all unless narrowed by `:only`/`:refer`);
//   * the vars it defines (top-level def-like forms), with the private bit.
// From that surface come the buffer-only linters that need no dependency I/O:
// `duplicate-require`, `refer-all`, `namespace-name-mismatch`, `redefined-var`.
// The cross-file slice (PLAN step 4) reuses the same `NsIndex` to build the
// require graph and resolve cross-namespace targets (jump-to-def; the
// dependency-reading lints and the `:unresolved` face later).
// ===========================================================================

static char *node_text_dup(const char *text, TSNode n) {
    uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
    size_t len = b - a;
    char *s = malloc(len + 1);
    memcpy(s, text + a, len);
    s[len] = '\0';
    return s;
}

// Whole-node bytes equal a C string.
static int node_text_eq(const char *text, TSNode n, const char *s) {
    uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
    size_t len = b - a, sl = strlen(s);
    return len == sl && memcmp(text + a, s, len) == 0;
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

static void push_req(NsIndex *ix, char *ns, uint32_t s, uint32_t e,
                     int refer_all, int from_use) {
    if (ix->n_requires == ix->cap_requires) {
        ix->cap_requires = ix->cap_requires ? ix->cap_requires * 2 : 8;
        ix->requires = realloc(ix->requires, ix->cap_requires * sizeof(ReqSpec));
    }
    // has_alias / has_refer_vec / used default 0 here; parse_lib_spec sets the
    // eligibility flags on the vector branch, and the scope pass sets `used'.
    ix->requires[ix->n_requires++] =
        (ReqSpec){ ns, s, e, refer_all, from_use, 0, 0, 0 };
}

static void push_var(NsIndex *ix, char *name, uint32_t ns, uint32_t ne,
                     uint32_t ds, uint32_t de, int private) {
    if (ix->n_vars == ix->cap_vars) {
        ix->cap_vars = ix->cap_vars ? ix->cap_vars * 2 : 16;
        ix->vars = realloc(ix->vars, ix->cap_vars * sizeof(VarDef));
    }
    ix->vars[ix->n_vars++] = (VarDef){ name, ns, ne, ds, de, private };
}

// Record an alias / referred-var mapping (both args are duplicated).
static void push_alias(NsIndex *ix, const char *text, TSNode sym, const char *ns) {
    if (ix->n_aliases == ix->cap_aliases) {
        ix->cap_aliases = ix->cap_aliases ? ix->cap_aliases * 2 : 8;
        ix->aliases = realloc(ix->aliases, ix->cap_aliases * sizeof(AliasEntry));
    }
    ix->aliases[ix->n_aliases++] =
        (AliasEntry){ node_text_dup(text, sym), strdup(ns) };
}

static void push_refer(NsIndex *ix, const char *text, TSNode sym, const char *ns) {
    if (ix->n_refers == ix->cap_refers) {
        ix->cap_refers = ix->cap_refers ? ix->cap_refers * 2 : 16;
        ix->refers = realloc(ix->refers, ix->cap_refers * sizeof(ReferEntry));
    }
    ix->refers[ix->n_refers++] =
        (ReferEntry){ node_text_dup(text, sym), strdup(ns),
                      ts_node_start_byte(sym), ts_node_end_byte(sym), 0 };
}

// Parse one lib spec (symbol | vector | prefix-list) into IX.  PREFIX is the
// enclosing prefix-list parent (or NULL); FROM_USE marks a `:use` clause, whose
// specs refer everything unless narrowed by `:only`/`:refer`.
static void parse_lib_spec(NsIndex *ix, const char *text, TSNode spec,
                           const char *prefix, int from_use) {
    spec = unwrap_meta(spec);
    if (ts_node_is_null(spec)) return;

    if (type_is(spec, "symbol")) {
        char *ns = with_prefix(prefix, node_text_dup(text, spec));
        push_req(ix, ns, ts_node_start_byte(spec), ts_node_end_byte(spec),
                 from_use, from_use);
        return;
    }
    if (type_is(spec, "vector_literal")) {
        TSNode lib = unwrap_meta(nth_form(spec, 0));
        if (ts_node_is_null(lib) || !type_is(lib, "symbol")) return;
        char *ns = with_prefix(prefix, node_text_dup(text, lib));
        int refer_all = from_use;
        // `:as' (a real, ns-loading alias) and a `:refer [..]'/`:only [..]'
        // vector each make the require eligible for `unused-namespace'.
        // `:as-alias' (keyword-only) and a plain spec do NOT -- matching
        // clj-kondo (an `:as-alias' / plain require is never flagged unused).
        int has_alias = 0, has_refer_vec = 0;
        uint32_t k = 1; TSNode opt;
        while (!ts_node_is_null(opt = nth_form(spec, k))) {
            TSNode ou = unwrap_meta(opt);
            if (type_is(ou, "keyword")) {
                if (kw_name_eq(text, ou, "as") || kw_name_eq(text, ou, "as-alias")) {
                    TSNode al = unwrap_meta(nth_form(spec, k + 1));
                    if (!ts_node_is_null(al) && type_is(al, "symbol"))
                        push_alias(ix, text, al, ns);
                    if (kw_name_eq(text, ou, "as")) has_alias = 1;
                    k += 2; continue;
                }
                if (kw_name_eq(text, ou, "refer") || kw_name_eq(text, ou, "only")) {
                    TSNode v = unwrap_meta(nth_form(spec, k + 1));
                    if (kw_name_eq(text, ou, "refer") && !ts_node_is_null(v) &&
                        type_is(v, "keyword") && kw_name_eq(text, v, "all")) {
                        refer_all = 1;
                    } else if (!ts_node_is_null(v) && type_is(v, "vector_literal")) {
                        refer_all = 0;
                        has_refer_vec = 1;
                        uint32_t r = 0; TSNode rs;     // referred var names
                        while (!ts_node_is_null(rs = nth_form(v, r))) {
                            TSNode ru = unwrap_meta(rs);
                            if (type_is(ru, "symbol")) push_refer(ix, text, ru, ns);
                            r++;
                        }
                    }
                    k += 2; continue;
                }
                k += 2; continue;   // :rename / other directive + value
            }
            k++;
        }
        push_req(ix, ns, ts_node_start_byte(lib), ts_node_end_byte(lib),
                 refer_all, from_use);
        ix->requires[ix->n_requires - 1].has_alias = has_alias;
        ix->requires[ix->n_requires - 1].has_refer_vec = has_refer_vec;
        return;
    }
    if (type_is(spec, "list_literal")) {
        // prefix list: (parent (child ...) child2 ...)
        TSNode par = unwrap_meta(nth_form(spec, 0));
        if (ts_node_is_null(par) || !type_is(par, "symbol")) return;
        char *newpref = with_prefix(prefix, node_text_dup(text, par));
        uint32_t k = 1; TSNode child;
        while (!ts_node_is_null(child = nth_form(spec, k))) {
            parse_lib_spec(ix, text, child, newpref, from_use);
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

// Process one `:require`/`:use' spec, descending through a `reader_conditional'
// (`#?' / `#?@') to the branch live for this file's dialect before flattening it
// with parse_lib_spec -- so a require nested in a reader conditional is extracted
// (and linted) like a plain one.  Honors exactly one branch (the file's platform
// feature, or `:default'), the same single-branch rule the scope pass and var
// extraction use; seeing a `:cljs'-only require from a `.clj'-dialect read is the
// deferred per-dialect cljc slice (PLAN step 4).  A non-conditional spec goes
// straight to parse_lib_spec.
static void parse_require_spec(NsIndex *ix, const char *text, const char *path,
                               TSNode spec, int from_use) {
    TSNode u = unwrap_meta(spec);
    if (ts_node_is_null(u)) return;
    if (type_is(u, "reader_conditional")) {
        TSNode branch = reader_conditional_branch(text, path, u);
        if (ts_node_is_null(branch)) return;
        TSNode marker = ts_node_child_by_field_name(u, "marker", 6);
        if (!ts_node_is_null(marker) && type_is(marker, "marker_splicing")) {
            // `#?@': the chosen branch is a vector whose ELEMENTS are each specs.
            TSNode bv = unwrap_meta(branch);
            if (type_is(bv, "vector_literal")) {
                TSNode el;
                for (uint32_t i = 0; !ts_node_is_null(el = nth_form(bv, i)); i++)
                    parse_require_spec(ix, text, path, el, from_use);
            }
            return;
        }
        parse_require_spec(ix, text, path, branch, from_use);   // `#?': one spec
        return;
    }
    parse_lib_spec(ix, text, u, NULL, from_use);
}

// Walk the `(ns ...)` form NS_LIST into F's NsIndex: record the ns name, check
// the ns/path match, flatten its require/use specs, and emit the buffer-only
// require diagnostics.
static void analyze_ns_form(FileNode *f, const char *text, TSNode ns_list) {
    NsIndex *ix = &f->index;
    TSNode name = unwrap_meta(nth_form(ns_list, 1));
    if (!ts_node_is_null(name) && type_is(name, "symbol")) {
        ix->ns_name  = node_text_dup(text, name);
        ix->ns_start = ts_node_start_byte(name);
        ix->ns_end   = ts_node_end_byte(name);
        if (has_clj_ext(f->path) && !ns_path_matches(ix->ns_name, f->path))
            push_diag(f, ix->ns_start, ix->ns_end,
                      SEV_WARNING, DIAG_NAMESPACE_NAME_MISMATCH,
                      strdup("namespace name does not match file path"));
    }

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
                            parse_require_spec(ix, text, f->path, spec, is_use);
                        j++;
                    }
                }
            }
        }
        k++;
    }

    for (size_t i = 0; i < ix->n_requires; i++) {
        ReqSpec *s = &ix->requires[i];
        if (s->refer_all)
            push_diag(f, s->start, s->end, SEV_WARNING, DIAG_REFER_ALL,
                      strdup(s->from_use ? "prefer :require with :refer over :use"
                                         : "avoid :refer :all"));
        for (size_t p = 0; p < i; p++) {
            if (s->ns && ix->requires[p].ns &&
                strcmp(s->ns, ix->requires[p].ns) == 0) {
                push_diag(f, s->start, s->end, SEV_WARNING, DIAG_DUPLICATE_REQUIRE,
                          msg_printf("duplicate require of %s", s->ns));
                break;
            }
        }
    }
}

// Top-level forms that define a var/type (a name at form index 1).  `declare`
// (forward decl) and `defmethod` (extends a multifn, defines nothing) are
// excluded so a later `def`/method does not read as a redefinition.
static const char *const VAR_DEF_FORMS[] = {
    "def", "defn", "defn-", "defmacro", "defmulti", "defonce", "definline",
    "deftest", "deftest-", "defprotocol", "deftype", "defrecord",
    "definterface", "defstruct", 0
};

// Forms whose method/instance bodies run on dispatch or method call -- NOT at
// load time -- so, like `fn`, they are a function boundary for the var-def pass:
// an inline `(def ...)` inside one is not the load-time surface and must not be
// recorded (else it false-positives `redefined-var`).  `defmethod`'s body is
// also scoped by the scope pass (analyze_fn_tail); the others are walked
// generically there (proper field/arglist binding is deferred -- see PLAN), but
// either way their bodies are never a load-time def site.
static const char *const METHOD_BODY_FORMS[] = {
    "defmethod", "reify", "proxy", "extend-type", "extend-protocol", 0
};

// Does NAME_W (possibly metadata-wrapped) carry `^:private` / `^{:private
// true}`?  Walks the `with_metadata' wrappers above the defined symbol.
static int name_has_private_meta(const char *text, TSNode name_w) {
    for (TSNode n = name_w; !ts_node_is_null(n) && type_is(n, "with_metadata");
         n = ts_node_child_by_field_name(n, "target", 6)) {
        TSNode meta = ts_node_child_by_field_name(n, "meta", 4);
        if (ts_node_is_null(meta)) continue;
        TSNode val = ts_node_child_by_field_name(meta, "value", 5);
        if (ts_node_is_null(val)) continue;
        if (type_is(val, "keyword") && kw_name_eq(text, val, "private")) return 1;
        if (type_is(val, "map_literal")) {
            uint32_t cnt = ts_node_named_child_count(val);
            for (uint32_t i = 0; i < cnt; i++) {
                TSNode pr = ts_node_named_child(val, i);
                if (!type_is(pr, "pair")) continue;
                TSNode key = unwrap_meta(ts_node_child_by_field_name(pr, "key", 3));
                TSNode pv  = unwrap_meta(ts_node_child_by_field_name(pr, "value", 5));
                if (!ts_node_is_null(key) && type_is(key, "keyword") &&
                    kw_name_eq(text, key, "private") &&
                    !ts_node_is_null(pv) && type_is(pv, "boolean") &&
                    node_text_eq(text, pv, "true"))
                    return 1;
            }
        }
    }
    return 0;
}

// Reader-macro wrappers whose contents are data / non-evaluated, not code.
static int is_opaque_wrapper(TSNode n) {
    const char *t = ts_node_type(n);
    return strcmp(t, "quote") == 0 || strcmp(t, "syntax_quote") == 0 ||
           strcmp(t, "var_quote") == 0 || strcmp(t, "discard") == 0 ||
           strcmp(t, "eval_literal") == 0;
}

// Intern one var named NAME[0,NAME_LEN) into F's NsIndex, with NAME_SPAN the
// defined symbol's byte span (the jump target) and DEF_SPAN the whole defining
// form.  NAME is copied (for the var) and, when RECORD_NAVS, copied again into a
// def-site NAV_VAR occurrence (find-references / jump-to-def; keyed by name, so a
// redefinition adds a second def nav).  The one place a var enters the index.
static void intern_var(FileNode *f, const char *name, size_t name_len,
                       uint32_t name_start, uint32_t name_end,
                       uint32_t def_start, uint32_t def_end,
                       int private, int record_navs) {
    char *dup = malloc(name_len + 1);
    memcpy(dup, name, name_len); dup[name_len] = '\0';
    push_var(&f->index, dup, name_start, name_end, def_start, def_end, private);
    if (record_navs)
        push_nav(f, name_start, name_end, NAV_VAR, -1, name, name_len, 1);
}

// Intern a synthesized factory var PREFIX+TNAME (e.g. `->Foo`, `map->Foo` for
// deftype/defrecord).  It has no source symbol of its own, so its name span is
// the type name's span (NAME_START/END) -- jump-to-def lands on the type form.
static void intern_factory(FileNode *f, const char *prefix,
                           const char *tname, size_t tlen,
                           uint32_t name_start, uint32_t name_end,
                           uint32_t def_start, uint32_t def_end, int record_navs) {
    size_t pl = strlen(prefix);
    char *built = malloc(pl + tlen + 1);
    memcpy(built, prefix, pl);
    memcpy(built + pl, tname, tlen);
    built[pl + tlen] = '\0';
    intern_var(f, built, pl + tlen, name_start, name_end,
               def_start, def_end, 0, record_navs);
    free(built);
}

// Each method signature `(method-name [args] "doc"?)` in a defprotocol /
// definterface body declares a var named for the method.  Walk the form's list
// children (the docstring + `defprotocol' option keyword/value pairs are not
// lists, so they are skipped) and intern each method name.  Clojure makes these
// public; mirrors clj-kondo's `:analysis' (which records protocol/interface
// method vars).
static void record_method_vars(FileNode *f, const char *text, TSNode u,
                               int record_navs) {
    uint32_t k = 2; TSNode c;
    while (!ts_node_is_null(c = nth_form(u, k))) {
        TSNode cu = unwrap_meta(c);
        if (type_is(cu, "list_literal")) {
            TSNode mname = unwrap_meta(nth_form(cu, 0));
            if (!ts_node_is_null(mname) && type_is(mname, "symbol")) {
                TSNode mnm = field_name_node(mname);
                if (!ts_node_is_null(mnm))
                    intern_var(f, text + ts_node_start_byte(mnm),
                               ts_node_end_byte(mnm) - ts_node_start_byte(mnm),
                               ts_node_start_byte(mname), ts_node_end_byte(mname),
                               ts_node_start_byte(cu), ts_node_end_byte(cu),
                               0, record_navs);
            }
        }
        k++;
    }
}

// Record the var(s) defined by def-like list U (already unwrapped; HEAD verified
// to be a def-like unqualified symbol) into F's NsIndex.  RECORD_NAVS adds the
// def-site navigation occurrence -- wanted for the live buffer (find-references
// / jump-to-def), skipped when indexing a dependency (its navs are never
// queried; only its var surface is).  Most forms define one var (the name at
// index 1); the few that synthesize more get them too, for clj-kondo `:analysis'
// parity: `defprotocol`/`definterface` -> one var per method name;
// `deftype` -> the positional factory `->Name`; `defrecord` -> `->Name` and the
// map factory `map->Name`.
static void record_var_def(FileNode *f, const char *text, TSNode u, TSNode head,
                           int record_navs) {
    TSNode name_w = nth_form(u, 1);
    if (ts_node_is_null(name_w)) return;
    int private = sym_name_eq(text, head, "defn-") ||
                  name_has_private_meta(text, name_w);
    TSNode name = unwrap_meta(name_w);
    if (ts_node_is_null(name) || !type_is(name, "symbol")) return;
    TSNode nm = field_name_node(name);
    if (ts_node_is_null(nm)) return;
    uint32_t ns = ts_node_start_byte(name), ne = ts_node_end_byte(name);
    uint32_t na = ts_node_start_byte(nm),   nb = ts_node_end_byte(nm);
    uint32_t ds = ts_node_start_byte(u),    de = ts_node_end_byte(u);
    // The primary var: the protocol/interface/type/var name itself.
    intern_var(f, text + na, nb - na, ns, ne, ds, de, private, record_navs);

    // Secondary vars some forms synthesize (see the function comment).
    if (sym_name_eq(text, head, "defprotocol") ||
        sym_name_eq(text, head, "definterface")) {
        record_method_vars(f, text, u, record_navs);
    } else if (sym_name_eq(text, head, "deftype") ||
               sym_name_eq(text, head, "defrecord")) {
        intern_factory(f, "->", text + na, nb - na, ns, ne, ds, de, record_navs);
        if (sym_name_eq(text, head, "defrecord"))
            intern_factory(f, "map->", text + na, nb - na, ns, ne, ds, de,
                           record_navs);
    }
}

// The reader-conditional feature keyword honored for PATH's dialect.  `.cljc`
// (and unknown) use `clj` as the primary platform; a `:default` branch is the
// fallback when the platform branch is absent.
static const char *dialect_feature(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "clj";
    if (strcmp(dot, ".cljs") == 0) return "cljs";
    if (strcmp(dot, ".cljd") == 0) return "cljd";
    if (strcmp(dot, ".cljr") == 0) return "cljr";
    return "clj";   // .clj / .cljc / .bb / unknown -> clj
}

// The reader-conditional branch live for PATH's dialect: the platform feature
// branch if present, else the `:default' branch, else a null node.  NODE is a
// `reader_conditional'; its `body' is the `(:feat form :feat form ...)' list.
// Shared by the scope pass (analyze_node) and var extraction (scan_var_defs) so
// both honor exactly one branch -- the single dialect-selection seam.
static TSNode reader_conditional_branch(const char *text, const char *path, TSNode node) {
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    if (!ts_node_is_null(body) && type_is(body, "list_literal")) {
        const char *feat = dialect_feature(path);
        TSNode fallback = body; int have_fallback = 0;
        for (uint32_t k = 0; ; k += 2) {
            TSNode kw  = unwrap_meta(nth_form(body, k));
            TSNode val = nth_form(body, k + 1);
            if (ts_node_is_null(kw) || ts_node_is_null(val)) break;
            if (!type_is(kw, "keyword")) continue;
            if (kw_name_eq(text, kw, feat)) return val;             // platform wins
            if (kw_name_eq(text, kw, "default")) { fallback = val; have_fallback = 1; }
        }
        if (have_fallback) return fallback;
    }
    return ts_node_named_child(node, ts_node_named_child_count(node));  // null
}

// Recursively record vars defined at load time, starting from a top-level form.
// We descend through everything (do/let/when/if/cond/reader-conditional, call
// args, data literals) because a `def` reachable that way IS interned when the
// enclosing top-level form is evaluated.  We stop -- treating the body as
// OPAQUE -- only where evaluation does not reach at load time or the body is not
// the public surface: a def-like form (its var is recorded, but its body is an
// inline-def handled by a separate linter), a `(fn ...)`/`(fn* ...)` form or
// `#(...)` literal (runs on call, not on load), a deferred-body form whose
// method/instance bodies run on dispatch or method call rather than at load
// (`defmethod`/`reify`/`proxy`/`extend-type`/`extend-protocol` -- see
// METHOD_BODY_FORMS), an `(ns ...)` form, a `(comment ...)` form, and
// quote/discard/eval reader wrappers.  A user
// `extra-def-form` (analysed like `defn`) is treated identically to a def-like
// form: its name is interned and its body is a function boundary -- so the scope
// pass and this pass agree on what such a form is.
static void scan_var_defs(Workspace *ws, FileNode *f, const char *text,
                          TSNode node, int record_navs) {
    if (ts_node_is_null(node)) return;
    node = unwrap_meta(node);
    if (ts_node_is_null(node)) return;

    if (is_opaque_wrapper(node)) return;         // '... `... #'... #_... #=...
    if (type_is(node, "fn_literal")) return;     // #(...) -- function boundary

    if (type_is(node, "reader_conditional")) {   // #?(...) / #?@(...)
        // Honor only the branch for this file's dialect (or :default), so a var
        // defined once per platform is not seen as a redefinition.
        scan_var_defs(ws, f, text, reader_conditional_branch(text, f->path, node),
                      record_navs);
        return;
    }

    if (type_is(node, "list_literal")) {
        TSNode head = unwrap_meta(nth_form(node, 0));
        if (!ts_node_is_null(head) && type_is(head, "symbol") &&
            !sym_has_namespace(head)) {
            if (name_in(text, head, VAR_DEF_FORMS) ||
                is_extra_def_form(ws, text, head)) {   // record, then stop
                record_var_def(f, text, node, head, record_navs);
                return;
            }
            if (name_in(text, head, FN_FORMS) ||       // (fn ...) / (fn* ...)
                name_in(text, head, METHOD_BODY_FORMS) || // defmethod/reify/...
                sym_name_eq(text, head, "ns") ||       // (ns ...)
                sym_name_eq(text, head, "comment"))     // (comment ...)
                return;                                 // opaque body
        }
        // Otherwise fall through: do/let/when/call/... descend into the body.
    }

    TSNode c;
    for (uint32_t k = 0; !ts_node_is_null(c = nth_form(node, k)); k++)
        scan_var_defs(ws, f, text, c, record_navs);
}

// Record every var defined at load time into F's NsIndex.  Top-level def-like
// forms, plus defs reachable through load-time control flow (top-level `do`,
// reader conditionals, `when`/`let`/... wrapping a def) are recorded; defs
// behind a function/ns/comment/quote boundary are not (see scan_var_defs).
// RECORD_NAVS adds def-site nav occurrences -- t for the live buffer, nil when
// indexing a dependency (whose navs are never queried).
static void extract_var_defs(Workspace *ws, FileNode *f, const char *text,
                             TSNode root, int record_navs) {
    scan_var_defs(ws, f, text, root, record_navs);
}

// `redefined-var`: a var name defined by more than one top-level form (warn on
// each definition after the first).
static void lint_redefined_var(FileNode *f) {
    NsIndex *ix = &f->index;
    for (size_t i = 0; i < ix->n_vars; i++) {
        for (size_t p = 0; p < i; p++) {
            if (strcmp(ix->vars[i].name, ix->vars[p].name) == 0) {
                push_diag(f, ix->vars[i].name_start, ix->vars[i].name_end,
                          SEV_WARNING, DIAG_REDEFINED_VAR,
                          msg_printf("redefined var %s", ix->vars[i].name));
                break;
            }
        }
    }
}

// `unused-namespace' + `unused-referred-var' (Tier 2, buffer-determinable): a
// required ns whose alias / `:refer'-ed vars / fully-qualified name is never
// used in the file, and each `:refer'-ed / `:only' var never used.  The scope
// pass set the `used' flags as it walked the buffer; this reports the rest.
// A plain require or an `:as-alias'-only require is never flagged unused
// (matching clj-kondo) -- only an `:as'-aliased or `:refer'/`:only' require is
// eligible.  Reads no dependencies: this is buffer-only, just emitted at the
// full tier (its PLAN Tier-2 cadence) so it does not flash during after-edit.
static void lint_unused_requires(FileNode *f) {
    NsIndex *ix = &f->index;
    for (size_t i = 0; i < ix->n_requires; i++) {
        ReqSpec *r = &ix->requires[i];
        if (r->used || r->refer_all) continue;
        if (!r->has_alias && !r->has_refer_vec) continue;   // plain / :as-alias
        push_diag(f, r->start, r->end, SEV_WARNING, DIAG_UNUSED_NAMESPACE,
                  msg_printf("namespace %s is required but never used",
                             r->ns ? r->ns : "?"));
    }
    for (size_t i = 0; i < ix->n_refers; i++) {
        ReferEntry *r = &ix->refers[i];
        if (r->used) continue;
        push_diag(f, r->start, r->end, SEV_WARNING, DIAG_UNUSED_REFERRED_VAR,
                  msg_printf("referred var %s/%s is never used",
                             r->ns ? r->ns : "?", r->name));
    }
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

// ===========================================================================
// Cross-file workspace foundation (build-order step 4).
//
// Files are resolved LAZILY, on demand: a namespace is munged to its relative
// file path and only the candidate paths (one per search dir x extension) are
// `stat'-ed -- no directory walk.  A resolved file is read once and cached as a
// FileNode (stat-gated on mtime); the `ns -> FileNode' map is `ws->files'
// filtered by ns_name.
//
// The search scope (where to look) is NOT stored on the workspace -- it is
// passed per call.  A consumer chooses it at invocation time (the classpath,
// the already-known workspace files, or a user-picked directory) and hands the
// directory list to `resolve_ns'; nothing about that choice is persisted.
//
// `resolve_ns' searches both kinds of classpath entry: a source **directory**
// (stat the candidate paths) and a **jar** (probe the candidate entries inside
// it via the vendored miniz reader -- read_jar_entry).  A jar entry is parsed
// and distilled once and cached as an immutable FileNode (jars do not change
// within a session), keyed by a synthetic "<jar>!<entry>" path.
//
// Consumer: cross-namespace `treejure-definition' (see resolve_cross_ns)
// resolves an aliased/qualified or `:refer'-ed var to its defining file via
// `resolve_ns' over the classpath -- now reaching library/`clojure.core' vars in
// jars, not just project sources.  Jump-to-def *into* a jar entry works: the
// returned location's `:file' is the synthetic "<jar>!<entry>" path, which Elisp
// opens via `treejure-jar-entry'.  (There is no var face -- treesit colors vars;
// resolution here serves navigation only.)  The dependency-reading cross-file
// lints/faces and find-usages build on the same primitives next.
// ===========================================================================

// The set of platforms a file participates in, as a bitmask.  `.cljc' counts as
// both clj and cljs; two files clash only if their platform sets overlap (so a
// `.clj'/`.cljs' pair sharing a name is fine, but `.clj'/`.cljc' is not).
enum { DIA_CLJ = 1, DIA_CLJS = 2, DIA_CLJR = 4, DIA_CLJD = 8 };
static unsigned dialect_mask(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return DIA_CLJ;
    if (strcmp(dot, ".clj") == 0 || strcmp(dot, ".bb") == 0) return DIA_CLJ;
    if (strcmp(dot, ".cljs") == 0) return DIA_CLJS;
    if (strcmp(dot, ".cljc") == 0) return DIA_CLJ | DIA_CLJS;
    if (strcmp(dot, ".cljr") == 0) return DIA_CLJR;
    if (strcmp(dot, ".cljd") == 0) return DIA_CLJD;
    return DIA_CLJ;
}

// Set F's NsIndex ns name from its first top-level `(ns ...)' form, if any.
// Lightweight seed for indexed (non-buffer) files: no requires/vars/diagnostics
// (those are recomputed by analyze_file when the file becomes the live buffer).
static void extract_ns_name(FileNode *f) {
    if (!f->tree) return;
    TSNode root = ts_tree_root_node(f->tree);
    TSNode c;
    for (uint32_t k = 0; !ts_node_is_null(c = nth_form(root, k)); k++) {
        TSNode u = unwrap_meta(c);
        if (!type_is(u, "list_literal")) continue;
        TSNode head = unwrap_meta(nth_form(u, 0));
        if (ts_node_is_null(head) || !type_is(head, "symbol") ||
            sym_has_namespace(head) || !sym_name_eq(f->text, head, "ns"))
            continue;
        TSNode name = unwrap_meta(nth_form(u, 1));
        if (!ts_node_is_null(name) && type_is(name, "symbol")) {
            f->index.ns_name  = node_text_dup(f->text, name);
            f->index.ns_start = ts_node_start_byte(name);
            f->index.ns_end   = ts_node_end_byte(name);
        }
        return;   // only the first ns form governs the file
    }
}

// Read PATH wholesale into a fresh NUL-terminated buffer (*len excludes NUL).
static char *read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    char *buf = NULL;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        if (sz >= 0 && fseek(fp, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sz, fp);
                buf[n] = '\0';
                *len = n;
            }
        }
    }
    fclose(fp);
    return buf;
}

// Strict RFC-3629 UTF-8 validation of BUF[0..LEN).  The module reads disk deps
// as UTF-8 (the Clojure source norm; what clj-kondo does); a dep that fails is
// marked opaque rather than risk an invalid-UTF-8 byte run reaching `make_string'
// once cross-file slices return dep-derived names to Emacs.  Rejects overlong
// encodings, surrogates (U+D800..DFFF) and code points above U+10FFFF.
static int is_valid_utf8(const char *buf, size_t len) {
    const unsigned char *s = (const unsigned char *)buf;
    for (size_t i = 0; i < len; ) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }            // ASCII
        size_t n; unsigned char lo = 0x80, hi = 0xBF; // continuation count/range
        if ((c & 0xE0) == 0xC0) { if (c < 0xC2) return 0; n = 1; }       // 2-byte
        else if ((c & 0xF0) == 0xE0) { n = 2;                            // 3-byte
            if (c == 0xE0) lo = 0xA0;               // exclude overlong
            else if (c == 0xED) hi = 0x9F; }        // exclude surrogates
        else if ((c & 0xF8) == 0xF0) { if (c > 0xF4) return 0; n = 3;    // 4-byte
            if (c == 0xF0) lo = 0x90;               // exclude overlong
            else if (c == 0xF4) hi = 0x8F; }        // exclude > U+10FFFF
        else return 0;                              // stray lead/continuation
        if (i + n >= len) return 0;                 // truncated multibyte
        if (s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (size_t j = 2; j <= n; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += n + 1;
    }
    return 1;
}

// Index (or refresh) the disk file at PATH into the workspace, stat-gated on
// MTIME so an unchanged file is parsed at most once.  A live-buffer FileNode is
// never read from disk -- its in-memory text and ns must win.
// Returns the (possibly cached) FileNode for PATH, or NULL if it could not be
// read and was not already interned -- so the caller can match its ns_name
// without a second `ws_find_file' scan.
static FileNode *index_disk_file(Workspace *ws, const char *path, time_t mtime) {
    FileNode *f = ws_find_file(ws, path);
    if (f && f->live) return f;                               // live buffer wins
    // Indexed and unchanged?  The cached NsIndex stands -- gated on mtime alone,
    // not on tree retention (the tree is dropped below).  `indexed_mtime' is 0
    // for a never-indexed / reverted node, and a real disk mtime is never 0.
    if (f && f->indexed_mtime != 0 && f->indexed_mtime == mtime) return f;
    size_t len; char *buf = read_file(path, &len);
    if (!buf) return f;
    if (!f) f = ws_intern_file(ws, path);
    if (!is_valid_utf8(buf, len)) {       // non-UTF-8 dep -> opaque, never resolved
        free(buf);
        filenode_clear_outputs(f);        // drop any stale index/ns/navs
        if (f->tree) { ts_tree_delete(f->tree); f->tree = NULL; }
        free(f->text); f->text = NULL; f->len = 0;
        f->opaque = 1;
        f->indexed_mtime = mtime;         // mtime-gate; re-checked only on change
        return f;
    }
    f->opaque = 0;
    filenode_reparse(ws, f, buf, len);   // takes ownership of `buf`
    filenode_clear_outputs(f);
    // Distil the dependency's surface: ns name + defined vars (what cross-ns
    // resolution looks up).  No requires/aliases/refers/diagnostics -- a dep is
    // queried for the vars it *defines*, not what it brings into scope; those are
    // recomputed by analyze_file if it ever becomes the live buffer.
    extract_ns_name(f);
    extract_var_defs(ws, f, f->text, ts_tree_root_node(f->tree), 0); // dep: no navs
    f->indexed_mtime = mtime;
    // Drop the heavy parse state.  A dependency is queried only for its distilled
    // NsIndex -- ns name + var defs, which carry self-contained byte offsets +
    // the path -- never re-walked, so its TSTree/text would otherwise pin
    // unbounded memory for the session.  Re-read lazily when the mtime changes.
    ts_tree_delete(f->tree); f->tree = NULL;
    free(f->text); f->text = NULL; f->len = 0;
    return f;
}

// Index ENTRY (e.g. "clojure/string.clj") of the jar at JAR_PATH into a FileNode
// keyed by KEY (the synthetic "<jar>!<entry>" path).  Jars are immutable per
// session (PLAN fact #5): a once-indexed entry is cached forever and never
// re-read -- so a cached KEY short-circuits before any jar I/O, and there is no
// mtime gate.  Returns the (possibly cached) FileNode, or NULL when the entry is
// absent in the jar (nothing is interned -- the resolver moves on / re-probes).
// Like a disk dep, only the distilled surface (ns name + var defs) is kept; the
// tree/text are dropped after extraction.
static FileNode *index_jar_entry(Workspace *ws, const char *jar_path,
                                 const char *entry, const char *key) {
    FileNode *f = ws_find_file(ws, key);
    if (f) return f;                      // immutable: cached entry stands
    size_t len; char *buf = read_jar_entry(jar_path, entry, &len);
    if (!buf) return NULL;                // entry not in this jar
    f = ws_intern_file(ws, key);
    f->is_jar = 1;
    if (!is_valid_utf8(buf, len)) {       // non-UTF-8 entry -> opaque, never resolved
        free(buf);
        f->opaque = 1;
        return f;
    }
    filenode_reparse(ws, f, buf, len);    // takes ownership of `buf`
    filenode_clear_outputs(f);
    extract_ns_name(f);
    extract_var_defs(ws, f, f->text, ts_tree_root_node(f->tree), 0); // dep: no navs
    ts_tree_delete(f->tree); f->tree = NULL;   // drop heavy state (surface kept)
    free(f->text); f->text = NULL; f->len = 0;
    return f;
}

// Source-file extensions, longest-lived first; `bb' last (rare, clj-equivalent).
static const char *const CLJ_EXTS[] = { ".clj", ".cljs", ".cljc", ".cljd", ".bb", 0 };

// Munge a namespace ("foo.bar-baz") to its relative file path ("foo/bar_baz").
static void ns_to_relpath(const char *ns, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; ns[i] && j + 1 < cap; i++) {
        char c = ns[i];
        out[j++] = (c == '.') ? '/' : (c == '-' ? '_' : c);
    }
    out[j] = '\0';
}

// Is CP a jar classpath entry (a ".jar" path) rather than a source directory?
static int is_jar_path(const char *cp) {
    size_t n = strlen(cp);
    return n >= 4 && memcmp(cp + n - 4, ".jar", 4) == 0;
}

// Probe REL (the ns-munged relative path) inside the jar at JAR for each source
// extension; return the FileNode for the entry that declares NS, or NULL.  The
// entry path is the jar-internal name ("clojure/string.clj"); the FileNode is
// keyed by the synthetic "<jar>!<entry>" so jar surfaces never collide with
// disk paths and are cached immutably (index_jar_entry).
static FileNode *resolve_ns_in_jar(Workspace *ws, const char *jar, const char *rel,
                                   const char *ns, unsigned mask) {
    for (size_t e = 0; CLJ_EXTS[e]; e++) {
        char entry[2200], key[8192];
        int n1 = snprintf(entry, sizeof entry, "%s%s", rel, CLJ_EXTS[e]);
        if (n1 < 0 || (size_t)n1 >= sizeof entry) continue;
        if ((dialect_mask(entry) & mask) == 0) continue;   // disjoint platforms
        int n2 = snprintf(key, sizeof key, "%s!%s", jar, entry);
        if (n2 < 0 || (size_t)n2 >= sizeof key) continue;
        FileNode *o = index_jar_entry(ws, jar, entry, key);
        if (o && !o->opaque && o->index.ns_name &&
            strcmp(o->index.ns_name, ns) == 0)
            return o;   // exists in the jar and declares NS
    }
    return NULL;
}

// Lazily resolve NS to its source FileNode by searching DIRS (N_DIRS entries --
// e.g. classpath directories and jars, or a user-picked dir, supplied per call
// and NOT stored).  Munges NS and probes only the candidate names (entry x
// extension) on a platform overlapping MASK -- no walk: a directory entry is
// `stat'-ed, a jar entry is looked up in the jar's central directory -- reads/
// caches each match and confirms it actually declares NS.  SKIP_PATH (e.g. the
// live buffer) is never returned.  NULL if unresolved.  The seam later
// cross-file consumers call.
static FileNode *resolve_ns(Workspace *ws, const char *const *dirs, size_t n_dirs,
                            const char *ns, unsigned mask, const char *skip_path) {
    if (!ns || n_dirs == 0) return NULL;
    char rel[2048];
    ns_to_relpath(ns, rel, sizeof rel);
    for (size_t r = 0; r < n_dirs; r++) {
        if (is_jar_path(dirs[r])) {        // a jar entry -- probe inside it
            FileNode *o = resolve_ns_in_jar(ws, dirs[r], rel, ns, mask);
            if (o) return o;
            continue;
        }
        for (size_t e = 0; CLJ_EXTS[e]; e++) {
            char path[4096];
            int n = snprintf(path, sizeof path, "%s/%s%s",
                             dirs[r], rel, CLJ_EXTS[e]);
            if (n < 0 || (size_t)n >= sizeof path) continue;
            if (skip_path && strcmp(path, skip_path) == 0) continue;
            if ((dialect_mask(path) & mask) == 0) continue;     // disjoint platforms
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            FileNode *o = index_disk_file(ws, path, st.st_mtime);
            if (o && !o->opaque && o->index.ns_name &&
                strcmp(o->index.ns_name, ns) == 0)
                return o;   // exists and declares NS
        }
    }
    return NULL;
}

// Run the buffer-local analysis on F's current tree.  Every fact computed here
// is buffer-only and reads NO dependencies -- at either tier.  CROSS_FILE no
// longer gates dependency I/O (the scope pass does none); it now selects only
// the *cadence* of the buffer-determinable Tier-2 lints (`unused-namespace' /
// `unused-referred-var'), emitted at the full tier so they do not flash on every
// after-edit check.  Cross-file dependency resolution lives entirely in the
// jump-to-def point query (resolve_cross_ns); the dependency-reading Tier-2
// diagnostics that will reintroduce check-time dep I/O land in a later slice.
static void analyze_file(Workspace *ws, FileNode *f, int cross_file) {
    filenode_clear_outputs(f);
    if (!f->tree) return;
    TSNode root = ts_tree_root_node(f->tree);

    // --- Buffer-only facts (both tiers; no dependency I/O) -----------------
    collect_grammar_diags(f, root);
    analyze_requires(f, f->text, root);
    extract_var_defs(ws, f, f->text, root, 1);   // live buffer: record def navs
    lint_redefined_var(f);

    Analyzer a = { .ws = ws, .f = f, .text = f->text,
                   .locals = NULL, .nlocals = 0, .cap_locals = 0,
                   .next_local_id = 0 };
    analyze_body(&a, root, 0);   // top level: every form is a reference context
    free(a.locals);

    // --- Full-tier-only facts (still buffer-only; cadence, not I/O) --------
    if (cross_file) {
        // `unused-namespace' / `unused-referred-var': buffer-determinable -- the
        // scope pass already flagged each used require / referred var above, so
        // this reads no dependencies.  Emitted only here (not the fast tier) so
        // it tracks the PLAN's Tier-2 cadence and does not flash on edit.
        lint_unused_requires(f);
        // Remaining Tier-2 work attaches here: the `:unresolved' face and the
        // dependency lints that DO need disk reads (unresolved-namespace,
        // undefined var, ...).  Those need exhaustive, jar-inclusive knowledge
        // to avoid false positives, so they land with a later slice (PLAN step 4).
    }

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

// Collect a Lisp list/vector of strings into a malloc'd char* array.  Bails to
// an empty result on a non-local exit (e.g. SEQ is not a sequence) rather than
// proceeding under a pending signal.
static char **copy_string_seq(emacs_env *env, emacs_value seq, size_t *out_n) {
    *out_n = 0;
    emacs_value len_v = env->funcall(env, env->intern(env, "length"), 1, &seq);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) return NULL;
    intmax_t n = env->extract_integer(env, len_v);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return || n <= 0)
        return NULL;
    char **arr = calloc((size_t)n, sizeof(char *));
    emacs_value eltf = env->intern(env, "elt");
    size_t got = 0;
    for (intmax_t i = 0; i < n; i++) {
        emacs_value idx = env->make_integer(env, i);
        emacs_value args[] = { seq, idx };
        emacs_value s = env->funcall(env, eltf, 2, args);
        if (env->non_local_exit_check(env) != emacs_funcall_exit_return) break;
        size_t l;
        char *c = copy_lisp_string(env, s, &l);   // NULL (+ pending exit) if not a string
        if (!c) break;
        arr[got++] = c;
    }
    *out_n = got;
    return arr;
}

// (treejure-init PROJECT &optional CLASSPATH) -> workspace user-ptr.
static emacs_value f_init(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    // Copy the (required) project dir first: a bad arg signals, and we bail
    // before allocating anything so nothing leaks.
    size_t l;
    char *project_dir = copy_lisp_string(env, args[0], &l);
    if (!project_dir) return Qnil(env);

    Workspace *ws = calloc(1, sizeof(Workspace));
    ws->parser = ts_parser_new();
    ts_parser_set_language(ws->parser, tree_sitter_treejure());
    ws->project_dir = project_dir;

    if (nargs > 1) {
        ws->classpath = copy_string_seq(env, args[1], &ws->n_classpath);
        // A malformed classpath signals; free the half-built workspace rather
        // than hand Emacs a user-ptr under a pending exit (which leaks it).
        if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
            finalizer_workspace(ws);
            return Qnil(env);
        }
    }

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
// Incremental reparse + grammar diagnostics + buffer-local scope pass.  The
// CROSS-FILE-P flag is routed to analyze_file; both tiers are buffer-only (no
// dependency I/O), and the flag now selects only the cadence of the full tier's
// `unused-namespace' / `unused-referred-var' Tier-2 lints (lint_unused_requires)
// so they do not flash on edit.  The dependency-reading Tier-2 lints/faces and
// the `:unresolved' face are still later slices (PLAN step 4), and will be what
// reintroduces check-time dependency I/O.
static emacs_value f_check_buffer(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    size_t tlen; char *text = copy_lisp_string(env, args[2], &tlen);
    if (!text) { free(path); return Qnil(env); }

    int cross_file = env->is_not_nil(env, args[3]);
    FileNode *f = ws_intern_file(ws, path);
    free(path);
    f->live = 1;                            // text is authoritative over disk
    f->opaque = 0;                          // live text from Emacs is valid UTF-8
    filenode_reparse(ws, f, text, tlen);    // takes ownership of `text`
    analyze_file(ws, f, cross_file);
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

// The navigation occurrence covering BYTE (symbol spans never overlap).
static NavRef *nav_at(FileNode *f, uint32_t byte) {
    for (size_t i = 0; i < f->nnavs; i++)
        if (f->navs[i].start <= byte && byte < f->navs[i].end) return &f->navs[i];
    return NULL;
}

// (:file PATH :beg B :end E) location plist (encoding "A").
static emacs_value location_to_lisp(emacs_env *env, FileNode *f,
                                    uint32_t beg, uint32_t end) {
    emacs_value pl[] = {
        env->intern(env, ":file"),
        env->make_string(env, f->path, (ptrdiff_t)strlen(f->path)),
        env->intern(env, ":beg"), env->make_integer(env, beg),
        env->intern(env, ":end"), env->make_integer(env, end)
    };
    return env->funcall(env, env->intern(env, "list"), 6, pl);
}

// The innermost `symbol' node covering BYTE, or a null node.
static TSNode symbol_at_byte(FileNode *f, uint32_t byte) {
    TSNode root = ts_tree_root_node(f->tree);
    TSNode n = ts_node_descendant_for_byte_range(root, byte, byte);
    while (!ts_node_is_null(n) && !type_is(n, "symbol")) n = ts_node_parent(n);
    return n;
}

// Resolve the cross-namespace var named by SYM -- a qualified `alias/name' (or
// fully-qualified `the.ns/name') or a bare `:refer'-ed `name' -- against F's
// aliases/refers and WS's classpath (source directories and jars alike).  Reads
// the dependency lazily (resolve_ns), exactly at the query that needs it.
// Returns the defining FileNode and, via OUT (when non-NULL), its VarDef; NULL
// when SYM does not resolve to a real var.  *OUT points into the dep's var
// array, valid until the next indexing call -- use it at once.  The sole caller
// is `treejure-definition' (jump-to-def): a single point query, so there is no
// per-pass memo -- each call resolves at most one namespace.
static FileNode *resolve_cross_ns_var(Workspace *ws, FileNode *f, TSNode sym,
                                      const VarDef **out) {
    TSNode nmf = field_name_node(sym);
    if (ts_node_is_null(nmf)) return NULL;
    uint32_t na = ts_node_start_byte(nmf), nb = ts_node_end_byte(nmf);
    size_t nlen = nb - na;
    const char *nm = f->text + na;
    NsIndex *ix = &f->index;

    const char *target_ns = NULL;
    char *lit = NULL;                  // owned literal-ns buffer (freed below)
    TSNode nsf = ts_node_child_by_field_name(sym, "namespace", 9);
    if (!ts_node_is_null(nsf)) {                       // qualified: alias or fqn
        uint32_t aa = ts_node_start_byte(nsf), ab = ts_node_end_byte(nsf);
        size_t alen = ab - aa;
        const char *al = f->text + aa;
        for (size_t i = 0; i < ix->n_aliases && !target_ns; i++)
            if (strlen(ix->aliases[i].alias) == alen &&
                memcmp(ix->aliases[i].alias, al, alen) == 0)
                target_ns = ix->aliases[i].ns;
        if (!target_ns) {                              // not an alias -> literal ns
            lit = malloc(alen + 1);
            memcpy(lit, al, alen);
            lit[alen] = '\0';
            target_ns = lit;
        }
    } else {                                           // bare: a referred var
        for (size_t i = 0; i < ix->n_refers && !target_ns; i++)
            if (strlen(ix->refers[i].name) == nlen &&
                memcmp(ix->refers[i].name, nm, nlen) == 0)
                target_ns = ix->refers[i].ns;
        if (!target_ns) return NULL;                   // same-ns/core/unresolved
    }

    // A fully-qualified reference to the file's OWN namespace (`my.ns/foo`
    // inside `my.ns`) resolves to an in-file var.  resolve_ns skips the file
    // itself (skip_path), so it would never resolve -- handle it here against
    // the file's own var surface instead.
    if (ix->ns_name && strcmp(target_ns, ix->ns_name) == 0) {
        free(lit);
        for (size_t v = 0; v < ix->n_vars; v++)
            if (strlen(ix->vars[v].name) == nlen &&
                memcmp(ix->vars[v].name, nm, nlen) == 0) {
                if (out) *out = &ix->vars[v];
                return f;
            }
        return NULL;
    }

    FileNode *dep = resolve_ns(ws, (const char *const *)ws->classpath,
                               ws->n_classpath, target_ns,
                               dialect_mask(f->path), f->path);
    free(lit);                         // resolve_ns copied what it needed
    if (!dep) return NULL;
    for (size_t v = 0; v < dep->index.n_vars; v++)
        if (strlen(dep->index.vars[v].name) == nlen &&
            memcmp(dep->index.vars[v].name, nm, nlen) == 0) {
            if (out) *out = &dep->index.vars[v];
            return dep;
        }
    return NULL;
}

// `treejure-definition's cross-namespace case: wrap resolve_cross_ns_var to a
// def-site location plist in the defining file, or nil.
static emacs_value resolve_cross_ns(emacs_env *env, Workspace *ws,
                                    FileNode *f, TSNode sym) {
    const VarDef *vd = NULL;
    FileNode *dep = resolve_cross_ns_var(ws, f, sym, &vd);
    if (!dep || !vd) return Qnil(env);
    return location_to_lisp(env, dep, vd->name_start, vd->name_end);
}

// (treejure-definition WS FILE BYTE) -> (:file f :beg b :end e) or nil.
// In-file first (cached analysis): a local usage -> its binding; a same-ns var
// usage -> its definition.  Otherwise cross-namespace: an aliased/qualified or
// `:refer'-ed var -> its def in the resolved dependency (lazy, via the
// classpath).  A core/unresolved target yields nil; a jar-backed target yields a
// location whose `:file' is the synthetic "<jar>!<entry>" path, which Elisp
// opens via `treejure-jar-entry'.
static emacs_value f_definition(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    FileNode *f = ws_find_file(ws, path);
    free(path);
    if (!f || !f->tree) return Qnil(env);

    uint32_t byte = (uint32_t)env->extract_integer(env, args[2]);
    NavRef *r = nav_at(f, byte);
    if (r && r->kind == NAV_LOCAL) {
        for (size_t i = 0; i < f->nnavs; i++) {
            NavRef *n = &f->navs[i];
            if (n->kind == NAV_LOCAL && n->is_def && n->local_id == r->local_id)
                return location_to_lisp(env, f, n->start, n->end);
        }
    } else if (r && r->name) {
        for (size_t v = 0; v < f->index.n_vars; v++)   // first def with this name
            if (strcmp(f->index.vars[v].name, r->name) == 0)
                return location_to_lisp(env, f, f->index.vars[v].name_start,
                                        f->index.vars[v].name_end);
    }
    // Not an in-file local/var: try cross-namespace resolution.
    TSNode sym = symbol_at_byte(f, byte);
    if (ts_node_is_null(sym)) return Qnil(env);
    return resolve_cross_ns(env, ws, f, sym);
}

// (treejure-references WS FILE BYTE &optional SCOPE-DIRS) -> list of locations.
// Every occurrence (definition + usages) of the local or same-namespace var at
// BYTE: locals are buffer-scoped by binding id; vars are matched by name.
// SCOPE-DIRS (the per-call cross-file search scope) is accepted for forward
// compatibility but not yet consulted -- var references are currently buffer-
// scoped; the cross-file scan lands in a later slice.
static emacs_value f_references(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    FileNode *f = ws_find_file(ws, path);
    free(path);
    if (!f) return Qnil(env);

    uint32_t byte = (uint32_t)env->extract_integer(env, args[2]);
    NavRef *r = nav_at(f, byte);
    if (!r) return Qnil(env);   // nav_at non-NULL implies f->nnavs > 0

    emacs_value *items = malloc(f->nnavs * sizeof(emacs_value));
    size_t k = 0;
    for (size_t i = 0; i < f->nnavs; i++) {
        NavRef *n = &f->navs[i];
        int match = (r->kind == NAV_LOCAL)
            ? (n->kind == NAV_LOCAL && n->local_id == r->local_id)
            : (n->kind == NAV_VAR && n->name && r->name &&
               strcmp(n->name, r->name) == 0);
        if (match) items[k++] = location_to_lisp(env, f, n->start, n->end);
    }
    emacs_value res = (k == 0) ? Qnil(env)
        : env->funcall(env, env->intern(env, "list"), (ptrdiff_t)k, items);
    free(items);
    return res;
}

// (treejure-close-buffer WS FILE) -> nil.  Revert the file to disk-backed,
// dropping its transient live tree/text (the node is kept for the session).
static emacs_value f_close_buffer(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    FileNode *f = ws_find_file(ws, path);
    free(path);
    if (f) filenode_revert_to_disk(f);
    return Qnil(env);
}

// (treejure-jar-entry JAR ENTRY) -> string | nil.  Read ENTRY's bytes from the
// jar at JAR via the vendored miniz reader so Elisp can OPEN a jar-backed
// jump-to-def target.  The entry's text is dropped from the cached FileNode
// after its surface is distilled, so it is re-read here on demand -- jars are
// immutable per session, so there is no staleness.  Returns the UTF-8 source as
// a Lisp string, or nil when the entry is absent or not valid UTF-8
// (`make_string' requires valid UTF-8).  Workspace-independent: a pure jar read,
// needing no FileNode -- the synthetic "<jar>!<entry>" path carries everything.
static emacs_value f_jar_entry(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    size_t jlen; char *jar = copy_lisp_string(env, args[0], &jlen);
    if (!jar) return Qnil(env);
    size_t elen; char *entry = copy_lisp_string(env, args[1], &elen);
    if (!entry) { free(jar); return Qnil(env); }
    size_t len; char *buf = read_jar_entry(jar, entry, &len);
    free(jar); free(entry);
    if (!buf) return Qnil(env);
    emacs_value res = is_valid_utf8(buf, len)
        ? env->make_string(env, buf, (ptrdiff_t)len)
        : Qnil(env);
    free(buf);
    return res;
}

// (treejure-set-def-forms WS NAMES) -> nil.  NAMES is a list/vector of macro
// name strings analysed like `defn` (the `replique-clojure-extra-def-forms`).
static emacs_value f_set_def_forms(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    // Build the new list first; only swap it in once it is known good.  A
    // malformed NAMES leaves a pending exit -- free the partial result and keep
    // the existing def-forms rather than installing a half-built list.
    size_t n; char **forms = copy_string_seq(env, args[1], &n);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
        for (size_t i = 0; i < n; i++) free(forms[i]);
        free(forms);
        return Qnil(env);
    }
    for (size_t i = 0; i < ws->n_def_forms; i++) free(ws->def_forms[i]);
    free(ws->def_forms);
    ws->def_forms = forms;
    ws->n_def_forms = n;
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

    bind_fn(env, "treejure-init",            1, 2, f_init);
    bind_fn(env, "treejure-check-buffer",    4, 4, f_check_buffer);
    bind_fn(env, "treejure-semantic-faces",  4, 4, f_semantic_faces);
    bind_fn(env, "treejure-definition",      3, 3, f_definition);
    bind_fn(env, "treejure-references",      3, 4, f_references);
    bind_fn(env, "treejure-close-buffer",    2, 2, f_close_buffer);
    bind_fn(env, "treejure-jar-entry",       2, 2, f_jar_entry);
    bind_fn(env, "treejure-set-def-forms",   2, 2, f_set_def_forms);
    bind_fn(env, "treejure-category-names",  0, 0, f_category_names);
    bind_fn(env, "treejure-diagnostic-ids",  0, 0, f_diagnostic_ids);

    return 0;
}
