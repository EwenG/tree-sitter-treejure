(ns manualtest.edf-refer-user
  ;; `:refer' a macro that is ALSO a project extra-def-form, then use it as a
  ;; def-form.  The head is consumed as a def-form, but it is still a USAGE of the
  ;; referred macro -- so the `:refer' must not read as unused.
  (:require [manualtest.macro-defs :refer [defcomponent]]))

(defcomponent widget [props] [:div props])
