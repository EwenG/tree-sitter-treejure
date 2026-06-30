#include <emacs-module.h>
#include <tree_sitter/api.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>
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
//   * **project-wide find-usages** (step 5) -- `treejure-references' with a
//     non-empty SCOPE-DIRS resolves the var at point to a canonical
//     (defining-ns, name) and returns EVERY occurrence across the chosen scope,
//     filtered by resolved identity: each scope file is read + walked by the
//     same buffer-only scope pass (distill_file at the USAGES level), then
//     matched (a file declaring the ns contributes def + same-ns navs; any file
//     contributes its cross-ns VarUsages whose resolved target ns matches).
//     Locals stay buffer-scoped.  The scope is a per-call directory list (not
//     persisted); the scan reads no jars and reaches no `resolve_ns' beyond the
//     one point query that fixes the identity.  A scanned file's navs/usages are
//     CACHED in its FileNode (mtime-gated, ANALYSIS_USAGES), so a repeat
//     find-usages -- and a jump-to-def / require-graph query, which need only the
//     surface ANALYSIS_USAGES subsumes -- reuse them with no re-parse.
//
// There is deliberately NO var face.  A resolved var -- same- or cross-namespace
// -- is left to the syntax layer: treesit already colors a qualified symbol's
// namespace and the def-name forms, so the semantic overlay paints only what
// treesit cannot (locals + form heads).  The scope pass stays buffer-only: it
// reaches no `resolve_ns' call at either tier and does NO dependency I/O;
// cross-namespace resolution survives only for the jump-to-def point query and
// the require graph (below).  The full tier emits the `unused-namespace' /
// `unused-referred-var' Tier-2 lints (lint_unused_requires) -- buffer-
// determinable from the require pass's alias/refer maps + the scope pass's usage
// marking, so they too read no dependencies (clj-kondo parity).
//
// The FULL tier also builds the forward **require graph** (build_require_graph):
// it resolves each of the file's requires to its dependency FileNode over the
// classpath via `resolve_ns' -- the first **check-time dependency I/O** (the fast
// tier stays dep-I/O-free).  This emits no diagnostic; an unresolved require just
// leaves a NULL edge.  `treejure-requires' exposes the edges.
//
// The **dependency-reading Tier-2 facts** consume that graph (resolve_var_usages
// + lint_unresolved_namespace), all full-tier:
//   * `undefined-var' -- a qualified/`:refer'-ed var whose target namespace DID
//     resolve (a non-NULL edge, or the file's own ns) but does not define it.
//     Emitted UNCONDITIONALLY: a resolved dep's var surface is authoritative, so
//     a miss is real however incomplete the classpath is (a library dep under the
//     interim classpath stays a NULL edge and is skipped, never mis-flagged).
//   * `unresolved-namespace' (a NULL edge) and the `:unresolved' face on a BARE
//     symbol -- these need exhaustive, jar-inclusive knowledge (a library require
//     is an unavoidable NULL edge; a bare core var lives in a jar), so they are
//     emitted ONLY when the workspace is `classpath_complete' (the JVM oracle's
//     classpath, PLAN step 6; `treejure-init's CLASSPATH-COMPLETE arg).  Under
//     the interim Elisp source-dirs-only classpath the flag is nil and they stay
//     silent -- the scope pass only RECORDS cross-ns usages (push_usage), never
//     resolving them, so it remains buffer-only at both tiers.
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
// The **cold full analysis** command is built (`treejure-analyze', step 5): an
// explicit chosen-scope scan that full-analyzes every source file under
// SCOPE-DIRS (analyze_file at the full tier, reading the dependency closure),
// returns an aggregate `(:files N :diagnostics M)' summary, and leaves each
// scanned disk file warm at ANALYSIS_USAGES so a later find-usages / jump-to-def
// reuses it.  Per-file diagnostics still surface lazily via treejure-check-buffer.
// A **per-pass `resolve_ns' memo** (NsCache) is threaded through analyze_file's
// full-tier resolvers (build_require_graph + the core-ns resolution in
// resolve_var_usages): the cold scan shares ONE memo across all files, so a
// namespace required by many of them (`clojure.core', shared libs) is resolved at
// most once for the whole scan rather than re-`stat'-ing the classpath per file
// (a single-file check passes NULL -- no ns repeats within one file).  It is
// behaviour-identical to a bare `resolve_ns' (keyed on (ns, dialect mask), with
// skip_path re-applied on lookup; see the NsCache comment).  Because the memo
// resolves the SKIP-FREE candidate, a self-require could otherwise re-read the
// file currently under analysis mid-walk -- prevented by `Workspace.analyzing'
// (index_disk_file returns the in-flight node untouched).
//
// What is still deferred: the JVM oracle that supplies the real jar-inclusive
// classpath enabling the gated facts above (step 6).  All positions are 0-based
// byte offsets; Elisp converts to buffer positions on apply.
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
    DIAG_UNRESOLVED_NAMESPACE,   // a require that resolves to nothing (Tier 2)
    DIAG_UNDEFINED_VAR,          // a qualified/:refer-ed var its dep lacks (Tier 2)
    DIAG__COUNT
};
static const char *const DIAG_IDS[DIAG__COUNT] = {
    ":syntax-error", ":missing-form", ":invalid-number", ":invalid-string",
    ":invalid-character", ":invalid-symbolic-value", ":unused-binding",
    ":duplicate-require", ":refer-all", ":namespace-name-mismatch",
    ":redefined-var", ":unused-namespace", ":unused-referred-var",
    ":unresolved-namespace", ":undefined-var"
};

// Severity.  The keyword is what Flymake gets; the level configured for an id
// (`treejure-set-levels', later) can downgrade/suppress it -- for now fixed.
enum { SEV_WARNING, SEV_ERROR };
static const char *const SEVERITY_NAMES[] = { ":warning", ":error" };

// ===========================================================================
// Small growable arrays.
// ===========================================================================

// Forward declaration: a ReqSpec carries a forward require-graph edge to its
// resolved dependency FileNode (defined below).
typedef struct FileNode FileNode;

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
    int      as_alias;    // `:as-alias' only -- a NON-loading alias; the ns need
                          // not exist, so it is never `unresolved-namespace'
    int      used;        // some usage referenced this ns (alias/fq/refer/`::')
    // Forward require-graph edge: the dependency FileNode `resolve_ns' found for
    // this require over the classpath (a source file or a jar entry), or NULL
    // when unresolved.  Set at the FULL tier only (build_require_graph); NULL at
    // the fast tier (the graph is a full-tier product) and after a fresh parse.
    // A BORROWED pointer -- the node is owned by `ws->files' (interned for the
    // session, never freed or moved until the workspace finalizer), so it stays
    // valid across checks; nsindex_clear must NOT free it.
    FileNode *resolved;
} ReqSpec;

// One var defined in the file (the public/private surface).  Cross-file
// resolution reads `name` (+ `private`); navigation reads the spans.  `declared`
// marks a var introduced only by `(declare ...)` -- counted as defined for
// resolution, but ignored by `redefined-var` (a forward decl is not a redef).
typedef struct {
    char    *name;             // unqualified var name (malloc'd)
    uint32_t name_start, name_end; // the defined symbol's span (jump target)
    uint32_t def_start, def_end;   // the whole (def... ) form's span
    int      private;          // defn- / ^:private / ^{:private true}
    int      declared;         // introduced by `(declare ...)` only
    int      synthesized;      // a generated secondary var (->Name / map->Name /
                               // a defprotocol/definterface method) -- counted in
                               // the var surface for resolution/navigation, but
                               // ignored by `redefined-var' (it is not a literal
                               // re-def of a hand-written name -- clj-kondo parity)
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

// A non-local symbol reference recorded for the dependency-reading Tier-2 pass
// (undefined-var + the `:unresolved' face).  Recorded by the scope pass WITHOUT
// any dependency I/O -- only buffer-only data is captured here; the full-tier
// `resolve_var_usages' resolves each against the require graph.  `ns' is the
// target namespace already resolved through the buffer's alias/refer maps (a
// qualified `a/foo' -> a's ns, or its literal qualifier; a `:refer'-ed bare
// `foo' -> the ns that referred it), or NULL for a bare symbol that matched
// neither a local, an in-file var, nor a refer -- a candidate `:unresolved'.
typedef struct {
    uint32_t start, end;  // the symbol occurrence's byte span (face/diag site)
    char    *ns;          // resolved target namespace (malloc'd), or NULL (bare)
    char    *name;        // the var name (malloc'd)
} VarUsage;

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

// How much analysis a (non-live) cached FileNode currently holds, gated by
// `indexed_mtime'.  A consumer asks index_disk_file for the level it needs; a
// node cached at >= that level (and mtime-fresh) is reused with NO re-parse.
// Levels are cumulative -- USAGES is a superset of SURFACE -- so a file analyzed
// for find-usages also satisfies a later jump-to-def / require-graph query, and
// a surface-only node is upgraded in place when find-usages first touches it.
// This is the find-usages **session cache**: a scanned file's navs/usages
// survive the call (mtime-gated), so a repeat find-usages does not re-analyze it.
enum {
    ANALYSIS_NONE = 0,    // never analyzed, opaque, or reverted
    ANALYSIS_SURFACE,     // ns name + defined-var surface (deps, require graph)
    ANALYSIS_USAGES       // + def/usage navs + cross-ns var usages (find-usages)
};

struct FileNode {
    char     *path;      // owned key (absolute file path)
    TSTree   *tree;      // NULL until first parse
    char     *text;      // last-parsed bytes (NUL-terminated copy), or NULL
    size_t    len;       // byte length of `text` (excluding the NUL)
    uint32_t  version;   // monotonic, bumped per successful reparse
    time_t    indexed_mtime; // mtime of the disk copy last indexed (0 = never)
    int       live;      // text came from a live buffer -> never clobber from disk
    int       opaque;    // dep that could not be read as UTF-8 -> never resolved
    int       is_jar;    // surface came from a jar entry -> immutable, never re-read
    int       analysis_level; // cached analysis depth (ANALYSIS_*) for a non-live
                              // node; with `indexed_mtime' gates cache reuse

    NsIndex   index;     // distilled ns surface (requires + vars), per check
    SemanticSpan *spans; // sorted by start, recomputed each check
    size_t        nspans, cap_spans;
    Diagnostic   *diags; // recomputed each check
    size_t        ndiags, cap_diags;
    NavRef       *navs;  // navigation occurrences (locals + same-ns vars)
    size_t        nnavs, cap_navs;
    VarUsage     *usages; // cross-ns / unresolved-candidate refs (full tier only)
    size_t        nusages, cap_usages;
};

// ===========================================================================
// StrMap -- a tiny path -> pointer hash (open addressing, FNV-1a, power-of-two
// table, linear probing).  Indexes `ws->files' by path and `ws->jar_dirs' by
// jar path, replacing the former linear scans.  Insert + lookup + iterate only;
// an entry is NEVER removed (a FileNode lives for the session -- close-buffer
// reverts it but keeps it), so there is no tombstone logic.  Keys are BORROWED
// -- they point into the stored object (a FileNode's `path' / a JarDir's
// `jar_path', both `strdup'-ed once and stable for the session) -- so the map
// frees only its slot array, never the keys.
// ===========================================================================

typedef struct { const char *key; void *val; } MapSlot;
typedef struct { MapSlot *slots; size_t cap, count; } StrMap;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static void strmap_grow(StrMap *m) {
    size_t ncap = m->cap ? m->cap * 2 : 16;
    MapSlot *ns = calloc(ncap, sizeof(MapSlot));
    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].key) {
            size_t j = fnv1a(m->slots[i].key) & (ncap - 1);
            while (ns[j].key) j = (j + 1) & (ncap - 1);
            ns[j] = m->slots[i];
        }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
}

// Insert KEY->VAL (KEY borrowed, must outlive the map).  Grows at 0.7 load.
static void strmap_put(StrMap *m, const char *key, void *val) {
    if ((m->count + 1) * 10 >= m->cap * 7) strmap_grow(m);
    size_t j = fnv1a(key) & (m->cap - 1);
    while (m->slots[j].key) {
        if (strcmp(m->slots[j].key, key) == 0) { m->slots[j].val = val; return; }
        j = (j + 1) & (m->cap - 1);
    }
    m->slots[j].key = key;
    m->slots[j].val = val;
    m->count++;
}

