(ns manualtest.extra-def-forms)

;; This project's `.dir-locals.el' declares `defroute' and `defcomponent' as
;; `replique-clojure-extra-def-forms'.  The semantic layer therefore analyses
;; them like `defn': the def-name is skipped and the param vector binds locals,
;; so params get :local faces and unused ones are greyed + warned.
;;
;; To see the difference, eval `(setq-local replique-clojure-extra-def-forms nil)'
;; and `M-x revert-buffer' — the param faces/warnings below disappear, because
;; the module then treats `defroute'/`defcomponent' as ordinary calls.

;; `req` is bound by the param vector and used → :local.
(defroute home [req]
  (str "home: " (:uri req)))

;; `req` used, `unused-q` never used → greyed + "unused binding unused-q".
(defroute search [req unused-q]
  (str "results for " (:q req)))

;; Multiple params, all used → all :local.
(defcomponent button [props children]
  [:button props children])

;; Destructuring works too (same machinery as defn).
(defcomponent card [{:keys [title body]}]
  [:div [:h1 title] [:p body]])     ; title, body used
