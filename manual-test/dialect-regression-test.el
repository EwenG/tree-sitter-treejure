;;; dialect-regression-test.el --- bench regression after per-dialect surfaces -*- lexical-binding: t; -*-

;; Asserts the PLAN invariant that survives the per-dialect `.cljc' change: the
;; cold-scan `treejure-analyze' :diagnostics total equals an independent per-file
;; full-tier `treejure-check-buffer' sum over the same files.  Also dumps each
;; file's diagnostic count so a human can eyeball the `.cljc' files.
;;
;; Run:  emacs --batch -l dialect-regression-test.el

;;; Code:
(require 'cl-lib)

(defconst drt-root (file-name-directory (or load-file-name buffer-file-name)))
(defconst drt-src (expand-file-name "src" drt-root))
(module-load (expand-file-name "../treejure-module.so" drt-root))
(declare-function treejure-init "treejure-module")
(declare-function treejure-check-buffer "treejure-module")
(declare-function treejure-analyze "treejure-module")
(declare-function treejure-close-buffer "treejure-module")

(defun drt-read (p)
  (with-temp-buffer (insert-file-contents p)
                    (buffer-substring-no-properties (point-min) (point-max))))

(let* ((files (sort (directory-files-recursively drt-src "\\.clj[scd]?\\'") #'string<))
       (drt-fail 0))

  ;; Per-file full-tier sum (fresh workspace, each file analyzed in isolation).
  (let* ((ws (treejure-init drt-src (list drt-src)))
         (per-file 0))
    (princ "== per-file full-tier diagnostic counts ==\n")
    (dolist (f files)
      (let* ((n (length (treejure-check-buffer ws f (drt-read f) t))))
        (princ (format "  %3d  %s\n" n (file-relative-name f drt-src)))
        (cl-incf per-file n)
        (treejure-close-buffer ws f)))
    (princ (format "  per-file total: %d\n\n" per-file))

    ;; Cold scan over the same scope (fresh workspace).
    (let* ((ws2 (treejure-init drt-src (list drt-src)))
           (summary (treejure-analyze ws2 (list drt-src)))
           (cold (plist-get summary :diagnostics))
           (cfiles (plist-get summary :files)))
      (princ (format "== treejure-analyze: %d files, %d diagnostics ==\n" cfiles cold))
      (princ (format "  [%s] cold-scan total == per-file total (%d == %d)\n"
                     (if (= cold per-file) "PASS" "FAIL") cold per-file))
      (unless (= cold per-file) (cl-incf drt-fail)))

    (princ (format "\n%s\n" (if (zerop drt-fail) "ALL PASS" "FAILURES")))
    (kill-emacs (if (zerop drt-fail) 0 1))))

;;; dialect-regression-test.el ends here
