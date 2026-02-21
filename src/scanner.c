#include "tree_sitter/parser.h"
#include "scanner_shared.h"
#include "numbers.c"
#include <wctype.h>
#include <ctype.h>
#include <string.h>

// ... [scan_character_type function (unchanged)] ...
static int scan_character_type(TSLexer *lexer) {
  lexer->advance(lexer, false); 
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

// ... [finish_string_content function (unchanged)] ...
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

// ... [scan_word function (unchanged)] ...
static bool scan_word(TSLexer *lexer, const bool *valid_symbols, char first_char) {
  char buffer[256];
  int i = 0;
  if (first_char != 0) buffer[i++] = first_char;

  while (!is_token_boundary(lexer->lookahead)) {
    if (lexer->lookahead == '/') {
      if (valid_symbols[IDENTIFIER_NAMESPACE]) {
          lexer->mark_end(lexer);
          lexer->advance(lexer, false); 
          if (!is_token_boundary(lexer->lookahead)) {
             if (i > 0) {
                 lexer->result_symbol = IDENTIFIER_NAMESPACE;
                 return true;
             }
          }
          if (i < 255) buffer[i++] = '/';
          lexer->mark_end(lexer);
      } else {
          if (i < 255) buffer[i++] = '/';
          lexer->advance(lexer, false);
          lexer->mark_end(lexer);
      }
    } else {
      if (i < 255) buffer[i++] = (char)lexer->lookahead;
      lexer->advance(lexer, false);
      lexer->mark_end(lexer);
    }
  }
  buffer[i] = '\0';
  
  if (i == 0) return false;

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
  if (lexer->lookahead == 0) return false;

  // 1. Whitespace
  if (is_clojure_whitespace(lexer->lookahead)) {
    if (valid_symbols[WHITESPACE_EXTERNAL]) {
      while (is_clojure_whitespace(lexer->lookahead)) {
        lexer->advance(lexer, false);
      }
      lexer->result_symbol = WHITESPACE_EXTERNAL;
      return true;
    }
    return false;
  }

  int32_t first = lexer->lookahead;

  // 2. Explicit Separator Slash
  if (first == '/') {
     if (valid_symbols[SLASH_SEPARATOR] && !valid_symbols[IDENTIFIER_NAME]) {
         lexer->advance(lexer, false);
         lexer->result_symbol = SLASH_SEPARATOR;
         return true;
     }
  }

  // 3. Markers & Literals
  if (first == '\\' && (valid_symbols[CHARACTER_EXTERNAL] || valid_symbols[ERRONEOUS_CHARACTER])) {
    lexer->result_symbol = scan_character_type(lexer); return true;
  }
  if (first == '"' && (valid_symbols[STRING_EXTERNAL] || valid_symbols[ERRONEOUS_STRING])) {
    lexer->advance(lexer, false);
    lexer->result_symbol = finish_string_content(lexer, STRING_EXTERNAL);
    return true;
  }
  if (first == '#' && (valid_symbols[REGEX_MARKER] || valid_symbols[SYMBOLIC_VALUE] || valid_symbols[ERRONEOUS_SYMBOLIC_VALUE])) {
    lexer->advance(lexer, false);
    
    // Regex string start (#")
    if (lexer->lookahead == '"' && valid_symbols[REGEX_MARKER]) {
      lexer->result_symbol = REGEX_MARKER;
      return true;
    }

    // Symbolic values (##...)
    if (lexer->lookahead == '#' && (valid_symbols[SYMBOLIC_VALUE] || valid_symbols[ERRONEOUS_SYMBOLIC_VALUE])) {
      lexer->advance(lexer, false); // Consume the second '#'
      
      char buffer[32];
      int i = 0;
      
      while (!is_token_boundary(lexer->lookahead)) {
        if (i < 31) {
          buffer[i++] = (char)lexer->lookahead;
        } else {
          i++; // Keep tracking length to accurately evaluate the fallback
        }
        lexer->advance(lexer, false);
      }
      
      if (i > 0 && i < 31) {
        buffer[i] = '\0';
        if (valid_symbols[SYMBOLIC_VALUE] && 
           (strcmp(buffer, "Inf") == 0 || strcmp(buffer, "-Inf") == 0 || strcmp(buffer, "NaN") == 0)) {
          lexer->result_symbol = SYMBOLIC_VALUE;
          return true;
        }
      }
      
      // Since it is '##' but not a valid payload, fallback as an error
      if (valid_symbols[ERRONEOUS_SYMBOLIC_VALUE]) {
        lexer->result_symbol = ERRONEOUS_SYMBOLIC_VALUE;
        return true;
      }
      return false;
    }

    // Returning false lets Tree-sitter try its internal keywords (like '#_', '#{', etc) cleanly
    return false;
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
      lexer->advance(lexer, false); lexer->result_symbol = AUTO_RESOLVE_MARKER; return true;
    }
    if (valid_symbols[KEYWORD_MARKER]) { lexer->result_symbol = KEYWORD_MARKER; return true; }
    return false;
  }

  // 4. Numbers
  if ((first == '+' || first == '-') && (valid_symbols[NUMBER] || valid_symbols[IDENTIFIER_NAME] || valid_symbols[IDENTIFIER_NAMESPACE])) {
    lexer->advance(lexer, false);
    if (isdigit(lexer->lookahead)) {
      return scan_number_word(lexer, first);
    }
    return scan_word(lexer, valid_symbols, (char)first);
  }

  if (isdigit(first) && (valid_symbols[NUMBER] || valid_symbols[ERRONEOUS_NUMBER])) {
    lexer->advance(lexer, false);
    return scan_number_word(lexer, first);
  }

  // 5. Identifiers (Word Scanning)
  if (valid_symbols[IDENTIFIER_NAME] || valid_symbols[IDENTIFIER_NAMESPACE] || 
      valid_symbols[NIL_LITERAL] || valid_symbols[BOOL_TRUE] || valid_symbols[BOOL_FALSE]) {
    
    if (!is_macro_terminating(first)) {
      
      // '#' and '\'' are not completely macro terminating because they can appear natively 
      // inside identifiers. However, they are reader macros, thus they cannot START an identifier 
      // at the beginning of a form. We safely fallback so the internal lexer handles them accurately.
      if ((first == '#' || first == '\'') && valid_symbols[IDENTIFIER_NAMESPACE]) {
        return false;
      }
      
      return scan_word(lexer, valid_symbols, 0);
    }
  }

  return false;
}
