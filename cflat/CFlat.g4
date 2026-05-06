/*
 [The "BSD licence"]
 Copyright (c) 2013 Sam Harwell
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 3. The name of the author may not be used to endorse or promote products
    derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/** C 2011 grammar built from the C11 Spec */

// $antlr-format alignTrailingComments true, columnLimit 150, minEmptyLines 1, maxEmptyLinesToKeep 1, reflowComments false, useTab false
// $antlr-format allowShortRulesOnASingleLine false, allowShortBlocksOnASingleLine true, alignSemicolons hanging, alignColons hanging

grammar CFlat;

primaryExpression
    : Constant
    | genericIdentifier
    | StringLiteral+
    | lambdaExpression
    | tupleExpression
    | '(' expression ')'
    | NameOf '(' expression ')'
    | TypeOf '(' expression ')'
    | TypeOf '(' typeSpecifier ')'
    ;

tupleExpression
    : '(' tupleConstructEntry (',' tupleConstructEntry)+ ')'
    ;

tupleConstructEntry
    : assignmentExpression
    ;

genericAssocList
    : genericAssociation (',' genericAssociation)*
    ;

genericAssociation
    : (typeName | 'default') ':' assignmentExpression
    ;

postfixExpression
    : primaryExpression (
        '[' expression ']'
        | '(' argumentExpressionList ')'
        | ('.' | '->' | QuestionDot) (Identifier | Move) genericTypeParameters?
        | '.' Tilde '(' ')'
        | '++'
        | '--'
    )*
    ;

argumentExpressionList
    : (argumentNamedExpression (',' argumentNamedExpression)*)?
    ;

argumentNamedExpression
    : (Identifier ':')? assignmentExpression
    | Identifier ':' '{' initializerList ','? '}'
    | '{' initializerList ','? '}'
    | '...'
    ;

unaryExpression
    : ('sizeof')* (
        postfixExpression
        | unaryOperator castExpression
        | ('sizeof' | 'alignof') '(' typeName ')'
        | newExpression
        | deleteExpression
        | moveExpression
        | operatorStringExpression
    )
    ;

operatorStringExpression
    : Operator String '(' argumentExpressionList ')'
    ;

unaryOperator
    : '&'
    | '*'
    | '+'
    | '-'
    | '~'
    | '!'
    ;

castExpression
    : '(' typeName ')' castExpression
    | unaryExpression
    | DigitSequence // for
    ;

multiplicativeExpression
    : castExpression (('*' | '/' | '%') castExpression)*
    ;

additiveExpression
    : multiplicativeExpression (('+' | '-') multiplicativeExpression)*
    ;

shiftExpression
    : additiveExpression (('<<' | ('>' '>')) additiveExpression)?
    ;

relationalExpression
    : shiftExpression (('<' | '>' | '<=' | '>=') shiftExpression)?
    ;

typeCheckExpression
    : relationalExpression (
        Is typeSpecifier
        | As typeSpecifier
    )*
    ;

equalityExpression
    : typeCheckExpression (('==' | '!=') typeCheckExpression)?
    ;

andExpression
    : equalityExpression ('&' equalityExpression)*
    ;

exclusiveOrExpression
    : andExpression ('^' andExpression)*
    ;

inclusiveOrExpression
    : exclusiveOrExpression ('|' exclusiveOrExpression)*
    ;

logicalAndExpression
    : inclusiveOrExpression ('&&' inclusiveOrExpression)*
    ;

logicalOrExpression
    : logicalAndExpression ('||' logicalAndExpression)*
    ;

conditionalExpression
    : logicalOrExpression ('?' expression ':' conditionalExpression)?
    | logicalOrExpression QuestionQuestion conditionalExpression
    ;

assignmentExpression
    : unaryExpression assignmentOperator assignmentExpression
    | conditionalExpression
    | DigitSequence // for
    ;

structOrUnionSpecifier
    : structClassUnion genericIdentifier '{' structDeclarationList '}'
    | structClassUnion genericIdentifier
    ;

assignmentOperator
    : '='
    | '*='
    | '/='
    | '%='
    | '+='
    | '-='
    | '<<='
    | '>>='
    | '&='
    | '^='
    | '|='
    | '??='
    ;

