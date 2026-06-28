(ns manualtest.syntax-quote)

;; ===========================================================================
;; Syntax-quote unquoting — a local used only under ~ / ~@ counts as USED
;; ===========================================================================
;;
;; Inside a syntax-quoted template ``(...)`` only the `unquote` (~) and
;; `unquote-splicing` (~@) targets are live code; everything else is data
;; (templated, namespace-qualified symbols).  So a binding referenced solely
;; through ~/~@ is a real usage and must NOT be greyed/warned — while a plain
;; templated symbol is data and does not count (see scoping.clj for the
;; no-unquote case).  Nested syntax-quotes raise the quoting level: an unquote
;; only "escapes" to live code when it cancels back to level 0.

;; `x` used only under ~  → :local, NOT warned.
(defmacro m-unquote [x]
  `(println ~x))

;; `xs` used only under ~@ → :local, NOT warned.
(defmacro m-splice [xs]
  `(vector ~@xs))

;; Mixed: `a` appears only as a TEMPLATED (quoted) symbol → data, so it is
;; unused (greyed + warned); `b` is unquoted → a real usage → :local.
(defmacro m-mixed [a b]          ; `a` greyed + warned;  `b` :local
  `(list 'a ~b))

;; Nested syntax-quote: the single ~ cancels only the INNER backtick, leaving
;; `n` still templated at the outer level → NOT live → unused (greyed + warned).
(defmacro m-nested [n]           ; `n` greyed + warned — still templated
  `(do `(inner ~n)))

;; Doubly unquoted (~~) under the nested backtick escapes all the way to live
;; code → `k` IS used → :local.
(defmacro m-nested-escape [k]    ; `k` :local (used)
  `(do `(inner ~~k)))

