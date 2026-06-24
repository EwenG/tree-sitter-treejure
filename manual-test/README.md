# Manual test bench — treejure semantic module

Open these files in Emacs with `replique-clojure-mode` active (the major mode
auto-enables `replique-clojure-semantic-mode` when the C module is available).
Two things to watch:

- **Semantic-face overlays** layered over treesit's highlighting:
  - `:local` → `replique-clojure-local-face` (defaults to the variable-name
    face): a resolved local binding **and every usage of it**.
  - `:local-unused` → `replique-clojure-unused-face` (defaults to `shadow`,
    i.e. greyed out): a local binding that is never used in its scope.
- **Flymake diagnostics** (underlines; `M-x flymake-show-buffer-diagnostics`):
  - `unused-binding` warnings (one per greyed binding, except `_`-names).
  - `duplicate-require`, `refer-all`, `namespace-name-mismatch` warnings from
    the buffer-only `(ns ...)` require pass.
  - Grammar-level errors: `syntax-error`, `missing-form`, `invalid-number`,
    `invalid-string`, `invalid-character`, `invalid-symbolic-value`.

What the module does **not** do yet (so don't expect it): `:global-var` /
`:unresolved` faces, cross-file resolution, jump-to-definition, find-usages,
arity checks. Those need the rest of the cross-file workspace tier (PLAN
step 4+) — the require pass above already flattens the specs that tier will
resolve.

## Files

| File | Exercises |
|------|-----------|
| `src/manualtest/locals.clj`         | `:local` faces, unused greyout, `_` suppression |
| `src/manualtest/binding_forms.clj`  | every supported binding form |
| `src/manualtest/destructuring.clj`  | sequential + associative destructuring |
| `src/manualtest/scoping.clj`        | shadowing, sequential scope, quote/discard opacity |
| `src/manualtest/requires.clj`       | `duplicate-require` / `refer-all` from the ns require pass |
| `src/manualtest/grammar_errors.clj` | all grammar-level diagnostics (this file is **intentionally broken**) |
| `src/manualtest/extra_def_forms.clj` | user macros analysed like `defn` (see `.dir-locals.el`) |
| `src/manualtest/cljs_demo.cljs`     | ClojureScript buffer (same local analysis) |
| `src/manualtest/reader_conditionals.cljc` | `.cljc` reader conditionals |

`.dir-locals.el` sets `replique-clojure-extra-def-forms` to `("defroute"
"defcomponent")` for this project; the semantic layer analyses those macros
like `defn` (params become locals).  `extra_def_forms.clj` shows it in action.

Each form is annotated with the faces/diagnostics it should produce. A quick
way to sanity-check the whole thing: `M-x flymake-show-project-diagnostics`.