expression
    : assignmentExpression
    //    : assignmentExpression (',' assignmentExpression)*
    ;

constantExpression
    : conditionalExpression
    ;

declaration
    : annotationList? declarationSpecifiers initDeclaratorList ';'
    | annotationList? enumSpecifier ';'
    ;

annotationArg
    : StringLiteral
    | Constant
    ;

annotation
    : '[' Identifier ('(' annotationArg ')')? ']'
    ;

annotationList
    : annotation+
    ;

declarationSpecifiers
    : declarationSpecifier+
    ;

declarationSpecifier
    : storageClassSpecifier
    | typeSpecifier '?'? (pointer | '[' assignmentExpression ']')?
    | typeQualifier
    | functionSpecifier
    | alignmentSpecifier
    ;

initDeclaratorList
    : initDeclarator (',' initDeclarator)*
    ;

initDeclarator
    : declarator ('=' initializer)?
    ;

storageClassSpecifier
    : 'typedef'
    | 'extern'
    | 'static'
    | ThreadLocal
    | 'register'
    ;

typeSpecifier
    : 'void'
    | 'char'
    | 'short'
    | 'int'
    | 'long'
    | 'float'
    | 'double'
    | 'signed'
    | 'unsigned'
    | 'bool'
    | 'string'
    | 'i8'
    | 'i16'
    | 'i32'
    | 'i64'
    | 'u8'
    | 'u16'
    | 'u32'
    | 'u64'
    | 'va_list'
    | structClassUnion
    | 'auto'
    | Move                           // soft keyword — ownership modifier on parameters
    | Identifier ('.' Identifier)+   // namespace-qualified type (e.g. MathAdv.MyNumber)
    | genericIdentifier
    | functionPointerSpecifier
    | tupleTypeSpecifier
    ;

tupleTypeSpecifier
    : '(' tupleTypeEntry (',' tupleTypeEntry)+ ')'
    | '(' tupleTypePackEntry ')'
    ;

tupleTypePackEntry
    : typeSpecifier Ellipsis
    ;

tupleTypeEntry
    : typeSpecifier pointer?
    ;

functionPointerSpecifier
    : Function '<' typeSpecifier pointer? '(' functionPointerParamList? ')' '>'
    | Function
    ;

functionPointerParamList
    : functionPointerParam (',' functionPointerParam)*
    ;

functionPointerParam
    : typeSpecifier pointer?
    ;

lambdaExpression
    : '(' lambdaParamList? ')' FatArrow lambdaBody
    ;

lambdaParamList
    : lambdaParam (',' lambdaParam)*
    ;

lambdaParam
    : typeSpecifier pointer? Identifier
    ;

lambdaBody
    : compoundStatement
    | assignmentExpression
    ;

genericTypeParameters
    : '<' typeParameterList '>'
    ;

typeParameterList
    : typeParameterEntry (',' typeParameterEntry)*
    ;

typeParameterEntry
    : typeSpecifier pointer? Ellipsis?
    ;

whereClause
    : Where typeParameterConstraint (',' typeParameterConstraint)*
    ;

typeParameterConstraint
    : Identifier ':' Identifier
    ;

structClassUnion
    : 'struct'
    | 'union'
    | Class
    ;

structDeclarationList
    : structDeclaration+
    ;

structDeclaration // The first two rules have priority order and cannot be simplified to one expression.
    : specifierQualifierList structDeclaratorList ';'
    | specifierQualifierList ';'
    ;

specifierQualifierList
    : typeQualifier? (typeSpecifier)*
    ;

structDeclaratorList
    : structDeclarator (',' structDeclarator)*
    ;

structDeclarator
    : initDeclarator
    | declarator? ':' constantExpression ('=' initializer)?
    ;

enumSpecifier
    : 'enum' Identifier ':' typeSpecifier '{' enumeratorList ','? '}'
    ;

enumeratorList
    : enumerator (',' enumerator)*
    ;

enumerator
    : enumerationConstant ('=' constantExpression)?
    ;

enumerationConstant
    : Identifier
    ;

typeQualifier
    : 'const'
    ;

functionSpecifier
    : 'inline'
    | 'stdcall'
    | 'declspec' '(' Identifier ')'
    ;

alignmentSpecifier
    : 'alignas' '(' (typeName | constantExpression) ')'
    ;

