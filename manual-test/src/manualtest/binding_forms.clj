(ns manualtest.binding-forms
  (:import [java.io StringWriter]))

;; Every supported binding form.  Convention below: each form binds one symbol
;; it USES (stays :local) and one it does NOT (greyed + "unused binding …").

;; ── let / let* ─────────────────────────────────────────────────────────────
(defn demo-let []
  (let [used   1
        unused 2]            ; greyed + warned
    used))

(defn demo-let* []
  (let* [used   1
         unused 2]           ; greyed + warned
    used))

;; ── if-let / when-let / if-some / when-some / when-first ───────────────────
(defn demo-if-let [m]
  (if-let [v (:value m)]     ; v used → :local
    v
    :none))

(defn demo-when-let [m]
  (when-let [v (:value m)]   ; v used
    (inc v)))

(defn demo-if-some [m]
  (if-some [v (:value m)]
    v
    :none))

(defn demo-when-some [m]
  (when-some [v (:value m)]
    v))

(defn demo-when-first [coll]
  (when-first [x coll]       ; x used
    x))

;; ── loop / recur ───────────────────────────────────────────────────────────
(defn demo-loop [n]
  (loop [i   0
         acc 0]              ; both used in the recur/return
    (if (< i n)
      (recur (inc i) (+ acc i))
      acc)))

;; ── binding (dynamic) ──────────────────────────────────────────────────────
(def ^:dynamic *level* 0)

(defn demo-binding []
  (binding [*level* 5]       ; the binding value position; *level* is a var
    *level*))

;; ── with-open ──────────────────────────────────────────────────────────────
(defn demo-with-open []
  (with-open [w (StringWriter.)]   ; w used → :local
    (.toString w)))

;; ── with-local-vars ────────────────────────────────────────────────────────
(defn demo-with-local-vars []
  (with-local-vars [counter 0]     ; counter used
    (var-set counter 1)
    (var-get counter)))

;; ── with-redefs ────────────────────────────────────────────────────────────
(defn demo-with-redefs []
  (with-redefs [demo-let (constantly :stubbed)]
    (demo-let)))

;; ── dotimes ────────────────────────────────────────────────────────────────
(defn demo-dotimes []
  (dotimes [i 3]             ; i used
    (println i)))

;; ── fn / fn* (named, multi-arity) ──────────────────────────────────────────
(def demo-fn
  (fn self                   ; `self` is bound in the body (self-recursion)
    ([] (self 0))            ; 0-arity calls the 1-arity
    ([x] (* x x))))          ; x used

(def demo-fn*
  (fn* [a b unused]          ; `unused` greyed + warned; a,b :local
    (+ a b)))

;; ── defn / defn- / defmacro (docstring + attr-map skipped) ─────────────────
(defn documented
  "A docstring (treesit faces it); the param after it is still analysed."
  {:added "1.0"}
  [x]                        ; x used
  (inc x))

(defn- private-fn [y]        ; y used
  (dec y))

;; defmacro binds its params like defn.  They are used here in plain
;; list-building so they resolve; using them inside a syntax-quote (`(… ~p))
;; would NOT count as a usage yet — see the quote-opacity note in scoping.clj.
(defmacro twice [body]       ; body used → :local
  (list 'do body body))

;; multi-arity defn
(defn poly
  ([] (poly 1))
  ([x] x)                    ; x used
  ([x y & more] (apply + x y more)))   ; more used (& rest)

;; ── defmethod (dispatch value, then params) ────────────────────────────────
(defmulti area :shape)

(defmethod area :circle [{:keys [r]}]   ; r used → :local (destructured)
  (* 3.14 r r))

;; ── letfn (mutual recursion) ───────────────────────────────────────────────
(defn demo-letfn [n]
  (letfn [(even? [x] (if (zero? x) true  (odd?  (dec x))))
          (odd?  [x] (if (zero? x) false (even? (dec x))))]
    (even? n)))              ; even?/odd? cross-reference → none unused

;; ── try/catch/finally (catch binds the exception) ──────────────────────────
(defn demo-catch []
  (try
    (/ 1 0)
    (catch ArithmeticException e   ; e used → :local
      (.getMessage e))
    (catch Exception _ignored      ; greyed, NOT warned (underscore)
      :other)
    (finally
      (println "done"))))

;; ── as-> ───────────────────────────────────────────────────────────────────
(defn demo-as-> [x]
  (as-> x $                  ; `$` threaded + used
    (inc $)
    (* $ 2)))

;; ── def / defonce (the name is skipped, init is analysed) ──────────────────
(def computed (let [base 10] (* base base)))   ; base used

(defonce singleton (atom nil))
