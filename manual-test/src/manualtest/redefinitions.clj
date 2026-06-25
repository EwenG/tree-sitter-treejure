(ns manualtest.redefinitions)

;; The var-definition pass (Tier 1, buffer-only) records every def-like form
;; that is interned at LOAD time — top-level, or reached through load-time
;; control flow (`do`, `when`, `let`, reader conditionals) — then flags a name
;; defined more than once → REDEFINED-VAR warning on each definition AFTER the
;; first.  Defs behind a function/quote/comment boundary are not recorded.
;; Watch for Flymake underlines.

(defn greet [ee] "hi")
;; `greet` defined a second time → REDEFINED-VAR warning here.
(def greet "bonjour")
;; …and a third time → another REDEFINED-VAR warning here.
(defmacro greet [] "hola")


;; --- What does NOT count as a redefinition -------------------------------

;; `declare` is a forward declaration, so a later real def of `pending` is fine.
(declare pending)
(defn pending [] :ok)               ; NO warning

;; Multiple arities are one definition, not a redefinition.
(defn arity ([] 0) ([x] x))         ; NO warning

;; `defmethod` extends a multimethod — it defines no var, so repeated methods
;; on the same multifn are fine.
(defmulti shape :kind)
(defmethod shape :circle [_] :round)  ; NO warning
(defmethod shape :square [_] :boxy)   ; NO warning

;; A def behind a FUNCTION boundary (inline def: only interned when the fn is
;; called) is not part of the load-time surface and does not collide with the
;; top-level `helper`.
(defn wrap [] (def helper 1))       ; inline def -- not recorded
(def helper 2)                      ; NO warning (def behind fn is excluded)
#(def helper 3)                     ; #(...) inline def -- also not recorded

;; --- Defs reachable at LOAD time DO count -------------------------------

;; A top-level `do` splices its children into the top level, so both defs are
;; interned when the file loads → the second is a REDEFINED-VAR warning.
(do
  (def via-do 1)
  (def via-do 2))                   ; REDEFINED-VAR warning here

;; `when`/`let` around a def still run at load time when the form is evaluated,
;; so this collides with the top-level `gated`.
(def gated 0)
(when true
  (def gated 1))                    ; REDEFINED-VAR warning here

;; Quoted / commented defs are data, never interned → no collision.
'(def gated 9)                      ; NO warning (quoted)
(comment (def gated 9))             ; NO warning (comment body opaque)
