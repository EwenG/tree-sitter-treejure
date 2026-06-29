# Manual test bench — treejure semantic module

Open these files in Emacs with `replique-clojure-mode` active (the major mode
auto-enables `replique-clojure-semantic-mode` when the C module is available).
Things to watch:

- **Semantic-face overlays** layered over treesit's highlighting:
  - `:local` → `replique-clojure-local-face` (defaults to the variable-name
    face): a resolved local binding **and every usage of it**.
  - `:local-unused` → `replique-clojure-unused-face` (defaults to `shadow`,
    i.e. greyed out): a local binding that is never used in its scope.
  - `:special-form` / `:macro-invocation` → form-head faces: a special form /
    core macro head, and a known non-core macro head (a user def-form).
  - **No var face.** A resolved var (same- or cross-namespace) is colored by the
    treesit **syntax** layer, not the semantic overlay — so the overlay paints
    only locals + form heads, which also makes `:local` stand out. Var usages
    still resolve under the hood (that drives `M-.`); resolution just no longer
    drives a face.
- **Flymake diagnostics** (underlines; `M-x flymake-show-buffer-diagnostics`):
  - `unused-binding` warnings (one per greyed binding, except `_`-names).
  - `duplicate-require`, `refer-all`, `namespace-name-mismatch` warnings from
    the buffer-only `(ns ...)` require pass, and `redefined-var` from the
    var-definition pass.
1  - `unused-namespace` / `unused-referred-var` (**full tier only** — save /
    buffer-switch / `M-.`, not while typing): a required namespace whose alias /
    `:refer`-ed vars / fully-qualified name is never used in the file, and each
    `:refer`-ed / `:only` var never used. Buffer-determinable (no jars), so it
    works on your own files; a **plain** or **`:as-alias`** require is never
    flagged (clj-kondo parity).
  - Grammar-level errors: `syntax-error`, `missing-form`, `invalid-number`,
    `invalid-string`, `invalid-character`, `invalid-symbolic-value`.
- **Navigation** via `xref`:
  - `M-.` (`xref-find-definitions`):
    - on a **local usage** jumps to its binding; on a **same-namespace var
      usage** jumps to its `def`/`defn`. By identity, so under shadowing it
      lands on the right binding.
    - on an **aliased/qualified** (`u/helper`, `app.util/helper`) or
      **`:refer`-ed** var jumps to its definition **in the other file** —
      provided that namespace resolves under a project source dir
      (`replique-clojure-semantic-source-dirs`, default `src`/`test`).
    - on a **core / library** symbol (`+`, `clojure.string/join`) yields nothing
      yet — those live in jars (PLAN jar slice).
  - `M-?` (`xref-find-references`) lists in-file occurrences of the local
    (binding + usages) or same-ns var (def + usages). Var references are
    **buffer-scoped** for now (project-wide find-usages is a later slice).

What the module does **not** do yet (so don't expect it): the **dependency-
reading** cross-file *diagnostics* — undefined-var, unresolved-namespace, arity
checks — and the `:unresolved` *face*; and project-wide find-usages. Those need
the rest of the cross-file workspace tier (PLAN step 4+).
(`unused-namespace`/`unused-referred-var` **are** done — they need no dependency
reads.)

**Jar-backed resolution** now exists in the C module (the jar slice: it reads
`clojure.core` / library namespaces out of jars on the classpath), but this
bench does **not** exercise it by default: the interim Elisp classpath seeds
only your project **source dirs** (`replique-clojure-semantic-source-dirs`), not
jars — the JVM oracle supplies the full jar classpath later (PLAN step 6). So
`clojure.core`/library symbols do not resolve for `M-.` here by default; the jar
capability is verified by the module's own white-box harness instead. (Once a
jar IS on the classpath — e.g. via `manualtest-jar-check` below — `M-.` on a jar
var navigates *into* the jar entry's source, opened read-only: the jar-nav
slice.)

## Files

