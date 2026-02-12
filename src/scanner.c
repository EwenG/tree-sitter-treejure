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
  CHARACTER_EXTERNAL, ERRONEOUS_CHARACTER
};

static bool is_clojure_whitespace(int32_t c) {
  return c == ' '  || c == '\t' || c == '\r' || c == '\n' || 
         c == ','  || c == '\f' || c == '\v' ||
         c == 0xA0 || c == 0xAD || 
         (c >= 0x2000 && c <= 0x200A) || 
         c == 0x2028 || c == 0x2029 || 
         c == 0x202F || c == 0x205F || c == 0x3000 || c == 0x1680 || c == 0x180E;
}

// Mirroring lijeur's macroMask (forbids starting symbols/numbers)
static bool is_macro(int32_t c) {
  return c == '"' || c == ':' || c == ';' || c == '\'' || c == '@' || 
         c == '^' || c == '`' || c == '~' || c == '(' || c == ')' || 
         c == '[' || c == ']' || c == '{' || c == '}' || c == '\\' || 
         c == '#'; 
         // Note: '%' removed from here. In Clojure, symbols can start with '%'
}

// Mirroring lijeur's macroTerminatingMask (ends symbols/keywords)
static bool is_macro_terminating(int32_t c) {
  return c == '"' || c == ';' || c == '@' || 
         c == '^' || c == '`' || c == '~' || c == '(' || c == ')' || 
         c == '[' || c == ']' || c == '{' || c == '}' || c == '\\';
}

static bool is_token_end(int32_t c) {
  return c == 0 || is_clojure_whitespace(c) || is_macro_terminating(c);
}

// COMPILER-GRADE NUMBER SCANNER
static bool finish_number(TSLexer *lexer, bool has_digits) {
  bool is_ratio = false;
  bool is_float = false;
  bool is_hex = false;
  bool is_radix = false;

  // 1. Handle Hex/Octal Prefix
  if (!has_digits && lexer->lookahead == '0') {
    has_digits = true;
    lexer->advance(lexer, false);
    if (lexer->lookahead == 'x' || lexer->lookahead == 'X') {
      is_hex = true;
      lexer->advance(lexer, false);
    }
  }

  // 2. Main Loop
  while (!is_token_end(lexer->lookahead)) {
    int32_t c = lexer->lookahead;

    if (isdigit(c)) {
      has_digits = true;
    } else if (is_hex && iswxdigit(c)) {
      has_digits = true;
    } else if (c == '.' && !is_hex && !is_ratio && !is_float) {
      is_float = true;
    } else if (c == '/' && !is_hex && !is_float && !is_ratio) {
      is_ratio = true;
    } else if ((c == 'e' || c == 'E') && !is_hex && !is_ratio) {
      is_float = true;
      lexer->advance(lexer, false);
      if (lexer->lookahead == '+' || lexer->lookahead == '-') lexer->advance(lexer, false);
      continue; 
    } else if ((c == 'r' || c == 'R') && has_digits && !is_radix && !is_float && !is_ratio && !is_hex) {
      is_radix = true;
    } else if (is_radix && iswalnum(c)) {
      has_digits = true;
    } else if ((c == 'N' || c == 'M') && has_digits) {
      lexer->advance(lexer, false);
      return is_token_end(lexer->lookahead);
    } else {
      return false; // Backtrack to symbol
    }
    lexer->advance(lexer, false);
  }
  return has_digits;
}

