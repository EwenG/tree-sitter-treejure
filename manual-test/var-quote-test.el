;;; var-quote-test.el --- jump/find-usages on a var-quote `#'x'  -*- lexical-binding: t; -*-

;; `bounds-of-thing-at-point' folds the `#'' prefix of a var-quote into the
;; symbol, so an `M-.' / `M-?' point query lands on the `#'' -- the module must
;; still resolve the var the var-quote names, at ANY byte in the `#'symbol' span.
;;
;; Run:  emacs --batch -l var-quote-test.el

;;; Code:

(require 'cl-lib)

(defconst vqt-root (file-name-directory (or load-file-name buffer-file-name)))
(defconst vqt-src (expand-file-name "src" vqt-root))
(module-load (expand-file-name "../treejure-module.so" vqt-root))
(declare-function treejure-init "treejure-module")
(declare-function treejure-check-buffer "treejure-module")
(declare-function treejure-definition "treejure-module")
(declare-function treejure-references "treejure-module")

(defvar vqt-fail 0)
(defun vqt-check (ok label)
  (princ (format "  [%s] %s\n" (if ok "PASS" "FAIL") label))
  (unless ok (cl-incf vqt-fail)))

(defun vqt-read (p)
  (with-temp-buffer (insert-file-contents p)
                    (buffer-substring-no-properties (point-min) (point-max))))

(defun vqt-byte (text needle)
  (let ((p (string-search needle text)))
    (and p (string-bytes (substring text 0 p)))))

(let* ((ws (treejure-init vqt-src (list vqt-src)))
       (p (expand-file-name "manualtest/var_quote.clj" vqt-src))
       (text (vqt-read p))
       (nav-target (expand-file-name "manualtest/nav_target.clj" vqt-src)))
  (treejure-check-buffer ws p text t)

  ;; A point query at EVERY byte of `#'local-var' (the `#', the `'', and the
  ;; symbol) must jump to the in-file def and find its usages.
  (let ((b (vqt-byte text "#'local-var")))
    (princ "== bare var-quote #'local-var ==\n")
    (dolist (off '(0 1 2))
      (let ((def (treejure-definition ws p (+ b off)))
            (refs (treejure-references ws p (+ b off) nil)))
        (vqt-check (and def (string-suffix-p "var_quote.clj" (plist-get def :file)))
                   (format "jump-to-def at byte+%d resolves" off))
        (vqt-check (= (length refs) 3)
                   (format "find-usages at byte+%d returns 3 occurrences" off)))))

  ;; A fully-qualified var-quote resolves cross-file, at any byte in the span.
  (let ((b (vqt-byte text "#'manualtest.nav-target/greet")))
    (princ "== qualified var-quote #'manualtest.nav-target/greet ==\n")
    (dolist (off '(0 1 2))
      (let ((def (treejure-definition ws p (+ b off))))
        (vqt-check (and def (string-suffix-p "nav_target.clj" (plist-get def :file)))
                   (format "cross-file jump-to-def at byte+%d lands in nav_target.clj" off)))))

  ;; A plain-quoted symbol `'x' (the `'' prefix or the symbol) navigates too.
  (let ((b (vqt-byte text "'quoted-var")))
    (princ "== bare quoted symbol 'quoted-var ==\n")
    (dolist (off '(0 1))
      (let ((def (treejure-definition ws p (+ b off))))
        (vqt-check (and def (string-suffix-p "var_quote.clj" (plist-get def :file)))
                   (format "jump-to-def at byte+%d resolves the in-file var" off)))))
  (let ((b (vqt-byte text "'manualtest.nav-target/max-size")))
    (princ "== qualified quoted symbol 'manualtest.nav-target/max-size ==\n")
    (dolist (off '(0 1))
      (let ((def (treejure-definition ws p (+ b off))))
        (vqt-check (and def (string-suffix-p "nav_target.clj" (plist-get def :file)))
                   (format "cross-file jump-to-def at byte+%d lands in nav_target.clj" off))))))

(princ (format "\n%s\n" (if (zerop vqt-fail) "ALL PASS" "FAILURES")))
(kill-emacs (if (zerop vqt-fail) 0 1))

;;; var-quote-test.el ends here
