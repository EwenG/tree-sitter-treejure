(ns manualtest.implicit-user
  ;; Plain `:require' (NOT `:require-macros') of macro namespaces -- cljs implicit
  ;; macro loading resolves their macros through the alias.
  (:require [manualtest.implicit-macros :as im]
            [manualtest.split :as s]))

;; A clj-only macro ns required plainly: its macro resolves through the alias.
(defn a [] (im/imac))       ; ok

;; A split runtime/macro ns (split.cljs + split.clj): the macro lives in the .clj
;; half and still resolves via implicit loading, without `:refer-macros'.
(defn c [] (s/mac 1))       ; ok
;; The runtime fn from the .cljs half.
(defn d [] (s/run-fn 1))    ; ok

;; The macro ns resolves but defines no such var -> still undefined-var (implicit
;; loading is not a blanket suppression).
(defn b [] (im/nope))       ; UNDEFINED-VAR
