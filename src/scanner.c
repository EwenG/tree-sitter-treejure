#include "tree_sitter/parser.h"
#include "scanner_shared.h"
#include "numbers.c"
#include <wctype.h>
#include <ctype.h>
#include <string.h>

static int scan_character_type(TSLexer *lexer) {
  lexer->advance(lexer, false); // Consume "\" 
  if (lexer->lookahead == 0) return ERRONEOUS_CHARACTER;
  
  char buffer[32]; int i = 0;
  buffer[i++] = (char)lexer->lookahead; 
  lexer->advance(lexer, false);

  if (is_token_boundary(lexer->lookahead)) return CHARACTER_EXTERNAL;

  while (!is_token_boundary(lexer->lookahead) && i < 31) {
    buffer[i++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  }
  buffer[i] = '\0';

  if (strcmp(buffer, "newline") == 0 || strcmp(buffer, "space") == 0 ||
      strcmp(buffer, "tab") == 0 || strcmp(buffer, "formfeed") == 0 ||
      strcmp(buffer, "backspace") == 0 || strcmp(buffer, "return") == 0) return CHARACTER_EXTERNAL;

  if (i == 5 && buffer[0] == 'u') {
    for (int j = 1; j < 5; j++) if (!isxdigit(buffer[j])) return ERRONEOUS_CHARACTER;
    return CHARACTER_EXTERNAL;
  }
  if (buffer[0] == 'o' && i > 1 && i < 5) {
    for (int j = 1; j < i; j++) if (buffer[j] < '0' || buffer[j] > '7') return ERRONEOUS_CHARACTER;
    return CHARACTER_EXTERNAL;
  }

  return ERRONEOUS_CHARACTER;
}

static int finish_string_content(TSLexer *lexer, int success_type) {
  bool escaped = false;
  while (lexer->lookahead != 0) {
    if (escaped) {
      lexer->advance(lexer, false);
      escaped = false;
    } else if (lexer->lookahead == '\\') {
      lexer->advance(lexer, false);
      escaped = true;
    } else if (lexer->lookahead == '"') {
      lexer->advance(lexer, false); 
      return success_type;
    } else {
      lexer->advance(lexer, false);
    }
  }
  return ERRONEOUS_STRING;
}

/**
 * Combined helper to scan Literals (nil, true, false), Namespaces, and Names.
 * This prevents the lexer from advancing and then returning false.
 */
static bool scan_word(TSLexer *lexer, const bool *valid_symbols, char first_char) {
  char buffer[256];
  int i = 0;

  if (first_char != 0) buffer[i++] = first_char;

  // 1. If the parser is ONLY looking for a Name (part after a slash), consume greedily
  if (valid_symbols[IDENTIFIER_NAME] && !valid_symbols[IDENTIFIER_NAMESPACE]) {
    while (!is_token_boundary(lexer->lookahead)) {
      if (i < 255) buffer[i++] = (char)lexer->lookahead;
      lexer->advance(lexer, false);
    }
    if (i == 0) return false;
    lexer->result_symbol = IDENTIFIER_NAME;
    return true;
  }

  // 2. Otherwise, consume until a boundary or a slash
  while (!is_token_boundary(lexer->lookahead) && lexer->lookahead != '/') {
    if (i < 255) buffer[i++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  }
  buffer[i] = '\0';

  if (i == 0 && lexer->lookahead != '/') return false;

  // 3. Disambiguate Literals
  // Only valid if not followed by a slash (e.g., 'true/foo' is a namespace)
  if (lexer->lookahead != '/') {
    if (strcmp(buffer, "nil") == 0 && valid_symbols[NIL_LITERAL]) {
      lexer->result_symbol = NIL_LITERAL; return true;
    }
    if (strcmp(buffer, "true") == 0 && valid_symbols[BOOL_TRUE]) {
      lexer->result_symbol = BOOL_TRUE; return true;
    }
    if (strcmp(buffer, "false") == 0 && valid_symbols[BOOL_FALSE]) {
      lexer->result_symbol = BOOL_FALSE; return true;
    }
  }

  // 4. Disambiguate Namespace
  if (lexer->lookahead == '/' && valid_symbols[IDENTIFIER_NAMESPACE]) {
    lexer->result_symbol = IDENTIFIER_NAMESPACE;
    return true;
  }

  // 5. Fallback to Name
  if (valid_symbols[IDENTIFIER_NAME]) {
    lexer->result_symbol = IDENTIFIER_NAME;
    return true;
  }

  return false;
}

void *tree_sitter_treejure_external_scanner_create() { return NULL; }
void tree_sitter_treejure_external_scanner_destroy(void *payload) {}
unsigned tree_sitter_treejure_external_scanner_serialize(void *payload, char *buffer) { return 0; }
void tree_sitter_treejure_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {}

bool tree_sitter_treejure_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  while (is_clojure_whitespace(lexer->lookahead)) lexer->advance(lexer, true);
  if (lexer->lookahead == 0) return false;
  
  int32_t first = lexer->lookahead;

  // Reader Macros
  if (first == '\\' && (valid_symbols[CHARACTER_EXTERNAL] || valid_symbols[ERRONEOUS_CHARACTER])) {
    lexer->result_symbol = scan_character_type(lexer); return true;
  }
  if (first == '"' && (valid_symbols[STRING_EXTERNAL] || valid_symbols[ERRONEOUS_STRING])) {
    lexer->advance(lexer, false);
    lexer->result_symbol = finish_string_content(lexer, STRING_EXTERNAL);
    return true;
  }
  if (first == '#' && valid_symbols[REGEX_EXTERNAL]) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '"') {
      lexer->advance(lexer, false);
      lexer->result_symbol = finish_string_content(lexer, REGEX_EXTERNAL);
      return true;
    }
    return false; // Let grammar handle other # dispatchers
  }

  // Simple Markers
  if (first == '~' && (valid_symbols[UNQUOTE_MARKER] || valid_symbols[UNQUOTE_SPLICING_MARKER])) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '@' && valid_symbols[UNQUOTE_SPLICING_MARKER]) {
      lexer->advance(lexer, false); lexer->result_symbol = UNQUOTE_SPLICING_MARKER; return true;
    }
    if (valid_symbols[UNQUOTE_MARKER]) { lexer->result_symbol = UNQUOTE_MARKER; return true; }
    return false;
  }
  if (first == '\'' && valid_symbols[QUOTE_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = QUOTE_MARKER; return true; }
  if (first == '`' && valid_symbols[SYNTAX_QUOTE_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = SYNTAX_QUOTE_MARKER; return true; }
  if (first == '@' && valid_symbols[DEREF_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = DEREF_MARKER; return true; }
  if (first == '^' && valid_symbols[META_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = META_MARKER; return true; }

  // Keywords
  if (first == ':' && (valid_symbols[KEYWORD_MARKER] || valid_symbols[AUTO_RESOLVE_MARKER])) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == ':' && valid_symbols[AUTO_RESOLVE_MARKER]) {
      lexer->advance(lexer, false); lexer->result_symbol = AUTO_RESOLVE_MARKER; return true;
    }
    if (valid_symbols[KEYWORD_MARKER]) { lexer->result_symbol = KEYWORD_MARKER; return true; }
    return false;
  }

  // Slashes
  if (first == '/' && valid_symbols[SLASH_SEPARATOR]) {
    lexer->advance(lexer, false); lexer->result_symbol = SLASH_SEPARATOR; return true;
  }

  // Numbers and Signed Identifiers
  if ((first == '+' || first == '-') && (valid_symbols[NUMBER] || valid_symbols[IDENTIFIER_NAME] || valid_symbols[IDENTIFIER_NAMESPACE])) {
    lexer->advance(lexer, false);
    if (isdigit(lexer->lookahead)) {
      return scan_number_word(lexer, first);
    }
    // Fallback: It's a signed identifier like '+foo' or just '+'
    return scan_word(lexer, valid_symbols, (char)first);
  }

  if (isdigit(first) && (valid_symbols[NUMBER] || valid_symbols[ERRONEOUS_NUMBER])) {
    lexer->advance(lexer, false);
    return scan_number_word(lexer, first);
  }

  // Identifiers and Literals
  if (valid_symbols[IDENTIFIER_NAME] || valid_symbols[IDENTIFIER_NAMESPACE] || 
      valid_symbols[NIL_LITERAL] || valid_symbols[BOOL_TRUE] || valid_symbols[BOOL_FALSE]) {
    if (!is_macro_terminating(first)) {
      return scan_word(lexer, valid_symbols, 0);
    }
  }

  return false;
}
