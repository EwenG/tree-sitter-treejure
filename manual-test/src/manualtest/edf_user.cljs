(ns manualtest.edf-user
  (:require [manualtest.edf-defs :as edf :refer [widget]]))

(defn a [] (edf/widget {}))    ; cross-ns qualified -> extra-def-form var
(defn b [] (widget {}))        ; :refer'd extra-def-form var

(defcomponent local-comp [x] [:span x])
(defn c [] (local-comp 1))     ; in-file extra-def-form var
