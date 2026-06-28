(ns manualtest.nav-target)

;; ===========================================================================
;; Cross-file jump-to-definition TARGET namespace.
;;
;; `manualtest.navigation` requires this ns (aliased, fully-qualified, and
;; `:refer`-ed).  `M-.` from there should land on the defs BELOW.  Nothing to do
;; in this file directly — open it only to confirm the jump arrives here.
;; ===========================================================================

;; `M-.` on `nt/greet`, `manualtest.nav-target/greet`, or the referred `greet`
;; in navigation.clj jumps to this name.
(defn greet [who]
  (str "hello " who))

;; `M-.` on `nt/max-size` (or the fully-qualified form) jumps here.
(def max-size 100)

;; A private var: `M-.` still navigates to it (navigation does not enforce
;; privacy; a `private-call` lint is a later slice).
(defn- secret-helper [x]
  (* x x))

;; Used here so it is not merely an unused private var.
(defn squared-plus-one [x]
  (+ 1 (secret-helper x)))

;; Multimethods extended cross-namespace from `defmethods.clj' (which requires
;; this ns aliased + `:refer'-ed only to use these as `defmethod' targets).
;; `M-.' on `nt/area' / the referred `shape-name' over there lands on these.
(defmulti area :kind)

(defmulti shape-name :kind)
