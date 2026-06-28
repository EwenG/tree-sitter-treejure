(ns manualtest.requires
  ;; The require pass flattens every require/use spec.  Watch for Flymake
  ;; underlines (M-x flymake-show-buffer-diagnostics).  Two families fire here:
  ;; the Tier-1 (buffer-only, after-edit) `duplicate-require` / `refer-all`, and
  ;; the Tier-2 (full-tier — save / buffer-switch) `unused-namespace` /
  ;; `unused-referred-var`.  Each spec is annotated with what it should produce.
  (:require [clojure.string :as str]
            ;; `clojure.string` is required a second time → DUPLICATE-REQUIRE
            ;; warning on this line's lib symbol.  (`str` is used below → no
            ;; unused warning.)
            [clojure.string :refer [join]]
            ;; `:refer :all` → REFER-ALL warning ("avoid :refer :all").  A
            ;; refer-all spec is never flagged unused (can't tell which vars
            ;; come from it).
            [clojure.set :refer :all]
            ;; Prefix lists expand to `clojure.walk` / `clojure.zip` — distinct
            ;; namespaces, so NO duplicate warning.  `clojure.walk` is plain →
            ;; never flagged unused; `clojure.zip` is aliased (`:as z`) and `z`
            ;; is never used → UNUSED-NAMESPACE on `zip`.
            (clojure walk [zip :as z]))
  ;; A bare `:use` spec refers everything → REFER-ALL warning
  ;; ("prefer :require with :refer over :use").
  (:use [clojure.pprint])
  ;; `:use … :only [doc]` brings in `doc`, which is never used →
  ;; UNUSED-NAMESPACE on `clojure.repl` AND UNUSED-REFERRED-VAR on `doc`.
  (:use [clojure.repl :only [doc]]))

;; The `namespace-name-mismatch` warning (ns name vs file path) cannot be shown
;; here without breaking this whole file's ns; it is exercised in the module's
;; batch tests instead.

(defn demo [coll]
  (str/join "," (join "" coll)))
