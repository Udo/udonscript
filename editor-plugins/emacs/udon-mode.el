;;; udon-mode.el --- Major mode for UdonScript -*- lexical-binding: t; -*-

(defvar udon-mode-hook nil)

(defvar udon-keywords
  '("function" "return" "var" "if" "else" "while" "for" "foreach" "in"
    "break" "continue" "switch" "case" "default" "on"))

(defvar udon-types '("vec2" "vec3" "vec4" "int" "float" "Int" "Float" "s32" "f32" "string" "String" "bool" "Bool" "array" "Array" "any" "Any"))

(defvar udon-font-lock-keywords
  `((,(regexp-opt udon-keywords 'symbols) . font-lock-keyword-face)
    (,(regexp-opt '("true" "false") 'symbols) . font-lock-constant-face)
    (,(regexp-opt udon-types 'symbols) . font-lock-type-face)
    ("\\<[0-9]+\\(?:\\.[0-9]+\\)?\\>" . font-lock-constant-face)
    ("\\$[A-Za-z_][A-Za-z0-9_]*" . font-lock-function-name-face)))

(defvar udon-mode-syntax-table
  (let ((st (make-syntax-table)))
    ;; // comments
    (modify-syntax-entry ?/ ". 124b" st)
    (modify-syntax-entry ?* ". 23" st)
    (modify-syntax-entry ?\n "> b" st)
    ;; strings
    (modify-syntax-entry ?\" "\"" st)
    (modify-syntax-entry ?' "\"" st)
    st))

(define-derived-mode udon-mode prog-mode "UdonScript"
  "Major mode for editing UdonScript."
  :syntax-table udon-mode-syntax-table
  (setq font-lock-defaults '(udon-font-lock-keywords))
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local indent-tabs-mode nil))

(add-to-list 'auto-mode-alist '("\\.udon\\'" . udon-mode))

(provide 'udon-mode)
;;; udon-mode.el ends here
