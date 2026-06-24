(ns manualtest.destructuring)

;; ===========================================================================
;; Sequential destructuring  [a b & rest :as all]
;; ===========================================================================

;; `a` and `b` used; `c` unused (greyed + warned).
(defn seq-basic [[a b c]]
  (+ a b))

;; rest binding via `&`, plus `:as` for the whole vector.
(defn seq-rest [[head & tail :as whole]]
  [head tail whole])         ; all three used

;; nested sequential pattern.
(defn seq-nested [[[x y] z]]
  (+ x y z))                 ; x, y, z all used

;; `&` rest unused → greyed + warned ("unused binding rest").
(defn seq-rest-unused [[first-item & rest]]
  first-item)


;; ===========================================================================
;; Associative destructuring  {:keys [..] :as m :or {..}}
;; ===========================================================================

;; `:keys` binds each symbol; `width` used, `height` unused (greyed + warned).
(defn assoc-keys [{:keys [width height]}]
  width)

;; `:strs` and `:syms` work the same way.
(defn assoc-strs [{:strs [a b]}]
  (str a b))                 ; a, b used

(defn assoc-syms [{:syms [x]}]
  x)                         ; x used

;; `:as` binds the whole map; `:or` supplies defaults (the default expression
;; is analysed, so `fallback` below is a USED reference, not a binding).
(def fallback 100)

(defn assoc-as-or [{:keys [size] :or {size fallback} :as opts}]
  [size opts])               ; size, opts used; fallback referenced in :or

;; Arbitrary `{binding lookup-key}` pairs: the KEY is the binding pattern, the
;; value is the lookup key.  `the-name` is bound + used.
(defn assoc-explicit [{the-name :name}]
  the-name)

;; nested associative inside sequential, and `:as` that goes unused.
(defn assoc-nested [{:keys [user]} [{:keys [id]} :as record]]
  [user id])                 ; user, id used; `record` (:as) unused → warned
