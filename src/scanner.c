#include "tree_sitter/parser.h"
#include <wctype.h>
#include <ctype.h>
#include <string.h>

enum TokenType {
  NUMBER,
  SYMBOL,
  KEYWORD,
  QUOTE_MARKER,
  SYNTAX_QUOTE_MARKER,
  DEREF_MARKER,
  META_MARKER,
  UNQUOTE_MARKER,
  UNQUOTE_SPLICING_MARKER,
  STRING_EXTERNAL,
  ERRONEOUS_STRING
};

static bool is_terminator(int32_t c) {
  return iswspace(c) || c == L',' ||
         c == L'(' || c == L')' || c == L'[' || c == L']' || 
         c == L'{' || c == L'}' || c == L'"' || c == L';' || c == 0;
}

static bool is_reader_macro(int32_t c) {
  return c == L'\'' || c == L'^' || c == L'#' || c == L'@' || c == L'~' || c == L'`';
}

static bool finish_number(TSLexer *lexer, bool has_digits) {
  bool is_ratio = false;
  bool is_float = false;
  bool is_hex = false;
  bool is_radix = false;

  if (!has_digits && lexer->lookahead == '0') {
    has_digits = true;
    lexer->advance(lexer, false);
    if (lexer->lookahead == 'x' || lexer->lookahead == 'X') {
      is_hex = true;
      lexer->advance(lexer, false);
    }
  }

  while (lexer->lookahead != 0 && !is_terminator(lexer->lookahead)) {
    int32_t c = lexer->lookahead;
    if (isdigit(c)) {
      has_digits = true;
    } else if (is_hex && iswxdigit(c)) {
      has_digits = true;
    } else if (c == '.' && !is_ratio && !is_float) {
      is_float = true;
    } else if (c == '/' && !is_ratio && !is_float) {
      is_ratio = true;
    } else if ((c == 'e' || c == 'E') && !is_ratio) {
      is_float = true;
      lexer->advance(lexer, false);
      if (lexer->lookahead == '+' || lexer->lookahead == '-') lexer->advance(lexer, false);
      continue;
    } else if ((c == 'r' || c == 'R') && has_digits && !is_radix && !is_float && !is_ratio) {
      is_radix = true;
    } else if (is_radix && iswalnum(c)) {
      has_digits = true;
    } else if ((c == 'N' || c == 'M') && has_digits) {
      lexer->advance(lexer, false);
      return is_terminator(lexer->lookahead);
    } else {
      return false;
    }
    lexer->advance(lexer, false);
  }
  return has_digits;
}

static bool finish_symbol(TSLexer *lexer, int char_count, int slash_count) {
  while (lexer->lookahead != 0 && !is_terminator(lexer->lookahead)) {
    if (lexer->lookahead == '/') slash_count++;
    lexer->advance(lexer, false);
    char_count++;
  }
  return char_count > 0;
}

static int scan_string_type(TSLexer *lexer) {
  lexer->advance(lexer, false); // Consume opening "
  bool escaped = false;
  while (lexer->lookahead != 0) {
    if (escaped) {
      lexer->advance(lexer, false);
      escaped = false;
    } else if (lexer->lookahead == '\\') {
      lexer->advance(lexer, false);
      escaped = true;
    } else if (lexer->lookahead == '"') {
      lexer->advance(lexer, false); // Consume closing "
      return STRING_EXTERNAL;
    } else {
      lexer->advance(lexer, false);
    }
  }
  return ERRONEOUS_STRING;
}

void *tree_sitter_clojure_external_scanner_create() { return NULL; }
void tree_sitter_clojure_external_scanner_destroy(void *payload) {}
unsigned tree_sitter_clojure_external_scanner_serialize(void *payload, char *buffer) { return 0; }
void tree_sitter_clojure_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {}

bool tree_sitter_clojure_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  while (iswspace(lexer->lookahead) || lexer->lookahead == ',') {
    lexer->advance(lexer, true);
  }

  if (lexer->lookahead == 0) return false;

  int32_t first = lexer->lookahead;

  // Strings
  if (first == '"' && (valid_symbols[STRING_EXTERNAL] || valid_symbols[ERRONEOUS_STRING])) {
    lexer->result_symbol = scan_string_type(lexer);
    return true; 
  }
  
  // UNQUOTE (~) and UNQUOTE-SPLICING (~@)
  if (first == '~') {
    // Only verify we want unquote markers to avoid consuming ~ in unexpected places (rare but safe)
    if (valid_symbols[UNQUOTE_MARKER] || valid_symbols[UNQUOTE_SPLICING_MARKER]) {
      lexer->advance(lexer, false);
      if (lexer->lookahead == '@' && valid_symbols[UNQUOTE_SPLICING_MARKER]) {
        lexer->advance(lexer, false);
        lexer->result_symbol = UNQUOTE_SPLICING_MARKER;
        return true;
      }
      if (valid_symbols[UNQUOTE_MARKER]) {
        lexer->result_symbol = UNQUOTE_MARKER;
        return true;
      }
      return false; // Matched ~ but not expected? Should be rare.
    }
  }

  // Simple Markers
  if (first == '\'' && valid_symbols[QUOTE_MARKER]) { 
      lexer->advance(lexer, false); 
      lexer->result_symbol = QUOTE_MARKER; 
      return true; 
  }
  
  if (first == '`' && valid_symbols[SYNTAX_QUOTE_MARKER]) {
      lexer->advance(lexer, false);
      lexer->result_symbol = SYNTAX_QUOTE_MARKER;
      return true;
  }
  
  if (first == '@' && valid_symbols[DEREF_MARKER]) { 
      lexer->advance(lexer, false); 
      lexer->result_symbol = DEREF_MARKER; 
      return true; 
  }
  
  if (first == '^' && valid_symbols[META_MARKER]) { 
      lexer->advance(lexer, false); 
      lexer->result_symbol = META_MARKER; 
      return true; 
  }

  // KEYWORDS
  if (valid_symbols[KEYWORD] && first == ':') {
    lexer->advance(lexer, false);
    int char_count = 1;
    // Handle ::keyword
    if (lexer->lookahead == ':') { lexer->advance(lexer, false); char_count++; }
    
    // We pass 0 for char_count because we've effectively consumed the prefix
    // and just want to scan the body.
    if (finish_symbol(lexer, char_count, 0)) {
      lexer->result_symbol = KEYWORD;
      return true;
    }
    return false;
  }

  // DISAMBIGUATE SIGN (+ or -)
  if (first == '+' || first == '-') {
    lexer->advance(lexer, false);
    
    // Check if it's a number
    if (isdigit(lexer->lookahead) && valid_symbols[NUMBER]) {
      if (finish_number(lexer, true)) { lexer->result_symbol = NUMBER; return true; }
    }
    
    // Check if it's a symbol
    if (valid_symbols[SYMBOL]) {
      // finish_symbol checking 1 char (the sign we just ate)
      if (finish_symbol(lexer, 1, 0)) { lexer->result_symbol = SYMBOL; return true; }
    }
    return false;
  }

  // NUMBERS
  if (isdigit(first) && valid_symbols[NUMBER]) {
    if (finish_number(lexer, false)) { lexer->result_symbol = NUMBER; return true; }
  }

  // SYMBOLS
  if (valid_symbols[SYMBOL] && !is_reader_macro(first) && !(first == L'"')) {
    if (finish_symbol(lexer, 0, 0)) {
      lexer->result_symbol = SYMBOL;
      return true;
    }
  }

  return false;
}