declarator
    : directDeclarator
    | directDeclarator '(' parameterTypeList ')' // function declaration
    | directDeclarator '(' identifierList? ')'
    ;

directDeclarator
    : (Identifier | Move)
    | (Identifier | Move) ':' DigitSequence  // bit field
    | (Identifier | Move) '[' assignmentExpression ']'  // C-style fixed-size array
    | '(' declarator ')'
    ;

pointer
    : ('*' typeQualifierList?)+ // ^ - Blocks language extension
    ;

typeQualifierList
    : typeQualifier+
    ;

parameterTypeList
    : parameterList (',' '...')?
    ;

parameterList
    : parameterDeclaration (',' parameterDeclaration)*
    ;

parameterDeclaration
    : declarationSpecifiers declarator ('=' initializer)?
    | declarationSpecifiers abstractDeclarator?
    ;

identifierList
    : Identifier (',' Identifier)*
    ;

typeName
    : specifierQualifierList abstractDeclarator?
    ;

abstractDeclarator
    : pointer
    ;

typedefName
    : Identifier
    ;

initializer
    : assignmentExpression
    | '{' initializerList ','? '}'
    | Default
    ;

initializerList
    : fieldInit (',' fieldInit)*
    ;

fieldInit
    : Identifier '=' assignmentExpression
    ;

statement
    : labeledStatement
    | compoundStatement
    | expectErrorStatement
    | expressionStatement
    | selectionStatement
    | iterationStatement
    | jumpStatement
    | lockStatement
    | ('volatile') '(' (
        logicalOrExpression (',' logicalOrExpression)*
    )? (':' (logicalOrExpression (',' logicalOrExpression)*)?)* ')' ';'
    ;

lockStatement
    : 'lock' '(' expression ')' compoundStatement
    ;

labeledStatement
    : Identifier ':' statement?
    | 'case' constantExpression ':' statement                          // C-style value case
    | 'default' ':' statement                                          // C-style default
    | 'case' typeSpecifier pointer Identifier? FatArrow statement      // arm-style type pointer case (e.g. case Quit* q =>)
    | 'case' constantExpression FatArrow statement                     // arm-style value/type case
    | 'default' FatArrow statement                                     // arm-style wildcard
    ;

compoundStatement
    : '{' blockItemList? '}'
    ;

blockItemList
    : blockItem+
    ;

blockItem
    : statement
    | declaration
    | usingDeclaration
    | destructuringDeclaration
    ;

destructuringDeclaration
    : '(' destructuringEntry (',' destructuringEntry)+ ')' '=' assignmentExpression ';'
    ;

destructuringEntry
    : declarationSpecifiers Identifier
    ;

expressionStatement
    : expression? ';'
    ;

selectionStatement
    : 'if' '(' expression ')' statement ('else' statement)?
    | 'if' 'const' '(' expression ')' statement ('else' statement)?
    | 'switch' '(' expression ')' statement
    ;

iterationStatement
    : While '(' expression ')' statement
    | Do statement While '(' expression ')' ';'
    | For '(' forCondition ')' statement
    | For '(' declarationSpecifiers Identifier In expression ')' statement
    ;

//    |   'for' '(' expression? ';' expression?  ';' forUpdate? ')' statement
//    |   For '(' declaration  expression? ';' expression? ')' statement

forCondition
    : (forDeclaration | expression?) ';' assignmentExpression ';' forExpression
    ;

forDeclaration
    : declarationSpecifiers initDeclaratorList?
    ;

forExpression
    : assignmentExpression (',' assignmentExpression)*
    ;

jumpStatement
    : (
        'continue'
        | 'break'
        | 'return' expression?
        | 'return' compoundStatement
        | 'return' Default
    ) ';'
    ;

expectErrorStatement
    : 'expect_error' '(' StringLiteral ')' (compoundStatement | ';')
    ;

expectErrorDeclaration
    : 'expect_error' '(' StringLiteral ')' '{' externalDeclaration+ '}'
    ;

compilationUnit
    : translationUnit? EOF
    ;

translationUnit
    : externalDeclaration+
    ;

externalDeclaration
    : annotationDefinition
    | classDefinition
    | structDefinition
    | programDefinition
    | functionDefinition
    | interfaceDefinition
    | declaration
    | namespaceDefinition
    | usingDeclaration
    | importDeclaration
    | ifConstDeclaration
    | expectErrorDeclaration
    | ';' // stray ;
    ;

