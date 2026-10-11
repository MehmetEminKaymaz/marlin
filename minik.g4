grammar minik;

program
    : declaration* EOF
    ;

// Üst seviyede yalnızca fonksiyonlar ve global değişkenler bulunabilir;
// çalıştırılabilir ifadeler (if, döngü, atama...) bir fonksiyonun içinde olmalı.
declaration
    : functionDecl
    | variableDefinition
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
    : IDENTIFIER '=' expression ';'                    #DirectAssignment
    | expression '[' exprList ']' '=' expression ';'   #ArrayElementAssignment
    ;

functionDecl
    : annotation? 'func' IDENTIFIER '(' paramList? ')' '->' variableType block
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
    | IDENTIFIER                                                    #IdentifierExpr
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
