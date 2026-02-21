#ifndef SCANNER_SHARED_H
#define SCANNER_SHARED_H

#include "tree_sitter/parser.h"
#include <wctype.h>
#include <ctype.h>
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
  ERRONEOUS_KEYWORD, ERRONEOUS_SYMBOL,
  ERRONEOUS_NUMBER,
  REGEX_MARKER
};

static bool is_clojure_whitespace(int32_t c) {
  return c == ' '  || c == '\t' || c == '\r' || c == '\n' || 
         c == ','  || c == '\f' || c == '\v' ||
         c == 0xA0 || c == 0xAD || (c >= 0x2000 && c <= 0x200A) || 
         c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || 
         c == 0x3000 || c == 0x1680 || c == 0x180E;
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
