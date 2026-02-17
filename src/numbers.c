#include "scanner_shared.h"

static bool is_number_end(int32_t c) {
    return c == 0 || is_clojure_whitespace(c) || is_macro(c);
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
