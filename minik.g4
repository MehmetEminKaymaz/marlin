grammar minik;

program
    : importDecl* declaration* EOF
    ;

// Başka bir dosyanın (modülün) fonksiyonlarını ve globallerini kullanıma açar.
// Modül yolu dizin ayırıcısı yerine nokta ile yazılır: araclar.vektor,
// araclar/vektor.minik dosyasıdır.
//
//   import araclar.vektor;                    araclar.vektor.norm(a)
//   import araclar.vektor as v;               v.norm(a)
//   from araclar.vektor import norm, ic as nokta;     norm(a), nokta(a, b)
importDecl
    : 'import' qualifiedName ('as' IDENTIFIER)? ';'                     #ImportModule
    | 'from' qualifiedName 'import' importName (',' importName)* ';'    #ImportNames
    ;

importName
    : IDENTIFIER ('as' IDENTIFIER)?
    ;

// Düz bir isim (x), modül üzerinden erişim (v.norm) ya da bir struct
// değişkeninin alanı (p.x, cember.merkez.x). Hangisi olduğunu Sema çözer.
qualifiedName
    : IDENTIFIER ('.' pathName)*
    ;

// Noktadan sonra tip adı olan kelimeler de isim olarak yazılabilir; böylece
// modüller konularının adını taşıyabilir (std.string).
pathName
    : IDENTIFIER
    | 'string'
    | 'list'
    | 'matrix'
    ;

// Üst seviyede yalnızca fonksiyonlar ve global değişkenler bulunabilir;
// çalıştırılabilir ifadeler (if, döngü, atama...) bir fonksiyonun içinde olmalı.
declaration
    : functionDecl
    | externDecl
    | structDecl
    | visibility? variableDefinition
    ;

// Kullanıcının tanımladığı kayıt tipi: adlandırılmış alanlar.
//
//   public struct Nokta {
//       float64 x;
//       float64 y;
//   }
//
// Değer `Nokta(1.0, 2.0)` ile oluşturulur (alanlar tanım sırasıyla), alanlara
// `p.x` ile erişilir.
structDecl
    : visibility? 'struct' IDENTIFIER '{' structField* '}'
    ;

structField
    : variableType IDENTIFIER ';'
    ;

// Tanımın başka modüllerden görünürlüğü. Yazılmazsa private: yalnızca
// tanımlandığı dosyada kullanılabilir. public tanımlar import edilebilir.
visibility
    : 'public'
    | 'private'
    ;

statement
    : variableDefinition                                #VariableDefStatement
    | assignmentStatement                               #AssignmentStmt
    | ifStatement                                       #IfStmt
    | whileStatement                                    #WhileStmt
    | forStatement                                      #ForStmt
    | 'break' ';'                                       #BreakStatement
    | 'continue' ';'                                    #ContinueStatement
    | 'return' expression? ';'                          #ReturnStatement
    | expression ';'                                    #ExpressionStatement
    | block                                             #BlockStatement
    ;

block
    : '{' statement* '}'
    ;

assignmentStatement
    : qualifiedName '=' expression ';'                 #DirectAssignment
    | expression '[' exprList ']' '=' expression ';'   #ArrayElementAssignment
    ;

functionDecl
    : annotation? visibility? 'func' IDENTIFIER '(' paramList? ')' '->' variableType block
    ;

// Gövdesi C ile, çalışma zamanı kütüphanesinde (runtime/) yazılmış fonksiyon.
// Yalnızca standart kütüphane dosyalarında (std/) yazılabilir; her zaman
// private'tır ve dışarıya sıradan bir minik fonksiyonuyla sarılarak açılır:
//
//   @pure extern func c_find(string s, string part, int64 start) -> int64;
//   public func find(string s, string part) -> int64 { return c_find(s, part, 0); }
//
// @pure: fonksiyonun yan etkisi yok (paralel döngüden çağrılabilir).
externDecl
    : annotation? 'extern' 'func' IDENTIFIER '(' paramList? ')' '->' variableType ';'
    ;

// Fonksiyonun çalışacağı hedef: @CPU ya da @GPU. Yazılmazsa @CPU.
annotation
    : '@' IDENTIFIER
    ;

