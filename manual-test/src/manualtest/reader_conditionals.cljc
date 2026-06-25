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

;; --- Branch-aware var defs (Tier-1 redefined-var) ------------------------
;; The var-definition pass honors only this file's dialect branch (`:clj` is
;; the primary for .cljc), so a var defined once per platform is NOT a
;; redefinition — but two defs inside the honored branch still collide.

;; Same name in both branches → recorded once (`:clj` only) → NO warning.
#?(:clj  (def per-platform 1)
   :cljs (def per-platform 2))

;; Two top-level defs inside the honored `:clj` branch → REDEFINED-VAR on the
;; second.  (The `:cljs` branch is not scanned for a .cljc file.)
#?(:clj  (do (def dup-in-branch 1)
             (def dup-in-branch 2))
   :cljs (def dup-in-branch 3))
