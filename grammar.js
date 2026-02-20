module.exports = grammar({
  name: 'treejure',

  extras: $ => [],

  conflicts: $ => [],

  externals: $ => [
    $._whitespace_external,
    $._number_external,
    $._keyword_marker,
    $._auto_resolve_marker,
    $._identifier_namespace,
    $._identifier_name,
    $._slash_separator,
    $._quote_marker,
    $._syntax_quote_marker,
    $._deref_marker,
    $._meta_marker,
    $._unquote_marker,
    $._unquote_splicing_marker, 
    $._string_external, 
    $._erroneous_string,
    $._nil,
    $._bool_true, 
    $._bool_false,
    $._character_external,
    $._erroneous_character,
    $._erroneous_keyword,
    $._erroneous_symbol,
    $._erroneous_number,
    $._regex_external,
  ],

  rules: {
    source: $ => seq(
      repeat($._gap),
      repeat(seq($._form, repeat($._gap)))
    ),

    _gap: $ => choice(
      $._whitespace_external,
      $.comment,
      $.discard
    ),

    _form: $ => choice(
      $._visible_form,
      $.invalid_character,
      $.invalid_number,
      $.invalid_symbol,
      $.invalid_keyword
    ),

    _visible_form: $ => choice(
      $.with_metadata,
      $._form_base
    ),

    // --- METADATA ---
    // FIXED: Target is now mandatory. This forces the parser to 
    // skip gaps (discards) to find the target.
    with_metadata: $ => prec.right(10, seq(
      field('meta', $.metadata),
      repeat($._gap), 
      field('target', $._visible_form)
    )),

    metadata: $ => seq(
      $._meta_marker,
      repeat($._gap),
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

    _collection: $ => choice(
      $.list_literal,
      $.vector_literal,
      $.map_literal,
      $.set_literal,
      $.namespaced_map_literal
    ),

    list_literal:   $ => seq('(', repeat(choice($._form, $._gap)), ')'),
    vector_literal: $ => seq('[', repeat(choice($._form, $._gap)), ']'),
    set_literal:    $ => seq('#{', repeat(choice($._form, $._gap)), '}'),
    
    map_literal: $ => seq(
      '{',
      repeat(choice(
        $.pair,
        $._gap
      )),
      '}'
    ),

    // FIXED: Changed repeat1($._gap) to repeat($._gap).
    // 1. Handles {:a:b} (no space).
    // 2. Handles {:a 1 :b} (last item has no space before '}', triggers MISSING value).
    pair: $ => seq(
      field('key', $._visible_form),
      repeat($._gap), 
      field('value', $._visible_form)
    ),

    namespaced_map_literal: $ => seq(
      '#',
      field('namespace', $.keyword), // Reuse keyword rule which handles :ns or ::ns
      repeat($._gap),
      field('body', $.map_literal)
    ),

    _identifier: $ => choice($.symbol, $.keyword),

    symbol: $ => choice(
      prec(2, seq(
        field('namespace', alias($._identifier_namespace, $.symbol_name)),
        $._slash_separator,
        field('name', alias($._identifier_name, $.symbol_name))
      )),
      prec(1, field('name', alias($._identifier_name, $.symbol_name)))
    ),

    keyword: $ => seq(
      field('marker', choice(
        alias($._keyword_marker, ':'),
        alias($._auto_resolve_marker, '::')
      )),
      choice(
        prec(2, seq(
          field('namespace', alias($._identifier_namespace, $.symbol_name)),
          $._slash_separator,
          field('name', alias($._identifier_name, $.symbol_name))
        )),
        prec(1, field('name', alias($._identifier_name, $.symbol_name)))
      )
    ),

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
      repeat($._gap), 
      field('target', $._visible_form)
    )),

    syntax_quote: $ => prec.right(10, seq(
      $._syntax_quote_marker,
      repeat($._gap),
      field('target', $._visible_form)
    )),

    var_quote: $ => prec.right(10, seq(
      token("#'"), 
      repeat($._gap), 
      field('target', $._visible_form)
    )),

    deref: $ => prec.right(10, seq(
      $._deref_marker,
      repeat($._gap), 
      field('target', $._visible_form)
    )),

    unquote: $ => prec.right(10, seq(
      $._unquote_marker,
      repeat($._gap), 
      field('target', $._visible_form)
    )),

    unquote_splicing: $ => prec.right(10, seq(
      $._unquote_splicing_marker, 
      repeat($._gap), 
      field('target', $._visible_form)
    )),
    
    fn_literal: $ => seq(token('#('), repeat(choice($._form, $._gap)), ')'),

    reader_conditional: $ => seq(
      field('marker', choice(
        alias('#?', $.marker),
        alias('#?@', $.marker_splicing)
      )),
      repeat($._gap),
      field('body', $.list_literal)
    ),

    tagged_literal: $ => prec.right(10, seq(
      '#',
      repeat($._gap),
      field('tag', $.symbol),
      repeat($._gap),
      field('target', $._visible_form)
    )),

    _literal: $ => choice(
      $.nil,
      $.boolean,
      $.symbolic_value,
      $.number,
      $.string,
      $.regex,
      $.character,
      $.symbolic_value
    ),

    nil:       $ => $._nil,
    boolean:   $ => choice($._bool_true, $._bool_false),
    symbolic_value: $ => token(choice(
      '##Inf',
      '##-Inf',
      '##NaN'
    )),
    number:    $ => $._number_external,
    string:    $ => $._string_external,
    regex:     $ => $._regex_external,
    character: $ => $._character_external,
    
    comment:   $ => token(seq(';', /[^\n\r]*/)),

    discard: $ => prec.right(10, seq(
      '#_', 
      repeat($._gap),
      field('target', $._visible_form)
    )),

    invalid_character: $ => $._erroneous_character,
    invalid_number:    $ => $._erroneous_number,
    invalid_symbol: $ => $._erroneous_symbol,
    invalid_keyword: $ => $._erroneous_keyword,
  }
});
