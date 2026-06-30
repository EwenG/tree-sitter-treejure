;;; dialect-surface-test.el --- verify per-dialect .cljc surfaces  -*- lexical-binding: t; -*-

;; Batch check of the per-dialect `.cljc' surfaces slice.  Loads the module
;; directly (no Emacs UI), builds a workspace over the bench src dir, and asserts
;; that cross-file resolution picks the dependency surface for the REQUIRER's
;; dialect:
;;   * a .clj user of `manualtest.dialect-surface' sees `clj-only' but not
;;     `cljs-only' (undefined-var on the latter);
;;   * a .cljs user sees `cljs-only' but not `clj-only' (undefined-var on the
;;     latter);
;;   * jump-to-def lands inside the .cljc for each platform's own var.
;;
;; Run:  emacs --batch -l dialect-surface-test.el

;;; Code:

(require 'cl-lib)

(defconst dst-root
  (file-name-directory (or load-file-name buffer-file-name)))
(defconst dst-src (expand-file-name "src" dst-root))
(defconst dst-so
  (expand-file-name "../treejure-module.so" dst-root))

(module-load dst-so)
(declare-function treejure-init "treejure-module")
(declare-function treejure-check-buffer "treejure-module")
(declare-function treejure-definition "treejure-module")
(declare-function treejure-diagnostic-ids "treejure-module")

(defvar dst-fail 0)
(defun dst-check (ok label)
  (princ (format "  [%s] %s\n" (if ok "PASS" "FAIL") label))
  (unless ok (cl-incf dst-fail)))

(defun dst-read (path)
  (with-temp-buffer
    (insert-file-contents path)
    (buffer-substring-no-properties (point-min) (point-max))))

(defun dst-byte (text needle)
  "0-based byte offset of NEEDLE in TEXT (ASCII bench files)."
  (let ((p (string-search needle text)))
    (and p (string-bytes (substring text 0 p)))))

(defun dst-diags (ws path)
  "Full-tier check of PATH; return list of (ID . FLAGGED-TEXT)."
  (let* ((text (dst-read path))
         (diags (treejure-check-buffer ws path text t)))
    (mapcar (lambda (d)
              (cons (plist-get d :id)
                    (substring text (plist-get d :beg) (plist-get d :end))))
            diags)))

(let* ((ws (treejure-init dst-src (list dst-src)))
       (cljc (expand-file-name "manualtest/dialect_surface.cljc" dst-src))
       (cljuser (expand-file-name "manualtest/dialect_clj_user.clj" dst-src))
       (cljsuser (expand-file-name "manualtest/dialect_cljs_user.cljs" dst-src)))

  (princ "== .clj requirer (reads the clj surface) ==\n")
  (let ((d (dst-diags ws cljuser)))
    (princ (format "  diags: %S\n" d))
    (let ((undef (cl-remove-if-not (lambda (x) (eq (car x) :undefined-var)) d)))
      (dst-check (= 1 (length undef)) "exactly one undefined-var")
      (dst-check (and undef (string-search "cljs-only" (cdr (car undef))))
                 "undefined-var is on ds/cljs-only")))

  (princ "== .cljs requirer (reads the cljs surface) ==\n")
  (let ((d (dst-diags ws cljsuser)))
    (princ (format "  diags: %S\n" d))
    (let ((undef (cl-remove-if-not (lambda (x) (eq (car x) :undefined-var)) d)))
      (dst-check (= 1 (length undef)) "exactly one undefined-var")
      (dst-check (and undef (string-search "clj-only" (cdr (car undef))))
                 "undefined-var is on ds/clj-only")))

  (princ "== the .cljc buffer itself (dual-dialect, deduped) ==\n")
  (let ((d (dst-diags ws cljc)))
    (princ (format "  diags: %S\n" d))
    (dst-check (null (cl-remove-if-not (lambda (x) (eq (car x) :redefined-var)) d))
               "per-platform `per-platform' def is NOT redefined-var"))

  (princ "== in-file jump-to-def across branches (within the .cljc) ==\n")
  (let* ((ctext (dst-read cljc)))
    (treejure-check-buffer ws cljc ctext t)
    ;; `(clj-only n)' usage (clj branch) -> its def in the clj surface (index).
    (let* ((b (dst-byte ctext "(clj-only n)"))
           (loc (treejure-definition ws cljc (+ b 1))))   ; point on `clj-only'
      (princ (format "  clj-only usage -> %S\n" loc))
      (dst-check (and loc (= (plist-get loc :beg) (dst-byte ctext "clj-only [x]")))
                 "clj-only usage jumps to its :clj def"))
    ;; `(cljs-only n)' usage (cljs branch) -> its def in the cljs surface (alt_index).
    (let* ((b (dst-byte ctext "(cljs-only n)"))
           (loc (treejure-definition ws cljc (+ b 1))))   ; point on `cljs-only'
      (princ (format "  cljs-only usage -> %S\n" loc))
      (dst-check (and loc (= (plist-get loc :beg) (dst-byte ctext "cljs-only [x]")))
                 "cljs-only usage jumps to its :cljs def (alt_index)")))

  ;; jump-to-def picks the right surface per dialect.
  (princ "== jump-to-def into the .cljc ==\n")
  (let* ((ctext (dst-read cljuser))
         (b-cljonly (dst-byte ctext "ds/clj-only"))
         (b-cljsonly (dst-byte ctext "ds/cljs-only")))
    ;; re-check so the cached analysis is current
    (treejure-check-buffer ws cljuser ctext t)
    (let ((loc (treejure-definition ws cljuser (+ b-cljonly 3)))) ; point on `clj-only'
      (princ (format "  clj-only -> %S\n" loc))
      (dst-check (and loc (string-suffix-p "dialect_surface.cljc" (plist-get loc :file)))
                 "clj user: ds/clj-only jumps into the .cljc"))
    (let ((loc (treejure-definition ws cljuser (+ b-cljsonly 3)))) ; point on `cljs-only'
      (princ (format "  cljs-only -> %S\n" loc))
      ;; from a .clj file the clj surface has no cljs-only; resolve_cross_ns_var
      ;; also tries the alt surface, so jump-to-def still lands (lenient point
      ;; query) -- but the diagnostic above correctly flags it undefined for clj.
      (dst-check (and loc (string-suffix-p "dialect_surface.cljc" (plist-get loc :file)))
                 "clj user: ds/cljs-only still jumps (lenient point query)")))

  (princ (format "\n%s (%d failure%s)\n"
                 (if (zerop dst-fail) "ALL PASS" "FAILURES")
                 dst-fail (if (= dst-fail 1) "" "s")))
  (kill-emacs (if (zerop dst-fail) 0 1)))

;;; dialect-surface-test.el ends here
