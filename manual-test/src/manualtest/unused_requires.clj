(ns manualtest.unused-requires
  ;; The `unused-namespace' / `unused-referred-var' lints (Tier 2, but buffer-
  ;; determinable — no dependency I/O).  They run on the FULL tier only: save the
  ;; buffer or switch away and back (not while typing) and watch Flymake
  ;; (M-x flymake-show-buffer-diagnostics).  Each spec is annotated with whether
  ;; it should warn; this mirrors clj-kondo exactly.
  (:require
   ;; USED (str/join below) → no warning.
   [clojure.string :as str]
   ;; aliased but NEVER used → UNUSED-NAMESPACE on `clojure.set'.
   [clojure.set :as set]
   ;; `walk' used, `prewalk' not → UNUSED-REFERRED-VAR on `prewalk' only
   ;; (the namespace itself is used, so no unused-namespace).
   [clojure.walk :refer [walk prewalk]]
   ;; plain require (no :as / :refer) → NEVER flagged (may be side-effecting).
   [clojure.edn]
   ;; `:as-alias' used only through the auto-resolved keyword `::route/x' below
   ;; → no warning (proves `::alias/kw' counts and `:as-alias' is eligible-free).
   [manualtest.routes :as-alias route]
   ;; `:as-alias' NEVER used → still NEVER flagged (`:as-alias' is never unused).
   [manualtest.widgets :as-alias widget]
   ;; aliased, used ONLY inside a syntax-quote template → no warning
   ;; (a syntax-quoted qualified symbol namespace-resolves at read time).
   [manualtest.macros :as mac]))

(defn demo [xs]
  (str/join "," (walk identity identity xs)))   ; uses str + walk

(def route-id ::route/x)                        ; uses the :as-alias `route'

(defmacro expand []
  `(mac/run))                                   ; uses `mac' via syntax-quote
