//
// Created by Eminf on 19.07.2026.
//

#ifndef EXPERESSIONKIND_H
#define EXPERESSIONKIND_H


enum class ExpressionKind {
    UnaryMinusExpression,
    NotExpression,
    MultiplyDivideExpression,
    AdditionSubtractExpression,
    RelationalExpression,
    LogicalAndExpression,
    LogicalOrExpression,
    ArrayAccessExpression,
    FunctionCallExpression,
    ParenthesesExpression,
    LambdaExpression,
    ListLiteralExpression,
    IdentifierExpression,
    NumberLiteralExpression,
    StringLiteralExpression,
    CharacterLiteralExpression,
    TrueLiteralExpression,
    FalseLiteralExpression,
};



#endif //EXPERESSIONKIND_H
