(ns manualtest.implicit-macros)

;; A clj-only macro namespace consumed from cljs through a PLAIN `:require' (no
;; `:require-macros').  ClojureScript's implicit macro loading makes its macros
;; available through the alias -- the module resolves the clj companion of a
;; runtime-required ns to check them.

(defmacro imac [] `(do nil))

(defn helper [x] x)
