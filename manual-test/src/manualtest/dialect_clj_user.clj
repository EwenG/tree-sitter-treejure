(ns manualtest.dialect-clj-user
  (:require [manualtest.dialect-surface :as ds]))

;; A .clj requirer reads the dependency's CLJ surface.
(defn use-shared [n] (ds/shared n))        ; ok: shared is non-conditional
(defn use-clj-only [n] (ds/clj-only n))    ; ok: clj-only is in the clj surface
(defn use-cljs-only [n] (ds/cljs-only n))  ; UNDEFINED-VAR: cljs-only is not in
                                           ; the clj surface