annotationDefinition
    : Annotation Identifier '{' declaration* '}' ';'
    ;

ifConstDeclaration
    : 'if' 'const' '(' expression ')' '{' ifConstBlock '}' ('else' '{' ifConstBlock '}')?
    ;

ifConstBlock
    : externalDeclaration*
    ;

usingDeclaration
    : Using (Identifier | String) '=' typeSpecifier ';'
    ;

importDeclaration
    : Import StringLiteral ';'
    ;

namespaceDefinition
    : Namespace Identifier '{' (externalDeclaration)* '}'
    ;

functionDefinition
    : declarationSpecifiers? (directDeclarator | operatorFunctionId) genericTypeParameters? '(' parameterTypeList? ')' whereClause? compoundStatement
    ;

structDefinition
    : 'struct' directDeclarator genericTypeParameters? whereClause? '{' (declaration | functionDefinition | destructorDefinition | structDefinition | classDefinition)* '}' ';'
    | 'union' directDeclarator genericTypeParameters? whereClause? '{' (declaration | functionDefinition | destructorDefinition | structDefinition | classDefinition)* '}' ';'
    ;

classDefinition
    : Class directDeclarator genericTypeParameters? whereClause? (':' genericIdentifier (',' genericIdentifier)*)? '{' (declaration | functionDefinition | destructorDefinition | structDefinition | classDefinition)* '}' ';'
    ;

programDefinition
    : 'program' directDeclarator '{' (declaration | functionDefinition | destructorDefinition)* '}' ';'
    ;

genericIdentifier
    : Identifier genericTypeParameters?
    ;

destructorDefinition
    : Tilde Identifier '(' ')' compoundStatement
    ;

newExpression
    : New typeSpecifier ('(' argumentExpressionList ')')?
    | New typeSpecifier '[' assignmentExpression ']'
    | New typeSpecifier '{' initializerList ','? '}'
    ;

moveExpression
    : Move unaryExpression
    ;

deleteExpression
    : Delete '[' deleteArraySize ']' expression
    | Delete '[' ']' expression
    | Delete expression
    ;

deleteArraySize
    : expression
    ;

operatorFunctionId
    : Operator (New | Delete | String
               | Plus | Minus | Star | Div | Mod
               | Equal | NotEqual | Less | LessEqual | Greater | GreaterEqual
               | LeftBracket RightBracket
               | Not | Tilde)
    ;

interfaceDefinition
    : Interface genericIdentifier (':' Identifier (',' Identifier)*)? '{' interfaceMethod* '}' ';'
    ;

interfaceMethod
    : declarationSpecifiers (directDeclarator | operatorFunctionId) '(' parameterTypeList? ')' ';'
    ;

Auto
    : 'auto'
    ;

Break
    : 'break'
    ;

Case
    : 'case'
    ;

Char
    : 'char'
    ;

Const
    : 'const'
    ;

Continue
    : 'continue'
    ;

Default
    : 'default'
    ;

Do
    : 'do'
    ;

Double
    : 'double'
    ;

Else
    : 'else'
    ;

Enum
    : 'enum'
    ;

Extern
    : 'extern'
    ;

Float
    : 'float'
    ;

For
    : 'for'
    ;

Function
    : 'function'
    ;

Goto
    : 'goto'
    ;

If
    : 'if'
    ;

Inline
    : 'inline'
    ;

Int
    : 'int'
    ;

Long
    : 'long'
    ;

Register
    : 'register'
    ;

Restrict
    : 'restrict'
    ;

Return
    : 'return'
    ;

Short
    : 'short'
    ;

Signed
    : 'signed'
    ;

Sizeof
    : 'sizeof'
    ;

Static
    : 'static'
    ;

Struct
    : 'struct'
    ;

Class
    : 'class'
    ;

NameOf
    : 'nameof'
    ;

TypeOf
    : 'typeof'
    ;

Is
    : 'is'
    ;

As
    : 'as'
    ;

Interface
    : 'interface'
    ;

Where
    : 'where'
    ;

Namespace
    : 'namespace'
    ;

Using
    : 'using'
    ;

