(ns manualtest.cond-requires
  ;; Reader-conditional requires.  A require nested in a `#?(...)' / `#?@(...)'
  ;; inside `(ns ... (:require ...))' is extracted (and linted) like a plain one,
  ;; honoring the branch live for this file's dialect — the same single-branch
  ;; rule the scope pass and var extraction already use.  A `.cljc' file is
  ;; analysed as :clj here; full per-dialect :cljs coverage (seeing a `:cljs'-only
  ;; require) is a later slice (PLAN step 4), so the conditionals below are :clj
  ;; only.  That also keeps this byte-identical to clj-kondo: clj-kondo lints clj
  ;; AND cljs as separate passes (no dedupe), so a require visible to BOTH
  ;; platforms warns twice; a `:clj'-only require warns exactly once — matching
  ;; this module's clj-only view.  The lints run on the FULL tier only (save /
  ;; buffer-switch / `M-.', not while typing).
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
