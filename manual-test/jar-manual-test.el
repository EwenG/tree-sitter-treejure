;;; jar-manual-test.el --- try jar-backed resolution by hand  -*- lexical-binding: t; -*-

;; The bench's normal classpath seeds only project source dirs (the JVM oracle
;; supplies the real jar classpath later), so jar-backed `:global-var' faces are
;; not exercised in-editor by default.  This little helper bridges that gap for
;; MANUAL testing: it adds a real `clojure-*.jar' to the CURRENT buffer's
;; workspace classpath and re-runs the full-tier check, so the jar slice becomes
;; visible.  Nothing here is product code -- it pokes at the semantic layer's
;; internals deliberately, only for the bench.
;;
;; Usage:
;;   1. Open `src/manualtest/jar_resolution.clj' (semantic mode auto-enables).
;;   2. M-x load-file RET .../manual-test/jar-manual-test.el RET
;;   3. In that buffer:  M-x manualtest-jar-check
;;      -> the aliased / qualified / `:refer'-ed `clojure.string'/`clojure.set'
;;         vars light up `:global-var' (default `font-lock-variable-name-face').
;;   4. M-x manualtest-jar-reset  -> drop the jar, back to source-dirs-only.
;;
;; If no jar is found under ~/.m2, set `manualtest-jar-classpath' to a list of
;; jar paths yourself, e.g.
;;   (setq manualtest-jar-classpath '("/path/to/clojure-1.12.0.jar"))

;;; Code:

(require 'seq)
(require 'replique-clojure-semantic)

(defvar manualtest-jar-classpath nil
  "Jar paths to add to the classpath for `manualtest-jar-check'.
When nil, clojure jars are auto-discovered under ~/.m2.")

(defun manualtest--find-clojure-jars ()
  "Return clojure jars found under the local Maven repo, or nil."
  (let ((dir (expand-file-name "~/.m2/repository/org/clojure")))
    (when (file-directory-p dir)
      (seq-remove
       (lambda (f) (string-match-p "\\(sources\\|javadoc\\)\\.jar\\'" f))
       (directory-files-recursively dir "clojure-[0-9].*\\.jar\\'")))))

(defun manualtest--require-semantic ()
  "Signal unless the current buffer runs the semantic layer."
  (unless (bound-and-true-p replique-clojure-semantic-mode)
    (user-error "Open a manual-test .clj first (replique-clojure-semantic-mode must be on)")))

(defun manualtest--gv-count ()
  "Return how many `:global-var' spans the module currently reports."
  (let* ((cats (treejure-category-names))
         (gv (seq-position cats :global-var))
         (end (1- (position-bytes (point-max))))
         (spans (treejure-semantic-faces replique-clojure--ws
                                         replique-clojure--file-id 0 end))
         (n (length spans)) (i 0) (c 0))
    (while (< i n)
      (when (and gv (= (aref spans (+ i 2)) gv)) (setq c (1+ c)))
      (setq i (+ i 3)))
    c))

;;;###autoload
(defun manualtest-jar-check ()
  "Add a real clojure jar to this buffer's classpath and re-check (full tier).
Jar-backed `:global-var' faces (clojure.string/clojure.set usages in
`jar_resolution.clj') should appear.  `manualtest-jar-reset' undoes it."
  (interactive)
  (manualtest--require-semantic)
  (let ((jars (or manualtest-jar-classpath (manualtest--find-clojure-jars))))
    (unless jars
      (user-error "No clojure jar found; set `manualtest-jar-classpath' to a jar list"))
    (let* ((root (replique-clojure--project-root))
           (cp   (append (replique-clojure--classpath root) jars))
           ;; A fresh workspace whose classpath includes the jar(s); rebind only
           ;; THIS buffer to it (the shared, source-dirs-only workspace is left
           ;; intact for `manualtest-jar-reset').
           (ws   (treejure-init root cp)))
      (setq replique-clojure--ws ws)
      (replique-clojure--push-def-forms)
      (replique-clojure--check t)
      (message "jar-check: +%d jar(s); %d :global-var span(s) now — look for painted jar vars"
               (length jars) (manualtest--gv-count)))))

;;;###autoload
(defun manualtest-jar-reset ()
  "Restore this buffer's normal (source-dirs-only) workspace and re-check."
  (interactive)
  (manualtest--require-semantic)
  (setq replique-clojure--ws
        (replique-clojure--get-workspace (replique-clojure--project-root)))
  (replique-clojure--push-def-forms)
  (replique-clojure--check t)
  (message "jar-reset: source-dirs-only classpath; %d :global-var span(s)"
           (manualtest--gv-count)))

(provide 'jar-manual-test)

;;; jar-manual-test.el ends here
