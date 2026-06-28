(ns manualtest.protocol-types)

;; ===========================================================================
;; Secondary var interning — the vars a def-form generates BEYOND its primary
;; name, for clj-kondo `:analysis` parity.  Each resolves same-namespace, so its
;; usages get the `:global-var` face (fast tier) and `M-.` / `M-?` work on them:
;;
;;   * defprotocol / definterface → one var per METHOD name;
;;   * deftype  → the positional factory `->Name`;
;;   * defrecord → `->Name` AND the map factory `map->Name`.
;;
;; The def NAME at each site is treesit-faced (a function/type name), not the
;; semantic overlay; only the USAGES below are `:global-var`.
;; ===========================================================================


;; --- defprotocol: the protocol var + each method var -----------------------

(defprotocol Shape
  "A 2D shape."
  (area [this] "the shape's area")
  (perimeter [this]))

;; `area` and `perimeter` here are same-ns var usages → `:global-var`; `M-.`
;; lands on their method signatures above, `M-?` lists the sig + this usage.
;; `s` is a :local.
(defn describe [s]
  (str (area s) " / " (perimeter s)))


;; --- defrecord: the type var + `->Circle` + `map->Circle` ------------------

(defrecord Circle [radius])

;; Both factory usages resolve same-ns → `:global-var`; `M-.` lands on the
;; defrecord form.  `r` is a :local.
(defn make-circle [r]
  [(->Circle r) (map->Circle {:radius r})])


;; --- deftype: the type var + `->Point` (NO map-> for deftype) ---------------

(deftype Point [x y])

;; `->Point` resolves same-ns → `:global-var`; `M-.` lands on the deftype.
(defn origin []
  (->Point 0 0))


;; --- definterface: the interface var + each method var ----------------------
;; (Recorded for `:analysis` parity; in practice interface methods are called
;; via interop `(.now obj)`, not as bare vars — so there is no bare usage to
;; paint here.  `M-.` on a method name at its declaration still resolves.)

(definterface IClock
  (now [])
  (tick [n]))
