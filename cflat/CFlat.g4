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
    | Identifier DoubleColon genericIdentifier   // global:: scope-escape qualifier ('global' is a contextual soft keyword, checked in the listener)
    | simdTypeSpecifier                          // static methods on the simd type: simd<T,N>.load(...) / .store(...)
    | genericIdentifier
    | StringLiteral+
    | lambdaExpression
    | tupleExpression
    | elementExpression
    | '(' expression ')'
    | NameOf '(' expression ')'
    | TypeOf '(' expression ')'
    | TypeOf '(' typeSpecifier ')'
    | IidOf '(' typeSpecifier ')'
    | WinrtDelegate '(' typeSpecifier ',' assignmentExpression ')'
    ;

tupleExpression
    : '(' tupleConstructEntry (',' tupleConstructEntry)+ ')'
    ;

// JSX-like element sugar (see doc/UI.md). Lowers 1:1 to construction + children
// add - LIBRARY-AGNOSTIC: <Tag/> constructs a type named Tag in scope, with no
// compiler dependency on any UI library. Attributes set fields; children are
// nested elements or {expr} interpolations added via the tag's add() method.
elementExpression
    : '<' Identifier elementAttribute* '/' '>'
    | '<' Identifier elementAttribute* '>' elementContent* '<' '/' Identifier '>'
    ;

elementAttribute
    : Identifier '=' StringLiteral
    | Identifier '=' '{' assignmentExpression '}'
    ;

elementContent
    : elementExpression
    | '{' assignmentExpression '}'
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
    | typeSpecifier '?'? pointer? arrayTypeSuffix?
    | typeQualifier
    | functionSpecifier
    | alignmentSpecifier
    ;

arrayDimSpec
    : ('[' assignmentExpression? ']')+   // empty brackets `T[]` = thin noalias array-view; sized `T[N]` = fixed array
    ;

// `[]`/`[N]` brackets optionally followed by a trailing '*'. Shared by declarationSpecifier
// and abstractDeclarator (cast/sizeof) so the two paths cannot drift. The trailing '*'
// (pointer-to-array-view `T[]*` / pointer-to-fixed-array `T[N]*`) is never a valid type;
// it is admitted here only so the listener can emit a targeted diagnostic instead of an
// ANTLR "no viable alternative" dump.
arrayTypeSuffix
    : arrayDimSpec arrayPtrSuffix?
    ;

arrayPtrSuffix
    : '*'+   // trailing '*' after '[]'/'[N]'; never a valid type - caught for a diagnostic
    ;

initDeclaratorList
    : initDeclarator (',' initDeclarator)*
    ;

initDeclarator
    : declarator ':' constantExpression ('=' initializer)?   // bitfield: int flags : 3 = 0;  (width 0 closes the unit)
    | declarator ('=' initializer)?
    | declarator '{' initializerList? ','? '}'
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
    | Move                           // soft keyword - ownership modifier on parameters
    | Identifier ('.' Identifier)+   // namespace-qualified type (e.g. MathAdv.MyNumber)
    | genericIdentifier
    | functionPointerSpecifier
    | simdTypeSpecifier
    | tupleTypeSpecifier
    ;

// Builtin special-form vector type: simd<T,N>. 'simd' is an inline-literal soft keyword
// (same mechanism as 'function'). N is parsed as an expression and constant-folded in the
// listener (mirrors arrayDimSpec); it must be a power-of-2 integer literal.
simdTypeSpecifier
    : 'simd' '<' typeSpecifier ',' assignmentExpression '>'
    ;

tupleTypeSpecifier
    : '(' tupleTypeEntry (',' tupleTypeEntry)+ ')'
    | '(' tupleTypePackEntry ')'
    ;

tupleTypePackEntry
    : typeSpecifier Ellipsis
    ;

tupleTypeEntry
    : typeSpecifier pointer? arrayTypeSuffix?   // `T[]` element = a noalias array-view member; `[N]`/`[]*` rejected in the listener
    ;

