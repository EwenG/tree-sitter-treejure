(ns manualtest.jar-resolution
  (:require [clojure.string :as str :refer [join]]
            [clojure.set :as set]))

;; ===========================================================================
;; Jar-backed resolution → jump-to-def into a jar — the jar slice.
;;
;; This file requires LIBRARY namespaces (`clojure.string`, `clojure.set`) that
;; live in JARS, not under a project source dir.  The C module CAN resolve them
;; (it reads the jar via miniz), but the bench's normal classpath seeds only
;; `src` — so by default they do not resolve in-editor.
;;
;;   To see it work:  M-x load-file  →  jar-manual-test.el (one dir up),
;;                    then in THIS buffer:  M-x manualtest-jar-check
;;
;; That re-checks the buffer with a real `clojure-*.jar` added to the classpath.
;; There is no var face, so the slice is exercised through JUMP-TO-DEF: `M-.' on
;; an aliased / fully-qualified / `:refer`-ed jar var below then opens the entry's
;; source read-only at the def — e.g. `str/upper-case' lands in
;; `clojure/string.clj'.  Run `M-x manualtest-jar-reset` to drop the jar again.
;; (Same-ns / project cross-file `M-.' works regardless of the jar.)
;; ===========================================================================


;; ---------------------------------------------------------------------------
;; 1. Jar vars whose `M-.' should land in the jar after `manualtest-jar-check`.
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
;; 2. What still does NOT resolve, even with the clojure jar on the classpath.
;; ---------------------------------------------------------------------------

;; clojure.core vars (`+`, `inc`, `str`, `map`) are IMPLICITLY referred — the
;; module does not model the implicit `clojure.core` refer yet, so a bare core
;; name is not in the refer map and `M-.' finds nothing.  (It is in a jar, but
;; the module only resolves aliased / qualified / explicitly-`:refer`-ed vars.)
(defn core-stays-plain [xs]
  (map inc (str xs)))

;; Resolves the jar NS but the var does not exist there → `M-.' finds nothing,
;; and — once the jar is on the classpath (`manualtest-jar-check`) — an
;; `undefined-var' WARNING: clojure.string resolved, so the missing var is real.
;; `undefined-var' is NOT gated (it only ever checks a resolved dep), so it fires
;; here even though the classpath is not marked complete.
(defn missing-in-jar [s]
  (str/this-does-not-exist s))

;; A bare symbol that is neither local, in-file, nor `:refer`-ed → an
;; `:unresolved' candidate.  No face under `manualtest-jar-check' (that face is
;; gated on a complete classpath, which jar-check does not set); it would paint
;; under `complete-classpath-manual-test.el'.
(defn undefined-symbol [x]
  (totally-undefined x))