Import
    : 'import'
    ;

Annotation
    : 'annotation'
    ;

In
    : 'in'
    ;

Switch
    : 'switch'
    ;

Typedef
    : 'typedef'
    ;

Union
    : 'union'
    ;

Unsigned
    : 'unsigned'
    ;

Void
    : 'void'
    ;

Volatile
    : 'volatile'
    ;

While
    : 'while'
    ;

Alignas
    : '_Alignas'
    ;

Alignof
    : '_Alignof'
    ;

Atomic
    : '_Atomic'
    ;

Bool
    : '_Bool'
    ;

Complex
    : '_Complex'
    ;

Generic
    : '_Generic'
    ;

Imaginary
    : '_Imaginary'
    ;

Noreturn
    : '_Noreturn'
    ;

ThreadLocal
    : 'thread_local'
    ;

New
    : 'new'
    ;

Delete
    : 'delete'
    ;

Move
    : 'move'
    ;

Operator
    : 'operator'
    ;

String
    : 'string'
    ;

LeftParen
    : '('
    ;

RightParen
    : ')'
    ;

LeftBracket
    : '['
    ;

RightBracket
    : ']'
    ;

LeftBrace
    : '{'
    ;

RightBrace
    : '}'
    ;

Less
    : '<'
    ;

LessEqual
    : '<='
    ;

Greater
    : '>'
    ;

GreaterEqual
    : '>='
    ;

LeftShift
    : '<<'
    ;

Plus
    : '+'
    ;

PlusPlus
    : '++'
    ;

Minus
    : '-'
    ;

MinusMinus
    : '--'
    ;

Star
    : '*'
    ;

Div
    : '/'
    ;

Mod
    : '%'
    ;

And
    : '&'
    ;

Or
    : '|'
    ;

AndAnd
    : '&&'
    ;

OrOr
    : '||'
    ;

Caret
    : '^'
    ;

Not
    : '!'
    ;

Tilde
    : '~'
    ;

QuestionDot
    : '?.'
    ;

QuestionQuestionAssign
    : '??='
    ;

QuestionQuestion
    : '??'
    ;

Question
    : '?'
    ;

Colon
    : ':'
    ;

Semi
    : ';'
    ;

Comma
    : ','
    ;

Assign
    : '='
    ;

// '*=' | '/=' | '%=' | '+=' | '-=' | '<<=' | '>>=' | '&=' | '^=' | '|='
StarAssign
    : '*='
    ;

DivAssign
    : '/='
    ;

ModAssign
    : '%='
    ;

PlusAssign
    : '+='
    ;

MinusAssign
    : '-='
    ;

LeftShiftAssign
    : '<<='
    ;

RightShiftAssign
    : '>>='
    ;

AndAssign
    : '&='
    ;

XorAssign
    : '^='
    ;

OrAssign
    : '|='
    ;

Equal
    : '=='
    ;

NotEqual
    : '!='
    ;

Arrow
    : '->'
    ;

FatArrow
    : '=>'
    ;

Dot
    : '.'
    ;

Ellipsis
    : '...'
    ;

// Antlr matches Rules by the order they appear in the grammar.
// Thus, declaring Constant before Identifier.
Constant
    : IntegerConstant
    | BooleanConstant
    | FloatingConstant
    //|   EnumerationConstant
    | CharacterConstant
    | NullPtrConstant
    ;

Identifier
    : IdentifierNondigit (IdentifierNondigit | Digit)*
    ;

fragment IdentifierNondigit
    : Nondigit
    | UniversalCharacterName
    //|   // other implementation-defined characters...
    ;

fragment Nondigit
    : [a-zA-Z_]
    ;

fragment Digit
    : [0-9]
    ;

fragment UniversalCharacterName
    : '\\u' HexQuad
    | '\\U' HexQuad HexQuad
    ;

fragment HexQuad
    : HexadecimalDigit HexadecimalDigit HexadecimalDigit HexadecimalDigit
    ;

fragment IntegerConstant
    : DecimalConstant IntegerSuffix?
    | OctalConstant IntegerSuffix?
    | HexadecimalConstant IntegerSuffix?
    | BinaryConstant
    ;

fragment BinaryConstant
    : '0' [bB] [0-1]+
    ;

fragment DecimalConstant
    : NonzeroDigit Digit*
    ;

