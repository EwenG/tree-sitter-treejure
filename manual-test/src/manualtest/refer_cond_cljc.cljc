(ns manualtest.refer-cond-cljc
  ;; A `.cljc' that `:refer's a macro used only on ONE platform.  The require is
  ;; non-conditional (parsed into both dialect surfaces), but the usage is in a
  ;; `#?(:cljs ...)' branch -- so the clj pass never sees it.  A refer used on
  ;; EITHER platform must not read as unused, so this file has NO diagnostics.
  (:require [manualtest.implicit-macros :refer [imac]]))

(defn a []
  #?(:cljs (imac)
     :clj  nil))
