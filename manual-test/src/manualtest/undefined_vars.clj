(ns manualtest.undefined-vars
  ;; Manual test for the dependency-reading `undefined-var' lint (full tier: it
  ;; recomputes on save / buffer-switch / `M-.', NOT while typing).  Watch for
  ;; Flymake underlines (`M-x flymake-show-buffer-diagnostics').
  ;;
  ;; `undefined-var' fires whenever a qualified or `:refer'-ed var's namespace
  ;; RESOLVED to a dependency that does not define the var.  It is UNCONDITIONAL
  ;; -- it needs no complete/oracle classpath -- because it only ever looks at a
  ;; require that actually resolved, where that dep's var surface is the truth.
  ;;
  ;; `manualtest.nav-target' is a sibling PROJECT source (under `src'), so it
  ;; resolves under the bench's interim classpath -- usages of vars it LACKS are
  ;; flagged here and now.  `clojure.*' lives in a jar (a NULL edge on this
  ;; classpath), so its usages are SKIPPED, never flagged -- no false positive.
  (:require [manualtest.nav-target :as nt :refer [greet bogus-ref]]
            [clojure.string :as str]))

;; ===========================================================================
;; POSITIVE cases — each should get an `undefined-var' warning.
;; ===========================================================================

;; Aliased `nt/...' resolves to nav_target.clj, which defines no `does-not-exist'
;; → UNDEFINED-VAR on `nt/does-not-exist'.
(defn via-alias [x]
  (nt/does-not-exist x))

;; A fully-qualified literal namespace, same story → UNDEFINED-VAR on
;; `manualtest.nav-target/no-such-var'.
(defn via-fqn [x]
  (manualtest.nav-target/no-such-var x))

;; A `:refer'-ed name nav-target does not define → UNDEFINED-VAR on the bare
;; `bogus-ref' usage.  (`greet', referred from the SAME spec, DOES exist — see
;; the negative cases — so it is never flagged.)
(defn via-refer [x]
  (bogus-ref x))

;; A fully-qualified reference to THIS file's own namespace, for a var defined
;; nowhere in it → UNDEFINED-VAR (the file's own surface is the dependency).
(defn via-self-fqn [x]
  (manualtest.undefined-vars/not-defined-here x))

;; ===========================================================================
;; NEGATIVE controls — NONE of these should warn.
;; ===========================================================================

;; Cross-ns vars that DO exist in nav_target.clj — aliased, fully-qualified, and
;; `:refer'-ed — all resolve → no warning.
(defn resolved-cross-ns [x]
  (+ (nt/greet x)
     manualtest.nav-target/max-size
     (greet x)))

;; Core vars live in a jar → their require is a NULL edge on the interim
;; classpath, so they are SKIPPED (never flagged) → no warning.
(defn core-vars [x]
  (+ (inc x) (count (str x))))

;; A jar-backed alias `str/...' — clojure.string is a jar (NULL edge here), so
;; even a genuinely-missing var is NOT flagged: we only check RESOLVED deps.
(defn jar-alias [x]
  (str/upper-case x))

;; Same-namespace var + local references resolve in-buffer → no warning.
(def threshold 10)

(defn in-file-and-locals [n]
  (let [doubled (* n 2)]
    (+ doubled threshold)))

;; A var defined LATER in this file, referred fully-qualified, still resolves —
;; the whole-file var surface is collected before the check → no warning.
(defn forward-ref [x]
  (manualtest.undefined-vars/defined-below x))

(defn defined-below [x] (* x x))