| File | Exercises |
|------|-----------|
| `src/manualtest/locals.clj`         | `:local` faces, unused greyout, `_` suppression |
| `src/manualtest/binding_forms.clj`  | every supported binding form (incl. uncalled fn self-name) |
| `src/manualtest/destructuring.clj`  | sequential + associative destructuring (incl. `:or` keys) |
| `src/manualtest/scoping.clj`        | shadowing, sequential scope, quote/discard opacity |
| `src/manualtest/syntax_quote.clj`   | syntax-quote: `~`/`~@` count as usages; nested-quote levels |
| `src/manualtest/ns_scope.clj`       | `(ns …)` form is data — no phantom var refs at require sites |
| `src/manualtest/requires.clj`       | `duplicate-require` / `refer-all` from the ns require pass |
| `src/manualtest/unused_requires.clj` | `unused-namespace` / `unused-referred-var` (full tier); alias/`::`-kw/syntax-quote (qualified **and** bare-`:refer`) count as usage; plain & `:as-alias` never flagged |
| `src/manualtest/redefinitions.clj`  | `redefined-var`; load-time descent (`do`/`when`) vs fn/quote/comment opacity |
| `src/manualtest/defmethods.clj`     | `defmethod` multifn is a *usage*: marks its require/`:refer` used (no false unused-require); resolves for `M-.` (no face); same-ns + cross-ns (`nt/area`, referred `shape-name` in `nav_target.clj`) |
| `src/manualtest/protocol_types.clj` | secondary var interning: `defprotocol`/`definterface` method vars + `deftype`/`defrecord` factory vars (`->Name`, `map->Name`) are interned so `M-.`/`M-?` reach their usages (no var face) |
| `src/manualtest/grammar_errors.clj` | all grammar-level diagnostics (this file is **intentionally broken**) |
| `src/manualtest/extra_def_forms.clj` | user macros analysed like `defn` (see `.dir-locals.el`) |
| `src/manualtest/cljs_demo.cljs`     | ClojureScript buffer (same local analysis) |
| `src/manualtest/reader_conditionals.cljc` | `.cljc` reader conditionals; branch-aware var defs (`:clj` honored) |
| `src/manualtest/cond_requires.cljc` | requires nested in a reader conditional (`#?`/`#?@` splicing) are extracted + linted for the file's dialect (`:clj`); diffs byte-identical to clj-kondo |
| `src/manualtest/global_vars.clj`    | the var-face decision: vars get **no** semantic face (treesit colors them), locals do; shadowing → `:local`; resolution still drives `M-.` |
| `src/manualtest/navigation.clj`     | `xref` `M-.` / `M-?`: locals, same-ns vars, cross-file (aliased/qualified/`:refer`-ed) jump-to-def |
| `src/manualtest/nav_target.clj`     | the cross-file jump **target** for `navigation.clj` + `defmethods.clj` (open only to confirm jumps arrive) |
| `src/manualtest/jar_resolution.clj` | **jar-backed** jump-to-def (clojure.string/clojure.set); needs `jar-manual-test.el` (`M-x manualtest-jar-check`) to put a jar on the classpath |

To try the **jar slice** in-editor, open `jar_resolution.clj`, then
`M-x load-file RET jar-manual-test.el RET` and `M-x manualtest-jar-check` — it
adds a real `clojure-*.jar` (auto-found under `~/.m2`) to that buffer's
classpath and re-checks, so the jar-backed usages light up. With the jar on the
classpath, `M-.` on a jar var (e.g. `str/upper-case`) now also navigates into
the jar entry's source (`clojure/string.clj`), opened read-only. `M-x
manualtest-jar-reset` drops it again. (This is bench-only scaffolding for the
gap until the JVM oracle supplies the real jar classpath.)

`.dir-locals.el` sets `replique-clojure-extra-def-forms` to `("defroute"
"defcomponent")` for this project; the semantic layer analyses those macros
like `defn` (params become locals).  `extra_def_forms.clj` shows it in action.

Each form is annotated with the faces/diagnostics it should produce. A quick
way to sanity-check the whole thing: `M-x flymake-show-project-diagnostics`.