static void *strmap_get(StrMap *m, const char *key) {
    if (m->cap == 0) return NULL;
    size_t j = fnv1a(key) & (m->cap - 1);
    while (m->slots[j].key) {
        if (strcmp(m->slots[j].key, key) == 0) return m->slots[j].val;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

static void strmap_free(StrMap *m) {
    free(m->slots);
    m->slots = NULL;
    m->cap = m->count = 0;
}

// A jar's distilled directory: the source-entry names it contains (clj-family
// only -- resolution never looks up `.class' etc.), read ONCE on first touch
// and kept for the session (jars are immutable, PLAN fact #5).  So a require
// that misses a jar costs an in-memory binary search instead of re-reading the
// jar's central directory on every probe.  An empty `entries' / zero
// `n_entries' also caches a jar that could not be opened, so it is not retried.
typedef struct {
    char  *jar_path;     // owned key
    char **entries;      // owned, sorted clj-source entry names
    size_t n_entries;
} JarDir;

// ===========================================================================
// Workspace: one per project, holds N FileNodes + the shared parser.
//
// `files' owns the N FileNodes; `file_map' indexes them by path (a StrMap hash)
// so intern/lookup is O(1), not a linear scan.  `classpath' is the per-call
// default search scope for cross-file resolution: `build_require_graph' and the
// jump-to-def point query pass it to `resolve_ns' (find-usages will pass a
// per-invocation scope instead -- see the search-scope note in PLAN).  A jar
// touched during resolution has its source-entry directory cached in `jar_dirs'
// (indexed by `jar_map'), so a missing candidate is an in-memory search.
// ===========================================================================

typedef struct {
    char      *project_dir;
    char     **classpath; size_t n_classpath;
    // Whether `classpath' is the project's EXHAUSTIVE, jar-inclusive closure
    // (the JVM oracle's output, PLAN step 6) rather than the interim Elisp
    // source-dirs-only heuristic.  The dependency-reading Tier-2 facts that need
    // exhaustive knowledge to stay false-positive-free -- `unresolved-namespace'
    // and the `:unresolved' face -- are emitted ONLY when this is set, so under
    // the interim classpath a library require (an unavoidable NULL edge) is never
    // mis-flagged.  `undefined-var' does NOT depend on it: it fires only on a
    // require that DID resolve (a non-NULL edge), so a missing var there is real
    // regardless of how complete the classpath is.
    int        classpath_complete;
    char     **def_forms; size_t n_def_forms; // user macros analysed like `defn`
    TSParser  *parser;    // one parser, language set once (not reentrant)
    FileNode **files;     size_t n_files, cap_files;
    StrMap     file_map;  // path -> FileNode* (index over `files')
    JarDir   **jar_dirs;  size_t n_jar_dirs, cap_jar_dirs;
    StrMap     jar_map;   // jar path -> JarDir* (index over `jar_dirs')
    // The FileNode currently inside `analyze_file', or NULL.  A dependency
    // resolution during the full tier (build_require_graph / the core-ns resolve)
    // must never re-read THIS file from disk: its tree/text/index are live on the
    // stack of the in-progress walk, and re-indexing it (filenode_reparse +
    // distill_file) would delete the tree mid-walk and wipe its half-built
    // outputs.  `resolve_ns' skips the requiring file by path, but the per-pass
    // memo resolves the SKIP-FREE candidate (so the cached value is shareable), so
    // a self-require would otherwise reach `index_disk_file' on the in-flight file;
    // this guard makes `index_disk_file' return it untouched (see there).
    FileNode  *analyzing;
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
    for (size_t i = 0; i < f->nusages; i++) {
        free(f->usages[i].ns); free(f->usages[i].name);
    }
    f->nusages = 0;
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
    free(f->usages);
    free(f->text);
    free(f->path);
    free(f);
}

static FileNode *ws_find_file(Workspace *ws, const char *path) {
    return strmap_get(&ws->file_map, path);
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
    strmap_put(&ws->file_map, f->path, f);   // key borrows the stable `f->path'
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
    f->opaque = 0;             // re-decided on the next disk read
    f->indexed_mtime = 0;      // force a fresh disk read on next access
    f->analysis_level = ANALYSIS_NONE;
}

static void finalizer_workspace(void *ptr) {
    if (!ptr) return;
    Workspace *ws = (Workspace *)ptr;
    for (size_t i = 0; i < ws->n_files; i++) filenode_free(ws->files[i]);
    free(ws->files);
    strmap_free(&ws->file_map);
    for (size_t i = 0; i < ws->n_jar_dirs; i++) {
        JarDir *jd = ws->jar_dirs[i];
        for (size_t j = 0; j < jd->n_entries; j++) free(jd->entries[j]);
        free(jd->entries);
        free(jd->jar_path);
        free(jd);
    }
    free(ws->jar_dirs);
    strmap_free(&ws->jar_map);
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
    // Null-safe: `ts_node_type' on a null node dereferences a NULL language.
    // Several callers pass an `unwrap_meta' result (null when a `with_metadata'
    // lacks a `target', e.g. inside an ERROR subtree) straight in -- a null node
    // is simply "not of type T".
    return !ts_node_is_null(n) && strcmp(ts_node_type(n), t) == 0;
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
    int        cross_file;      // record cross-ns var usages (full tier only)
    int        in_unbound_body; // >0 inside a deftype/reify/extend-* body whose
                                // member positions we do not yet bind
    int        in_anon_fn;      // >0 inside a #(...) fn literal: a bare `%'/`%n'/
                                // `%&' symbol there is an implicit arg local
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

// Record a cross-ns / unresolved-candidate var reference for the full-tier
// dependency-reading pass (resolve_var_usages).  NS (the target namespace, NUL-
// free, length NS_LEN) is NULL for a bare symbol that resolved to nothing
// locally; NAME (length NAME_LEN) is the var name.  Both are copied.  Buffer-
// only: this just captures data -- it never resolves a dependency.
static void push_usage(FileNode *f, uint32_t start, uint32_t end,
                       const char *ns, size_t ns_len,
                       const char *name, size_t name_len) {
    if (f->nusages == f->cap_usages) {
        f->cap_usages = f->cap_usages ? f->cap_usages * 2 : 32;
        f->usages = realloc(f->usages, f->cap_usages * sizeof(VarUsage));
    }
    char *ns_dup = NULL;
    if (ns) { ns_dup = malloc(ns_len + 1); memcpy(ns_dup, ns, ns_len); ns_dup[ns_len] = '\0'; }
    char *nm_dup = malloc(name_len + 1);
    memcpy(nm_dup, name, name_len); nm_dup[name_len] = '\0';
    f->usages[f->nusages++] = (VarUsage){ start, end, ns_dup, nm_dup };
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
                             const char *name, size_t len, int record_usage) {
    NsIndex *ix = &a->f->index;
    for (size_t v = 0; v < ix->n_vars; v++) {
        if (strlen(ix->vars[v].name) == len &&
            memcmp(ix->vars[v].name, name, len) == 0) {
            push_nav(a->f, ss, se, NAV_VAR, -1, name, len, 0);
            // An in-file def and a `:refer' of the same name collide in the
            // global ns (ambiguous, unlike a lexical local shadow): count the
            // occurrence as using the refer too, so the require is not falsely
            // flagged unused (matching clj-kondo's leniency here).  Resolved
            // in-file -> no usage recorded (it cannot be undefined/unresolved).
            mark_refer_used(ix, name, len);
            return;
        }
    }
    // Not an in-file var: a bare name may be a `:refer'-ed var (a known cross-ns
    // target whose providing require we mark used), or else a symbol that
    // resolves to nothing locally -- an `:unresolved' candidate (it may still be
    // a core/refer-all/interop name; resolve_var_usages decides at the full tier).
    int matched_refer = 0;
    const char *refer_ns = NULL;
    for (size_t i = 0; i < ix->n_refers; i++)
        if (strlen(ix->refers[i].name) == len &&
            memcmp(ix->refers[i].name, name, len) == 0) {
            ix->refers[i].used = 1;
            mark_req_ns_used(ix, ix->refers[i].ns);
            if (!matched_refer) { refer_ns = ix->refers[i].ns; matched_refer = 1; }
        }
    if (record_usage) {
        if (matched_refer)
            // A `:refer'-ed var is a known cross-ns target -> reliable, always
            // recorded (even inside an unbound body, where it is a real ref).
            push_usage(a->f, ss, se, refer_ns, strlen(refer_ns), name, len);
        else if (!a->in_unbound_body && !(len > 0 && name[len - 1] == '#'))
            // A truly-bare candidate -> suppressed when:
            //   * inside a deftype/reify/extend-* body, where it may be an unbound
            //     field/method-name/param rather than a var reference; or
            //   * it ends in `#' -- the auto-gensym convention (e.g. an `x#' from a
            //     syntax-quote template escaped via `~x#'): a generated symbol, not
            //     a var, so flagging it `:unresolved'/`undefined-var' would
            //     false-positive (clj-kondo treats trailing-`#' as a gensym).
            push_usage(a->f, ss, se, NULL, 0, name, len);
    }
}

// A `#(...)' fn-literal implicit argument: `%', `%&', or `%N' (N digits).  These
// are not bound by any explicit vector, so the scope pass treats each as a local
// of the enclosing fn literal (see analyze_node's fn_literal handler).
static int is_anon_arg(const char *name, size_t len) {
    if (len == 0 || name[0] != '%') return 0;
    if (len == 1) return 1;                       // %
    if (len == 2 && name[1] == '&') return 1;     // %&
    for (size_t i = 1; i < len; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;                                      // %1, %2, ...
}

// Resolve a qualified `q/name' reference: ALWAYS mark its qualifier namespace
// used (for `unused-namespace' -- buffer-only, both tiers), and -- only at the
// full tier (`cross_file') -- record a cross-ns VarUsage whose target ns is `q'
// resolved through the alias map (an unaliased qualifier is a literal fully-
// qualified ns), the seed the full-tier `undefined-var' pass consumes.  Buffer-
// only either way: a plain alias lookup, never resolve_ns.  Shared by resolve_ref
// and the var-quote / `(var ..)' resolver.
static void resolve_qualified_ref(Analyzer *a, TSNode sym, TSNode nm) {
    TSNode nsf = ts_node_child_by_field_name(sym, "namespace", 9);
    if (ts_node_is_null(nsf)) return;
    const char *q = a->text + ts_node_start_byte(nsf);
    size_t qlen = ts_node_end_byte(nsf) - ts_node_start_byte(nsf);
    NsIndex *ix = &a->f->index;
    mark_ns_qualifier_used(ix, q, qlen);
    if (!a->cross_file) return;          // cross-ns usage recording: full tier only
    uint32_t ss = ts_node_start_byte(sym), se = ts_node_end_byte(sym);
    const char *tns = q; size_t tlen = qlen;
    for (size_t i = 0; i < ix->n_aliases; i++)
        if (strlen(ix->aliases[i].alias) == qlen &&
            memcmp(ix->aliases[i].alias, q, qlen) == 0) {
            tns = ix->aliases[i].ns; tlen = strlen(tns); break;
        }
    uint32_t na = ts_node_start_byte(nm), nb = ts_node_end_byte(nm);
    push_usage(a->f, ss, se, tns, tlen, a->text + na, nb - na);
}

// Resolve SYM as a VAR reference only (never a local) -- a `#'x' var-quote target
// or a `(var x)' argument.  A var-quote / `var' form names a var even if a
// like-named local is in scope, so locals are not consulted; a qualified `q/x'
// marks its namespace used (clj-kondo parity -- this is what fixes a spurious
// `unused-namespace' on a namespace used only through `#'the.ns/x').
static void resolve_var_ref(Analyzer *a, TSNode sym) {
    TSNode nm = field_name_node(sym);
    if (ts_node_is_null(nm)) return;
    if (!sym_has_namespace(sym)) {
        uint32_t ss = ts_node_start_byte(sym), se = ts_node_end_byte(sym);
        uint32_t na = ts_node_start_byte(nm), nb = ts_node_end_byte(nm);
        resolve_bare_var(a, ss, se, a->text + na, nb - na, a->cross_file);
    } else {
        resolve_qualified_ref(a, sym, nm);
    }
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
        // A `#(...)' anonymous-fn arg (`%'/`%n'/`%&') with no matching local yet:
        // it has no explicit binding site, so lazily bind it as a local of the
        // enclosing fn literal (the first occurrence is its binding; later ones
        // match the loop above).  Marked used up front -- an anon arg is never
        // reported unused -- and painted `:local' by pop_scope when the fn
        // literal's frame closes.
        if (a->in_anon_fn && is_anon_arg(name, len)) {
            push_local(a, sym);
            a->locals[a->nlocals - 1].used = 1;
            return;
        }
        // Not a local: an in-file var usage or a `:refer'-ed var (no face).  At
        // the full tier this also records an unresolved/undefined-var candidate.
        resolve_bare_var(a, ss, se, name, len, a->cross_file);
    } else {
        // Qualified `q/name': the qualifier `q' (an alias, or a literal fully-
        // qualified ns) is a namespace usage; also record the cross-ns var usage
        // for the full-tier undefined-var pass (buffer-only -- never resolve_ns).
        resolve_qualified_ref(a, sym, nm);
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
    "loop", "loop*", "with-open", "with-local-vars",
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
// the remainder as references.  (`defprotocol`/`definterface` are NOT here: their
// bodies are pure declarations, handled opaquely in analyze_binding_form so the
// interned method-var declaration sites are not re-recorded as usages.
// `deftype`/`defrecord` are NOT here either -- they go through
// UNBOUND_BODY_FORMS, which suppresses bare `:unresolved' recording for their
// fields/method positions.)
static const char *const OTHER_DEF_FORMS[] = {
    "defmulti", "definline", "deftest", "deftest-", "defstruct", 0
};
// Forms whose member positions -- deftype/defrecord FIELDS, and the method
// NAMES + PARAMS of reify/proxy/extend-type/extend-protocol/extend -- the scope
// pass does not yet BIND (proper field/arglist binding is deferred, PLAN).  It
// walks their bodies generically, so a bare field/method-name/param would be
// recorded as a bare `:unresolved' candidate and false-positive the face.  We
// walk the body with `in_unbound_body' set, which suppresses bare-candidate
// recording there (a QUALIFIED cross-ns reference inside -- e.g. `nt/area' in a
// defmethod-free body, or a `:refer'-ed var -- stays reliable and is recorded).
// `defmethod' is NOT here: analyze_fn_tail already binds its params.
static const char *const UNBOUND_BODY_FORMS[] = {
    "deftype", "defrecord", "reify", "proxy",
    "extend-type", "extend-protocol", "extend", 0
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
    if (sym_name_eq(text, head, "declare")) {
        // `(declare a b c)': the names are vars interned by extract_var_defs,
        // not references -- opaque to the scope pass (the head is already
        // painted `:special-form'), so the names are left unresolved.
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
    if (name_in(text, head, UNBOUND_BODY_FORMS)) {
        // deftype/defrecord skip the type name at index 1 (interned by
        // extract_var_defs); reify/proxy/extend-* have no name (walk from 1).
        // `in_unbound_body' suppresses bare `:unresolved' recording for the
        // fields / method names / method params we do not yet bind.
        uint32_t from = (sym_name_eq(text, head, "deftype") ||
                         sym_name_eq(text, head, "defrecord")) ? 2 : 1;
        a->in_unbound_body++;
        analyze_body(a, list, from);
        a->in_unbound_body--;
        return 1;
    }
    if (name_in(text, head, DEF_FORMS) || name_in(text, head, OTHER_DEF_FORMS)) {
        // (def name docstring? init?), (defmulti name ...), (defstruct name ...):
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
                // record_usage = 0: a syntax-quoted symbol is templated data, not
                // a live reference, so it is not an undefined/unresolved candidate
                // (it auto-qualifies at read time; flagging it would false-
                // positive).  The refer/ns usage marking above still runs.
                resolve_bare_var(a, ts_node_start_byte(node), ts_node_end_byte(node),
                                 a->text + ts_node_start_byte(nm),
                                 ts_node_end_byte(nm) - ts_node_start_byte(nm), 0);
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
        // The metadata map is live code too: a `^{:k local}' references a binding,
        // a `^Type' hint references a class/var.  Resolve it as well as the
        // decorated form -- otherwise a symbol used ONLY in metadata is invisible
        // (a false `unused-binding', a missed namespace/refer usage).  clj-kondo
        // analyzes metadata likewise.  Nested wrappers (`^:a ^:b form') recurse
        // through `target'.
        TSNode meta = ts_node_child_by_field_name(node, "meta", 4);
        if (!ts_node_is_null(meta))
            analyze_node(a, ts_node_child_by_field_name(meta, "value", 5));
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
    // A var-quote `#'x' / `#'ns/x' references a VAR (never a local): mark its
    // namespace used and record its usage/nav, so find-references and
    // `unused-namespace' stay correct (clj-kondo counts a var-quote as a usage).
    if (strcmp(t, "var_quote") == 0) {
        TSNode tgt = unwrap_meta(ts_node_child_by_field_name(node, "target", 6));
        if (!ts_node_is_null(tgt) && type_is(tgt, "symbol"))
            resolve_var_ref(a, tgt);
        return;
    }
    // Plain quoted / discarded / eval-literal subtrees are pure data.
    if (strcmp(t, "quote") == 0 || strcmp(t, "discard") == 0 ||
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
        TSNode raw_head = nth_form(node, 0);
        TSNode head = unwrap_meta(raw_head);
        int bare_head = !ts_node_is_null(head) && type_is(head, "symbol") &&
                        !sym_has_namespace(head);
        if (!bare_head) {
            analyze_body(a, node, 0);   // non-bare head: every child is a reference
            return;
        }
        // The head's own metadata (`^:foo when', `^Type ctor') is live code.  The
        // dispatch below consumes/paints the head and walks the body from index 1,
        // so analyze the head metadata here -- otherwise a symbol used only in the
        // head's metadata would be invisible (a missed local / namespace usage).
        // Walk EVERY wrapper level: `^:a ^:b head' nests `with_metadata' nodes, so
        // a single peel would miss the inner wrapper's value (cf.
        // `name_has_private_meta').
        for (TSNode mw = raw_head; type_is(mw, "with_metadata");
             mw = ts_node_child_by_field_name(mw, "target", 6)) {
            TSNode meta = ts_node_child_by_field_name(mw, "meta", 4);
            if (!ts_node_is_null(meta))
                analyze_node(a, ts_node_child_by_field_name(meta, "value", 5));
        }
        // Paint the head's semantic face before dispatching.  A known non-core
        // macro (a user-declared def-form) reads as `:macro-invocation'; a special
        // form / core macro reads as `:special-form'.  Classifying here -- and
        // walking the body from index 1 below -- examines the head exactly once.
        int core = 0;
        if (is_extra_def_form(a->ws, a->text, head))
            push_span(a->f, ts_node_start_byte(head), ts_node_end_byte(head),
                      CAT_MACRO_INVOCATION);
        else if ((core = name_in(a->text, head, CORE_FORMS)))
            push_span(a->f, ts_node_start_byte(head), ts_node_end_byte(head),
                      CAT_SPECIAL_FORM);
        // `(quote ...)` is pure data (like the `'...` reader node).  `(var x)`
        // mirrors `#'x': its argument is a VAR reference -- resolve it (so the
        // namespace of a `(var the.ns/x)` counts as used), but never as a local.
        if (sym_name_eq(a->text, head, "quote"))
            return;
        if (sym_name_eq(a->text, head, "var")) {
            TSNode arg = unwrap_meta(nth_form(node, 1));
            if (!ts_node_is_null(arg) && type_is(arg, "symbol"))
                resolve_var_ref(a, arg);
            return;
        }
        if (analyze_binding_form(a, node, head))
            return;
        if (core) {                 // non-binding core form (if/when/cond/...)
            analyze_body(a, node, 1);   // head already painted -- skip it
            return;
        }
        // An ordinary call with a bare head: resolve the head as a function/var
        // reference (its metadata handled above), then walk the args.
        resolve_ref(a, head);
        analyze_body(a, node, 1);
        return;
    }

    // A `#(...)' fn literal: open a scope so its implicit args (`%'/`%n'/`%&')
    // bind as locals -- resolve_ref binds each lazily while `in_anon_fn' is set --
    // then pop, painting each `:local' and grouping its usages for navigation.
    // Clojure forbids NESTING fn literals, but a regular `(fn ...)` may nest
    // inside, where `%' still refers to THIS literal; `in_anon_fn' is a counter
    // the regular-fn handler leaves untouched, so it stays set across the nest.
    if (strcmp(t, "fn_literal") == 0) {
        size_t mark = a->nlocals;
        a->in_anon_fn++;
        analyze_body(a, node, 0);
        a->in_anon_fn--;
        pop_scope(a, mark);
        return;
    }

    // Other collections / reader macros: walk their form children.
    if (strcmp(t, "vector_literal") == 0 || strcmp(t, "map_literal") == 0 ||
        strcmp(t, "set_literal") == 0 || strcmp(t, "namespaced_map_literal") == 0 ||
        strcmp(t, "pair") == 0 ||
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
    // `resolved' (the forward require-graph edge) is NULL until the full-tier
    // build_require_graph populates it.
    ix->requires[ix->n_requires++] =
        (ReqSpec){ ns, s, e, refer_all, from_use, 0, 0, 0, 0, NULL };
}

static void push_var(NsIndex *ix, char *name, uint32_t ns, uint32_t ne,
                     uint32_t ds, uint32_t de, int private, int declared,
                     int synthesized) {
    if (ix->n_vars == ix->cap_vars) {
        ix->cap_vars = ix->cap_vars ? ix->cap_vars * 2 : 16;
        ix->vars = realloc(ix->vars, ix->cap_vars * sizeof(VarDef));
    }
    ix->vars[ix->n_vars++] =
        (VarDef){ name, ns, ne, ds, de, private, declared, synthesized };
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
        int has_alias = 0, has_refer_vec = 0, as_alias = 0;
        uint32_t k = 1; TSNode opt;
        while (!ts_node_is_null(opt = nth_form(spec, k))) {
            TSNode ou = unwrap_meta(opt);
            if (type_is(ou, "keyword")) {
                if (kw_name_eq(text, ou, "as") || kw_name_eq(text, ou, "as-alias")) {
                    TSNode al = unwrap_meta(nth_form(spec, k + 1));
                    if (!ts_node_is_null(al) && type_is(al, "symbol"))
                        push_alias(ix, text, al, ns);
                    if (kw_name_eq(text, ou, "as")) has_alias = 1;
                    else as_alias = 1;   // `:as-alias' -- non-loading; ns may not exist
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
        ix->requires[ix->n_requires - 1].as_alias = as_alias;
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
// the ns/path match, and flatten its require/use specs.  The buffer-only require
// diagnostics (refer-all / duplicate-require) are emitted once over the FULL
// require set by lint_require_specs -- after any top-level `(require ...)` forms
// are also collected (see analyze_requires) -- so a top-level duplicate is caught.
static void analyze_ns_form(FileNode *f, const char *text, TSNode ns_list) {
    NsIndex *ix = &f->index;
    TSNode name = unwrap_meta(nth_form(ns_list, 1));
    if (!ts_node_is_null(name) && type_is(name, "symbol")) {
        free(ix->ns_name);   // a top-level (in-ns ...) may have set it first
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
                // Intentionally NOT handled here -- each needs a consumer not yet
                // built, so tracking them now would be untested, unconsumed index
                // (PLAN: no dead code):
                //   * `:import' (Java/JS classes) -> interop resolution;
                //   * `:refer-clojure :exclude'    -> core-var exclusion (the gated
                //     undefined-var / `:unresolved' slice);
                //   * `:require-macros' (cljs)     -> the deferred **cljs tier**
                //     (PLAN: "v1 = clj only").  It cannot just be folded into
                //     `:require': a macro ns lives on the *clj* side (a different
                //     dialect mask) and shares its name with the runtime ns, so the
                //     ns-string-keyed consumers (`find_require_by_ns', the require
                //     graph's dialect resolve, the `mark_*_used' family) would
                //     cross-resolve the two and mis-fire undefined-var /
                //     unresolved-namespace / unused-namespace.  Handle it properly
                //     when the cljs tier lands (macros-aware require records +
                //     clj-side resolution), not as a half-measure.
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
}

// Top-level forms that define a var/type (a name at form index 1).  `declare`
// is included -- it forward-declares one or more vars, interned with a
// `declared' bit (record_var_def) so cross-file / in-file resolution sees them,
// while `lint_redefined_var' ignores them (a `declare' then `def' of the same
// name is not a redefinition).  `defmethod' is excluded: it extends a multifn,
// defining nothing -- recording it would read a later `def'/method as a redef.
static const char *const VAR_DEF_FORMS[] = {
    "def", "defn", "defn-", "defmacro", "defmulti", "defonce", "definline",
    "deftest", "deftest-", "defprotocol", "deftype", "defrecord",
    "definterface", "defstruct", "declare", 0
};

// Forms whose method/instance bodies run on dispatch or method call -- NOT at
// load time -- so, like `fn`, they are a function boundary for the var-def pass:
// an inline `(def ...)` inside one is not the load-time surface and must not be
// recorded (else it false-positives `redefined-var`).  `defmethod`'s body is
// also scoped by the scope pass (analyze_fn_tail); the others are walked
// generically there (proper field/arglist binding is deferred -- see PLAN), but
// either way their bodies are never a load-time def site.
static const char *const METHOD_BODY_FORMS[] = {
    "defmethod", "reify", "proxy", "extend", "extend-type", "extend-protocol", 0
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
                       int private, int declared, int synthesized,
                       int record_navs) {
    char *dup = malloc(name_len + 1);
    memcpy(dup, name, name_len); dup[name_len] = '\0';
    push_var(&f->index, dup, name_start, name_end, def_start, def_end,
             private, declared, synthesized);
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
               def_start, def_end, 0 /*private*/, 0 /*declared*/,
               1 /*synthesized*/, record_navs);
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
                               0 /*private*/, 0 /*declared*/, 1 /*synthesized*/,
                               record_navs);
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
    // `(declare a b c)`: forward-declares one or more vars.  Each name is
    // interned with the `declared' bit set, so cross-file / in-file resolution
    // sees the var while `lint_redefined_var' skips it (a `declare' then `def'
    // of the same name is not a redefinition).
    if (sym_name_eq(text, head, "declare")) {
        uint32_t ds = ts_node_start_byte(u), de = ts_node_end_byte(u);
        uint32_t k = 1; TSNode c;
        while (!ts_node_is_null(c = nth_form(u, k))) {
            TSNode name = unwrap_meta(c);
            if (type_is(name, "symbol")) {
                TSNode nm = field_name_node(name);
                if (!ts_node_is_null(nm))
                    // `(declare ^:private foo)' forward-declares a private var --
                    // read the metadata off the (possibly wrapped) name like the
                    // non-declare path does.
                    intern_var(f, text + ts_node_start_byte(nm),
                               ts_node_end_byte(nm) - ts_node_start_byte(nm),
                               ts_node_start_byte(name), ts_node_end_byte(name),
                               ds, de, name_has_private_meta(text, c),
                               1 /*declared*/, 0 /*synthesized*/, record_navs);
            }
            k++;
        }
        return;
    }
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
    intern_var(f, text + na, nb - na, ns, ne, ds, de, private,
               0 /*declared*/, 0 /*synthesized*/, record_navs);

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
// METHOD_BODY_FORMS), an `(ns ...)` form, and quote/discard/eval reader
// wrappers.  A `(comment ...)` body is NOT opaque -- it is descended like `do`
// (matching the scope pass), so a `def` inside a comment is recorded.  A user
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
                sym_name_eq(text, head, "ns"))         // (ns ...)
                return;                                 // opaque body
            // `(comment ...)` is deliberately NOT a boundary -- its body is
            // descended like `do`, so a `def` nested in a comment is interned
            // into the file surface, matching the scope pass (which already
            // walks comment bodies): both passes treat `(comment ...)`
            // transparently.
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
//
// DELIBERATE: defs nested in a `(comment ...)` count toward this.  scan_var_defs
// descends comment bodies transparently (like `do`) so a comment-nested def is
// part of the file's var surface -- navigable, and symmetric with the scope
// pass, which already walks comment bodies.  A consequence is that a `def` in a
// rich-comment block plus a same-named top-level `def` trips `redefined-var`.
// That is intentional, not an oversight: do NOT special-case `(comment ...)`
// out here without also making scan_var_defs treat it as opaque again (the two
// must agree on what the surface is).  This is a conscious divergence from
// clj-kondo, which does not flag a redefinition across a comment boundary.
static void lint_redefined_var(FileNode *f) {
    NsIndex *ix = &f->index;
    for (size_t i = 0; i < ix->n_vars; i++) {
        // A `declare' never redefines, and a synthesized factory/method var
        // (`->Name'/`map->Name', a protocol/interface method) is not a literal
        // re-def: a hand-written `(defn ->Foo ...)' beside a `(defrecord Foo)' or
        // two protocols sharing a method name must NOT trip `redefined-var'
        // (clj-kondo parity).  Synthesized vars also carry the *type* name's span,
        // so flagging one would point the diagnostic at the wrong token.
        if (ix->vars[i].declared || ix->vars[i].synthesized) continue;
        for (size_t p = 0; p < i; p++) {
            if (ix->vars[p].declared || ix->vars[p].synthesized) continue;
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

// `refer-all` + `duplicate-require` (Tier 1, buffer-only), emitted once over the
// file's FULL require set -- the `(ns ...)` :require/:use specs PLUS any top-level
// `(require ...)` / `(use ...)` forms -- so a top-level require that duplicates an
// ns one is flagged at its (later) site, matching clj-kondo.
static void lint_require_specs(FileNode *f) {
    NsIndex *ix = &f->index;
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

// Unwrap a `quote` reader wrapper (`'x` -> `x`).  Top-level `(require '...)` /
// `(use '...)` / `(alias '...)` args are quoted, unlike the `(ns ...)` :require
// specs, which are bare.
static TSNode unwrap_quote(TSNode n) {
    if (!ts_node_is_null(n) && type_is(n, "quote"))
        return ts_node_child_by_field_name(n, "target", 6);
    return n;
}

// A top-level `(require '[lib :as a :refer [..]] '[lib2] :reload)` or
// `(use '...)`: each spec arg is unwrapped from its `quote` wrapper, then
// flattened by parse_require_spec exactly like an `(ns ...)` :require spec.
// Trailing flag keywords (`:reload`, `:verbose`, ...) -- bare, not quoted -- are
// skipped.
static void analyze_toplevel_require(FileNode *f, const char *text, TSNode list,
                                     int from_use) {
    uint32_t k = 1; TSNode arg;
    while (!ts_node_is_null(arg = nth_form(list, k))) {
        TSNode spec = unwrap_quote(unwrap_meta(arg));
        if (!ts_node_is_null(spec) && !type_is(spec, "keyword"))
            parse_require_spec(&f->index, text, f->path, spec, from_use);
        k++;
    }
}

// A top-level `(alias 'short 'the.namespace)`: register the alias -> namespace
// mapping so `short/var` resolves, mirroring `(ns ... (:require [the.ns :as
// short]))`.  `(alias ...)` only names an already-loaded ns, so it adds the alias
// the resolver needs but no require edge.
static void analyze_toplevel_alias(FileNode *f, const char *text, TSNode list) {
    TSNode al = unwrap_quote(unwrap_meta(nth_form(list, 1)));
    TSNode ns = unwrap_quote(unwrap_meta(nth_form(list, 2)));
    if (ts_node_is_null(al) || ts_node_is_null(ns) ||
        !type_is(al, "symbol") || !type_is(ns, "symbol"))
        return;
    char *ns_str = node_text_dup(text, ns);
    push_alias(&f->index, text, al, ns_str);
    free(ns_str);                          // push_alias copied it
}

// A top-level `(in-ns 'my.ns)`: name the file's namespace when no `(ns ...)`
// governs it (the common REPL/script pattern).  A single NsIndex cannot model a
// mid-file ns switch, so only the first governing namespace is recorded.
static void analyze_toplevel_inns(FileNode *f, const char *text, TSNode list) {
    if (f->index.ns_name) return;          // an (ns ...) / earlier in-ns governs
    TSNode ns = unwrap_quote(unwrap_meta(nth_form(list, 1)));
    if (ts_node_is_null(ns) || !type_is(ns, "symbol")) return;
    f->index.ns_name  = node_text_dup(text, ns);
    f->index.ns_start = ts_node_start_byte(ns);
    f->index.ns_end   = ts_node_end_byte(ns);
}

// Distil the file's namespace surface: the first `(ns ...)` form (its name +
// :require/:use specs), PLUS any `(require ...)` / `(use ...)` / `(alias ...)` /
// `(in-ns ...)` reached at load time -- at the top level OR through a top-level
// `do`/`when`/`let`/reader-conditional (the common script/REPL idiom).  The
// require diagnostics are emitted once over the combined set (lint_require_specs).
//
// scan_requires largely mirrors scan_var_defs' load-time descent: it STOPS --
// treating the body as opaque -- at a def-like form, a `(fn ...)`, a
// method/instance body (defmethod/reify/...), the `(ns ...)` form, and
// quote/discard/eval wrappers, and descends `do`/`let`/`when`/reader-conditionals.
// It diverges in ONE place: `(comment ...)' is opaque here but transparent in
// scan_var_defs -- requires inside a rich comment are scratch and must not be
// linted/graphed, whereas a comment-nested def is harmless navigation (see the
// `comment' stop below).  Only the first `(ns ...)` governs (SEEN_NS).
static void scan_requires(Workspace *ws, FileNode *f, const char *text,
                          TSNode node, int *seen_ns) {
    if (ts_node_is_null(node)) return;
    node = unwrap_meta(node);
    if (ts_node_is_null(node)) return;
    if (is_opaque_wrapper(node)) return;            // '... `... #'... #_... #=...
    if (type_is(node, "fn_literal")) return;        // #(...) -- function boundary
    if (type_is(node, "reader_conditional")) {      // honor this dialect's branch
        scan_requires(ws, f, text,
                      reader_conditional_branch(text, f->path, node), seen_ns);
        return;
    }
    if (type_is(node, "list_literal")) {
        TSNode head = unwrap_meta(nth_form(node, 0));
        if (!ts_node_is_null(head) && type_is(head, "symbol") &&
            !sym_has_namespace(head)) {
            if (sym_name_eq(text, head, "ns")) {
                if (!*seen_ns) {                     // only the first ns governs
                    analyze_ns_form(f, text, node);
                    *seen_ns = 1;
                }
                return;                              // never descend an ns body
            }
            if (sym_name_eq(text, head, "require")) {
                analyze_toplevel_require(f, text, node, 0); return; }
            if (sym_name_eq(text, head, "use")) {
                analyze_toplevel_require(f, text, node, 1); return; }
            if (sym_name_eq(text, head, "alias")) {
                analyze_toplevel_alias(f, text, node); return; }
            if (sym_name_eq(text, head, "in-ns")) {
                analyze_toplevel_inns(f, text, node); return; }
            // `(comment ...)' is an opaque boundary HERE -- deliberately ASYMMETRIC
            // with scan_var_defs, which descends it.  A require/use inside a
            // rich-comment block is REPL scratch, not the load-time surface, and
            // descending it drives diagnostics (duplicate-require / refer-all /
            // unused-namespace) and require-graph edges that clj-kondo does not
            // emit -- where a comment-nested *def* only adds harmless navigation.
            // Requires carry diagnostic consequences; var-defs do not, so the two
            // scans legitimately diverge on what `(comment ...)' is.
            if (sym_name_eq(text, head, "comment")) return;
            // Same opaque boundaries as scan_var_defs -- a require nested in a
            // def value / fn body / method body is not the load-time surface.
            if (name_in(text, head, VAR_DEF_FORMS) ||
                is_extra_def_form(ws, text, head) ||
                name_in(text, head, FN_FORMS) ||
                name_in(text, head, METHOD_BODY_FORMS))
                return;
        }
    }
    // do/let/when/comment/call/... -- descend looking for load-time requires.
    TSNode c;
    for (uint32_t k = 0; !ts_node_is_null(c = nth_form(node, k)); k++)
        scan_requires(ws, f, text, c, seen_ns);
}

static void analyze_requires(Workspace *ws, FileNode *f, const char *text,
                             TSNode root) {
    int seen_ns = 0;
    scan_requires(ws, f, text, root, &seen_ns);
    lint_require_specs(f);
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

// Seed F's NsIndex ns name for a *dependency* index, mirroring scan_requires'
// load-time descent so a dependency is named the SAME way the live buffer names
// itself.  Without this the seed saw only a top-level `(ns ...)' list, so a dep
// that declares its namespace via `(in-ns 'the.ns)' or inside a top-level
// `do'/`when'/`let'/reader-conditional yielded a NULL ns_name and never resolved
// (a require/jump-to-def false-negative) -- yet the same file resolved fine when
// opened as the live buffer.  Records ONLY the name (no require flattening, no
// diagnostics): a dependency is queried for the vars it defines, and resolve_ns
// confirms it by ns_name.  A real `(ns ...)' wins over an earlier `(in-ns ...)',
// matching analyze_ns_form / analyze_toplevel_inns; only the first governs.
static void scan_ns_name(Workspace *ws, const char *text, const char *path,
                         TSNode node, FileNode *f, int *seen_ns) {
    if (ts_node_is_null(node)) return;
    node = unwrap_meta(node);
    if (ts_node_is_null(node)) return;
    if (is_opaque_wrapper(node)) return;            // '... `... #'... #_... #=...
    if (type_is(node, "fn_literal")) return;        // #(...) -- function boundary
    if (type_is(node, "reader_conditional")) {
        scan_ns_name(ws, text, path, reader_conditional_branch(text, path, node),
                     f, seen_ns);
        return;
    }
    if (type_is(node, "list_literal")) {
        TSNode head = unwrap_meta(nth_form(node, 0));
        if (!ts_node_is_null(head) && type_is(head, "symbol") &&
            !sym_has_namespace(head)) {
            if (sym_name_eq(text, head, "ns")) {
                if (!*seen_ns) {                    // first ns form governs
                    TSNode name = unwrap_meta(nth_form(node, 1));
                    if (!ts_node_is_null(name) && type_is(name, "symbol")) {
                        free(f->index.ns_name);     // override an earlier in-ns
                        f->index.ns_name  = node_text_dup(text, name);
                        f->index.ns_start = ts_node_start_byte(name);
                        f->index.ns_end   = ts_node_end_byte(name);
                    }
                    *seen_ns = 1;
                }
                return;                             // never descend an ns body
            }
            if (sym_name_eq(text, head, "in-ns")) {
                if (!f->index.ns_name) {            // only if no ns/in-ns governs yet
                    TSNode ns = unwrap_quote(unwrap_meta(nth_form(node, 1)));
                    if (!ts_node_is_null(ns) && type_is(ns, "symbol")) {
                        f->index.ns_name  = node_text_dup(text, ns);
                        f->index.ns_start = ts_node_start_byte(ns);
                        f->index.ns_end   = ts_node_end_byte(ns);
                    }
                }
                return;
            }
            // EXACTLY the boundaries scan_requires uses for the LIVE buffer --
            // including the `(comment ...)' stop and the user extra-def-form stop.
            // This is the whole point: a dependency must be named the SAME way it
            // would name itself when opened (see the header comment).  A `(ns ...)'
            // / `(in-ns ...)' nested only in a rich comment is REPL scratch -- the
            // namespace is not really established -- so it must NOT name the dep
            // either, else resolve_ns confirms a spurious cross-file edge.
            if (sym_name_eq(text, head, "comment")) return;
            if (name_in(text, head, VAR_DEF_FORMS) ||
                is_extra_def_form(ws, text, head) ||
                name_in(text, head, FN_FORMS) ||
                name_in(text, head, METHOD_BODY_FORMS))
                return;
        }
    }
    TSNode c;
    for (uint32_t k = 0; !ts_node_is_null(c = nth_form(node, k)); k++)
        scan_ns_name(ws, text, path, c, f, seen_ns);
}

// Lightweight seed for indexed (non-buffer) files: the ns name only -- no
// requires/vars/diagnostics (those are recomputed by analyze_file when the file
// becomes the live buffer).
static void extract_ns_name(Workspace *ws, FileNode *f) {
    if (!f->tree) return;
    int seen_ns = 0;
    scan_ns_name(ws, f->text, f->path, ts_tree_root_node(f->tree), f, &seen_ns);
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
                // Normalize CRLF -> LF so disk byte offsets match how Emacs
                // decodes the same file (it strips CR from DOS line endings on
                // read).  Without this a cross-file jump-to-def / find-usages
                // location into a CRLF dep drifts one byte per preceding line.
                // Scoped to this single disk-read point; the live active buffer
                // is already LF (Emacs decoded it before pushing the text).
                size_t w = 0;
                for (size_t r = 0; r < n; r++) {
                    if (buf[r] == '\r' && r + 1 < n && buf[r + 1] == '\n') continue;
                    buf[w++] = buf[r];
                }
                buf[w] = '\0';
                *len = w;
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

// Prune a FileNode's outputs to the find-usages session-cache SHAPE: drop the
// live-only face spans and local navs, keeping only the NAV_VAR navs (the
// cross-ns usages and the ns surface are left untouched).  Shared by distill_file
// (the USAGES level) and the cold-scan analyze path, so the two never diverge on
// what a cached USAGES node holds.
static void prune_navs_to_usages(FileNode *f) {
    f->nspans = 0;                       // faces: live buffer only
    size_t w = 0;                        // keep only NAV_VAR navs
    for (size_t i = 0; i < f->nnavs; i++) {
        if (f->navs[i].kind == NAV_VAR) f->navs[w++] = f->navs[i];
        else free(f->navs[i].name);      // a local: name is NULL (free is a no-op)
    }
    f->nnavs = w;
}

// Reduce a FileNode freshly analyzed at the FULL tier (analyze_file, cross_file=1)
// to the ANALYSIS_USAGES session cache: drop the live-only products (diagnostics,
// face spans, local navs) and the heavy parse state (tree + text), keeping the ns
// surface + NAV_VAR navs + cross-ns usages that find-usages / jump-to-def / the
// require graph read.  Used by the cold-scan analyze path after it has counted a
// file's diagnostics, so a large scope leaves warm USAGES nodes rather than N
// pinned parse trees.  (distill_file reaches the same shape from the other side --
// it never produces diagnostics, so it prunes navs directly.)
static void prune_to_usages_cache(FileNode *f) {
    for (size_t i = 0; i < f->ndiags; i++) free(f->diags[i].message);
    f->ndiags = 0;
    prune_navs_to_usages(f);
    f->analysis_level = ANALYSIS_USAGES;
    if (f->tree) { ts_tree_delete(f->tree); f->tree = NULL; }
    free(f->text); f->text = NULL; f->len = 0;
}

// Analyze F's current tree to LEVEL, then DROP the heavy parse state (tree +
// text): the distilled products carry self-contained byte spans, names and the
// file path, so the tree is never needed again.
//   * SURFACE -- ns name + defined-var surface (what deps / the require graph
//     look up).  No requires/aliases/refers/diagnostics: a dep is queried for the
//     vars it *defines*, not what it brings into scope.
//   * USAGES  -- additionally the buffer-only scope pass (requires + aliases/
//     refers + var-defs with def-navs + the cross_file reference walk) that
//     find-usages matches, WITHOUT diagnostics / the require graph / the
//     require-spec lints (find-usages reads none of them).  Its live-buffer-only
//     products are pruned: the face spans (read only for the active buffer) are
//     dropped, and the local navs are dropped (a local is never a cross-file
//     find-usages match).  If the file is later opened live, f_check_buffer
//     reparses it from scratch, so nothing kept here is trusted for the live path.
static void distill_file(Workspace *ws, FileNode *f, int level) {
    filenode_clear_outputs(f);
    if (!f->tree) { f->analysis_level = ANALYSIS_NONE; return; }
    TSNode root = ts_tree_root_node(f->tree);
    if (level >= ANALYSIS_USAGES) {
        int seen_ns = 0;
        scan_requires(ws, f, f->text, root, &seen_ns);   // ns + aliases/refers
        extract_var_defs(ws, f, f->text, root, 1);       // vars + def-site navs
        Analyzer a = { .ws = ws, .f = f, .text = f->text,
                       .locals = NULL, .nlocals = 0, .cap_locals = 0,
                       .next_local_id = 0, .cross_file = 1, .in_unbound_body = 0 };
        analyze_body(&a, root, 0);
        free(a.locals);
        prune_navs_to_usages(f);             // faces + local navs: live buffer only
    } else {
        extract_ns_name(ws, f);              // surface: ns name only
        extract_var_defs(ws, f, f->text, root, 0);       // dep: no navs
    }
    f->analysis_level = level;
    ts_tree_delete(f->tree); f->tree = NULL;
    free(f->text); f->text = NULL; f->len = 0;
}

// Index (or refresh) the disk file at PATH into the workspace to at least LEVEL,
// stat-gated on MTIME so an unchanged file is parsed at most once and a file
// already analyzed deep enough is reused untouched -- the session cache: a
// find-usages-scanned node keeps its navs/usages across calls, and a jump-to-def
// / require-graph query reuses them.  A live-buffer FileNode is never read from
// disk -- its in-memory text and ns must win.  Returns the (possibly cached)
// FileNode for PATH, or NULL if it could not be read and was not already interned
// -- so the caller can match its ns_name without a second `ws_find_file' scan.
static FileNode *index_disk_file(Workspace *ws, const char *path,
                                 time_t mtime, int level) {
    FileNode *f = ws_find_file(ws, path);
    if (f && f->live) return f;                               // live buffer wins
    if (f && f == ws->analyzing) return f;   // never re-read the file under analysis
                                             // (a self-require's skip-free resolve)
    // Cached and unchanged?  Reuse when the file is opaque (terminal -- it
    // resolves to nothing) or already analyzed to at least the requested level.
    // `indexed_mtime' is 0 for a never-indexed / reverted node, and a real disk
    // mtime is never 0.  A shallower cache (SURFACE when USAGES is asked) falls
    // through to re-read: the tree was dropped after the last distill, so the
    // deeper pass needs a fresh parse anyway.
    if (f && f->indexed_mtime != 0 && f->indexed_mtime == mtime &&
        (f->opaque || f->analysis_level >= level))
        return f;
    size_t len; char *buf = read_file(path, &len);
    if (!buf) return f;
    if (!f) f = ws_intern_file(ws, path);
    if (!is_valid_utf8(buf, len)) {       // non-UTF-8 dep -> opaque, never resolved
        free(buf);
        filenode_clear_outputs(f);        // drop any stale index/ns/navs
        if (f->tree) { ts_tree_delete(f->tree); f->tree = NULL; }
        free(f->text); f->text = NULL; f->len = 0;
        f->opaque = 1;
        f->analysis_level = ANALYSIS_NONE;
        f->indexed_mtime = mtime;         // mtime-gate; re-checked only on change
        return f;
    }
    f->opaque = 0;
    filenode_reparse(ws, f, buf, len);   // takes ownership of `buf`
    distill_file(ws, f, level);          // analyzes to LEVEL, drops tree/text
    f->indexed_mtime = mtime;
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
    // A jar entry is immutable and find-usages skips jars, so SURFACE (ns + var
    // surface) is all it is ever queried for; distill_file drops the tree/text.
    distill_file(ws, f, ANALYSIS_SURFACE);
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

static int cstr_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Return JAR_PATH's cached source-entry directory, reading it once on first
// touch (jars are immutable per session, so it is never refreshed).  Keeps only
// clj-family entries -- resolution never looks up `.class' etc. -- sorted for
// binary search.  A jar that cannot be opened is cached empty so it is never
// re-probed.  Indexed by `jar_map', so the lookup is O(1) across requires.
static JarDir *jar_get_or_load(Workspace *ws, const char *jar_path) {
    JarDir *jd = strmap_get(&ws->jar_map, jar_path);
    if (jd) return jd;
    jd = calloc(1, sizeof(JarDir));
    jd->jar_path = strdup(jar_path);
    size_t n = 0;
    char **all = jar_list_entries(jar_path, &n);   // every file entry, or NULL
    if (all) {
        jd->entries = malloc(n * sizeof(char *));
        size_t got = 0;
        for (size_t i = 0; i < n; i++) {
            if (has_clj_ext(all[i])) jd->entries[got++] = all[i];
            else free(all[i]);                     // drop non-source entries
        }
        free(all);
        jd->n_entries = got;
        // Trim the array to the kept source entries -- a jar is mostly `.class',
        // so the full-count allocation would otherwise pin unused slots all
        // session (the comment's "bounds memory" only holds with this).
        if (got) {
            jd->entries = realloc(jd->entries, got * sizeof(char *));
            if (got > 1) qsort(jd->entries, got, sizeof(char *), cstr_cmp);
        } else {
            free(jd->entries);
            jd->entries = NULL;
        }
    }
    if (ws->n_jar_dirs == ws->cap_jar_dirs) {
        ws->cap_jar_dirs = ws->cap_jar_dirs ? ws->cap_jar_dirs * 2 : 8;
        ws->jar_dirs = realloc(ws->jar_dirs, ws->cap_jar_dirs * sizeof(JarDir *));
    }
    ws->jar_dirs[ws->n_jar_dirs++] = jd;
    strmap_put(&ws->jar_map, jd->jar_path, jd);    // key borrows `jd->jar_path'
    return jd;
}

// Binary-search JD's sorted source-entry names for ENTRY.
static int jar_has_entry(JarDir *jd, const char *entry) {
    size_t lo = 0, hi = jd->n_entries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(jd->entries[mid], entry);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

// Probe REL (the ns-munged relative path) inside the jar at JAR for each source
// extension; return the FileNode for the entry that declares NS, or NULL.  The
// jar's source-entry directory is cached on first touch (jar_get_or_load), so a
// candidate that is absent costs an in-memory binary search -- no jar I/O -- and
// only a real hit reads + parses the entry.  The entry path is the jar-internal
// name ("clojure/string.clj"); the FileNode is keyed by the synthetic
// "<jar>!<entry>" so jar surfaces never collide with disk paths and are cached
// immutably (index_jar_entry).
static FileNode *resolve_ns_in_jar(Workspace *ws, const char *jar, const char *rel,
                                   const char *ns, unsigned mask) {
    JarDir *jd = jar_get_or_load(ws, jar);
    if (jd->n_entries == 0) return NULL;               // empty or unreadable jar
    for (size_t e = 0; CLJ_EXTS[e]; e++) {
        char entry[2200], key[8192];
        int n1 = snprintf(entry, sizeof entry, "%s%s", rel, CLJ_EXTS[e]);
        if (n1 < 0 || (size_t)n1 >= sizeof entry) continue;
        if ((dialect_mask(entry) & mask) == 0) continue;   // disjoint platforms
        if (!jar_has_entry(jd, entry)) continue;           // absent: no jar I/O
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
            FileNode *o = index_disk_file(ws, path, st.st_mtime, ANALYSIS_SURFACE);
            if (o && !o->opaque && o->index.ns_name &&
                strcmp(o->index.ns_name, ns) == 0)
                return o;   // exists and declares NS
        }
    }
    return NULL;
}

// ===========================================================================
// Per-pass `resolve_ns' memo (ns, dialect mask) -> FileNode.
//
// `resolve_ns' over a directory classpath `stat's each (entry x extension)
// candidate before it finds (or fails to find) the file.  In a multi-file pass
// -- the cold `treejure-analyze' scan, where `build_require_graph' resolves every
// file's requires -- a namespace required by many files (`clojure.core', and
// every shared lib) would otherwise re-`stat' the source dirs once per requiring
// file.  This memo collapses repeats to one `resolve_ns' per (ns, mask) for the
// lifetime of a pass.  (`resolve_ns' already caches the resolved FileNode
// persistently in `ws->files', so the parse / jar I/O happens once regardless;
// the memo additionally elides the repeated candidate `stat's.)  It is NOT
// persistent: disk state can change between passes, so a fresh memo is created
// per scan and freed at its end (a single-file check passes NULL -- no ns repeats
// within one file, so it gets nothing from a memo, and the hot interactive path
// stays allocation-free).
//
// A FileNode pointer stays valid across further indexing within a pass: nodes are
// individually heap-allocated and interned for the session (never moved or
// freed), and re-indexing a dep from its unchanged on-disk bytes re-derives the
// same surface -- so caching the pointer is safe even when the dep is itself
// re-scanned later in the same pass.
//
// Keyed on (ns, mask): a `.clj' (mask clj) and a `.cljs' (mask cljs) file can
// resolve the same ns to different files (foo.clj vs foo.cljs), so the dialect
// mask is part of the key.  The cached value is the SKIP-FREE resolution
// (`skip_path' = NULL); `resolve_ns_memo' re-applies any caller `skip_path' on
// top.  `skip_path' only ever removes the requiring file itself as a candidate,
// which changes the answer solely for a self-require (the ns resolves to the very
// file asking) -- detected as `cached->path == skip_path' and recomputed without
// polluting the memo, so the memo is behaviour-identical to a bare `resolve_ns'.
typedef struct {
    char     **names;
    unsigned  *masks;
    FileNode **deps;
    size_t     n, cap;
} NsCache;

static FileNode *ns_cache_lookup(NsCache *c, const char *ns, unsigned mask, int *found) {
    for (size_t i = 0; i < c->n; i++)
        if (c->masks[i] == mask && strcmp(c->names[i], ns) == 0) {
            *found = 1; return c->deps[i];
        }
    *found = 0;
    return NULL;
}

static void ns_cache_store(NsCache *c, const char *ns, unsigned mask, FileNode *dep) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->names = realloc(c->names, c->cap * sizeof(char *));
        c->masks = realloc(c->masks, c->cap * sizeof(unsigned));
        c->deps  = realloc(c->deps,  c->cap * sizeof(FileNode *));
    }
    c->names[c->n] = strdup(ns);
    c->masks[c->n] = mask;
    c->deps[c->n]  = dep;
    c->n++;
}

static void ns_cache_free(NsCache *c) {
    for (size_t i = 0; i < c->n; i++) free(c->names[i]);
    free(c->names); free(c->masks); free(c->deps);
    c->names = NULL; c->masks = NULL; c->deps = NULL; c->n = c->cap = 0;
}

// `resolve_ns' through the per-pass memo CACHE (NULL CACHE -> resolve directly).
// See the NsCache comment for the (ns, mask) key and the `skip_path' correctness
// argument.
static FileNode *resolve_ns_memo(NsCache *cache, Workspace *ws,
                                 const char *const *dirs, size_t n_dirs,
                                 const char *ns, unsigned mask,
                                 const char *skip_path) {
    if (!cache || !ns) return resolve_ns(ws, dirs, n_dirs, ns, mask, skip_path);
    int found = 0;
    FileNode *r0 = ns_cache_lookup(cache, ns, mask, &found);
    if (!found) {
        r0 = resolve_ns(ws, dirs, n_dirs, ns, mask, NULL);   // canonical: no skip
        ns_cache_store(cache, ns, mask, r0);
    }
    // A self-require (the ns resolves to the file `skip_path' names) is the one
    // case where the skip changes the answer: recompute it, leaving the canonical
    // entry intact for every other requiring file.
    if (r0 && skip_path && r0->path && strcmp(r0->path, skip_path) == 0)
        return resolve_ns(ws, dirs, n_dirs, ns, mask, skip_path);
    return r0;
}

// Resolve F's forward require graph: for each require in its NsIndex, store the
// dependency FileNode `resolve_ns' finds over the workspace classpath (a source
// file or a jar entry), or NULL when it does not resolve.  This is the slice
// that **reintroduces check-time dependency I/O** -- reads each needed dep
// lazily, mtime-gated by index_disk_file (jars are immutable per session), so an
// unchanged dep is reused and a changed one re-read (pull-based staleness, PLAN
// fact #4).  It is full-tier only and emits NO diagnostic: an unresolved require
// just leaves a NULL edge.  The dependency-reading Tier-2 lints
// (unresolved-namespace, undefined var) and project-wide find-usages will read
// these edges next (the require graph is their seed); `treejure-requires' exposes
// them for inspection.  Skips F itself (skip_path) so a require never resolves to
// the live buffer's stale on-disk copy.  CACHE is the optional per-pass
// `resolve_ns' memo (NULL for a single-file check; the cold scan shares one across
// all files, so a shared lib is resolved at most once for the whole scan).
static void build_require_graph(Workspace *ws, FileNode *f, NsCache *cache) {
    NsIndex *ix = &f->index;
    if (ws->n_classpath == 0) return;          // no classpath -> nothing to resolve
    unsigned mask = dialect_mask(f->path);
    for (size_t i = 0; i < ix->n_requires; i++) {
        ReqSpec *r = &ix->requires[i];
        if (!r->ns) continue;
        r->resolved = resolve_ns_memo(cache, ws, (const char *const *)ws->classpath,
                                      ws->n_classpath, r->ns, mask, f->path);
    }
}

// ===========================================================================
// Dependency-reading Tier-2 facts (full tier): undefined-var,
// unresolved-namespace, and the `:unresolved' face.
//
// All consume the require graph (build_require_graph) plus the cross-ns / bare
// usages the scope pass recorded (push_usage).  Two false-positive postures:
//   * `undefined-var' fires ONLY on a require that DID resolve (a non-NULL edge,
//     or the file's own ns) -- a missing var in a dep we actually read is real
//     regardless of how complete the classpath is, so it is emitted
//     UNCONDITIONALLY.  A library dep under the interim source-dirs-only
//     classpath stays a NULL edge and is skipped, so this never fires on one.
//   * `unresolved-namespace' (a NULL edge) and the `:unresolved' face on a BARE
//     symbol need exhaustive, jar-inclusive knowledge -- a library require is an
//     unavoidable NULL edge and a bare core var lives in a jar -- so they are
//     emitted ONLY when ws->classpath_complete (the JVM oracle's classpath,
//     PLAN step 6).  Until then the interim Elisp client leaves the flag nil and
//     these stay silent.
// ===========================================================================

// Does DEP define a var named NAME[0,LEN)?  Scans its distilled var surface.
static int dep_defines_var(FileNode *dep, const char *name, size_t len) {
    for (size_t v = 0; v < dep->index.n_vars; v++)
        if (strlen(dep->index.vars[v].name) == len &&
            memcmp(dep->index.vars[v].name, name, len) == 0)
            return 1;
    return 0;
}

// Find the var named NAME[0,LEN) in IX, preferring a real definition over a bare
// `(declare ...)` of the same name (so jump-to-def lands on the def, not the
// forward decl).  Returns NULL when no var of that name is defined.
static const VarDef *find_vardef(NsIndex *ix, const char *name, size_t len) {
    const VarDef *decl = NULL;
    for (size_t v = 0; v < ix->n_vars; v++)
        if (strlen(ix->vars[v].name) == len &&
            memcmp(ix->vars[v].name, name, len) == 0) {
            if (!ix->vars[v].declared) return &ix->vars[v];
            if (!decl) decl = &ix->vars[v];
        }
    return decl;
}

// The first require whose resolved namespace equals NS, or NULL.
static ReqSpec *find_require_by_ns(NsIndex *ix, const char *ns) {
    for (size_t i = 0; i < ix->n_requires; i++)
        if (ix->requires[i].ns && strcmp(ix->requires[i].ns, ns) == 0)
            return &ix->requires[i];
    return NULL;
}

// NAME[0,LEN) equals one of SET (a NULL-terminated C-string array).
static int cstr_in_set(const char *name, size_t len, const char *const *set) {
    for (size_t i = 0; set[i]; i++)
        if (strlen(set[i]) == len && memcmp(set[i], name, len) == 0) return 1;
    return 0;
}

// Heuristic: a bare symbol that must NOT be flagged `:unresolved' because it is
// (likely) Java interop or a reader-introduced name, not a var reference.  Only
// ever suppresses the face -- never adds one -- so erring broad here is safe.
static int looks_non_var(const char *name, size_t len) {
    if (len == 0) return 1;
    char c0 = name[0];
    if (c0 == '%' || c0 == '.' || c0 == '&') return 1;   // fn-lit arg / .method / &
    if (c0 >= 'A' && c0 <= 'Z') return 1;                 // Class-like
    if (name[len - 1] == '.') return 1;                   // Ctor.
    // A dotted bare name is Java interop (a package/class path like
    // `java.util.Date`) only when a segment is capitalized; a purely lowercase
    // dotted symbol (`foo.bar`) is treated as a genuine -- possibly unresolved
    // -- reference rather than blanket-suppressed (lowercase-only class paths
    // effectively do not occur).
    if (memchr(name, '.', len))
        for (size_t i = 1; i < len; i++)
            if (name[i] >= 'A' && name[i] <= 'Z') return 1;
    return 0;
}

// The implicit core namespace for PATH's dialect (every Clojure file refers it),
// or NULL when its core var set is not modeled.
static const char *core_ns_for(const char *path) {
    const char *feat = dialect_feature(path);   // clj/cljs/cljr/cljd (cljc -> clj)
    if (strcmp(feat, "cljs") == 0) return "cljs.core";
    if (strcmp(feat, "clj") == 0)  return "clojure.core";
    return NULL;   // cljr/cljd: core var set not modeled yet -> never flag bare
}

// Any require that brings vars in by a wildcard (`:refer :all' / bare `:use')?
// When so, a bare symbol might come from it, so we cannot call it unresolved --
// suppress the `:unresolved' face for bare candidates entirely.
static int has_refer_all(NsIndex *ix) {
    for (size_t i = 0; i < ix->n_requires; i++)
        if (ix->requires[i].refer_all) return 1;
    return 0;
}

// `unresolved-namespace' (gated): a require whose forward edge is NULL -- it
// resolved to nothing on the (complete) classpath.
static void lint_unresolved_namespace(Workspace *ws, FileNode *f) {
    if (!ws->classpath_complete) return;
    NsIndex *ix = &f->index;
    for (size_t i = 0; i < ix->n_requires; i++) {
        ReqSpec *r = &ix->requires[i];
        if (!r->ns || r->resolved) continue;
        // `:as-alias' is a non-loading alias -- the namespace need not exist
        // (its purpose is to alias an as-yet-unwritten / keyword-only ns), so a
        // NULL edge there is not an error (clj-kondo never flags it).
        if (r->as_alias) continue;
        push_diag(f, r->start, r->end, SEV_WARNING, DIAG_UNRESOLVED_NAMESPACE,
                  msg_printf("unresolved namespace %s", r->ns));
    }
}

// Resolve every recorded cross-ns / bare usage against the require graph and
// (for bare candidates) the implicit core namespace -- emitting `undefined-var'
// (unconditional, resolved deps only) and the `:unresolved' face (gated; see the
// section comment).  A var defined only by a macro we cannot expand (PLAN fact
// #3) is absent from the dep's distilled surface, so it would mis-flag --
// `:lint-as'/hooks and `replique-clojure-extra-def-forms' are the escape hatch
// until macro knowledge lands.
// CACHE is the optional per-pass `resolve_ns' memo (see build_require_graph):
// the implicit core ns is resolved once per file here, but sharing the cold
// scan's memo also collapses the core resolution across all scanned files.
static void resolve_var_usages(Workspace *ws, FileNode *f, NsCache *cache) {
    NsIndex *ix = &f->index;
    int refer_all = has_refer_all(ix);
    // Resolve the implicit core ns at most once per pass (the hot case: every
    // bare core symbol hits it).  Only sound -- and only reached -- under a
    // complete classpath; resolve_ns caches the FileNode, so this is one lookup.
    FileNode *core_dep = NULL;
    int core_resolved = 0;

    for (size_t u = 0; u < f->nusages; u++) {
        VarUsage *use = &f->usages[u];
        size_t nlen = strlen(use->name);
        if (use->ns) {                              // qualified / `:refer'-ed
            FileNode *dep = NULL;
            if (ix->ns_name && strcmp(use->ns, ix->ns_name) == 0) {
                dep = f;                            // fully-qualified self-ref
            } else {
                ReqSpec *r = find_require_by_ns(ix, use->ns);
                if (r && r->resolved) dep = r->resolved;
            }
            if (!dep || dep->opaque) continue;      // unresolved/source-less: not our call
            if (dep_defines_var(dep, use->name, nlen)) continue;
            push_diag(f, use->start, use->end, SEV_WARNING, DIAG_UNDEFINED_VAR,
                      msg_printf("var %s/%s is undefined", use->ns, use->name));
            if (ws->classpath_complete)
                push_span(f, use->start, use->end, CAT_UNRESOLVED);
        } else {                                    // bare unresolved candidate
            if (!ws->classpath_complete) continue;  // gated: could be a core var
            if (refer_all) continue;                // a wildcard could provide it
            if (looks_non_var(use->name, nlen)) continue;
            if (cstr_in_set(use->name, nlen, CORE_FORMS)) continue;  // special form
            if (!core_resolved) {
                const char *core = core_ns_for(f->path);
                core_dep = core
                    ? resolve_ns_memo(cache, ws, (const char *const *)ws->classpath,
                                      ws->n_classpath, core, dialect_mask(f->path), f->path)
                    : NULL;
                core_resolved = 1;
            }
            // If the implicit core ns is not resolvable -- a partial/misconfigured
            // "complete" classpath, or a dialect whose core we do not model
            // (cljr/cljd, or cljs with no cljs.core on the path) -- we cannot tell
            // a core var from a genuine unknown, so suppress the face entirely
            // rather than flag every bare core symbol.  A truly complete classpath
            // resolves its core, so a real unknown is still painted.
            if (!core_dep || core_dep->opaque) continue;
            if (dep_defines_var(core_dep, use->name, nlen)) continue;  // a core var
            push_span(f, use->start, use->end, CAT_UNRESOLVED);
        }
    }
}

// Run the buffer-local analysis on F's current tree.  The scope pass and every
// buffer-only fact reach NO `resolve_ns' call and read NO dependencies -- at
// either tier (the scope pass only RECORDS cross-ns usages at the full tier; it
// resolves none).  CROSS_FILE gates the FULL tier, which:
//   * emits the buffer-determinable Tier-2 lints (`unused-namespace' /
//     `unused-referred-var', dependency-free -- here only so they do not flash on
//     every after-edit check);
//   * builds the forward **require graph** (build_require_graph) -- the first
//     check-time dependency I/O; and
//   * runs the **dependency-reading Tier-2 facts** that consume it:
//     `unresolved-namespace' (gated), `undefined-var' (unconditional, resolved
//     deps only), and the `:unresolved' face (gated) -- see resolve_var_usages.
// The fast tier stays dependency-I/O-free.  Cross-namespace jump-to-def still
// resolves on its own in the point query (resolve_cross_ns).  CACHE is the
// optional per-pass `resolve_ns' memo passed to the full-tier resolvers (NULL for
// a single-file check; the cold scan threads one shared memo through every file).
static void analyze_file(Workspace *ws, FileNode *f, int cross_file, NsCache *cache) {
    filenode_clear_outputs(f);
    if (!f->tree) return;
    // Guard F against being re-read from disk by a dependency resolution it
    // triggers (a self-require under the per-pass memo's skip-free resolve); see
    // `Workspace.analyzing'.  Saved/restored so nested analysis (none today) is safe.
    FileNode *prev_analyzing = ws->analyzing;
    ws->analyzing = f;
    TSNode root = ts_tree_root_node(f->tree);

    // --- Buffer-only facts (both tiers; no dependency I/O) -----------------
    collect_grammar_diags(f, root);
    analyze_requires(ws, f, f->text, root);
    extract_var_defs(ws, f, f->text, root, 1);   // live buffer: record def navs
    lint_redefined_var(f);

    Analyzer a = { .ws = ws, .f = f, .text = f->text,
                   .locals = NULL, .nlocals = 0, .cap_locals = 0,
                   .next_local_id = 0, .cross_file = cross_file,
                   .in_unbound_body = 0 };
    analyze_body(&a, root, 0);   // top level: every form is a reference context
    free(a.locals);

    // --- Full-tier-only facts --------------------------------------------
    if (cross_file) {
        // `unused-namespace' / `unused-referred-var': buffer-determinable -- the
        // scope pass already flagged each used require / referred var above, so
        // this reads no dependencies.  Emitted only here (not the fast tier) so
        // it tracks the PLAN's Tier-2 cadence and does not flash on edit.
        lint_unused_requires(f);
        // The forward require graph: resolve each require to its dependency
        // FileNode over the classpath (lazy, mtime-gated).  This DOES read
        // dependencies -- the first check-time dependency I/O -- but emits no
        // diagnostic itself; it is the seed the dependency-reading Tier-2 facts
        // below consume.
        build_require_graph(ws, f, cache);
        // The dependency-reading Tier-2 facts that consume the graph: a require
        // with a NULL edge (`unresolved-namespace', gated), a qualified/`:refer'-ed
        // var its resolved dep does not define (`undefined-var', unconditional --
        // resolved deps only), and the `:unresolved' face (gated).  See the
        // section comment above resolve_var_usages for the false-positive posture.
        lint_unresolved_namespace(ws, f);
        resolve_var_usages(ws, f, cache);
    }

    if (f->nspans > 1)
        qsort(f->spans, f->nspans, sizeof(SemanticSpan), span_cmp);
    ws->analyzing = prev_analyzing;
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

// Extract a 0-based byte-offset arg.  Returns -1 -- so the caller bails to nil
// rather than proceed under a pending signal / with a wrapped `uint32_t' -- when
// the arg is not an integer (`extract_integer' left a non-local exit) or is
// negative (an out-of-range position).  Mirrors the bail-on-non-local-exit
// discipline of copy_lisp_string / copy_string_seq for the numeric args.
static intmax_t extract_byte_arg(emacs_env *env, emacs_value v) {
    intmax_t n = env->extract_integer(env, v);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) return -1;
    return n < 0 ? -1 : n;
}

// (treejure-init PROJECT &optional CLASSPATH CLASSPATH-COMPLETE) -> workspace
// user-ptr.  CLASSPATH-COMPLETE non-nil marks CLASSPATH as the exhaustive,
// jar-inclusive closure (the oracle's output) -- it gates the diagnostics that
// would otherwise false-positive under the interim source-dirs-only classpath
// (see Workspace.classpath_complete).  The interim Elisp client leaves it nil.
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

    if (nargs > 2) ws->classpath_complete = env->is_not_nil(env, args[2]);

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
// CROSS-FILE-P flag is routed to analyze_file.  The fast tier (nil) is
// buffer-only (no dependency I/O); the full tier (t) adds the buffer-determinable
// `unused-namespace' / `unused-referred-var' lints, builds the forward require
// graph (build_require_graph) -- the first check-time dependency I/O (lazy,
// mtime-gated) -- and runs the dependency-reading Tier-2 facts that consume it:
// `undefined-var' (unconditional, resolved deps only) plus, when the workspace is
// `classpath_complete', `unresolved-namespace' and the `:unresolved' face.
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
    analyze_file(ws, f, cross_file, NULL);  // single file: no ns repeats -> no memo
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

    intmax_t beg = extract_byte_arg(env, args[2]);
    intmax_t end = extract_byte_arg(env, args[3]);
    if (beg < 0 || end < 0) return Qnil(env);

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
        const VarDef *vd = find_vardef(ix, nm, nlen);
        if (vd) { if (out) *out = vd; return f; }
        return NULL;
    }

    FileNode *dep = resolve_ns(ws, (const char *const *)ws->classpath,
                               ws->n_classpath, target_ns,
                               dialect_mask(f->path), f->path);
    free(lit);                         // resolve_ns copied what it needed
    if (!dep) return NULL;
    const VarDef *vd = find_vardef(&dep->index, nm, nlen);
    if (vd) { if (out) *out = vd; return dep; }
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

    intmax_t byte_arg = extract_byte_arg(env, args[2]);
    if (byte_arg < 0) return Qnil(env);
    uint32_t byte = (uint32_t)byte_arg;
    NavRef *r = nav_at(f, byte);
    if (r && r->kind == NAV_LOCAL) {
        for (size_t i = 0; i < f->nnavs; i++) {
            NavRef *n = &f->navs[i];
            if (n->kind == NAV_LOCAL && n->is_def && n->local_id == r->local_id)
                return location_to_lisp(env, f, n->start, n->end);
        }
    } else if (r && r->name) {
        const VarDef *vd = find_vardef(&f->index, r->name, strlen(r->name));
        if (vd) return location_to_lisp(env, f, vd->name_start, vd->name_end);
    }
    // Not an in-file local/var: try cross-namespace resolution.
    TSNode sym = symbol_at_byte(f, byte);
    if (ts_node_is_null(sym)) return Qnil(env);
    return resolve_cross_ns(env, ws, f, sym);
}

// ===========================================================================
// Project-wide find-usages (build-order step 5) -- the cross-file reference
// scan over a per-call search scope.
//
// `treejure-references' resolves the var at point to a canonical identity
// (defining-ns, name) and returns EVERY occurrence of that var across the
// chosen scope, filtered by resolved identity (not text) -- so a same-name
// var in another namespace, or a shadowing local, never matches.  Locals stay
// buffer-scoped (a local cannot escape its file).
//
// The scope (SCOPE-DIRS) is a per-call directory/file list chosen by the
// consumer (the classpath, the known WS files, or a picked dir -- see the
// search-scope note in PLAN); nothing about it is persisted.  Each scope file
// is read + walked by the SAME buffer-only scope pass the live check uses
// (analyze_file_usages), then matched: a file declaring the canonical ns
// contributes its def + same-ns usages (NAV_VAR by name); ANY file contributes
// its cross-ns usages whose alias/refer-resolved target ns equals the canonical
// ns (VarUsage).  Both are buffer-determinable -- the scan reaches no
// `resolve_ns' beyond the one point query that fixes the identity, and reads no
// jars.  This is the PLAN's "global var-usages index, a one-time scan deferred
// to first use"; here it is recomputed per call (an explicit, user-initiated
// command, not the hot path) -- a session cache is a later optimization.
// ===========================================================================

// A growable list of file paths (the collected scope).
typedef struct { char **v; size_t n, cap; } PathVec;
static void pathvec_push(PathVec *pv, char *p) {
    if (pv->n == pv->cap) {
        pv->cap = pv->cap ? pv->cap * 2 : 32;
        pv->v = realloc(pv->v, pv->cap * sizeof(char *));
    }
    pv->v[pv->n++] = p;
}

// Recursively collect clj-family source files under DIR into PV (skipping
// dotfiles / dot-directories).  Plain `opendir'/`readdir' -- no `env', so this
// could later run on the cold-scan worker thread (PLAN execution model).  DEPTH
// is bounded: `stat' follows directory symlinks, so a cyclic symlink (a dir
// linking to an ancestor) would otherwise recurse forever -- the cap stops the
// stack overflow while still descending any real source tree.
#define COLLECT_MAX_DEPTH 64
static void collect_dir(const char *dir, PathVec *pv, int depth) {
    if (depth > COLLECT_MAX_DEPTH) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;             // ., .., hidden
        size_t need = strlen(dir) + 1 + strlen(e->d_name) + 1;
        char *full = malloc(need);
        snprintf(full, need, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) { collect_dir(full, pv, depth + 1); free(full); }
            else if (S_ISREG(st.st_mode) && has_clj_ext(full)) pathvec_push(pv, full);
            else free(full);
        } else free(full);
    }
    closedir(d);
}

// Resolve SCOPE-DIRS to a sorted, de-duplicated list of source-file paths.  A
// directory entry is descended; a clj-family file entry is taken directly (so a
// "workspace files" scope can pass file paths); a jar entry is skipped
// (find-usages is over the project's own source, not library jars).
static void collect_scope(const char *const *dirs, size_t n, PathVec *pv) {
    for (size_t i = 0; i < n; i++) {
        if (is_jar_path(dirs[i])) continue;
        struct stat st;
        if (stat(dirs[i], &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) collect_dir(dirs[i], pv, 0);
        else if (S_ISREG(st.st_mode) && has_clj_ext(dirs[i]))
            pathvec_push(pv, strdup(dirs[i]));
    }
    if (pv->n > 1) qsort(pv->v, pv->n, sizeof(char *), cstr_cmp);
}

// Append G's occurrences of the var (CANON_NS, NAME[0,NLEN)) to ITEMS/*K:
//   * if G declares CANON_NS, its def + same-ns usages (NAV_VAR by name);
//   * any cross-ns usage of it (VarUsage whose resolved target ns == CANON_NS).
// ITEMS must hold at least G->nnavs + G->nusages more slots.
static void harvest_matches(emacs_env *env, FileNode *g, const char *canon_ns,
                            const char *name, size_t nlen,
                            emacs_value *items, size_t *k) {
    if (g->index.ns_name && strcmp(g->index.ns_name, canon_ns) == 0) {
        for (size_t i = 0; i < g->nnavs; i++) {
            NavRef *nv = &g->navs[i];
            if (nv->kind == NAV_VAR && nv->name &&
                strlen(nv->name) == nlen && memcmp(nv->name, name, nlen) == 0)
                items[(*k)++] = location_to_lisp(env, g, nv->start, nv->end);
        }
    }
    for (size_t i = 0; i < g->nusages; i++) {
        VarUsage *u = &g->usages[i];
        if (u->ns && strcmp(u->ns, canon_ns) == 0 &&
            strlen(u->name) == nlen && memcmp(u->name, name, nlen) == 0)
            items[(*k)++] = location_to_lisp(env, g, u->start, u->end);
    }
}

// (treejure-references WS FILE BYTE &optional SCOPE-DIRS) -> list of locations.
// Every occurrence of the local or var at BYTE, by resolved identity.
//   * A LOCAL is buffer-scoped (it cannot cross files): binding + usages, matched
//     by binding id (shadowing-correct) -- SCOPE-DIRS is ignored.
//   * A VAR is resolved to its canonical (defining-ns, name) -- an in-file def /
//     same-ns usage uses this file's ns; an aliased/qualified/`:refer'-ed usage
//     resolves cross-namespace (resolve_cross_ns_var, the one point query).  With
//     no SCOPE-DIRS the result is buffer-scoped (this file's occurrences only);
//     with SCOPE-DIRS every file in that scope is scanned and matched.
static emacs_value f_references(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    FileNode *f = ws_find_file(ws, path);
    free(path);
    if (!f || !f->tree) return Qnil(env);   // no analysis -> nothing to resolve
                                            // (also guards symbol_at_byte below)

    intmax_t byte_arg = extract_byte_arg(env, args[2]);
    if (byte_arg < 0) return Qnil(env);
    uint32_t byte = (uint32_t)byte_arg;
    NavRef *r = nav_at(f, byte);

    // --- Locals: buffer-scoped, matched by binding id (the original path). ---
    if (r && r->kind == NAV_LOCAL) {
        emacs_value *items = malloc(f->nnavs * sizeof(emacs_value));
        size_t k = 0;
        for (size_t i = 0; i < f->nnavs; i++) {
            NavRef *n = &f->navs[i];
            if (n->kind == NAV_LOCAL && n->local_id == r->local_id)
                items[k++] = location_to_lisp(env, f, n->start, n->end);
        }
        emacs_value res = (k == 0) ? Qnil(env)
            : env->funcall(env, env->intern(env, "list"), (ptrdiff_t)k, items);
        free(items);
        return res;
    }

    // --- An in-file var in a file with NO `ns' form has no canonical
    // cross-file identity; answer buffer-scoped by name (the pre-step-5
    // behavior), so M-? still works in a scratch/ns-less buffer. ---
    if (r && r->name && !f->index.ns_name) {
        emacs_value *items = malloc(f->nnavs * sizeof(emacs_value));
        size_t k = 0;
        for (size_t i = 0; i < f->nnavs; i++) {
            NavRef *n = &f->navs[i];
            if (n->kind == NAV_VAR && n->name && strcmp(n->name, r->name) == 0)
                items[k++] = location_to_lisp(env, f, n->start, n->end);
        }
        emacs_value res = (k == 0) ? Qnil(env)
            : env->funcall(env, env->intern(env, "list"), (ptrdiff_t)k, items);
        free(items);
        return res;
    }

    // --- Var: resolve to a canonical (defining-ns, name).  Own both strings, so
    // they survive the scope scan re-indexing the dep that produced them. ---
    char *canon_ns = NULL, *target_name = NULL;
    if (r && r->name) {                       // in-file def or same-ns usage
        canon_ns = strdup(f->index.ns_name);  // non-NULL here (guarded above)
        target_name = strdup(r->name);
    } else {                                  // aliased/qualified/`:refer'-ed
        TSNode sym = symbol_at_byte(f, byte);
        if (!ts_node_is_null(sym)) {
            const VarDef *vd = NULL;
            FileNode *dep = resolve_cross_ns_var(ws, f, sym, &vd);
            if (dep && vd && dep->index.ns_name) {
                canon_ns = strdup(dep->index.ns_name);
                target_name = strdup(vd->name);
            }
        }
    }
    if (!canon_ns || !target_name) { free(canon_ns); free(target_name); return Qnil(env); }
    size_t nlen = strlen(target_name);

    // --- Collect the scope (empty -> buffer-only). ---
    PathVec scope = {0};
    if (nargs >= 4) {
        size_t n_dirs; char **dirs = copy_string_seq(env, args[3], &n_dirs);
        if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
            for (size_t i = 0; i < n_dirs; i++) free(dirs[i]);
            free(dirs); free(canon_ns); free(target_name);
            return Qnil(env);
        }
        if (dirs) {
            collect_scope((const char *const *)dirs, n_dirs, &scope);
            for (size_t i = 0; i < n_dirs; i++) free(dirs[i]);
            free(dirs);
        }
    }

    // Results, grown lazily: each harvested file needs at most nnavs+nusages
    // more slots (ENSURE reserves them before harvest_matches fills).
    emacs_value *items = NULL; size_t k = 0, items_cap = 0;
    #define ENSURE(extra) do { \
        if (k + (extra) > items_cap) { \
            items_cap = (k + (extra)) * 2 + 16; \
            items = realloc(items, items_cap * sizeof(emacs_value)); \
        } } while (0)

    // The query file's own occurrences (always included, scope or not).
    ENSURE(f->nnavs + f->nusages);
    harvest_matches(env, f, canon_ns, target_name, nlen, items, &k);

    // Each other scope file: a live node is used as-is (never clobber live text);
    // a disk node is resolved through the mtime-gated USAGES cache -- analyzed on
    // first touch, then REUSED untouched on a later find-usages / jump-to-def
    // whose mtime is unchanged (the session cache).  index_disk_file keeps only
    // the light navs/usages, dropping the tree/text, so retained memory is bounded
    // by symbols rather than parsed trees; a file changed on disk is re-read.
    for (size_t i = 0; i < scope.n; i++) {
        const char *sp = scope.v[i];
        if (i > 0 && strcmp(sp, scope.v[i - 1]) == 0) continue;   // de-dup
        if (strcmp(sp, f->path) == 0) continue;                   // already done
        FileNode *g = ws_find_file(ws, sp);
        if (g && g->live) {
            ENSURE(g->nnavs + g->nusages);
            harvest_matches(env, g, canon_ns, target_name, nlen, items, &k);
            continue;
        }
        struct stat st;
        if (stat(sp, &st) != 0) continue;
        g = index_disk_file(ws, sp, st.st_mtime, ANALYSIS_USAGES);
        if (!g || g->opaque) continue;
        ENSURE(g->nnavs + g->nusages);
        harvest_matches(env, g, canon_ns, target_name, nlen, items, &k);
    }
    #undef ENSURE

    for (size_t i = 0; i < scope.n; i++) free(scope.v[i]);
    free(scope.v);
    free(canon_ns); free(target_name);

    emacs_value res = (k == 0) ? Qnil(env)
        : env->funcall(env, env->intern(env, "list"), (ptrdiff_t)k, items);
    free(items);
    return res;
}

// ===========================================================================
// Cold full analysis (build-order step 5) -- `treejure-analyze'.
//
// An explicit, user-initiated command that analyzes a chosen scope up front
// (otherwise analysis stays lazy, per buffer / per query).  It collects every
// clj-family source file under SCOPE-DIRS -- the same per-call scope model as
// find-usages (classpath / known WS files / a picked dir, not persisted) -- and
// runs the FULL-tier analysis (analyze_file, cross_file=1) on each: the same
// diagnostics + require graph + dependency-reading Tier-2 lints the live
// full-tier check runs, reading the dependency closure.  It returns only an
// aggregate summary `(:files N :diagnostics M)'; per-file diagnostics are still
// pulled lazily via treejure-check-buffer when a file is opened.  A scanned disk
// file is left warm at ANALYSIS_USAGES (tree/text dropped) so a subsequent
// find-usages / jump-to-def reuses it (the session cache); an open analyzed
// buffer is analyzed in place and kept live.  Reads no jars beyond what the
// require graph resolves.
// ===========================================================================

// Full-analyze the disk file at PATH for the cold scan: (re-)read + parse it, run
// analyze_file at the full tier to count its diagnostics, then prune the node to
// the ANALYSIS_USAGES session cache (mtime-gated) so it stays warm.  Returns the
// diagnostic count, or -1 when the file is a live buffer (the caller analyzes
// those in place) or could not be read as UTF-8 (marked opaque, not counted).
// Unlike the lazy index_disk_file path, the cold scan re-parses every file even
// when mtime-fresh: a cached USAGES node has no diagnostics (they are pruned), so
// the count is only available from a fresh full-tier analysis -- appropriate for
// an explicit, one-shot command.  CACHE is the cold scan's shared per-pass
// `resolve_ns' memo, threaded into analyze_file's full-tier resolvers.
static intmax_t analyze_scope_file(Workspace *ws, const char *path, time_t mtime,
                                   NsCache *cache) {
    FileNode *f = ws_find_file(ws, path);
    if (f && f->live) return -1;             // a live buffer: caller handles it
    size_t len; char *buf = read_file(path, &len);
    if (!buf) return -1;
    if (!f) f = ws_intern_file(ws, path);
    if (!is_valid_utf8(buf, len)) {          // non-UTF-8 dep -> opaque, not counted
        free(buf);
        filenode_clear_outputs(f);
        if (f->tree) { ts_tree_delete(f->tree); f->tree = NULL; }
        free(f->text); f->text = NULL; f->len = 0;
        f->opaque = 1;
        f->analysis_level = ANALYSIS_NONE;
        f->indexed_mtime = mtime;
        return -1;
    }
    f->opaque = 0;
    filenode_reparse(ws, f, buf, len);       // takes ownership of `buf`
    analyze_file(ws, f, 1, cache);            // full tier: diags + graph + lints
    intmax_t nd = (intmax_t)f->ndiags;
    prune_to_usages_cache(f);                 // drop heavy state, keep USAGES cache
    f->indexed_mtime = mtime;
    return nd;
}

// (treejure-analyze WS SCOPE-DIRS) -> (:files N :diagnostics M).  See the section
// comment above: a cold full-tier scan over the chosen scope, returning aggregate
// counts and warming the session cache.
static emacs_value f_analyze(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);

    PathVec scope = {0};
    size_t n_dirs; char **dirs = copy_string_seq(env, args[1], &n_dirs);
    if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
        for (size_t i = 0; i < n_dirs; i++) free(dirs[i]);
        free(dirs);
        return Qnil(env);
    }
    if (dirs) {
        collect_scope((const char *const *)dirs, n_dirs, &scope);
        for (size_t i = 0; i < n_dirs; i++) free(dirs[i]);
        free(dirs);
    }

    // One per-pass `resolve_ns' memo shared across every file in the scan, so a
    // namespace required by many files (`clojure.core', shared libs) is resolved
    // at most once for the whole cold analysis (see the NsCache comment).
    NsCache scan_cache = {0};
    size_t n_files = 0;
    uintmax_t n_diags = 0;
    for (size_t i = 0; i < scope.n; i++) {
        const char *sp = scope.v[i];
        if (i > 0 && strcmp(sp, scope.v[i - 1]) == 0) continue;   // de-dup (sorted)
        FileNode *f = ws_find_file(ws, sp);
        if (f && f->live) {                       // open analyzed buffer: in place
            analyze_file(ws, f, 1, &scan_cache);
            n_files++; n_diags += f->ndiags;
            continue;
        }
        struct stat st;
        if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        intmax_t nd = analyze_scope_file(ws, sp, st.st_mtime, &scan_cache);
        if (nd >= 0) { n_files++; n_diags += (uintmax_t)nd; }
    }
    ns_cache_free(&scan_cache);

    for (size_t i = 0; i < scope.n; i++) free(scope.v[i]);
    free(scope.v);

    emacs_value pl[] = {
        env->intern(env, ":files"),       env->make_integer(env, (intmax_t)n_files),
        env->intern(env, ":diagnostics"), env->make_integer(env, (intmax_t)n_diags)
    };
    return env->funcall(env, env->intern(env, "list"), 4, pl);
}

// (treejure-requires WS FILE) -> list of (:ns NS :file PATH-or-nil) plists.
// FILE's forward require graph as built by the last FULL-tier check: each
// required namespace and the dependency file `resolve_ns' resolved it to over the
// classpath (a source path, or the synthetic "<jar>!<entry>" path for a jar
// dep), or nil when it did not resolve (unknown ns, or the graph was not built --
// only a fast-tier check has run).  A pure read of the cached edges -- it does NO
// I/O; run a full-tier `treejure-check-buffer' first to (re)build the graph.
// Exposes the require graph for inspection/tests; the dependency-reading lints
// read the same edges directly in C.
static emacs_value f_requires(emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *d) {
    Workspace *ws = env->get_user_ptr(env, args[0]);
    if (!ws) return Qnil(env);
    size_t plen; char *path = copy_lisp_string(env, args[1], &plen);
    if (!path) return Qnil(env);
    FileNode *f = ws_find_file(ws, path);
    free(path);
    if (!f) return Qnil(env);

    NsIndex *ix = &f->index;
    if (ix->n_requires == 0) return Qnil(env);
    emacs_value listf  = env->intern(env, "list");
    emacs_value k_ns   = env->intern(env, ":ns");
    emacs_value k_file = env->intern(env, ":file");
    emacs_value *items = malloc(ix->n_requires * sizeof(emacs_value));
    for (size_t i = 0; i < ix->n_requires; i++) {
        ReqSpec *r = &ix->requires[i];
        emacs_value ns_v = r->ns
            ? env->make_string(env, r->ns, (ptrdiff_t)strlen(r->ns)) : Qnil(env);
        emacs_value file_v = (r->resolved && r->resolved->path)
            ? env->make_string(env, r->resolved->path,
                               (ptrdiff_t)strlen(r->resolved->path))
            : Qnil(env);
        emacs_value pl[] = { k_ns, ns_v, k_file, file_v };
        items[i] = env->funcall(env, listf, 4, pl);
    }
    emacs_value res = env->funcall(env, listf, (ptrdiff_t)ix->n_requires, items);
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

    bind_fn(env, "treejure-init",            1, 3, f_init);
    bind_fn(env, "treejure-check-buffer",    4, 4, f_check_buffer);
    bind_fn(env, "treejure-semantic-faces",  4, 4, f_semantic_faces);
    bind_fn(env, "treejure-definition",      3, 3, f_definition);
    bind_fn(env, "treejure-references",      3, 4, f_references);
    bind_fn(env, "treejure-analyze",         2, 2, f_analyze);
    bind_fn(env, "treejure-requires",        2, 2, f_requires);
    bind_fn(env, "treejure-close-buffer",    2, 2, f_close_buffer);
    bind_fn(env, "treejure-jar-entry",       2, 2, f_jar_entry);
    bind_fn(env, "treejure-set-def-forms",   2, 2, f_set_def_forms);
    bind_fn(env, "treejure-category-names",  0, 0, f_category_names);
    bind_fn(env, "treejure-diagnostic-ids",  0, 0, f_diagnostic_ids);

    return 0;
}
