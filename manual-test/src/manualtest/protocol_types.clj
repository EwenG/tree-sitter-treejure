(ns manualtest.protocol-types)

;; ===========================================================================
;; Secondary var interning — the vars a def-form generates BEYOND its primary
;; name, for clj-kondo `:analysis` parity.  Each resolves same-namespace, so
;; `M-.` / `M-?` work on its usages (interning is what navigation needs):
;;
;;   * defprotocol / definterface → one var per METHOD name;
;;   * deftype  → the positional factory `->Name`;
;;   * defrecord → `->Name` AND the map factory `map->Name`.
;;
;; No semantic var face (treesit colors the names + usages); the point here is
;; that the secondary vars are interned, so jump-to-def / find-references reach
;; the USAGES below.
;; ===========================================================================


;; --- defprotocol: the protocol var + each method var -----------------------

(defprotocol Shape
  "A 2D shape."
  (area [this] "the shape's area")
  (perimeter [this]))

;; `area` and `perimeter` here are same-ns var usages (no face); `M-.`
;; lands on their method signatures above, `M-?` lists the sig + this usage.
;; `s` is a :local.
(defn describe [s]
  (str (area s) " / " (perimeter s)))


;; --- defrecord: the type var + `->Circle` + `map->Circle` ------------------

(defrecord Circle [radius])

;; Both factory usages resolve same-ns (no face); `M-.` lands on the
;; defrecord form.  `r` is a :local.
(defn make-circle [r]
  [(->Circle r) (map->Circle {:radius r})])


;; --- deftype: the type var + `->Point` (NO map-> for deftype) ---------------

(deftype Point [x y])

;; `->Point` resolves same-ns (no face); `M-.` lands on the deftype.
(defn origin []
  (->Point 0 0))


;; --- definterface: the interface var + each method var ----------------------
;; (Recorded for `:analysis` parity; in practice interface methods are called
;; via interop `(.now obj)`, not as bare vars — so there is no bare usage to
;; paint here.  `M-.` on a method name at its declaration still resolves.)

(definterface IClock
  (now [])
  (tick [n]))
