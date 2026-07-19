//
// Created by Eminf on 19.07.2026.
//

#ifndef ASTVISITOR_H
#define ASTVISITOR_H

class Program;
class Type;
class IntegerLiteral;
class FloatLiteral;
class BooleanLiteral;
class CharLiteral;
class StringLiteral;
class Identifier;
class ListLiteral;
class ArrayAccessExpr;
class FunctionCallExpr;
class UnaryExpr;
class BinaryExpr;
class BlockStmt;
class ExprStmt;
class VariableDecl;
class ParamDecl;
class FunctionDecl;
class LambdaExpr;
class IfStmt;
class WhileStmt;
class ForRangeStmt;
class ForCollectionStmt;
class BreakStmt;
class ContinueStmt;
class ReturnStmt;
class AssignmentStmt;

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(Program* node) = 0;
    virtual void visit(Type* node) = 0;
    virtual void visit(IntegerLiteral* node) = 0;
    virtual void visit(FloatLiteral* node) = 0;
    virtual void visit(BooleanLiteral* node) = 0;
    virtual void visit(CharLiteral* node) = 0;
    virtual void visit(StringLiteral* node) = 0;
    virtual void visit(Identifier* node) = 0;
    virtual void visit(ListLiteral* node) = 0;
    virtual void visit(ArrayAccessExpr* node) = 0;
    virtual void visit(FunctionCallExpr* node) = 0;
    virtual void visit(UnaryExpr* node) = 0;
    virtual void visit(BinaryExpr* node) = 0;
    virtual void visit(BlockStmt* node) = 0;
    virtual void visit(ExprStmt* node) = 0;
    virtual void visit(VariableDecl* node) = 0;
    virtual void visit(ParamDecl* node) = 0;
    virtual void visit(FunctionDecl* node) = 0;
    virtual void visit(LambdaExpr* node) = 0;
    virtual void visit(IfStmt* node) = 0;
    virtual void visit(WhileStmt* node) = 0;
    virtual void visit(ForRangeStmt* node) = 0;
    virtual void visit(ForCollectionStmt* node) = 0;
    virtual void visit(BreakStmt* node) = 0;
    virtual void visit(ContinueStmt* node) = 0;
    virtual void visit(ReturnStmt* node) = 0;
    virtual void visit(AssignmentStmt* node) = 0;

};



#endif //ASTVISITOR_H
