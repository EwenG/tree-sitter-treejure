#include "scanner_shared.h"

typedef enum { NUM_DECIMAL, NUM_HEX, NUM_OCTAL, NUM_RADIX } NumberKind;

static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }
static bool is_hex_digit(int32_t c) { 
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' || c <= 'F'); 
}

static bool scan_number_word(TSLexer *lexer, int32_t first_char) {
    NumberKind kind = NUM_DECIMAL;
    bool has_digits = false;
    bool has_dot = false;
    bool has_slash = false;
    bool has_exp = false;
    bool contains_non_octal_digit = false;
    bool started_with_zero = false;
    bool is_valid = true;

    if (first_char == '+' || first_char == '-') {
        has_digits = false; 
    } else if (is_digit(first_char)) {
        has_digits = true;
        if (first_char == '0') {
            started_with_zero = true;
            kind = NUM_OCTAL;
        }
    }

    if (first_char == '0' && (lexer->lookahead == 'x' || lexer->lookahead == 'X')) {
        kind = NUM_HEX;
        lexer->advance(lexer, false);
        has_digits = false;
    }

    // Numbers stop at a token boundary (whitespace, parens, or reader macros)
    while (!is_token_boundary(lexer->lookahead) && 
           lexer->lookahead != '\'' && 
           lexer->lookahead != '#') {
        int32_t c = lexer->lookahead;
        bool consumed = false;

        if (kind == NUM_HEX) {
            if (is_hex_digit(c)) { has_digits = true; consumed = true; }
        } else if (kind == NUM_RADIX) {
            if (isalnum(c)) { has_digits = true; consumed = true; }
        } else {
            // Decimal / Octal logic...
            if (is_digit(c)) {
                has_digits = true; consumed = true;
                if (c == '8' || c == '9') contains_non_octal_digit = true;
            } else if (c == '.') {
                if (!has_dot && !has_slash && !has_exp) {
                    has_dot = true; kind = NUM_DECIMAL; consumed = true;
                }
            } else if (c == '/') {
                if (!has_dot && !has_slash && !has_exp && has_digits) {
                    has_slash = true; has_digits = false; kind = NUM_DECIMAL; consumed = true;
                }
            } else if (c == 'e' || c == 'E') {
                if (!has_exp && !has_slash && has_digits) {
                    has_exp = true; has_digits = false; kind = NUM_DECIMAL; consumed = true;
                    lexer->advance(lexer, false);
                    if (lexer->lookahead == '+' || lexer->lookahead == '-') lexer->advance(lexer, false);
                    continue;
                }
            } else if (c == 'r' || c == 'R') {
                if (!has_dot && !has_slash && !has_exp && has_digits) {
                    kind = NUM_RADIX; has_digits = false; consumed = true;
                }
            } else if (c == 'N' || c == 'M') {
                lexer->advance(lexer, false);
                if (!is_token_boundary(lexer->lookahead)) is_valid = false;
                goto finish;
            }
        }

        if (!consumed) {
            // Hit an illegal char like ':' which is NOT a boundary
            is_valid = false;
            break;
        }
        lexer->advance(lexer, false);
    }

 finish:
    if (!is_valid) {
        // Swallow the whole invalid word (until whitespace or terminating macro)
        while (!is_token_boundary(lexer->lookahead)) lexer->advance(lexer, false);
        lexer->result_symbol = ERRONEOUS_NUMBER;
        return true;
    }

    bool is_erroneous_octal = (kind == NUM_OCTAL && started_with_zero && contains_non_octal_digit && !has_dot && !has_slash && !has_exp);
    if (has_digits && !is_erroneous_octal) {
        lexer->result_symbol = NUMBER;
    } else {
        lexer->result_symbol = ERRONEOUS_NUMBER;
    }
    return true;
}
