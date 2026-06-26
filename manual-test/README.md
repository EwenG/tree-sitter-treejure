# Manual test bench — treejure semantic module

Open these files in Emacs with `replique-clojure-mode` active (the major mode
auto-enables `replique-clojure-semantic-mode` when the C module is available).
Things to watch:

- **Semantic-face overlays** layered over treesit's highlighting:
  - `:local` → `replique-clojure-local-face` (defaults to the variable-name
    face): a resolved local binding **and every usage of it**.
  - `:local-unused` → `replique-clojure-unused-face` (defaults to `shadow`,
    i.e. greyed out): a local binding that is never used in its scope.
- **Flymake diagnostics** (underlines; `M-x flymake-show-buffer-diagnostics`):
  - `unused-binding` warnings (one per greyed binding, except `_`-names).
  - `duplicate-require`, `refer-all`, `namespace-name-mismatch` warnings from
    the buffer-only `(ns ...)` require pass, and `redefined-var` from the
    var-definition pass.
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

What the module does **not** do yet (so don't expect it): cross-file
*diagnostics* or *faces* — `:global-var` / `:unresolved`, undefined-var, unused
require, arity checks — and **jar-backed** resolution (so `clojure.core` and
library namespaces don't resolve), and project-wide find-usages. Those need the
rest of the cross-file workspace tier (PLAN step 4+).

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
| `src/manualtest/redefinitions.clj`  | `redefined-var`; load-time descent (`do`/`when`) vs fn/quote/comment opacity |
| `src/manualtest/grammar_errors.clj` | all grammar-level diagnostics (this file is **intentionally broken**) |
| `src/manualtest/extra_def_forms.clj` | user macros analysed like `defn` (see `.dir-locals.el`) |
| `src/manualtest/cljs_demo.cljs`     | ClojureScript buffer (same local analysis) |
| `src/manualtest/reader_conditionals.cljc` | `.cljc` reader conditionals; branch-aware var defs (`:clj` honored) |
| `src/manualtest/navigation.clj`     | `xref` `M-.` / `M-?`: locals, same-ns vars, and cross-file (aliased/qualified/`:refer`-ed) jump-to-def |
| `src/manualtest/nav_target.clj`     | the cross-file jump **target** for `navigation.clj` (open only to confirm jumps arrive) |

`.dir-locals.el` sets `replique-clojure-extra-def-forms` to `("defroute"
"defcomponent")` for this project; the semantic layer analyses those macros
like `defn` (params become locals).  `extra_def_forms.clj` shows it in action.

Each form is annotated with the faces/diagnostics it should produce. A quick
way to sanity-check the whole thing: `M-x flymake-show-project-diagnostics`.