fragment OctalConstant
    : '0' OctalDigit*
    ;

fragment HexadecimalConstant
    : HexadecimalPrefix HexadecimalDigit+
    ;

fragment HexadecimalPrefix
    : '0' [xX]
    ;

fragment NonzeroDigit
    : [1-9]
    ;

fragment OctalDigit
    : [0-7]
    ;

fragment HexadecimalDigit
    : [0-9a-fA-F]
    ;

fragment IntegerSuffix
    : UnsignedSuffix LongSuffix?
    | UnsignedSuffix LongLongSuffix
    | LongSuffix UnsignedSuffix?
    | LongLongSuffix UnsignedSuffix?
    ;

fragment UnsignedSuffix
    : [uU]
    ;

fragment LongSuffix
    : [lL]
    ;

fragment LongLongSuffix
    : 'll'
    | 'LL'
    ;

NullPtrConstant
    : 'nullptr'
    ;

BooleanConstant
    : TRUE
    | FALSE
    ;

TRUE 
    : 'true'
    ;

FALSE 
    : 'false'
    ;

fragment FloatingConstant
    : DecimalFloatingConstant
    | HexadecimalFloatingConstant
    ;

fragment DecimalFloatingConstant
    : FractionalConstant ExponentPart? FloatingSuffix?
    | DigitSequence ExponentPart FloatingSuffix?
    ;

fragment HexadecimalFloatingConstant
    : HexadecimalPrefix (HexadecimalFractionalConstant | HexadecimalDigitSequence) BinaryExponentPart FloatingSuffix?
    ;

fragment FractionalConstant
    : DigitSequence? '.' DigitSequence
    | DigitSequence '.'
    ;

fragment ExponentPart
    : [eE] Sign? DigitSequence
    ;

fragment Sign
    : [+-]
    ;

DigitSequence
    : Digit+
    ;

fragment HexadecimalFractionalConstant
    : HexadecimalDigitSequence? '.' HexadecimalDigitSequence
    | HexadecimalDigitSequence '.'
    ;

fragment BinaryExponentPart
    : [pP] Sign? DigitSequence
    ;

fragment HexadecimalDigitSequence
    : HexadecimalDigit+
    ;

fragment FloatingSuffix
    : [flFL]
    ;

fragment CharacterConstant
    : '\'' CCharSequence '\''
    | 'L\'' CCharSequence '\''
    | 'u\'' CCharSequence '\''
    | 'U\'' CCharSequence '\''
    ;

fragment CCharSequence
    : CChar+
    ;

fragment CChar
    : ~['\\\r\n]
    | EscapeSequence
    ;

fragment EscapeSequence
    : SimpleEscapeSequence
    | OctalEscapeSequence
    | HexadecimalEscapeSequence
    | UniversalCharacterName
    ;

fragment SimpleEscapeSequence
    : '\\' ['"?abfnrtv\\{}]
    ;

fragment OctalEscapeSequence
    : '\\' OctalDigit OctalDigit? OctalDigit?
    ;

fragment HexadecimalEscapeSequence
    : '\\x' HexadecimalDigit+
    ;

StringLiteral
    : EncodingPrefix? '"' SCharSequence? '"'
    ;

fragment EncodingPrefix
    : 'u8'
    | 'u'
    | 'U'
    | 'L'
    ;

fragment SCharSequence
    : SChar+
    ;

fragment SChar
    : ~["\\\r\n]
    | EscapeSequence
    | '\\\n'   // Added line
    | '\\\r\n' // Added line
    ;

MultiLineMacro
    : '#' (~[\n]*? '\\' '\r'? '\n')+ ~ [\n]+ -> channel (HIDDEN)
    ;

Directive
    : '#' ~ [\n]* -> channel (HIDDEN)
    ;

AsmBlock
    : 'asm' ~'{'* '{' ~'}'* '}' -> channel(HIDDEN)
    ;

Whitespace
    : [ \t]+ -> channel(HIDDEN)
    ;

Newline
    : ('\r' '\n'? | '\n') -> channel(HIDDEN)
    ;

BlockComment
    : '/*' .*? '*/' -> channel(HIDDEN)
    ;

LineComment
    : '//' ~[\r\n]* -> channel(HIDDEN)
    ;