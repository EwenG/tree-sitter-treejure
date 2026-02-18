#include "scanner_shared.h"

typedef enum { NUM_DECIMAL, NUM_HEX, NUM_OCTAL, NUM_RADIX } NumberKind;

static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

static bool is_hex_digit(int32_t c) { 
    return (c >= '0' && c <= '9') || 
           (c >= 'a' && c <= 'f') || 
           (c >= 'A' && c <= 'F'); 
}

static int get_digit_value(int32_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 100; // Sentinel for invalid
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
    int radix_base = 0;

    // 1. Handle Leading Sign or First Digit
    if (first_char == '+' || first_char == '-') {
        has_digits = false; 
    } else if (is_digit(first_char)) {
        has_digits = true;
        radix_base = first_char - '0';
        if (first_char == '0') {
            started_with_zero = true;
            kind = NUM_OCTAL;
        }
    }

    // 2. Handle Hex start (0x...)
    if (first_char == '0' && (lexer->lookahead == 'x' || lexer->lookahead == 'X')) {
        kind = NUM_HEX;
        lexer->advance(lexer, false);
        has_digits = false;
    }

    // 3. Main Loop
    while (!is_token_boundary(lexer->lookahead) && 
           lexer->lookahead != '\'' && 
           lexer->lookahead != '#') {
        int32_t c = lexer->lookahead;
        bool consumed = false;

        switch (kind) {
            case NUM_HEX:
                if (is_hex_digit(c)) { 
                    has_digits = true; 
                    consumed = true; 
                }
                break;

            case NUM_RADIX:
                if (isalnum(c)) {
                    int val = get_digit_value(c);
                    if (val < radix_base) {
                        has_digits = true; 
                        consumed = true;
                    } else {
                        is_valid = false; // Digit exceeds base
                    }
                }
                break;

            default: // Decimal or Octal
                if (is_digit(c)) {
                    has_digits = true; 
                    consumed = true;
                    if (c == '8' || c == '9') contains_non_octal_digit = true;
                    if (radix_base < 100) radix_base = radix_base * 10 + (c - '0');
                } else if (c == '.') {
                    // Dot not allowed if we already have a dot, a slash, or an exponent
                    if (!has_dot && !has_slash && !has_exp) {
                        has_dot = true; 
                        kind = NUM_DECIMAL; // Promote octal to decimal float
                        consumed = true;
                    }
                } else if (c == '/') {
                    // Ratio not allowed if we have a dot or exponent
                    if (!has_dot && !has_slash && !has_exp && has_digits) {
                        has_slash = true; 
                        has_digits = false; 
                        kind = NUM_DECIMAL; 
                        consumed = true;
                    }
                } else if (c == 'e' || c == 'E') {
                    // Exponent not allowed if we already have one or have a slash
                    if (!has_exp && !has_slash && has_digits) {
                        has_exp = true;
                        has_digits = false; // Must have digits after 'e'
                        kind = NUM_DECIMAL;
                        lexer->advance(lexer, false);
                        // Consume optional sign in exponent
                        if (lexer->lookahead == '+' || lexer->lookahead == '-') {
                            lexer->advance(lexer, false);
                        }
                        continue; // Skip standard advance at bottom
                    }
                } else if (c == 'r' || c == 'R') {
                    if (!has_dot && !has_slash && !has_exp && has_digits && radix_base >= 2 && radix_base <= 36) {
                        kind = NUM_RADIX; has_digits = false; consumed = true;
                    } else {
                        is_valid = false; 
                    }
                } else if (c == 'N') {
                    // BigInt: Integers only (no dot, no exponent, no slash)
                    if (has_dot || has_exp || has_slash || !has_digits) is_valid = false;
                    lexer->advance(lexer, false);
                    if (!is_token_boundary(lexer->lookahead) && lexer->lookahead != '\'' && lexer->lookahead != '#') is_valid = false;
                    goto finish;
                } else if (c == 'M') {
                    // BigDecimal: No ratios
                    if (has_slash || !has_digits) is_valid = false;
                    lexer->advance(lexer, false);
                    if (!is_token_boundary(lexer->lookahead) && lexer->lookahead != '\'' && lexer->lookahead != '#') is_valid = false;
                    goto finish;
                }
                break;
        }

        if (!consumed) {
            is_valid = false;
            break;
        }
        lexer->advance(lexer, false);
    }

 finish:
    if (!is_valid) {
        while (!is_token_boundary(lexer->lookahead) && lexer->lookahead != '\'' && lexer->lookahead != '#') {
            lexer->advance(lexer, false);
        }
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
