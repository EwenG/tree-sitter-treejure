(ns manualtest.reader-conditionals)

;; A .cljc buffer: `replique-clojure-clojurec-mode' derives from
;; `replique-clojure-mode', so the semantic layer is active.  The scope pass
;; walks INTO reader-conditional branches, so locals used in any branch resolve
;; correctly (no false "unused" warnings).
;;
;; v1 analyses every branch (it does not yet pick one dialect), which is exactly
;; what you want for not-missing usages.

;; `x` is used inside both branches of the reader conditional → :local, no
;; warning.  `unused` is used in NEITHER → greyed + warned.
(defn platform-inc [x unused]
  #?(:clj  (inc x)
     :cljs (inc x)))

;; A local used only in ONE branch still counts as used (not warned).
(defn describe [v]
  #?(:clj  (str "JVM: " v)
     :cljs (str "JS: "  v)))

;; Splicing reader conditional `#?@` in a NON-quoted vector: `items` resolves.
(defn wrap [items]
  (vec (concat [:start] #?@(:clj  [items]
                           :cljs [items])
               [:end])))

;; let bindings around a reader conditional: `base` used in both branches.
(defn scaled [n]
  (let [base (* n 10)]
    #?(:clj  (long base)
       :cljs base)))
