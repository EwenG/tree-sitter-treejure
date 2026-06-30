(ns manualtest.cond-requires
  ;; Reader-conditional requires.  A require nested in a `#?(...)' / `#?@(...)'
  ;; inside `(ns ... (:require ...))' is extracted (and linted) like a plain one,
  ;; honoring the branch live for each dialect.  A `.cljc' file is now analysed
  ;; once per platform (clj + cljs), each pass building its own require surface,
  ;; and the two passes' diagnostics are deduped: a require visible to BOTH
  ;; platforms (non-conditional, unused) reports ONCE, and a `:clj'-only require
  ;; reports once from the clj pass.  (This is the PLAN's "diagnostics on
  ;; non-conditional code deduped across dialects to a single report" — a
  ;; deliberate divergence from clj-kondo, which lints clj/cljs separately with no
  ;; dedupe and so double-reports a both-platform finding.)  The conditionals
  ;; below are all `:clj'-only, so each warns exactly once.  The lints run on the
  ;; FULL tier only (save / buffer-switch / `M-.', not while typing).
  (:require
   ;; The one USED require — plain so the unconditional `str/join' usage resolves
   ;; on every platform → no warning here, and no cljs unresolved-namespace.
   [clojure.string :as str]
   ;; `#?' conditional, aliased, NEVER used → UNUSED-NAMESPACE on `clojure.set'.
   #?(:clj [clojure.set :as cset])
   ;; `#?@' SPLICING: each element of the chosen branch vector is its own spec.
   ;; Spliced require, never used → UNUSED-NAMESPACE on `clojure.walk'.
   #?@(:clj [[clojure.walk :as w]])
   ;; `:refer'-ed var inside a conditional, never used → the ns is unused too:
   ;; UNUSED-NAMESPACE on `clojure.zip' AND UNUSED-REFERRED-VAR on `clojure.zip/up'
   ;; (proves refer tracking survives the conditional).
   #?(:clj [clojure.zip :refer [up]])))

(defn demo [xs]
  (str/join "," xs))                            ; uses `str' (the only used require)
