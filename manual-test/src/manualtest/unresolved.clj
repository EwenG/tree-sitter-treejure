(ns manualtest.unresolved
  ;; Manual test for the two GATED dependency-reading facts: the `:unresolved'
  ;; FACE and the `unresolved-namespace' diagnostic.  Both need an exhaustive,
  ;; jar-inclusive classpath to stay false-positive-free (a library require is an
  ;; unavoidable NULL edge; a bare core var lives in a jar), so they are OFF
  ;; under the bench's interim source-dirs-only classpath -- the JVM oracle
  ;; supplies the real classpath later (PLAN step 6).
  ;;
  ;; So by DEFAULT (interim classpath) you should see NEITHER fact in this file.
  ;; To turn them ON, load the bench harness and flip the gate:
  ;;   M-x load-file RET .../manual-test/complete-classpath-manual-test.el RET
  ;;   M-x manualtest-complete-check   ; adds the clojure jar + marks the
  ;;                                   ; workspace classpath-complete, then re-checks
  ;;   M-x manualtest-complete-reset   ; back to the interim (gated-off) workspace
  ;;
  ;; With the gate ON: the `:unresolved' face (→ `replique-clojure-unresolved-face',
  ;; default the warning face) paints the truly-unresolved symbols below, and
  ;; `unresolved-namespace' underlines the missing require — while every negative
  ;; control stays clean.  (`undefined-var' is NOT gated and is exercised in
  ;; `undefined_vars.clj' instead.)
  (:require [manualtest.nav-target :as nt]
            [clojure.string :as str]
            ;; This namespace exists nowhere — not under `src', not in any jar.
            ;; Gate ON → UNRESOLVED-NAMESPACE on `manualtest.no-such-ns'.
            ;; (nav-target resolves under `src' and clojure.string resolves in the
            ;; jar the harness adds, so this is the only require flagged.)
            [manualtest.no-such-ns :as missing]))

;; ===========================================================================
;; `:unresolved' FACE — bare symbols that resolve to NOTHING (gate ON only).
;; ===========================================================================

;; Neither a local, an in-file var, a `:refer'-ed var, nor a core var → both
;; `totally-undefined' and `another-missing' get the `:unresolved' face.
(defn truly-unresolved [x]
  (totally-undefined (another-missing x)))

;; A qualified usage of the unresolvable require: its namespace does not resolve,
;; so there is no `undefined-var' (we cannot know the var is missing) and no
;; face — only the require itself is flagged (above).  It also keeps the `missing'
;; alias USED, so no `unused-namespace' noise muddies this file.
(defn uses-missing-alias [x]
  (missing/whatever x))

;; ===========================================================================
;; NEGATIVE controls — these stay UNPAINTED even with the gate ON (the face is
;; deliberately conservative, so it never false-positives).
;; ===========================================================================

;; Core vars resolve in `clojure.core' (in the jar the harness adds) → no face.
(defn core-controls [x]
  (+ (inc x) (dec x)))

;; Java-interop shapes are skipped — a `Class/static', a `.method', a `Ctor.',
;; and a capitalised class name are never read as unresolved vars → no face.
(defn interop-controls [x]
  (Math/abs (.intValue (Integer. x))))

;; A fn-literal arg `%' is reader-introduced, not a var reference → no face.
(def squares (map #(* % %) (range 5)))

;; Resolved cross-ns (`nt/greet'), jar (`str/join'), in-file (`base') and local
;; (`y') references all resolve → no face.
(def base 1)

(defn resolved-controls [x]
  (let [y (nt/greet x)]
    (str/join "," [y base])))
