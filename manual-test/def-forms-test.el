;;; def-forms-test.el --- verify `replique-clojure-extra-def-forms' cross-file  -*- lexical-binding: t; -*-

;; A var defined by a project extra-def-form (`defcomponent'/`defroute',
;; declared via `.dir-locals.el' and pushed with `treejure-set-def-forms') must
;; resolve cross-namespace, not read as `undefined-var'.  The subtle case: the
;; dependency defining the var may have been indexed & CACHED under the OLD
;; def-forms, so `treejure-set-def-forms' must invalidate cached dep surfaces.
;;
;; Asserts, on the cljs pair `manualtest.edf-user' -> `manualtest.edf-defs':
;;   * with NO def-forms, the extra-def-form vars are undefined-var (baseline);
;;   * `treejure-set-def-forms' AFTER the dep was already cached re-resolves them;
;;   * setting def-forms up front also resolves them.
;;
;; Run:  emacs --batch -l def-forms-test.el

;;; Code:

(require 'cl-lib)

(defconst dft-root (file-name-directory (or load-file-name buffer-file-name)))
(defconst dft-src (expand-file-name "src" dft-root))
(module-load (expand-file-name "../treejure-module.so" dft-root))
(declare-function treejure-init "treejure-module")
(declare-function treejure-check-buffer "treejure-module")
(declare-function treejure-set-def-forms "treejure-module")

(defvar dft-fail 0)
(defun dft-check (ok label)
  (princ (format "  [%s] %s\n" (if ok "PASS" "FAIL") label))
  (unless ok (cl-incf dft-fail)))

(defun dft-read (p)
  (with-temp-buffer (insert-file-contents p)
                    (buffer-substring-no-properties (point-min) (point-max))))

(defun dft-undefined (ws p)
  "Full-tier check of P; return the list of undefined-var flagged texts."
  (let ((text (dft-read p)))
    (delq nil
          (mapcar (lambda (d)
                    (and (eq (plist-get d :id) :undefined-var)
                         (substring text (plist-get d :beg) (plist-get d :end))))
                  (treejure-check-buffer ws p text t)))))

(let ((user (expand-file-name "manualtest/edf_user.cljs" dft-src)))

  ;; --- Invalidation path: dep cached under empty def-forms, then changed. ----
  (let ((ws (treejure-init dft-src (list dft-src))))
    (princ "== no def-forms (baseline) ==\n")
    (let ((u (dft-undefined ws user)))
      (princ (format "  undefined: %S\n" u))
      (dft-check (and (member "edf/widget" u) (member "widget" u))
                 "extra-def-form vars are undefined-var without def-forms"))
    ;; This check already CACHED `edf_defs.cljs' distilled without def-forms.
    (treejure-set-def-forms ws ["defroute" "defcomponent"])
    (princ "== after treejure-set-def-forms (cache must invalidate) ==\n")
    (let ((u (dft-undefined ws user)))
      (princ (format "  undefined: %S\n" u))
      (dft-check (null u)
                 "cross-ns + :refer'd + in-file extra-def-form vars all resolve")))

  ;; --- def-forms set up front (fresh workspace). ----------------------------
  (let ((ws (treejure-init dft-src (list dft-src))))
    (treejure-set-def-forms ws ["defroute" "defcomponent"])
    (princ "== def-forms set before any check ==\n")
    (let ((u (dft-undefined ws user)))
      (princ (format "  undefined: %S\n" u))
      (dft-check (null u) "extra-def-form vars resolve when set up front")))

  ;; --- A `:refer'-ed extra-def-form macro used as a def-form is not unused. --
  (let ((ws (treejure-init dft-src (list dft-src)))
        (ru (expand-file-name "manualtest/edf_refer_user.cljs" dft-src)))
    (treejure-set-def-forms ws ["defroute" "defcomponent"])
    (let* ((text (dft-read ru))
           (diags (mapcar (lambda (d) (cons (plist-get d :id)
                                            (substring text (plist-get d :beg) (plist-get d :end))))
                          (treejure-check-buffer ws ru text t))))
      (princ "== :refer'd extra-def-form macro used as a def-form ==\n")
      (princ (format "  diags: %S\n" diags))
      (dft-check (null diags)
                 "a :refer'd def-form macro used as `(defcomponent ...)' is not unused"))))

(princ (format "\n%s\n" (if (zerop dft-fail) "ALL PASS" "FAILURES")))
(kill-emacs (if (zerop dft-fail) 0 1))

;;; def-forms-test.el ends here
