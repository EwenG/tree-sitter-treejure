(ns manualtest.edf-defs)

;; Vars defined by project `replique-clojure-extra-def-forms' macros
;; (`defcomponent'/`defroute') in a ClojureScript file.
(defcomponent widget [props] [:div props])

(defroute home [req] (:uri req))
