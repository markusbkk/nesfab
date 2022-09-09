#include "ext_lex_tables.hpp"
namespace ext_lex
{
extern unsigned const lexer_ec_table[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 26, 52, 0, 0, 0, 78, 104, 130, 0, 0, 0, 0, 156, 0,
    182, 0, 208, 0, 234, 0, 0, 0, 260, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 286, 312, 0, 0, 0, 338, 364, 390, 0, 0, 0, 0, 416, 0,
    442, 0, 468, 0, 494, 0, 0, 0, 520, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
extern token_type_t const lexer_transition_table[546] = {

    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 0, 3, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 2, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0,
    0, 0, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 18,
    3, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 19, 0, 0, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 21, 0, 0, 0,
    0, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 17, 0, 0, 0, 4, 0, 3, 0,
    2, 22, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12,
    0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 0, 3, 20, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 13, 0, 0, 0, 0, 0, 16, 4, 0, 3, 0, 2, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 15, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 14, 0, 0, 0, 0, 0, 0, 4, 0,
    3, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 5, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 4, 0, 3, 0, 2, 0, 1, 0, 18, 0, 0, 0, 0, 0,
    0, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0,
    2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 8, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 24, 0, 22, 0, 0, 0, 0, 0, 0, 0,
    4, 0, 3, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 2, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0, 20, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 4, 0,
    3, 0, 2, 0, 1, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 23,
    0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 2, 0, 1, 0,
    0, 0,
};
} // namespace ext_lex
