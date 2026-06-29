(ns manualtest.global-vars
  (:require [manualtest.nav-target :as nt :refer [greet]]
            [clojure.string :as s]))

;; ===========================================================================
;; The var-face decision — vars carry NO semantic face.
;;
;; There is deliberately no `:global-var' (or any var) face: a resolved var,
;; same- or cross-namespace, is colored by the treesit SYNTAX layer (it already
;; faces a qualified symbol's namespace and the def-name forms).  The semantic
;; overlay paints only what treesit cannot — `:local' bindings + their usages,
;; the `:special-form'/`:macro-invocation' form heads, and (later) `:unresolved'.
;;
;; So what to watch here is the ABSENCE of a var face plus the presence of the
;; `:local' face — the locals stand out precisely because vars around them are
;; left to treesit.  Var usages still resolve under the hood (that drives `M-.';
;; see navigation.clj) — resolution just no longer drives a face.
;; ===========================================================================


;; ---------------------------------------------------------------------------
;; 1. Same-namespace vars — no semantic face; locals are faced.
;; ---------------------------------------------------------------------------

;; The def NAME `base-url` is treesit-faced (a def name); no semantic overlay.
(def base-url "https://example.com")

;; Both `base-url` usages below get NO semantic face (treesit colors them).
;; `path` is a param → `:local' (faced), and locals win by identity anyway.
(defn endpoint [path]
  (str base-url path))

;; `endpoint` here is a same-ns var usage → no face.  `str` is core → no face.
;; `n` is `:local'.
(defn endpoints [n]
  (endpoint (str "/page/" n)))


;; ---------------------------------------------------------------------------
;; 2. Local SHADOWS a var name — the inner use IS faced (`:local').
;; ---------------------------------------------------------------------------

;; `base-url` is also a var (section 1).  Inside the `let` it is rebound as a
;; LOCAL, so the `base-url` in the body is `:local' (faced) — the one case where
;; a name that elsewhere reads as a var is painted, because here it is a local.
;; The identity check: same text, different binding.
(defn shadowing []
  (let [base-url "http://localhost"]
    base-url))


;; ---------------------------------------------------------------------------
;; 3. Cross-namespace vars — still no face; `M-.' resolves them (full tier).
;;    All resolve into nav_target.clj under the `src` source dir.
;; ---------------------------------------------------------------------------

;; Aliased `nt/greet`, fully-qualified `manualtest.nav-target/max-size`, and the
;; `:refer'-ed bare `greet' all RESOLVE (jump-to-def lands in nav_target.clj),
;; but none gets a semantic face — treesit colors the namespace part.
(defn aliased-call []
  (nt/greet "world"))

(defn qualified-call []
  (* 2 manualtest.nav-target/max-size))

(defn referred-call []
  (greet "there"))


;; ---------------------------------------------------------------------------
;; 4. The dependency-reading Tier-2 facts (full tier).
;;
;; `undefined-var` fires whenever a require RESOLVED to a dependency that does
;; not define the var — so it is on regardless of classpath completeness (a
;; resolved dep's surface is authoritative).  `unresolved-namespace` and the
;; `:unresolved` FACE need an exhaustive, jar-inclusive classpath to avoid false
;; positives (a library require is an unavoidable NULL edge, a bare core var
;; lives in a jar), so they are gated on a "classpath-complete" workspace — off
;; under the interim source-dirs-only classpath, on once the JVM oracle supplies
;; the full classpath (PLAN step 6).
;; ---------------------------------------------------------------------------

;; Core vars (`+`, `inc`, `str`) → no `undefined-var` (clojure.core is a jar:
;; under the interim classpath its require is a NULL edge so these are skipped;
;; under a complete classpath they resolve into the jar).  No `:unresolved` face
;; either — gated off interim, and resolved (so still none) on a complete one.
(defn core-symbols [x]
  (+ (inc x) (str x)))

;; Jar-backed alias `s/upper-case` → clojure.string is a jar; under the interim
;; classpath its require is a NULL edge so NO `undefined-var` fires, and under a
;; complete classpath it resolves into the jar (var present) — clean either way.
(defn jar-alias [x]
  (s/upper-case x))

;; Resolves the NS (`manualtest.nav-target` is a project source under `src`) but
;; the var does not exist there → `UNDEFINED-VAR` warning on `nt/does-not-exist`
;; (a resolved dep that lacks the var — fires under any classpath).  With a
;; complete classpath it also gets the `:unresolved` face.
(defn missing-var [x]
  (nt/does-not-exist x))

;; A bare symbol that is neither local, in-file, nor referred → an `:unresolved`
;; candidate.  No warning; the `:unresolved` FACE appears only on a complete
;; classpath (interim, it could still be a core/library var, so it is left
;; unpainted to stay false-positive-free).
(defn undefined-symbol [x]
  (totally-undefined x))
