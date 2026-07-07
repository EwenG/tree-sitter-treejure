(ns manualtest.macro-defs)

;; A pure-Clojure macro namespace consumed from ClojureScript through
;; `:require-macros' / `:refer-macros'.  In cljs these macros are loaded on the
;; CLJ side (this .clj file), never from a .cljs runtime file -- the case the
;; separate macro-require graph resolves.

(defmacro my-macro [x] `(inc ~x))

(defmacro other-macro [x] `(dec ~x))

;; A def-form-style macro: also declared as a project `extra-def-form', so it can
;; be `:refer'-ed and used as `(defcomponent name [args] ...)'.
(defmacro defcomponent [name args & body] `(def ~name (fn ~args ~@body)))

;; A plain fn (not a macro) -- present in the clj surface too.
(defn helper [x] (* x 2))
