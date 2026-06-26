(ns manualtest.locals)

;; ===========================================================================
;; :local faces — a binding and every usage of it
;; ===========================================================================

;; `x` is bound once and used twice.  All three `x` occurrences carry the
;; :local face (the binding in the param vector + the two in the body).
(defn sum-twice [x]
  (+ x x))

;; Both `a` and `b` are bound and used → all four occurrences are :local.
(defn add [a b]
  (+ a b))


;; ===========================================================================
;; unused-binding — greyed out (:local-unused) + a Flymake warning
;; ===========================================================================

;; `unused` is never referenced.  It is greyed and Flymake reports
;; "unused binding unused".  `a` and `b` stay :local.
(defn add-with-extra [a b unused]
  (+ a b))

;; In a let: `kept` is used, `dropped` is not (greyed + warned).
(defn let-demo []
  (let [kept    1
        dropped 2]
    kept))


;; ===========================================================================
;; Underscore suppression — greyed, but NO warning
;; ===========================================================================

;; `_` and `_event` are conventionally "intentionally ignored": they get the
;; greyout face but produce NO unused-binding warning.  `x` is used → :local.
(defn handler [_ _event x]
  x)

;; A `_`-prefixed binding in a let, also unused-but-not-warned.
(defn ignore-in-let []
  (let [_result (println "side effect")
        n       42]
    n))

(handler)
