(ns manualtest.global-vars
  (:require [manualtest.nav-target :as nt :refer [greet]]
            ;; `clojure.string` lives in a jar, so it does NOT resolve yet —
            ;; usages through `s/...` stay unpainted (see section 4).
            [clojure.string :as s]))

;; ===========================================================================
;; :global-var faces — var USAGES that positively resolve.
;;
;; The face (default `font-lock-variable-name-face`) marks a symbol that the
;; module resolves to a real var.  Two flavours, on two cadences:
;;
;;   * SAME-NAMESPACE usages (this file's own defs) are painted on the FAST
;;     tier — immediately, as you type (after the short idle debounce).
;;   * CROSS-NAMESPACE usages (aliased / fully-qualified / `:refer`-ed) are
;;     painted on the FULL tier only — after `save`, switching to/from the
;;     buffer, or `M-.`.  So: edit, then `C-x C-s`, to see them appear.
;;
;; A core / library / jar-backed symbol gets NO face — that is not a false
;; report, just "not resolved yet" (the same boundary as `M-.` jump-to-def).
;; A binding NAME at its def site is never painted (treesit already faces it).
;; ===========================================================================


;; ---------------------------------------------------------------------------
;; 1. Same-namespace vars (FAST tier — appear immediately on edit).
;; ---------------------------------------------------------------------------

;; The def NAME `base-url` here is NOT :global-var (treesit faces a def name).
(def base-url "https://example.com")

;; Both `base-url` usages below ARE :global-var.  `path` is a param → :local,
;; NOT :global-var (locals win; resolution is by identity, not by text).
(defn endpoint [path]
  (str base-url path))

;; `endpoint` here is :global-var (a same-ns defn usage).  `str` is core → no
;; face.  `n` is :local.
(defn endpoints [n]
  (endpoint (str "/page/" n)))


;; ---------------------------------------------------------------------------
;; 2. Local SHADOWS a var name — the inner use is :local, not :global-var.
;; ---------------------------------------------------------------------------

;; `base-url` is also a var (section 1).  Inside the `let` it is rebound as a
;; LOCAL, so the `base-url` in the body is :local (greenish), NOT :global-var.
;; This is the identity check: same text, different binding.
(defn shadowing []
  (let [base-url "http://localhost"]
    base-url))


;; ---------------------------------------------------------------------------
;; 3. Cross-namespace vars (FULL tier — save the buffer to see these paint).
;;    All resolve into nav_target.clj under the `src` source dir.
;; ---------------------------------------------------------------------------

;; Aliased: `nt/greet` is :global-var (the whole `nt/greet` symbol).
(defn aliased-call []
  (nt/greet "world"))

;; Fully-qualified: the whole `manualtest.nav-target/max-size` is :global-var.
(defn qualified-call []
  (* 2 manualtest.nav-target/max-size))

;; `:refer`-ed bare name: `greet` is :global-var.
(defn referred-call []
  (greet "there"))


;; ---------------------------------------------------------------------------
;; 4. What does NOT get a :global-var face (negative cases — no face, no warning).
;; ---------------------------------------------------------------------------

;; Core vars (`+`, `inc`, `str`) → no face: they live in jars.
(defn core-symbols [x]
  (+ (inc x) (str x)))

;; Jar-backed alias: `s/upper-case` → no face (`clojure.string` is a jar).
(defn jar-alias [x]
  (s/upper-case x))

;; Resolves the NS but the var does not exist there → no face (and, by design,
;; no `unresolved`/`undefined-var` warning yet — that needs the jar slice).
(defn missing-var [x]
  (nt/does-not-exist x))

;; A bare symbol that is neither local, in-file, nor referred → no face.
(defn undefined-symbol [x]
  (totally-undefined x))
