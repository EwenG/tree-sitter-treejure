(ns manualtest.ns-scope
  (:require [clojure.string :refer [join]]))

;; ===========================================================================
;; The (ns ...) form is data, not a value-reference context
;; ===========================================================================
;;
;; The require/use specs in an `(ns ...)` form name libraries, aliases and
;; referred vars — they are NOT usages of in-file vars.  The scope pass skips
;; the whole form (the dedicated require pass owns it), so a var defined in the
;; file that happens to share a name with a require symbol picks up no phantom
;; occurrence at the require site.
;;
;; Here the file both `:refer`s `join` (from clojure.string) and defines its own
;; `join` below — a deliberate name clash to exercise the analyzer.  Put point
;; on `join` in `use-join` and press `M-?` (xref-find-references): it lists
;; exactly TWO occurrences — the in-file `(defn join …)` and the call below —
;; and does NOT include the `:refer [join]` in the ns header above.

(defn join [xs]              ; in-file var named `join`
  (apply str xs))

(defn use-join []
  (join ["a" "b"]))          ; M-? here → def + this call only (ns header excluded)
