// A precise set of Clojure whitespace: 
// standard \s (space, tab, newline, etc) + comma + specific Unicode separators
const WHITESPACE_CHAR = /[ ,\t\r\n\f\v\u00A0\u00AD\u1680\u180e\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]/;
const WHITESPACE = token(repeat1(WHITESPACE_CHAR));

module.exports = grammar({
  name: 'treejure',

  extras: $ => [
    WHITESPACE,
    $.comment,
  ],

  conflicts: $ => [
  ],

  externals: $ => [
    $._number_external,
    $._symbol_external,
    $._keyword_external,
    $._quote_marker,          // '
    $._syntax_quote_marker,   // `
    $._deref_marker,          // @
    $._meta_marker,           // ^
    $._unquote_marker,        // ~
    $._unquote_splicing_marker, // ~@
    $._string_external, 
    $._erroneous_string,
    $._nil,
    $._bool_true, 
    $._bool_false,
    $._character_external,
    $._erroneous_character,
    $._erroneous_keyword,
    $._erroneous_symbol,
    $._erroneous_number
  ],

  rules: {
    source: $ => repeat($._form),

    _form: $ => choice(
      $._visible_form,
      $.discard,
      $.invalid_character,
      $.invalid_number
    ),

    // Excludes 'discard' so macros don't stop at #_
    _visible_form: $ => choice(
      $.with_metadata,
      $._form_base
    ),

    // --- METADATA ---
    with_metadata: $ => prec.right(10, seq(
      field('meta', $.metadata),
      repeat($.discard), 
      field('target', optional($._visible_form))
    )),

    metadata: $ => seq(
      $._meta_marker,
      field('value', choice(
        $.keyword,
        $.symbol,
        $.map_literal
      ))
    ),

    _form_base: $ => choice(
      $._literal,
      $._collection,
      $._identifier,
      $._reader_macro
    ),

    // --- COLLECTIONS ---
    _collection: $ => choice(
      $.list_literal,
      $.vector_literal,
      $.map_literal,
      $.set_literal
    ),

    list_literal:   $ => seq('(', repeat($._form), ')'),
    vector_literal: $ => seq('[', repeat($._form), ']'),
    set_literal:    $ => seq('#{', repeat($._form), '}'),
    map_literal:    $ => seq('{', repeat($.pair), '}'),
    pair:           $ => seq(field('key', $._form), field('value', $._form)),

    _identifier: $ => choice($.symbol, $.keyword),
    symbol:  $ => $._symbol_external,
    keyword: $ => $._keyword_external,

    // --- READER MACROS ---
    _reader_macro: $ => choice(
      $.quote,
      $.syntax_quote,
      $.var_quote,
      $.deref,
      $.unquote_splicing,
      $.unquote,
      $.fn_literal,
      $.reader_conditional,
      $.tagged_literal
    ),

    quote: $ => prec.right(10, seq(
      $._quote_marker,
      repeat($.discard), 
      field('target', $._visible_form)
    )),

    syntax_quote: $ => prec.right(10, seq(
      $._syntax_quote_marker,
      repeat($.discard),
      field('target', $._visible_form)
    )),

    var_quote: $ => prec.right(10, seq(
      token("#'"), 
      repeat($.discard), 
      field('target', $._visible_form)
    )),

    deref: $ => prec.right(10, seq(
      $._deref_marker,
      repeat($.discard), 
      field('target', $._visible_form)
    )),

    unquote: $ => prec.right(10, seq(
      $._unquote_marker,
      repeat($.discard), 
      field('target', $._visible_form)
    )),

    unquote_splicing: $ => prec.right(10, seq(
      $._unquote_splicing_marker, 
      repeat($.discard), 
      field('target', $._visible_form)
    )),
    
    fn_literal: $ => seq(token('#('), repeat($._form), ')'),

    reader_conditional: $ => seq(
      field('marker', choice(
        alias('#?', $.marker),
        alias('#?@', $.marker_splicing)
      )),
      field('body', $.list_literal)
    ),

    tagged_literal: $ => prec.right(10, seq(
      '#',
      field('tag', $.symbol),
      repeat($.discard),
      field('target', $._visible_form)
    )),

    // Ensure specific literals (nil/bool) are chosen over generic external numbers/symbols
    _literal: $ => choice(
      $.nil,
      $.boolean,
      $.number,
      $.string,
      $.regex,
      $.character
    ),

    nil: $ => $._nil,
    boolean: $ => choice($._bool_true, $._bool_false),
    number:    $ => $._number_external,
    string:    $ => $._string_external,
    regex:     $ => seq('#"', repeat(/[^"\\]|\\./), '"'),
    character: $ => $._character_external,
    comment:   $ => token(seq(';', /[^\n\r]*/)),

    // Recursively handles chained discards: #_ #_ 1 2
    discard: $ => prec.right(10, seq(
      '#_', 
      repeat($.discard),
      field('target', $._visible_form)
    )),

    invalid_character: $ => $._erroneous_character,
    invalid_number:    $ => $._erroneous_number,
  }
});
