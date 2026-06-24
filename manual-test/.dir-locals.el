;;; Directory Local Variables.  See (info "(emacs) Directory Variables").
;;
;; Declares two project-specific macros as "def-forms".  Both the treesit
;; syntax layer (font-lock) and the treejure semantic layer honour this: the
;; semantic layer analyses `defroute' / `defcomponent' like `defn' (skips the
;; def name, binds the param vector as locals).  See extra_def_forms.clj.

((replique-clojure-mode
  . ((replique-clojure-extra-def-forms . ("defroute" "defcomponent")))))
