(ns manualtest.requires
  ;; The require pass (Tier 1, buffer-only — no dependency I/O) flattens every
  ;; require/use spec and flags three things.  Watch for Flymake underlines
  ;; (M-x flymake-show-buffer-diagnostics):
  (:require [clojure.string :as str]
            ;; `clojure.string` is required a second time → DUPLICATE-REQUIRE
            ;; warning on this line's lib symbol.
            [clojure.string :refer [join]]
            ;; `:refer :all` → REFER-ALL warning ("avoid :refer :all").
            [clojure.set :refer :all]
            ;; Prefix lists expand to `clojure.walk` / `clojure.zip` — distinct
            ;; namespaces, so NO duplicate warning here.
            (clojure walk [zip :as z]))
  ;; A bare `:use` spec refers everything → REFER-ALL warning
  ;; ("prefer :require with :refer over :use").
  (:use [clojure.pprint])
  ;; `:use` narrowed by `:only` is fine → NO warning.
  (:use [clojure.repl :only [doc]]))

;; The `namespace-name-mismatch` warning (ns name vs file path) cannot be shown
;; here without breaking this whole file's ns; it is exercised in the module's
;; batch tests instead.

(defn demo [coll]
  (str/join "," (join "" coll)))
