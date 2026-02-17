#ifndef SCANNER_SHARED_H
#define SCANNER_SHARED_H

#include "tree_sitter/parser.h"
#include <wctype.h>
#include <ctype.h>
#include <string.h>

enum TokenType {
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
  REGEX_EXTERNAL
};

static bool is_clojure_whitespace(int32_t c) {
  return c == ' '  || c == '\t' || c == '\r' || c == '\n' || 
         c == ','  || c == '\f' || c == '\v' ||
         c == 0xA0 || c == 0xAD || (c >= 0x2000 && c <= 0x200A) || 
         c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || 
         c == 0x3000 || c == 0x1680 || c == 0x180E;
}

/**
 * Mirroring LispReader.isMacro(ch)
 */
static bool is_macro(int32_t c) {
    return c == '"' || c == ':' || c == ';' || c == '\'' || c == '@' || 
           c == '^' || c == '`' || c == '~' || c == '(' || c == ')' || 
           c == '[' || c == ']' || c == '{' || c == '}' || c == '\\' || 
           c == '#';
}

/**
 * Mirroring LispReader.isTerminatingMacro(ch)
 * Excludes '#', '\'', and ':'
 */
static bool is_macro_terminating(int32_t c) {
    if (!is_macro(c)) return false;
    return (c != '#' && c != '\'' && c != ':');
}

#endif
