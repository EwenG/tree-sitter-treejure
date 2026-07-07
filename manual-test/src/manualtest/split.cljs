(ns manualtest.split)

;; The RUNTIME half of the split namespace (see `split.clj' for the macro half).
;; A cljs `:require [manualtest.split :refer [run-fn]]' resolves `run-fn' against
;; this cljs surface; the sibling `:refer-macros [mac]' resolves against the clj
;; surface instead.

(defn run-fn [x] (+ x 1))
