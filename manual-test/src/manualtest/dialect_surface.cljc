(ns manualtest.dialect-surface)

;; A .cljc dependency whose var surface is DIALECT-DEPENDENT.  The treejure
;; module distils it once per active platform: the `:clj' branch fills the clj
;; surface (FileNode.index), the `:cljs' branch the cljs surface (alt_index).  A
;; requirer resolves a usage against the surface for ITS OWN dialect, so a
;; `:cljs'-only var is real for a .cljs requirer but undefined for a .clj one.

;; Non-conditional: defined for BOTH platforms.
(defn shared [x] (inc x))

;; `:clj'-only var: present in the clj surface, ABSENT from the cljs surface.
#?(:clj (defn clj-only [x] (* x 2)))

;; `:cljs'-only var: present in the cljs surface, ABSENT from the clj surface.
#?(:cljs (defn cljs-only [x] (+ x 1)))

;; Same name defined once per platform → NOT a redefinition (each surface holds
;; it once).
#?(:clj  (def per-platform :jvm)
   :cljs (def per-platform :js))

;; In-file cross-branch usage: each branch uses ITS platform's var, so
;; jump-to-def within this .cljc buffer must reach the def in the matching
;; surface (the cljs usage resolves against alt_index, the clj one against index).
(defn use-internal [n]
  #?(:clj  (clj-only n)
     :cljs (cljs-only n)))
