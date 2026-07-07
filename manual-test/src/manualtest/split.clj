(ns manualtest.split)

;; The MACRO half of a split runtime/macro namespace: `manualtest.split' exists
;; as both `split.clj' (this file -- macros) and `split.cljs' (runtime fns).  A
;; cljs requirer's `:refer-macros [mac]' must resolve `mac' HERE (the clj side),
;; not against the cljs runtime surface which does not define it.

(defmacro mac [x] `(+ ~x 1))
