#ifndef SCANNER_SHARED_H
#define SCANNER_SHARED_H

#include "tree_sitter/parser.h"
#include <string.h>

enum TokenType {
  WHITESPACE_EXTERNAL,
  NUMBER,
  KEYWORD_MARKER,
  AUTO_RESOLVE_MARKER,
  IDENTIFIER_NAMESPACE, // 2: ee
  IDENTIFIER_NAME,      // 3: rr/tt
  SLASH_SEPARATOR,
  QUOTE_MARKER, SYNTAX_QUOTE_MARKER, DEREF_MARKER, META_MARKER,
  UNQUOTE_MARKER, UNQUOTE_SPLICING_MARKER,
  STRING_EXTERNAL, ERRONEOUS_STRING,
  NIL_LITERAL, BOOL_TRUE, BOOL_FALSE,
  CHARACTER_EXTERNAL, ERRONEOUS_CHARACTER,
  ERRONEOUS_NUMBER,
  REGEX_MARKER,
  SYMBOLIC_VALUE,
  ERRONEOUS_SYMBOLIC_VALUE
};

static bool is_clojure_whitespace(int32_t c) {
  // Clojure treats a char as whitespace when Character.isWhitespace(ch) || ch == ','.
  // Java's isWhitespace covers ASCII controls 0x09-0x0D and 0x1C-0x1F, plus the
  // Unicode space separators (Zs/Zl/Zp) but EXPLICITLY excludes the non-breaking
  // spaces U+00A0, U+2007 and U+202F.
  return c == ' '  || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' ||
         c == ',' ||
         (c >= 0x1C && c <= 0x1F) ||         // file/group/record/unit separators
         c == 0x1680 ||                      // ogham space mark
         (c >= 0x2000 && c <= 0x2006) ||     // en quad .. six-per-em space
         (c >= 0x2008 && c <= 0x200A) ||     // punctuation/thin/hair space (U+2007 excluded)
         c == 0x2028 || c == 0x2029 ||       // line / paragraph separator
         c == 0x205F ||                      // medium mathematical space
         c == 0x3000;                        // ideographic space
}

/**
 * Terminating Macros: Characters that split any word (Symbol, Number, Character).
 * Added @, ^, `, ~, and \ to the standard set.
 * Excluded ' and # because they can be part of symbols.
 */
static bool is_macro_terminating(int32_t c) {
    return c == '"' || c == ';' || c == '(' || c == ')' || 
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '@' || c == '^' || c == '`' || c == '~' || c == '\\';
}

/**
 * General boundary for most tokens.
 */
static bool is_token_boundary(int32_t c) {
    return c == 0 || is_clojure_whitespace(c) || is_macro_terminating(c);
}

#endif

