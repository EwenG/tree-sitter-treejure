(ns manualtest.reader-conditionals)

;; A .cljc buffer: `replique-clojure-clojurec-mode' derives from
;; `replique-clojure-mode', so the semantic layer is active.  A .cljc file is now
;; analysed ONCE PER ACTIVE DIALECT (clj + cljs): the clj pass walks the `:clj'
;; branches, the cljs pass the `:cljs' branches, and the two passes' diagnostics /
;; faces / navs are unioned then deduped — non-conditional code reports once,
;; conditional code per platform.  So a local used in EITHER branch resolves
;; (no false "unused"), and a var defined once per platform is not a redefinition.

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

;; --- Per-dialect var defs (Tier-1 redefined-var) -------------------------
;; The var-definition pass runs once per dialect into a distinct surface, so a
;; var defined once per platform lands in two different surfaces and is NOT a
;; redefinition — but two defs of one name inside a SINGLE platform's branch
;; still collide in that platform's surface.

;; Same name in each branch → recorded once per surface → NO warning.
#?(:clj  (def per-platform 1)
   :cljs (def per-platform 2))

;; Two top-level defs inside the SAME `:clj` branch → REDEFINED-VAR on the second
;; (in the clj surface).  The cljs pass sees one def → no warning; the two passes'
;; diagnostics dedup, so this reports exactly once.
#?(:clj  (do (def dup-in-branch 1)
             (def dup-in-branch 2))
   :cljs (def dup-in-branch 3))
