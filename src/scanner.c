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
  ERRONEOUS_STRING,
  NIL_LITERAL,
  BOOL_TRUE,
  BOOL_FALSE,
  CHARACTER_EXTERNAL,
  ERRONEOUS_CHARACTER
};

static bool is_clojure_whitespace(int32_t c) {
  return c == ' '  || c == '\t' || c == '\r' || c == '\n' || 
         c == ','  || c == '\f' || c == '\v' ||
         c == 0xA0 || // non-breaking space
         c == 0xAD || // soft hyphen
         (c >= 0x2000 && c <= 0x200A) || // various thin/hair spaces
         c == 0x2028 || c == 0x2029 ||   // line/para separators
         c == 0x202F || c == 0x205F || c == 0x3000 || c == 0x1680 || c == 0x180E;
}

// Update is_terminator to use our new helper
static bool is_terminator(int32_t c) {
  return is_clojure_whitespace(c) ||
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

static bool scan_identifier(TSLexer *lexer, int char_count, int result_type) {
  while (lexer->lookahead != 0 && !is_terminator(lexer->lookahead)) {
    lexer->advance(lexer, false);
    char_count++;
  }
  if (char_count > 0) {
    lexer->result_symbol = result_type;
    return true;
  }
  return false;
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

static int scan_character_type(TSLexer *lexer) {
  lexer->advance(lexer, false); // Consume the backslash '\'

  // If backslash is the last character in the file, it's an error
  if (lexer->lookahead == 0) return ERRONEOUS_CHARACTER;

  char buffer[32];
  int i = 0;

  // RULE: The first character after '\' is ALWAYS part of the literal,
  // even if it is a terminator like ',' or '(' or ' '.
  buffer[i++] = (char)lexer->lookahead;
  lexer->advance(lexer, false);

  // If the next character is a terminator, then this was a single-character 
  // literal (like '\,' or '\a' or '\('). We are done.
  if (is_terminator(lexer->lookahead)) {
    return CHARACTER_EXTERNAL;
  }

  // Otherwise, it might be a named character like "\newline" or "\space"
  while (!is_terminator(lexer->lookahead) && i < 31) {
    buffer[i++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  }
  buffer[i] = '\0';

  // 1. Named characters
  if (strcmp(buffer, "newline") == 0 ||
      strcmp(buffer, "space") == 0 ||
      strcmp(buffer, "tab") == 0 ||
      strcmp(buffer, "formfeed") == 0 ||
      strcmp(buffer, "backspace") == 0 ||
      strcmp(buffer, "return") == 0) {
    return CHARACTER_EXTERNAL;
  }

  // 2. Unicode (uXXXX)
  if (i == 5 && buffer[0] == 'u') {
    bool valid = true;
    for (int j = 1; j < 5; j++) if (!isxdigit(buffer[j])) valid = false;
    if (valid) return CHARACTER_EXTERNAL;
  }

  // 3. Octal (oXXX)
  if (i == 4 && buffer[0] == 'o') {
    bool valid = true;
    for (int j = 1; j < 4; j++) if (buffer[j] < '0' || buffer[j] > '7') valid = false;
    if (valid) return CHARACTER_EXTERNAL;
  }

  // 4. If it's a multi-character word that isn't a valid name, it's an error (e.g. \abc)
  return ERRONEOUS_CHARACTER;
}

// Helper to check for a specific word followed by a terminator
static bool scan_exact_word(TSLexer *lexer, const char *word, int length, int result_type) {
  for (int i = 0; i < length; i++) {
    if (lexer->lookahead != word[i]) return false;
    lexer->advance(lexer, false);
  }
  if (is_terminator(lexer->lookahead)) {
    lexer->result_symbol = result_type;
    return true;
  }
  return false;
}

void *tree_sitter_treejure_external_scanner_create() { return NULL; }
void tree_sitter_treejure_external_scanner_destroy(void *payload) {}
unsigned tree_sitter_treejure_external_scanner_serialize(void *payload, char *buffer) { return 0; }
void tree_sitter_treejure_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {}

bool tree_sitter_treejure_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  while (is_clojure_whitespace(lexer->lookahead)) {
    lexer->advance(lexer, true);
  }

  if (lexer->lookahead == 0) return false;

  int32_t first = lexer->lookahead;

  // Check for nil, true, false BEFORE symbols
  if (first == 'n' && valid_symbols[NIL_LITERAL]) {
    if (scan_exact_word(lexer, "nil", 3, NIL_LITERAL)) return true;
    // If it wasn't "nil", it might be a symbol "nilly", so we must handle that below
  }
  if (first == 't' && valid_symbols[BOOL_TRUE]) {
    if (scan_exact_word(lexer, "true", 4, BOOL_TRUE)) return true;
  }
  if (first == 'f' && valid_symbols[BOOL_FALSE]) {
    if (scan_exact_word(lexer, "false", 5, BOOL_FALSE)) return true;
  }

  // Character
  if (first == '\\' && (valid_symbols[CHARACTER_EXTERNAL] || valid_symbols[ERRONEOUS_CHARACTER])) {
    lexer->result_symbol = scan_character_type(lexer);
    return true; // Return true to prevent backtracking
  }

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
  if (first == ':' && valid_symbols[KEYWORD]) {
    lexer->advance(lexer, false);
    int c = 1;
    if (lexer->lookahead == ':') { lexer->advance(lexer, false); c++; }
    return scan_identifier(lexer, c, KEYWORD);
  }

  // DISAMBIGUATE SIGN (+ or -)
  if (first == '+' || first == '-') {
    lexer->advance(lexer, false);
    if (isdigit(lexer->lookahead) && valid_symbols[NUMBER]) {
      if (finish_number(lexer, true)) { lexer->result_symbol = NUMBER; return true; }
    }
    if (valid_symbols[SYMBOL]) return scan_identifier(lexer, 1, SYMBOL);
    return false;
  }

  // NUMBERS
  if (isdigit(first) && valid_symbols[NUMBER]) {
    if (finish_number(lexer, false)) { lexer->result_symbol = NUMBER; return true; }
  }

  // SYMBOLS
  if (valid_symbols[SYMBOL] && !is_reader_macro(first)) {
    return scan_identifier(lexer, 0, SYMBOL);
  }

  return false;
}
