;;; complete-classpath-manual-test.el --- try the gated dep-reading facts  -*- lexical-binding: t; -*-

;; The bench's normal classpath seeds only project source dirs AND is marked
;; INCOMPLETE, so the two false-positive-prone dependency-reading facts -- the
;; `:unresolved' FACE and the `unresolved-namespace' diagnostic -- are gated OFF
;; (the JVM oracle supplies the real jar-inclusive classpath later, PLAN step 6).
;; This helper bridges that gap for MANUAL testing: it rebuilds the current
;; buffer's workspace with a real `clojure-*.jar' on the classpath AND the
;; `classpath-complete' flag set (`treejure-init's 3rd argument), then re-runs the
;; full-tier check -- so those gated facts light up.  `undefined-var', which is
;; NOT gated, works WITHOUT this (see `undefined_vars.clj').  Nothing here is
;; product code -- it pokes the semantic layer's internals deliberately, only for
;; the bench.
;;
;; The clojure jar matters: with it on the (now complete) classpath, `clojure.core'
;; / `clojure.string' resolve, so core vars and `str/...' usages are clean -- it is
;; what makes the gated facts false-positive-free.
;;
;; Usage:
;;   1. Open `src/manualtest/unresolved.clj' (semantic mode auto-enables).
;;   2. M-x load-file RET .../manual-test/complete-classpath-manual-test.el RET
;;   3. M-x manualtest-complete-check
;;      -> the `:unresolved' face paints `totally-undefined' / `another-missing',
;;         and `unresolved-namespace' underlines the `manualtest.no-such-ns'
;;         require (M-x flymake-show-buffer-diagnostics).  Core vars / interop /
;;         resolved refs stay clean.
;;   4. M-x manualtest-complete-reset  -> back to the interim (gated-off) workspace.
;;
;; If no jar is found under ~/.m2, set `manualtest-complete-classpath' to a list
;; of jar paths yourself, e.g.
;;   (setq manualtest-complete-classpath '("/path/to/clojure-1.12.0.jar"))

;;; Code:

(require 'seq)
(require 'replique-clojure-semantic)

(declare-function treejure-init "treejure-module")

(defvar manualtest-complete-classpath nil
  "Extra (jar) paths to add for `manualtest-complete-check'.
When nil, clojure jars are auto-discovered under ~/.m2.")

(defun manualtest-complete--find-clojure-jars ()
  "Return clojure jars found under the local Maven repo, or nil."
  (let ((dir (expand-file-name "~/.m2/repository/org/clojure")))
    (when (file-directory-p dir)
      (seq-remove
       (lambda (f) (string-match-p "\\(sources\\|javadoc\\)\\.jar\\'" f))
       (directory-files-recursively dir "clojure-[0-9].*\\.jar\\'")))))

(defun manualtest-complete--require-semantic ()
  "Signal unless the current buffer runs the semantic layer."
  (unless (bound-and-true-p replique-clojure-semantic-mode)
    (user-error "Open a manual-test .clj first (replique-clojure-semantic-mode must be on)")))

;;;###autoload
(defun manualtest-complete-check ()
  "Rebuild this buffer's workspace classpath-complete (+ a jar) and re-check.
Flips the gate that enables the `:unresolved' face and `unresolved-namespace'.
`manualtest-complete-reset' undoes it."
  (interactive)
  (manualtest-complete--require-semantic)
  (let ((jars (or manualtest-complete-classpath
                  (manualtest-complete--find-clojure-jars))))
    (unless jars
      (user-error "No clojure jar found; set `manualtest-complete-classpath' to a jar list"))
    (let* ((root (replique-clojure--project-root))
           (cp   (append (replique-clojure--classpath root) jars))
           ;; A fresh workspace whose classpath includes the jar(s) AND is marked
           ;; complete (the 3rd arg) -- so the gated facts are reachable AND
           ;; false-positive-free (clojure.core resolves).  Rebind only THIS
           ;; buffer to it; the shared, interim workspace is left intact for reset.
           (ws   (treejure-init root cp t)))
      (setq replique-clojure--ws ws)
      (replique-clojure--push-def-forms)
      (replique-clojure--check t)
      (message "complete-check: classpath-complete ON (+%d jar) — :unresolved face + unresolved-namespace now live"
               (length jars)))))

;;;###autoload
(defun manualtest-complete-reset ()
  "Restore this buffer's normal interim (gated-off) workspace and re-check."
  (interactive)
  (manualtest-complete--require-semantic)
  (setq replique-clojure--ws
        (replique-clojure--get-workspace (replique-clojure--project-root)))
  (replique-clojure--push-def-forms)
  (replique-clojure--check t)
  (message "complete-reset: interim classpath (gate OFF) — :unresolved face + unresolved-namespace suppressed"))

(provide 'complete-classpath-manual-test)

;;; complete-classpath-manual-test.el ends here
