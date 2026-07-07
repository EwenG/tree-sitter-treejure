(ns manualtest.var-quote
  (:require [manualtest.nav-target :as nt]))

;; Var-quote (`#'x`) navigation.  `bounds-of-thing-at-point' folds the `#'`
;; prefix into the symbol, so a point query can land on the `#'` -- jump-to-def
;; and find-usages must still resolve the var it names.

(def local-var 1)
(def quoted-var 2)

(def a #'local-var)                         ; bare var-quote -> in-file def
(def b #'manualtest.nav-target/greet)       ; fully-qualified var-quote -> cross-file
(def c local-var)                           ; a plain usage (find-usages sees both)

;; A plain quote `'x' has the same `'' prefix that `bounds-of-thing-at-point'
;; sweeps in; a quoted symbol is navigable too (though, as data, it is not a
;; usage -- find-usages from it lists the def + real usages, not the quote).
(def d 'quoted-var)                          ; bare quoted symbol -> in-file def
(def e 'manualtest.nav-target/max-size)      ; qualified quoted symbol -> cross-file
