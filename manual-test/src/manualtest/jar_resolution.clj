(ns manualtest.jar-resolution
  (:require [clojure.string :as str :refer [join]]
            [clojure.set :as set]))

;; ===========================================================================
;; Jar-backed :global-var faces — the jar slice.
;;
;; This file requires LIBRARY namespaces (`clojure.string`, `clojure.set`) that
;; live in JARS, not under a project source dir.  The C module CAN now resolve
;; them (it reads the jar via miniz), but the bench's normal classpath seeds
;; only `src` — so by default NOTHING below paints.
;;
;;   To see it work:  M-x load-file  →  jar-manual-test.el (one dir up),
;;                    then in THIS buffer:  M-x manualtest-jar-check
;;
;; That re-checks the buffer with a real `clojure-*.jar` added to the classpath.
;; The aliased / fully-qualified / `:refer`-ed jar vars below then light up with
;; the `:global-var' face (default `font-lock-variable-name-face`).  Run
;; `M-x manualtest-jar-reset` to drop the jar again.
;;
;; (Jump-to-def *into* a jar entry is a later slice: `M-.' on a jar var paints a
;; face but does not navigate.  Same-ns / project cross-file `M-.' still works.)
;; ===========================================================================


;; ---------------------------------------------------------------------------
;; 1. Jar vars that SHOULD paint :global-var after `manualtest-jar-check`.
;; ---------------------------------------------------------------------------

;; Aliased: the whole `str/upper-case` resolves to clojure.string/upper-case.
(defn shout [s]
  (str/upper-case s))

;; Fully-qualified: `clojure.string/trim` resolves the literal ns in the jar.
(defn clean [s]
  (clojure.string/trim s))

;; `:refer`-ed bare name: `join` resolves to clojure.string/join.
(defn comma-join [xs]
  (join ", " xs))

;; A second jar ns, aliased: `set/union` resolves to clojure.set/union.
(defn merge-sets [a b]
  (set/union a b))


;; ---------------------------------------------------------------------------
;; 2. What still gets NO face, even with the clojure jar on the classpath.
;; ---------------------------------------------------------------------------

;; clojure.core vars (`+`, `inc`, `str`, `map`) are IMPLICITLY referred — the
;; module does not model the implicit `clojure.core` refer yet, so a bare core
;; name is not in the refer map and stays unpainted.  (It is in a jar, but the
;; module only paints aliased / qualified / explicitly-`:refer`-ed jar vars.)
(defn core-stays-plain [xs]
  (map inc (str xs)))

;; Resolves the jar NS but the var does not exist there → no face, and (by
;; design) no `unresolved`/`undefined-var` warning yet.
(defn missing-in-jar [s]
  (str/this-does-not-exist s))

;; A bare symbol that is neither local, in-file, nor `:refer`-ed → no face.
(defn undefined-symbol [x]
  (totally-undefined x))
