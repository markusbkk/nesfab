#include <cstdint>
#include <string_view>
namespace asm_lex
{
using token_type_t = std::uint16_t;
constexpr token_type_t TOK_ERROR = 0;
constexpr token_type_t TOK_ADC = 1;
constexpr token_type_t TOK_AND = 2;
constexpr token_type_t TOK_ASL = 3;
constexpr token_type_t TOK_BCC = 4;
constexpr token_type_t TOK_BCS = 5;
constexpr token_type_t TOK_BEQ = 6;
constexpr token_type_t TOK_BIT = 7;
constexpr token_type_t TOK_BMI = 8;
constexpr token_type_t TOK_BNE = 9;
constexpr token_type_t TOK_BPL = 10;
constexpr token_type_t TOK_BRK = 11;
constexpr token_type_t TOK_BVC = 12;
constexpr token_type_t TOK_BVS = 13;
constexpr token_type_t TOK_CLC = 14;
constexpr token_type_t TOK_CLD = 15;
constexpr token_type_t TOK_CLI = 16;
constexpr token_type_t TOK_CLV = 17;
constexpr token_type_t TOK_CMP = 18;
constexpr token_type_t TOK_CPX = 19;
constexpr token_type_t TOK_CPY = 20;
constexpr token_type_t TOK_DEC = 21;
constexpr token_type_t TOK_DEX = 22;
constexpr token_type_t TOK_DEY = 23;
constexpr token_type_t TOK_EOR = 24;
constexpr token_type_t TOK_INC = 25;
constexpr token_type_t TOK_INX = 26;
constexpr token_type_t TOK_INY = 27;
constexpr token_type_t TOK_JMP = 28;
constexpr token_type_t TOK_JSR = 29;
constexpr token_type_t TOK_LDA = 30;
constexpr token_type_t TOK_LDX = 31;
constexpr token_type_t TOK_LDY = 32;
constexpr token_type_t TOK_LSR = 33;
constexpr token_type_t TOK_NOP = 34;
constexpr token_type_t TOK_ORA = 35;
constexpr token_type_t TOK_PHA = 36;
constexpr token_type_t TOK_PHP = 37;
constexpr token_type_t TOK_PLA = 38;
constexpr token_type_t TOK_PLP = 39;
constexpr token_type_t TOK_ROL = 40;
constexpr token_type_t TOK_ROR = 41;
constexpr token_type_t TOK_RTI = 42;
constexpr token_type_t TOK_RTS = 43;
constexpr token_type_t TOK_SBC = 44;
constexpr token_type_t TOK_SEC = 45;
constexpr token_type_t TOK_SED = 46;
constexpr token_type_t TOK_SEI = 47;
constexpr token_type_t TOK_STA = 48;
constexpr token_type_t TOK_STX = 49;
constexpr token_type_t TOK_STY = 50;
constexpr token_type_t TOK_TAX = 51;
constexpr token_type_t TOK_TAY = 52;
constexpr token_type_t TOK_TSX = 53;
constexpr token_type_t TOK_TXA = 54;
constexpr token_type_t TOK_TXS = 55;
constexpr token_type_t TOK_TYA = 56;
constexpr token_type_t TOK_LAX = 57;
constexpr token_type_t TOK_AXS = 58;
constexpr token_type_t TOK_ANC = 59;
constexpr token_type_t TOK_ALR = 60;
constexpr token_type_t TOK_ARR = 61;
constexpr token_type_t TOK_SAX = 62;
constexpr token_type_t TOK_SKB = 63;
constexpr token_type_t TOK_IGN = 64;
constexpr token_type_t TOK_DCP = 65;
constexpr token_type_t TOK_ISC = 66;
constexpr token_type_t TOK_RLA = 67;
constexpr token_type_t TOK_RRA = 68;
constexpr token_type_t TOK_SLO = 69;
constexpr token_type_t TOK_SRE = 70;
constexpr token_type_t TOK_eof = 71;
constexpr token_type_t TOK_END = 72;
inline std::string_view token_name(token_type_t type)
{
    using namespace std::literals;
    switch(type)
    {
    default: return "?BAD?"sv;
    case TOK_ADC: return "ADC"sv;
    case TOK_AND: return "AND"sv;
    case TOK_ASL: return "ASL"sv;
    case TOK_BCC: return "BCC"sv;
    case TOK_BCS: return "BCS"sv;
    case TOK_BEQ: return "BEQ"sv;
    case TOK_BIT: return "BIT"sv;
    case TOK_BMI: return "BMI"sv;
    case TOK_BNE: return "BNE"sv;
    case TOK_BPL: return "BPL"sv;
    case TOK_BRK: return "BRK"sv;
    case TOK_BVC: return "BVC"sv;
    case TOK_BVS: return "BVS"sv;
    case TOK_CLC: return "CLC"sv;
    case TOK_CLD: return "CLD"sv;
    case TOK_CLI: return "CLI"sv;
    case TOK_CLV: return "CLV"sv;
    case TOK_CMP: return "CMP"sv;
    case TOK_CPX: return "CPX"sv;
    case TOK_CPY: return "CPY"sv;
    case TOK_DEC: return "DEC"sv;
    case TOK_DEX: return "DEX"sv;
    case TOK_DEY: return "DEY"sv;
    case TOK_EOR: return "EOR"sv;
    case TOK_INC: return "INC"sv;
    case TOK_INX: return "INX"sv;
    case TOK_INY: return "INY"sv;
    case TOK_JMP: return "JMP"sv;
    case TOK_JSR: return "JSR"sv;
    case TOK_LDA: return "LDA"sv;
    case TOK_LDX: return "LDX"sv;
    case TOK_LDY: return "LDY"sv;
    case TOK_LSR: return "LSR"sv;
    case TOK_NOP: return "NOP"sv;
    case TOK_ORA: return "ORA"sv;
    case TOK_PHA: return "PHA"sv;
    case TOK_PHP: return "PHP"sv;
    case TOK_PLA: return "PLA"sv;
    case TOK_PLP: return "PLP"sv;
    case TOK_ROL: return "ROL"sv;
    case TOK_ROR: return "ROR"sv;
    case TOK_RTI: return "RTI"sv;
    case TOK_RTS: return "RTS"sv;
    case TOK_SBC: return "SBC"sv;
    case TOK_SEC: return "SEC"sv;
    case TOK_SED: return "SED"sv;
    case TOK_SEI: return "SEI"sv;
    case TOK_STA: return "STA"sv;
    case TOK_STX: return "STX"sv;
    case TOK_STY: return "STY"sv;
    case TOK_TAX: return "TAX"sv;
    case TOK_TAY: return "TAY"sv;
    case TOK_TSX: return "TSX"sv;
    case TOK_TXA: return "TXA"sv;
    case TOK_TXS: return "TXS"sv;
    case TOK_TYA: return "TYA"sv;
    case TOK_LAX: return "LAX"sv;
    case TOK_AXS: return "AXS"sv;
    case TOK_ANC: return "ANC"sv;
    case TOK_ALR: return "ALR"sv;
    case TOK_ARR: return "ARR"sv;
    case TOK_SAX: return "SAX"sv;
    case TOK_SKB: return "SKB"sv;
    case TOK_IGN: return "IGN"sv;
    case TOK_DCP: return "DCP"sv;
    case TOK_ISC: return "ISC"sv;
    case TOK_RLA: return "RLA"sv;
    case TOK_RRA: return "RRA"sv;
    case TOK_SLO: return "SLO"sv;
    case TOK_SRE: return "SRE"sv;
    case TOK_eof: return "eof"sv;
    }
}
inline std::string_view token_string(token_type_t type)
{
    using namespace std::literals;
    switch(type)
    {
    default: return "?BAD?"sv;
    case TOK_ADC: return "ADC"sv;
    case TOK_AND: return "AND"sv;
    case TOK_ASL: return "ASL"sv;
    case TOK_BCC: return "BCC"sv;
    case TOK_BCS: return "BCS"sv;
    case TOK_BEQ: return "BEQ"sv;
    case TOK_BIT: return "BIT"sv;
    case TOK_BMI: return "BMI"sv;
    case TOK_BNE: return "BNE"sv;
    case TOK_BPL: return "BPL"sv;
    case TOK_BRK: return "BRK"sv;
    case TOK_BVC: return "BVC"sv;
    case TOK_BVS: return "BVS"sv;
    case TOK_CLC: return "CLC"sv;
    case TOK_CLD: return "CLD"sv;
    case TOK_CLI: return "CLI"sv;
    case TOK_CLV: return "CLV"sv;
    case TOK_CMP: return "CMP"sv;
    case TOK_CPX: return "CPX"sv;
    case TOK_CPY: return "CPY"sv;
    case TOK_DEC: return "DEC"sv;
    case TOK_DEX: return "DEX"sv;
    case TOK_DEY: return "DEY"sv;
    case TOK_EOR: return "EOR"sv;
    case TOK_INC: return "INC"sv;
    case TOK_INX: return "INX"sv;
    case TOK_INY: return "INY"sv;
    case TOK_JMP: return "JMP"sv;
    case TOK_JSR: return "JSR"sv;
    case TOK_LDA: return "LDA"sv;
    case TOK_LDX: return "LDX"sv;
    case TOK_LDY: return "LDY"sv;
    case TOK_LSR: return "LSR"sv;
    case TOK_NOP: return "NOP"sv;
    case TOK_ORA: return "ORA"sv;
    case TOK_PHA: return "PHA"sv;
    case TOK_PHP: return "PHP"sv;
    case TOK_PLA: return "PLA"sv;
    case TOK_PLP: return "PLP"sv;
    case TOK_ROL: return "ROL"sv;
    case TOK_ROR: return "ROR"sv;
    case TOK_RTI: return "RTI"sv;
    case TOK_RTS: return "RTS"sv;
    case TOK_SBC: return "SBC"sv;
    case TOK_SEC: return "SEC"sv;
    case TOK_SED: return "SED"sv;
    case TOK_SEI: return "SEI"sv;
    case TOK_STA: return "STA"sv;
    case TOK_STX: return "STX"sv;
    case TOK_STY: return "STY"sv;
    case TOK_TAX: return "TAX"sv;
    case TOK_TAY: return "TAY"sv;
    case TOK_TSX: return "TSX"sv;
    case TOK_TXA: return "TXA"sv;
    case TOK_TXS: return "TXS"sv;
    case TOK_TYA: return "TYA"sv;
    case TOK_LAX: return "LAX"sv;
    case TOK_AXS: return "AXS"sv;
    case TOK_ANC: return "ANC"sv;
    case TOK_ALR: return "ALR"sv;
    case TOK_ARR: return "ARR"sv;
    case TOK_SAX: return "SAX"sv;
    case TOK_SKB: return "SKB"sv;
    case TOK_IGN: return "IGN"sv;
    case TOK_DCP: return "DCP"sv;
    case TOK_ISC: return "ISC"sv;
    case TOK_RLA: return "RLA"sv;
    case TOK_RRA: return "RRA"sv;
    case TOK_SLO: return "SLO"sv;
    case TOK_SRE: return "SRE"sv;
    case TOK_eof: return "file ending"sv;
    }
}
#define asm_lex_TOK_KEY_CASES \

constexpr token_type_t TOK_LAST_STATE = 71;
constexpr token_type_t TOK_START = 244;
extern unsigned const lexer_ec_table[256];
extern token_type_t const lexer_transition_table[12236];
} // namespace asm_lex
