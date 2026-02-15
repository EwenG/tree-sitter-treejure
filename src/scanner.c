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
  lexer->advance(lexer, false); // Consume "\" 
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
      lexer->advance(lexer, false); // Consume closing "
      return success_type;
    } else {
      lexer->advance(lexer, false);
    }
  }
  return ERRONEOUS_STRING;
}

static bool scan_word(TSLexer *lexer, const bool *valid_symbols) {
  char buffer[256];
  int i = 0;
  
  // 1. Consume the whole word until a boundary or a slash
  while (lexer->lookahead != 0 && 
         !is_clojure_whitespace(lexer->lookahead) && 
         !is_macro_terminating(lexer->lookahead)) {
    if (lexer->lookahead == '/') break;
    if (i < 255) buffer[i++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  }
  buffer[i] = '\0';

  if (i == 0) return false;

  // 2. Check for Reserved Literals (Priority 1)
  if (strcmp(buffer, "nil") == 0 && valid_symbols[NIL_LITERAL]) {
    lexer->result_symbol = NIL_LITERAL; return true;
  }
  if (strcmp(buffer, "true") == 0 && valid_symbols[BOOL_TRUE]) {
    lexer->result_symbol = BOOL_TRUE; return true;
  }
  if (strcmp(buffer, "false") == 0 && valid_symbols[BOOL_FALSE]) {
    lexer->result_symbol = BOOL_FALSE; return true;
  }

  // 3. Check for Namespace (Priority 2: word followed by /)
  if (lexer->lookahead == '/' && valid_symbols[IDENTIFIER_NAMESPACE]) {
    lexer->result_symbol = IDENTIFIER_NAMESPACE;
    return true;
  }

  // 4. Check for Name (Priority 3: generic name)
  if (valid_symbols[IDENTIFIER_NAME]) {
    // If the parser is looking for a name and we found a slash, 
    // it means we are in the 'rr/tt' part. Keep consuming through slashes.
    while (lexer->lookahead != 0 && 
           !is_clojure_whitespace(lexer->lookahead) && 
           !is_macro_terminating(lexer->lookahead)) {
      lexer->advance(lexer, false);
    }
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

  if (first == '#' && (valid_symbols[REGEX_EXTERNAL])) {
    lexer->advance(lexer, false); // consume #
    
    // Check if it's a regex
    if (lexer->lookahead == '"') {
      lexer->advance(lexer, false); // consume "
      lexer->result_symbol = finish_string_content(lexer, REGEX_EXTERNAL);
      return true;
    }
    
    // If it was just a symbol with # (like foo#), we cannot consume it here.
    // However, if we've already advanced, Tree-sitter won't let us "un-advance".
    // This is why # dispatches should ideally check lookahead BEFORE advancing.
    // For now, if we don't find a regex, return false to let other rules try.
    return false; 
  }
  if (first == '"' && (valid_symbols[STRING_EXTERNAL] || valid_symbols[ERRONEOUS_STRING])) {
    lexer->advance(lexer, false); // consume "
    lexer->result_symbol = finish_string_content(lexer, STRING_EXTERNAL);
    return true;
  }
  if (first == '\\' && (valid_symbols[CHARACTER_EXTERNAL] || valid_symbols[ERRONEOUS_CHARACTER])) {
    lexer->result_symbol = scan_character_type(lexer); return true;
  }
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


  if (first == ':' && (valid_symbols[KEYWORD_MARKER] || valid_symbols[AUTO_RESOLVE_MARKER])) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == ':' && valid_symbols[AUTO_RESOLVE_MARKER]) {
      lexer->advance(lexer, false);
      lexer->result_symbol = AUTO_RESOLVE_MARKER;
      return true;
    }
    if (valid_symbols[KEYWORD_MARKER]) {
      lexer->result_symbol = KEYWORD_MARKER;
      return true;
    }
    return false;
  }


  if (first == '/' && valid_symbols[SLASH_SEPARATOR]) {
    lexer->advance(lexer, false);
    lexer->result_symbol = SLASH_SEPARATOR;
    return true;
  }

  if ((first == '+' || first == '-') && valid_symbols[NUMBER] && (valid_symbols[IDENTIFIER_NAMESPACE] || valid_symbols[IDENTIFIER_NAME])) {
    lexer->advance(lexer, false);

    // A: Numeric Literal Check (e.g., +1, -12.3, -1/2)
    if (isdigit(lexer->lookahead)) {
      if (finish_number(lexer, true)) {
        lexer->result_symbol = NUMBER;
        return true;
      }
      // If finish_number failed (e.g. 123abc), it will have consumed the word.
      // In that case, we fall through to treat the whole thing as an identifier.
    }

    // B: Identifier Logic (+, -, +foo, -ns/bar)
    // We treat the sign as the start of a "word" and follow the Namespace/Name logic.
    
    bool found_slash = false;

    // Check for Namespace first: Peek ahead for a slash
    if (valid_symbols[IDENTIFIER_NAMESPACE]) {
      lexer->mark_end(lexer); // Current position is after the sign
      
      // Lookahead loop to find '/'
      while (lexer->lookahead != 0 && 
             !is_clojure_whitespace(lexer->lookahead) && 
             !is_macro_terminating(lexer->lookahead)) {
        if (lexer->lookahead == '/') {
          found_slash = true;
          break;
        }
        lexer->advance(lexer, false);
      }

      if (found_slash) {
        lexer->result_symbol = IDENTIFIER_NAMESPACE;
        lexer->mark_end(lexer); // Commit text up to (but not including) the slash
        return true;
      }
      // If no slash found, Tree-sitter will automatically retry the scan call,
      // and we will hit the IDENTIFIER_NAME block below.
    }

    // Check for Name: Greedy consumption until whitespace or terminator
    if (valid_symbols[IDENTIFIER_NAME]) {
      lexer->result_symbol = IDENTIFIER_NAME;
      lexer->mark_end(lexer);
      return true;
    }

    return false;
  }

  if((first == 'n' && valid_symbols[NIL_LITERAL]) || (first == 't' && valid_symbols[BOOL_TRUE]) || (first == 'f' && valid_symbols[BOOL_FALSE])) {
    return scan_word(lexer, valid_symbols);
  }

  if (valid_symbols[IDENTIFIER_NAME] && valid_symbols[IDENTIFIER_NAMESPACE]) {
    if (!is_macro_terminating(first) && !isdigit(first)) {
      return scan_word(lexer, valid_symbols);
    }
  }

  if (valid_symbols[IDENTIFIER_NAME] && !valid_symbols[IDENTIFIER_NAMESPACE]) {
    return scan_word(lexer, valid_symbols);
  }

  if (valid_symbols[IDENTIFIER_NAMESPACE] && !valid_symbols[IDENTIFIER_NAME]) {
    return scan_word(lexer, valid_symbols);
  }
  
  if (isdigit(first) && (valid_symbols[NUMBER] || valid_symbols[ERRONEOUS_NUMBER])) {
    return finish_number(lexer, false);
  }
  
  return false;
}
