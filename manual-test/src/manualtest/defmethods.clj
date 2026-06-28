(ns manualtest.defmethods
  ;; Exercises the `defmethod' multifn reference.  `defmethod' does NOT define
  ;; the multifn — it extends an existing one — so the multifn NAME at form
  ;; index 1 is a *usage*, not a definition.  Three things this must get right,
  ;; all matching clj-kondo exactly:
  ;;   * a cross-ns multifn marks its require USED → no false `unused-namespace'
  ;;     (and a `:refer'-ed multifn → no false `unused-referred-var');
  ;;   * the multifn name gets the `:global-var' face — same-ns on the fast tier,
  ;;     cross-ns on the full tier (save / buffer-switch / `M-.');
  ;;   * `M-?' (find-references) on a same-ns multifn includes the defmethod site.
  ;; If the multifn reference were dropped (the bug this guards against), the two
  ;; requires below would BOTH be flagged unused — clj-kondo flags NEITHER.
  (:require
   [manualtest.nav-target :as nt :refer [shape-name]]))

;; ---------------------------------------------------------------------------
;; 1. Same-namespace multimethod: defmulti + defmethod (FAST tier).
;; ---------------------------------------------------------------------------

;; `describe' is a same-ns var (defmulti).  The def NAME is treesit-faced here,
;; NOT `:global-var'.
(defmulti describe :kind)

;; The `describe' multifn name here IS a same-ns var usage → `:global-var'
;; (fast tier).  `M-?' on `describe' includes this site (+ the defmulti, + the
;; method below).  `shape' is a :local (destructured param).
(defmethod describe :circle [{:keys [shape]}]
  (str "circle " shape))

;; A second method — another same-ns `:global-var' occurrence of `describe'.
(defmethod describe :default [_]
  "unknown")

;; ---------------------------------------------------------------------------
;; 2. Cross-namespace multimethod (FULL tier): extends `nt/area'.
;; ---------------------------------------------------------------------------

;; The aliased multifn `nt/area' is a cross-ns usage → it marks the
;; `manualtest.nav-target' require USED (so: NO `unused-namespace' on `nt')
;; and gets `:global-var' on the whole `nt/area' symbol (full tier).
;; `side' is a :local.
(defmethod nt/area :square [{:keys [side]}]
  (* side side))

;; ---------------------------------------------------------------------------
;; 3. `:refer'-ed multifn as the defmethod target (FULL tier).
;; ---------------------------------------------------------------------------

;; The bare referred multifn `shape-name' marks the `:refer [shape-name]' USED
;; (so: NO `unused-referred-var') and gets `:global-var' (full tier).
(defmethod shape-name :triangle [_]
  "triangle")

;; ---------------------------------------------------------------------------
;; 4. A `defmethod' body is a function boundary for the var-def pass too.
;; ---------------------------------------------------------------------------

;; `cached' is a top-level var.
(def cached 1)

;; An inline `(def cached ...)' inside a method body runs on DISPATCH, not at
;; load time — exactly like an inline def inside `defn' — so it is NOT part of
;; the load-time surface and must NOT collide with the top-level `cached'
;; above: NO `redefined-var'.  (Guards the var-def pass treating `defmethod'
;; as opaque, matching the scope pass; clj-kondo agrees — no redefined-var.)
(defmethod describe :inline-def [_]
  (def cached 2))

;; ---------------------------------------------------------------------------
;; 5. The other deferred-body forms are var-def boundaries too.
;;    `reify' / `proxy' / `extend-type' / `extend-protocol' method bodies run on
;;    method call, not at load — so an inline `(def acc ...)' inside any of them
;;    is NOT the load-time surface and must NOT collide with the top-level `acc':
;;    NO `redefined-var' for any of the four.
;;    (clj-kondo agrees on `redefined-var' — 0 — though it additionally flags its
;;    own `inline-def' lint on each, which this module does not implement yet.
;;    The differential that matters here is redefined-var: both 0.)
;; ---------------------------------------------------------------------------

(def acc 1)

(defprotocol Greet (hi [this]))

(reify Object
  (toString [_] (def acc 2)))            ; inline def in reify        — NOT recorded

(proxy [Object] []
  (toString [] (def acc 3)))             ; inline def in proxy        — NOT recorded

(extend-type Long
  Greet
  (hi [_] (def acc 4)))                  ; inline def in extend-type  — NOT recorded

(extend-protocol Greet
  Double
  (hi [_] (def acc 5)))                  ; inline def in extend-proto — NOT recorded
