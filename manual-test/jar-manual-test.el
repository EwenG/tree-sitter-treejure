;;; jar-manual-test.el --- try jar-backed resolution by hand  -*- lexical-binding: t; -*-

;; The bench's normal classpath seeds only project source dirs (the JVM oracle
;; supplies the real jar classpath later), so jar-backed resolution is not
;; exercised in-editor by default.  This little helper bridges that gap for
;; MANUAL testing: it adds a real `clojure-*.jar' to the CURRENT buffer's
;; workspace classpath and re-runs the full-tier check, so the jar slice becomes
;; reachable.  Nothing here is product code -- it pokes at the semantic layer's
;; internals deliberately, only for the bench.
;;
;; There is no semantic var face (resolved vars are colored by the treesit
;; syntax layer), so the jar slice is exercised through JUMP-TO-DEFINITION: with
;; a jar on the classpath, `M-.' on a jar var navigates *into* the jar entry's
;; source, opened read-only (the jar-nav slice).
;;
;; Usage:
;;   1. Open `src/manualtest/jar_resolution.clj' (semantic mode auto-enables).
;;   2. M-x load-file RET .../manual-test/jar-manual-test.el RET
;;   3. In that buffer:  M-x manualtest-jar-check
;;      -> point on an aliased / fully-qualified jar var (e.g. `str/upper-case')
;;         and `M-.' now jumps into `clojure/string.clj' inside the jar.
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

(defun manualtest--jar-def-count ()
  "Return how many namespaced symbol usages here jump-to-def into a jar.
Scans for `ns/name' tokens and asks the module to resolve each; counts those
whose definition lands in a synthetic \"<jar>!<entry>\" location.  A signal that
the jar slice is reachable, not an exhaustive count (bare `:refer'-ed jar vars
have no namespace token and are skipped -- try `M-.' on those by hand)."
  (let ((c 0))
    (save-excursion
      (goto-char (point-min))
      (while (re-search-forward "[[:alnum:].$*+!?<>=&_-]+/[[:alnum:].$*+!?<>=&_-]+" nil t)
        (let* ((byte (1- (position-bytes (match-beginning 0))))
               (loc  (ignore-errors
                       (treejure-definition replique-clojure--ws
                                            replique-clojure--file-id byte)))
               (file (plist-get loc :file)))
          (when (and file (string-match-p "\\.jar!" file))
            (setq c (1+ c))))))
    c))

;;;###autoload
(defun manualtest-jar-check ()
  "Add a real clojure jar to this buffer's classpath and re-check (full tier).
Jar-backed jump-to-def then works: `M-.' on a jar var (the
clojure.string/clojure.set usages in `jar_resolution.clj') navigates into the
jar entry's source, opened read-only.  `manualtest-jar-reset' undoes it."
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
      (message "jar-check: +%d jar(s); %d namespaced jar jump-to-def target(s) — try M-. on a jar var"
               (length jars) (manualtest--jar-def-count)))))

;;;###autoload
(defun manualtest-jar-reset ()
  "Restore this buffer's normal (source-dirs-only) workspace and re-check."
  (interactive)
  (manualtest--require-semantic)
  (setq replique-clojure--ws
        (replique-clojure--get-workspace (replique-clojure--project-root)))
  (replique-clojure--push-def-forms)
  (replique-clojure--check t)
  (message "jar-reset: source-dirs-only classpath; %d jar jump-to-def target(s)"
           (manualtest--jar-def-count)))

(provide 'jar-manual-test)

;;; jar-manual-test.el ends here