static int scan_character_type(TSLexer *lexer) {
  lexer->advance(lexer, false); // Consume \ 
  if (lexer->lookahead == 0) return ERRONEOUS_CHARACTER;

  char buffer[32];
  int i = 0;
  buffer[i++] = (char)lexer->lookahead;
  lexer->advance(lexer, false);

  // If followed by space or macro, it's a 1-char literal (e.g. \()
  if (is_token_end(lexer->lookahead)) return CHARACTER_EXTERNAL;

  // Read word
  while (!is_token_end(lexer->lookahead) && i < 31) {
    buffer[i++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  }
  buffer[i] = '\0';

  if (strcmp(buffer, "newline") == 0 || strcmp(buffer, "space") == 0 ||
      strcmp(buffer, "tab") == 0 || strcmp(buffer, "formfeed") == 0 ||
      strcmp(buffer, "backspace") == 0 || strcmp(buffer, "return") == 0) {
    return CHARACTER_EXTERNAL;
  }

  // Unicode \uXXXX
  if (i == 5 && buffer[0] == 'u') {
    for (int j = 1; j < 5; j++) if (!isxdigit(buffer[j])) return ERRONEOUS_CHARACTER;
    return CHARACTER_EXTERNAL;
  }

  // Octal \oXXX
  if (buffer[0] == 'o') {
    if (i < 2 || i > 4) return ERRONEOUS_CHARACTER;
    for (int j = 1; j < i; j++) if (buffer[j] < '0' || buffer[j] > '7') return ERRONEOUS_CHARACTER;
    return CHARACTER_EXTERNAL;
  }

  return ERRONEOUS_CHARACTER;
}

// ... scan_string_type and scan_exact_word remain the same ...

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

static bool scan_exact_word(TSLexer *lexer, const char *word, int length, int result_type) {
  for (int i = 0; i < length; i++) {
    if (lexer->lookahead != word[i]) return false;
    lexer->advance(lexer, false);
  }
  if (is_token_end(lexer->lookahead)) {
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
  while (is_clojure_whitespace(lexer->lookahead)) lexer->advance(lexer, true);
  if (lexer->lookahead == 0) return false;

  int32_t first = lexer->lookahead;

  // 1. Dispatch
  if (first == '"' && (valid_symbols[STRING_EXTERNAL] || valid_symbols[ERRONEOUS_STRING])) {
    lexer->result_symbol = scan_string_type(lexer); return true;
  }
  if (first == '\\' && (valid_symbols[CHARACTER_EXTERNAL] || valid_symbols[ERRONEOUS_CHARACTER])) {
    lexer->result_symbol = scan_character_type(lexer); return true;
  }

  // 2. Unquote / Splicing
  if (first == '~') {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '@' && valid_symbols[UNQUOTE_SPLICING_MARKER]) {
      lexer->advance(lexer, false); lexer->result_symbol = UNQUOTE_SPLICING_MARKER; return true;
    }
    if (valid_symbols[UNQUOTE_MARKER]) { lexer->result_symbol = UNQUOTE_MARKER; return true; }
    return false;
  }

  // 3. Simple Markers
  if (first == '\'' && valid_symbols[QUOTE_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = QUOTE_MARKER; return true; }
  if (first == '`' && valid_symbols[SYNTAX_QUOTE_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = SYNTAX_QUOTE_MARKER; return true; }
  if (first == '@' && valid_symbols[DEREF_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = DEREF_MARKER; return true; }
  if (first == '^' && valid_symbols[META_MARKER]) { lexer->advance(lexer, false); lexer->result_symbol = META_MARKER; return true; }

  // 4. Literals
  if (first == 'n' && valid_symbols[NIL_LITERAL] && scan_exact_word(lexer, "nil", 3, NIL_LITERAL)) return true;
  if (first == 't' && valid_symbols[BOOL_TRUE] && scan_exact_word(lexer, "true", 4, BOOL_TRUE)) return true;
  if (first == 'f' && valid_symbols[BOOL_FALSE] && scan_exact_word(lexer, "false", 5, BOOL_FALSE)) return true;

  // 5. Keywords
  if (first == ':' && valid_symbols[KEYWORD]) {
    lexer->advance(lexer, false); int c = 1;
    if (lexer->lookahead == ':') { lexer->advance(lexer, false); c++; }
    return scan_identifier(lexer, c, KEYWORD);
  }

  // 6. Sign / Numbers
  if (first == '+' || first == '-') {
    lexer->advance(lexer, false);
    if (isdigit(lexer->lookahead) && valid_symbols[NUMBER]) {
      if (finish_number(lexer, true)) { lexer->result_symbol = NUMBER; return true; }
    }
    if (valid_symbols[SYMBOL]) return scan_identifier(lexer, 1, SYMBOL);
    return false;
  }
  if (isdigit(first) && valid_symbols[NUMBER]) {
    if (finish_number(lexer, false)) { lexer->result_symbol = NUMBER; return true; }
  }

  // 7. Symbols (Catch-all)
  if (valid_symbols[SYMBOL] && !is_macro(first)) {
    return scan_identifier(lexer, 0, SYMBOL);
  }

  return false;
}
