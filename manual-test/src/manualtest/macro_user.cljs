(ns manualtest.macro-user
  ;; Macros come from the CLJ side, resolved through the separate macro-require
  ;; graph.  Before it existed, every one of these read as `undefined-var'.
  (:require-macros [manualtest.macro-defs :as m :refer [my-macro]])
  (:require [manualtest.split :as s :refer [run-fn] :refer-macros [mac]]))

;; `:require-macros ... :refer' -- bare macro use resolves to macro_defs.clj.
(defn a [x] (my-macro x))        ; ok

;; `:require-macros ... :as' -- qualified macro use resolves to macro_defs.clj.
(defn b [x] (m/other-macro x))   ; ok

;; A runtime fn from split.cljs (the runtime require).
(defn c [x] (run-fn x))          ; ok

;; A macro from split.clj via the `:refer-macros' option on a `:require' spec --
;; the split runtime/macro case (was undefined-var before the macro graph).
(defn d [x] (mac x))             ; ok

;; A genuinely missing macro var: the macro ns resolves (macro_defs.clj) but does
;; not define it -> the macro graph still reports it, proving it is not a blanket
;; suppression.
(defn e [x] (m/no-such-macro x)) ; UNDEFINED-VAR
