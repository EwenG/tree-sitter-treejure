(ns manualtest.grammar-errors)

;; ===========================================================================
;; THIS FILE IS INTENTIONALLY BROKEN.
;;
;; It exercises the Tier-0, grammar-level diagnostics the module surfaces (also
;; faced red by treesit itself).  Run `M-x flymake-show-buffer-diagnostics' to
;; see all six.  The errors are ordered so the token-level ones stay contained;
;; the unterminated string at the very end deliberately runs to EOF.
;; ===========================================================================

;; invalid-number — a malformed numeric token (two decimal points).
(def bad-number 1.2.3)

;; invalid-character — a character literal that is not a single/named char.
(def bad-char \abc)

;; invalid-symbolic-value — only ##Inf, ##-Inf, ##NaN are valid.
(def bad-symbolic ##foo)

;; syntax-error — a stray, unbalanced closing paren produces an ERROR node.
(+ 1 2))

;; invalid-string + missing-form — the unterminated string runs to end-of-file
;; (invalid-string), and the enclosing list is never closed (missing-form).
;; Keep this LAST.
(def bad-string "oops
