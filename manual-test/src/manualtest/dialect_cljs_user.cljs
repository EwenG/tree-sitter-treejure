(ns manualtest.dialect-cljs-user
  (:require [manualtest.dialect-surface :as ds]))

;; A .cljs requirer reads the dependency's CLJS surface.
(defn use-shared [n] (ds/shared n))        ; ok: shared is non-conditional
(defn use-cljs-only [n] (ds/cljs-only n))  ; ok: cljs-only is in the cljs surface
(defn use-clj-only [n] (ds/clj-only n))    ; UNDEFINED-VAR: clj-only is not in
                                           ; the cljs surface