// `function<...>` is the thin C function pointer; `Lambda<...>` is the fat owning
// closure (library type). Both share this rule; the listener distinguishes them
// by which keyword token is present.
functionPointerSpecifier
    : Function '<' typeSpecifier pointer? '(' functionPointerParamList? ')' '>'
    | Function
    | Lambda '<' typeSpecifier pointer? '(' functionPointerParamList? ')' '>'
    | Lambda
    ;

functionPointerParamList
    : functionPointerParam (',' functionPointerParam)*
    ;

functionPointerParam
    : Move? typeSpecifier pointer?
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
    : typeSpecifier pointer? arrayTypeSuffix? Ellipsis?   // `T[]` arg = a noalias array-view; `[N]`/`[]*` rejected in the listener
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
    | 'cdecl'
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
    : lockClause? declarationSpecifiers declarator ('=' initializer)?
    | lockClause? declarationSpecifiers abstractDeclarator?
    ;

identifierList
    : Identifier (',' Identifier)*
    ;

typeName
    : specifierQualifierList abstractDeclarator?
    ;

abstractDeclarator
    : pointer
    | pointer? arrayTypeSuffix   // `(T[])` cast target = the noalias array-view escape (explicit T* -> T[]); trailing '*' caught for a diagnostic
    ;

typedefName
    : Identifier
    ;

initializer
    : assignmentExpression
    | '{' initializerList? ','? '}'
    | Default
    ;

initializerList
    : fieldInit (',' fieldInit)*
    ;

fieldInit
    : Identifier '=' assignmentExpression               // named struct field:    {x = 1}
    | assignmentExpression ':' assignmentExpression      // dictionary key:value:   {k: v}
    | assignmentExpression                               // positional list/array:  {v0, v1}
    ;

statement
    : labeledStatement
    | compoundStatement
    | expectErrorStatement
    | expressionStatement
    | selectionStatement
    | iterationStatement
    | vectorizeStatement
    | jumpStatement
    | lockStatement
    | ('volatile') '(' (
        logicalOrExpression (',' logicalOrExpression)*
    )? (':' (logicalOrExpression (',' logicalOrExpression)*)?)* ')' ';'
    ;

// 'vectorize' is a prefix modifier on a counted loop. It reuses iterationStatement
// so 'vectorize for (...)' and 'vectorize while (...)' both parse; codegen rejects
// the loop forms that cannot be vectorized (do-while, foreach). 'vectorize' is a
// soft keyword (inline literal, same mechanism as 'lock'/'program').
vectorizeStatement
    : 'vectorize' iterationStatement
    ;

lockStatement
    : lockClause compoundStatement
    ;

lockClause
    : 'lock' '(' lockArgList ')'
    ;

lockArgList
    : expression (',' expression)*
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
    : Auto? '(' destructuringEntry (',' destructuringEntry)+ ')' '=' assignmentExpression ';'
    ;

destructuringEntry
    : declarationSpecifiers Identifier
    | Identifier
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
    : 'expect_error' '(' StringLiteral ')' ('{' externalDeclaration+ '}' | ';')
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
    : Using (Identifier | String) '=' typeSpecifier pointer? arrayTypeSuffix? ';'
    ;

importDeclaration
    : Import importGroup (As Identifier)? libClause? defineClause* cacheClause? ';'
    | Import 'program' StringLiteral As Identifier ';'
    | Import 'package' StringLiteral libClause? defineClause* cacheClause? ';'
    | Import 'package-vcpkg' StringLiteral fromClause defineClause* ';'
    ;

// A plain file import target: either a single bare filename or a brace-wrapped comma
// list of them. The list form is a shorthand for writing several `import "file";` lines
// (so `import { "sqlite3.h", "sqlite3.c" };` binds the header and compiles the .c). Each
// entry routes exactly like a plain `import "x";`; the optional `as` / `cache` on the
// importDeclaration apply only when the group holds a single filename. The other
// importDeclaration alternatives (program/package/package-vcpkg) keep a single direct
// StringLiteral, so that accessor stays singular for them.
importGroup
    : StringLiteral
    | '{' StringLiteral (',' StringLiteral)* ','? '}'
    ;

