(ns manualtest.navigation
  (:require [manualtest.nav-target :as nt]
            [manualtest.nav-target :refer [greet]]))

;; ===========================================================================
;; Navigation bench — `xref`.  Put point on a symbol and try:
;;   `M-.`  (xref-find-definitions)   — jump to its definition
;;   `M-,`  (xref-go-back)            — return
;;   `M-?`  (xref-find-references)    — list its occurrences
;;
;; Resolution is by IDENTITY, not text, so shadowing lands on the right binding.
;; Cross-file jumps need `manualtest.nav-target` to resolve under a project
;; source dir (`replique-clojure-semantic-source-dirs`, here `src`).
;; ===========================================================================


;; ---------------------------------------------------------------------------
;; 1. Locals — binding <-> usages, buffer-scoped.
;; ---------------------------------------------------------------------------

;; `M-.` on either `x` in the body jumps to the param `x`.
;; `M-?` on any `x` lists all three (the param + two usages).
(defn double-sum [x]
  (+ x x))

;; `M-.` on the inner `y` (in `(* y y)`) jumps to the LET binding, NOT the
;; param — shadowing.  `M-?` on the inner `y` lists only the let binding + its
;; use; `M-?` on the PARAM `y` lists just the param (it is shadowed below).
(defn shadow-demo [y]
  (let [y (inc y)]
    (* y y)))


;; ---------------------------------------------------------------------------
;; 2. Same-namespace vars — usage -> def, def + usages.
;; ---------------------------------------------------------------------------

(def base-url "https://example.com")

;; `M-.` on `base-url` here jumps UP to the def above.
;; `M-?` on either lists the def + this usage.
(defn endpoint [path]
  (str base-url path))

;; `M-.` on `endpoint` in `call-endpoint` jumps to the defn above.
(defn call-endpoint []
  (endpoint "/status"))


;; ---------------------------------------------------------------------------
;; 3. Cross-file — jumps into nav_target.clj.
;; ---------------------------------------------------------------------------

;; `M-.` on `nt/greet` (try both the `nt` part and the `greet` part) jumps to
;; `greet` in nav_target.clj.
(defn aliased-call []
  (nt/greet "world"))

;; `M-.` on the fully-qualified `manualtest.nav-target/max-size` jumps to the
;; `max-size` def in nav_target.clj.
(defn fully-qualified-call []
  (* 2 manualtest.nav-target/max-size))

;; `M-.` on the bare, `:refer`-ed `greet` also jumps to nav_target.clj.
(defn referred-call []
  (greet "there"))


;; ---------------------------------------------------------------------------
;; 4. What does NOT resolve yet.
;; ---------------------------------------------------------------------------

;; `M-.` on a core / library symbol (`str`, `+`, `inc`, `clojure.string/...`)
;; yields nothing — those live in jars, which the cross-file slice does not read
;; yet.  (`M-?` is now project-wide: it prompts for a scope and lists usages
;; across the chosen source dirs by resolved identity — e.g. `M-?` on `greet`
;; here also finds its usages in `global_vars.clj` / `undefined_vars.clj`.)
(defn core-symbols []
  (str (+ 1 2) (inc 3)))