paramList
    : param (',' param)*
    ;

param
    : variableType IDENTIFIER
    ;

variableDefinition
    : variableType IDENTIFIER '=' expression ';'        #VariableTypeDefinitionWithExpression
    | variableType IDENTIFIER ';'                       #VariableTypeDefinitionEmpty
    | 'let' IDENTIFIER '=' expression ';'               #VariableLetDefinitionWithExpression
    ;

ifStatement
    : 'if' expression block ('else' 'if' expression block)* ('else' block)?
    ;

whileStatement
    : 'while' expression block
    ;

// 'parallel': yinelemeler birbirinden bağımsızdır ve aynı anda çalışabilir.
// (Yalnızca aralık döngüsünde geçerlidir; Sema denetler.)
forStatement
    : parallel='parallel'? 'for' IDENTIFIER 'in' expression '..' expression ('step' expression)? block   #ForRangeStatement
    | parallel='parallel'? 'for' IDENTIFIER 'in' expression block                                        #ForCollectionStatement
    ;

// Alternatiflerin sırası önceliği belirler (üstteki daha sıkı bağlanır).
// Sonek işlemleri ([] ve çağrı) tekli işleçlerden önce gelmeli; aksi halde
// -a[0] ifadesi (-a)[0], !f(x) ifadesi (!f)(x) olarak ayrışır.
expression
    : expression '[' exprList ']'                                   #ArrayAccessExpr   // dizi[i], matris[i, j]
    | expression '(' exprList? ')'                                  #FunctionCallExpr
    | expression '.' IDENTIFIER                                     #MemberAccessExpr  // f().x  (p.x: qualifiedName)
    | '-' expression                                                #UnaryMinusExpr
    | '!' expression                                                #NotExpr
    | expression op=('*'|'/'|'%') expression                        #MulDivExpr
    | expression op=('+'|'-') expression                            #AddSubExpr
    | expression op=('<'|'<='|'>'|'>='|'=='|'!=') expression        #RelationalExpr
    | expression op='&&' expression                                 #LogicalAndExpr
    | expression op='||' expression                                 #LogicalOrExpr
    | '(' expression ')'                                            #ParenExpr
    | lambdaExpr                                                    #LambdaExpression
    | '[' exprList? ']'                                             #ListLiteralExpr
    | qualifiedName                                                 #IdentifierExpr
    | NUMBER                                                        #NumberExpr
    | STRING                                                        #StringExpr
    | CHAR                                                          #CharExpr
    | 'true'                                                        #TrueExpr
    | 'false'                                                       #FalseExpr
    ;

exprList
    : expression (',' expression)*
    ;

lambdaExpr
    : '(' paramList? ')' '->' variableType ('|' expression | block)
    ;

variableType
    : baseType ('[' NUMBER ']')?                                    #ArrayOrBaseType
    | 'list' '<' variableType '>'                                   #ListType
    | 'matrix' '<' variableType (',' NUMBER)? '>'                   #MatrixType   // matrix<T> = matrix<T, 2>; sayı: boyut sayısı
    | '(' typeList? ')' '->' variableType                           #LambdaType
    | qualifiedName                                                 #NamedType    // struct: Nokta, geometri.Nokta
    ;

typeList
    : variableType (',' variableType)*
    ;

baseType
    : 'int8' | 'int16' | 'int' | 'int64'
    | 'uint8' | 'uint16' | 'uint' | 'uint64'
    | 'float' | 'float64'
    | 'bool' | 'char' | 'string' | 'void'
    ;

// Lexer Rules
IDENTIFIER: [a-zA-Z_][a-zA-Z_0-9]*;
NUMBER: [0-9]+ ('.' [0-9]+)?;
STRING: '"' (~["\\] | '\\' .)* '"';
CHAR: '\'' (~['\\] | '\\' .) '\'';

WS : [ \t\r\n]+ -> skip ;

// Line comments and block comments
LINE_COMMENT: '//' ~[\r\n]* -> skip;
BLOCK_COMMENT: '/*' .*? '*/' -> skip;