// Optional inline import library (or libraries) for a header binding:
//   import "windows.h" lib "user32.lib";
//   import "windows.h" lib { "user32.lib", "gdi32.lib" };   // brace list for several
// The brace-list form mirrors the `import { "a", "b" }` group spelling and exists because
// one header (e.g. <windows.h>) commonly fans out to several system import libs. A bare
// library name that is not found beside the importing .cb is resolved against the system
// lib search paths (the Windows SDK dir cflat already discovered for the header). Kept as
// a sub-rule so importDeclaration still has a single direct StringLiteral.
libClause
    : 'lib' StringLiteral
    | 'lib' '{' StringLiteral (',' StringLiteral)* ','? '}'
    ;

// Optional inline preprocessor define(s) for a header binding, scoped to THIS
// import's clang AST dump and appended on top of the process-wide --c-define:
//   import package "x.h" lib "y.lib" define "FOO" define "BAR=2";
//   import package-vcpkg "SDL3/SDL.h" from "sdl3" define "SDL_MAIN_HANDLED";
// Sub-rule (like libClause) so importDeclaration keeps a single direct StringLiteral.
defineClause
    : 'define' StringLiteral
    ;

// Optional inline opt-in to the persistent C-header disk cache:
//   import "windows.h" cache;
//   import package "curl/curl.h" lib "libcurl.lib" cache;
// `cache` is a soft keyword (inline literal, like lib/define/from). When present, the
// extracted declarations for this header are cached to %USERPROFILE%\.cflat\cheaders so
// the next cold compile loads the JSON instead of re-running the clang header parse.
cacheClause
    : 'cache'
    ;

// Required port name (with optional [features]) on an `import package-vcpkg` line:
//   import package-vcpkg "curl/curl.h" from "curl";
//   import package-vcpkg "curl/curl.h" from "curl[ssl]";
// Sub-rule so importDeclaration keeps a single direct StringLiteral.
fromClause
    : 'from' StringLiteral
    ;

namespaceDefinition
    : Namespace Identifier ('.' Identifier)* '{' (externalDeclaration)* '}'
    ;

functionDefinition
    : declarationSpecifiers? (directDeclarator | operatorFunctionId) genericTypeParameters? '(' parameterTypeList? ')' whereClause? lockClause? compoundStatement
    ;

structDefinition
    : 'struct' alignmentSpecifier? directDeclarator genericTypeParameters? whereClause? '{' aggregateMember* '}' ';'
    | 'union' alignmentSpecifier? directDeclarator genericTypeParameters? whereClause? '{' aggregateMember* '}' ';'
    ;

classDefinition
    : annotationList? Class alignmentSpecifier? directDeclarator genericTypeParameters? whereClause? (':' genericIdentifier (',' genericIdentifier)*)? '{' aggregateMember* '}' ';'
    ;

// A struct/class body member. Extracted into a named rule (rather than an inline
// `(declaration | functionDefinition | ...)*` closure) so ANTLR's adaptive prediction
// gets the same full-context decision it uses for `externalDeclaration+` at file scope.
// The inline closure could not disambiguate a field whose type carries an empty-bracket
// arrayDimSpec (the `int[]` array-view) from a `functionDefinition`, since both begin
// with `declarationSpecifiers`; the named rule resolves it.
aggregateMember
    : declaration
    | functionDefinition
    | destructorDefinition
    | structDefinition
    | classDefinition
    | lockFieldGroup
    ;

lockFieldGroup
    : lockClause '{' (declaration | functionDefinition)+ '}'
    ;

programDefinition
    : 'program' directDeclarator (':' Identifier (',' Identifier)*)? '{' (declaration | functionDefinition | destructorDefinition)* '}' ';'
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
               | LeftShift | Greater Greater
               | LeftBracket RightBracket
               | Arrow
               | Not | Tilde)
    ;

interfaceDefinition
    : annotationList? Interface genericIdentifier (':' Identifier (',' Identifier)*)? '{' interfaceMethod* '}' ';'
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

Lambda
    : 'Lambda'
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

IidOf
    : 'iidof'
    ;

WinrtDelegate
    : 'winrtDelegate'
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

DoubleColon
    : '::'
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