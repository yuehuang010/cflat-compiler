
// Generated from C.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"




class  CLexer : public antlr4::Lexer {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, T__3 = 4, T__4 = 5, T__5 = 6, T__6 = 7, 
    Auto = 8, Break = 9, Case = 10, Char = 11, Const = 12, Continue = 13, 
    Default = 14, Do = 15, Double = 16, Else = 17, Enum = 18, Extern = 19, 
    Float = 20, For = 21, Goto = 22, If = 23, Inline = 24, Int = 25, Long = 26, 
    Register = 27, Restrict = 28, Return = 29, Short = 30, Signed = 31, 
    Sizeof = 32, Static = 33, Struct = 34, Switch = 35, Typedef = 36, Union = 37, 
    Unsigned = 38, Void = 39, Volatile = 40, While = 41, Alignas = 42, Alignof = 43, 
    Atomic = 44, Bool = 45, Complex = 46, Generic = 47, Imaginary = 48, 
    Noreturn = 49, StaticAssert = 50, ThreadLocal = 51, LeftParen = 52, 
    RightParen = 53, LeftBracket = 54, RightBracket = 55, LeftBrace = 56, 
    RightBrace = 57, Less = 58, LessEqual = 59, Greater = 60, GreaterEqual = 61, 
    LeftShift = 62, RightShift = 63, Plus = 64, PlusPlus = 65, Minus = 66, 
    MinusMinus = 67, Star = 68, Div = 69, Mod = 70, And = 71, Or = 72, AndAnd = 73, 
    OrOr = 74, Caret = 75, Not = 76, Tilde = 77, Question = 78, Colon = 79, 
    Semi = 80, Comma = 81, Assign = 82, StarAssign = 83, DivAssign = 84, 
    ModAssign = 85, PlusAssign = 86, MinusAssign = 87, LeftShiftAssign = 88, 
    RightShiftAssign = 89, AndAssign = 90, XorAssign = 91, OrAssign = 92, 
    Equal = 93, NotEqual = 94, Arrow = 95, Dot = 96, Ellipsis = 97, Identifier = 98, 
    Constant = 99, DigitSequence = 100, StringLiteral = 101, MultiLineMacro = 102, 
    Directive = 103, AsmBlock = 104, Whitespace = 105, Newline = 106, BlockComment = 107, 
    LineComment = 108
  };

  explicit CLexer(antlr4::CharStream *input);

  ~CLexer() override;


  std::string getGrammarFileName() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const std::vector<std::string>& getChannelNames() const override;

  const std::vector<std::string>& getModeNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;

  const antlr4::atn::ATN& getATN() const override;

  // By default the static state used to implement the lexer is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

  // Individual action functions triggered by action() above.

  // Individual semantic predicate functions triggered by sempred() above.

};

