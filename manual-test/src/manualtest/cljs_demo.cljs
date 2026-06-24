(ns manualtest.cljs-demo)

;; A ClojureScript buffer: `replique-clojure-clojurescript-mode' derives from
;; `replique-clojure-mode', so the semantic layer is active here too.  The
;; buffer-local analysis (locals, unused, grammar errors) is dialect-agnostic,
;; so everything below behaves exactly as in a .clj file.
;;
;; NOTE: cljs-specific resolution (js/ interop, :require-macros, the cljs core
;; var set) is NOT analysed yet — only locals are.  Don't expect faces on
;; `js/console` etc.

;; :local faces + an unused param (greyed + warned).
(defn greet [name unused]
  (str "Hello, " name "!"))

;; let with a used and an unused binding.
(defn area [r]
  (let [pi     3.14159
        scratch 0]            ; `scratch` greyed + warned
    (* pi r r)))

;; destructuring works the same in cljs.
(defn full-name [{:keys [first last]}]
  (str first " " last))      ; first, last used

;; an anonymous fn literal's params from `fn` (the `#(...)` reader literal does
;; not bind names in this slice, so prefer `fn` when you want :local faces).
(def doubler
  (fn [x] (* x 2)))          ; x used → :local

;; js interop is left to treesit/the cross-file tier — `n` is still a resolved
;; local, but `js/Math.sqrt` gets no semantic face yet.
(defn hypot [n]
  (.sqrt js/Math n))         ; n → :local
