(ns manualtest.scoping)

;; ===========================================================================
;; Shadowing — inner binding hides the outer one
;; ===========================================================================

;; The OUTER `x` is never used (the body's `x` refers to the inner binding), so
;; the outer `x` is greyed + warned, while the inner `x` and its usage are
;; :local.  Resolution is innermost-first.
(defn shadow []
  (let [x 1]                 ; outer x → greyed + "unused binding x"
    (let [x 2]               ; inner x → :local
      (inc x))))             ; refers to inner x

;; Same idea with a parameter shadowed by a let.
(defn shadow-param [n]       ; this `n` is shadowed below → unused → warned
  (let [n 99]                ; inner n → :local
    n))


;; ===========================================================================
;; Sequential scope — a let init sees the bindings before it
;; ===========================================================================

;; `b`'s init references `a`, so `a` IS used (no warning).  Both end :local.
(defn sequential []
  (let [a 1
        b (+ a 1)]
    b))

;; `earlier` is used only inside `later`'s init; still counts as used.
(defn sequential-2 []
  (let [earlier 10
        later   (* earlier 2)]
    later))


;; ===========================================================================
;; Quote / syntax-quote / discard opacity — quoted symbols are data, not refs
;; ===========================================================================

;; The quoted `x`s are DATA, not references: they must NOT get the :local face,
;; and because the binding `x` is therefore never really used, it is greyed +
;; warned.  (A naive walker would wrongly paint the quoted `x`s.)
(defn quoted []
  (let [x 1]                 ; greyed + warned — the body only quotes x
    '(x x x)))

;; A syntax-quote with NO unquote is data too: the templated `y`s are not
;; usages, so `y` is greyed + warned.  (Unquoted `~y` WOULD count — see
;; syntax_quote.clj.)
(defn syntax-quoted []
  (let [y 2]                 ; greyed + warned — only templated, never unquoted
    `(y y)))

;; `#_` discards its next form; the discarded `z` usage does not count, so the
;; binding `z` is unused → greyed + warned.
(defn discarded []
  (let [z 3]                 ; greyed + warned
    #_z
    :done))
