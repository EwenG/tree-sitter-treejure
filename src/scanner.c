#include "tree_sitter/parser.h"
#include <wctype.h>
#include <ctype.h>
#include <string.h>

enum TokenType {
  NUMBER, SYMBOL, KEYWORD,
  QUOTE_MARKER, SYNTAX_QUOTE_MARKER, DEREF_MARKER, META_MARKER,
  UNQUOTE_MARKER, UNQUOTE_SPLICING_MARKER,
  STRING_EXTERNAL, ERRONEOUS_STRING,
  NIL_LITERAL, BOOL_TRUE, BOOL_FALSE,
  CHARACTER_EXTERNAL, ERRONEOUS_CHARACTER,
  ERRONEOUS_KEYWORD, ERRONEOUS_SYMBOL,
  ERRONEOUS_NUMBER
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

/**
 * Boundary for Numbers. Numbers stop at ANY macro.
 */
static bool is_number_end(int32_t c) {
    return c == 0 || is_clojure_whitespace(c) || is_macro(c);
}

/**
 * Boundary for Symbols, Keywords, and Character names.
 */
static bool is_token_end(int32_t c) {
    return c == 0 || is_clojure_whitespace(c) || is_macro_terminating(c);
}

static bool finish_number(TSLexer *lexer, bool has_digits) {
  bool is_hex = false, is_radix = false, is_float = false, is_ratio = false;

  if (!has_digits && lexer->lookahead == '0') {
    has_digits = true; lexer->advance(lexer, false);
    if (lexer->lookahead == 'x' || lexer->lookahead == 'X') { is_hex = true; lexer->advance(lexer, false); }
  }

  while (!is_number_end(lexer->lookahead)) {
    int32_t c = lexer->lookahead;

    if (isdigit(c)) { has_digits = true; }
    else if (is_hex && isxdigit(c)) { has_digits = true; }
    else if (c == '.' && !is_hex && !is_ratio && !is_float) is_float = true;
    else if (c == '/' && !is_hex && !is_float && !is_ratio) is_ratio = true;
    else if ((c == 'e' || c == 'E') && !is_hex && !is_ratio) {
      is_float = true; lexer->advance(lexer, false);
      if (lexer->lookahead == '+' || lexer->lookahead == '-') lexer->advance(lexer, false);
      continue;
    }
    else if ((c == 'r' || c == 'R') && has_digits && !is_radix && !is_float && !is_ratio && !is_hex) is_radix = true;
    else if (is_radix && iswalnum(c)) has_digits = true;
    else if ((c == 'N' || c == 'M') && has_digits) {
      lexer->advance(lexer, false);
      if (is_number_end(lexer->lookahead)) {
        lexer->result_symbol = NUMBER;
        return true;
      }
      goto number_error;
    } 
    else { goto number_error; }
    lexer->advance(lexer, false);
  }

  if (has_digits) {
    lexer->result_symbol = NUMBER;
    return true;
  }
  return false;

number_error:
  while (!is_number_end(lexer->lookahead)) lexer->advance(lexer, false);
  lexer->result_symbol = ERRONEOUS_NUMBER;
  return true;
}

static int scan_character_type(TSLexer *lexer) {
  lexer->advance(lexer, false); // Consume \ 
  if (lexer->lookahead == 0) return ERRONEOUS_CHARACTER;
  
  char buffer[32]; int i = 0;
  buffer[i++] = (char)lexer->lookahead; 
  lexer->advance(lexer, false);

  if (is_token_end(lexer->lookahead)) return CHARACTER_EXTERNAL;

  while (!is_token_end(lexer->lookahead) && i < 31) {
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

static bool scan_identifier(TSLexer *lexer, int char_count, int result_type) {
  while (!is_token_end(lexer->lookahead)) {
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
  lexer->advance(lexer, false);
  bool escaped = false;
  while (lexer->lookahead != 0) {
    if (escaped) { lexer->advance(lexer, false); escaped = false; }
    else if (lexer->lookahead == '\\') { lexer->advance(lexer, false); escaped = true; }
    else if (lexer->lookahead == '"') { lexer->advance(lexer, false); return STRING_EXTERNAL; }
    else lexer->advance(lexer, false);
  }
  return ERRONEOUS_STRING;
}

static bool scan_exact_word(TSLexer *lexer, const char *word, int len, int res) {
  for (int i = 0; i < len; i++) {
    if (lexer->lookahead != word[i]) return false;
    lexer->advance(lexer, false);
  }
  if (is_token_end(lexer->lookahead)) { lexer->result_symbol = res; return true; }
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

  if (first == '"' && (valid_symbols[STRING_EXTERNAL] || valid_symbols[ERRONEOUS_STRING])) {
    lexer->result_symbol = scan_string_type(lexer); return true;
  }
  if (first == '\\' && (valid_symbols[CHARACTER_EXTERNAL] || valid_symbols[ERRONEOUS_CHARACTER])) {
    lexer->result_symbol = scan_character_type(lexer); return true;
  }
  if (first == '~') {
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

  if (first == 'n' && valid_symbols[NIL_LITERAL] && scan_exact_word(lexer, "nil", 3, NIL_LITERAL)) return true;
  if (first == 't' && valid_symbols[BOOL_TRUE] && scan_exact_word(lexer, "true", 4, BOOL_TRUE)) return true;
  if (first == 'f' && valid_symbols[BOOL_FALSE] && scan_exact_word(lexer, "false", 5, BOOL_FALSE)) return true;

  if (first == ':' && valid_symbols[KEYWORD]) {
    lexer->advance(lexer, false); int c = 1;
    if (lexer->lookahead == ':') { lexer->advance(lexer, false); c++; }
    return scan_identifier(lexer, c, KEYWORD);
  }
  if (first == '+' || first == '-') {
    lexer->advance(lexer, false);
    if (isdigit(lexer->lookahead) && valid_symbols[NUMBER] && finish_number(lexer, true)) return true;
    if (valid_symbols[SYMBOL]) return scan_identifier(lexer, 1, SYMBOL);
    return false;
  }
  if (isdigit(first) && (valid_symbols[NUMBER] || valid_symbols[ERRONEOUS_NUMBER])) {
    return finish_number(lexer, false);
  }
  if (valid_symbols[SYMBOL] && !is_macro(first) && !isdigit(first)) {
    return scan_identifier(lexer, 0, SYMBOL);
  }
  return false;
}
