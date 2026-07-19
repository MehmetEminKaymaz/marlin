grammar minik;

program
    : declaration* EOF
    ;

declaration
    : functionDecl
    | statement
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
    | expression '[' expression ']' '=' expression ';' #ArrayElementAssignment
    ;

functionDecl
    : 'func' IDENTIFIER '(' paramList? ')' '->' variableType block
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

forStatement
    : 'for' IDENTIFIER 'in' expression '..' expression ('step' expression)? block   #ForRangeStatement
    | 'for' IDENTIFIER 'in' expression block                                        #ForCollectionStatement
    ;

expression
    : '-' expression                                                #UnaryMinusExpr
    | '!' expression                                                #NotExpr
    | expression op=('*'|'/'|'%') expression                        #MulDivExpr
    | expression op=('+'|'-') expression                            #AddSubExpr
    | expression op=('<'|'<='|'>'|'>='|'=='|'!=') expression        #RelationalExpr
    | expression op='&&' expression                                 #LogicalAndExpr
    | expression op='||' expression                                 #LogicalOrExpr
    | expression '[' expression ']'                                 #ArrayAccessExpr
    | expression '(' exprList? ')'                                  #FunctionCallExpr
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
