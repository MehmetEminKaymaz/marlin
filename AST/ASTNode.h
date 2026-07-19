//
// Created by Eminf on 18.07.2026.
//

#ifndef ASTNODE_H
#define ASTNODE_H

#include <memory>
#include <vector>
#include "ASTVisitor.h"
#include "Enums/TypeKind.h"


class ASTNode {
    public:
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;

    int line = 0;
    int column = 0;

};

class Expression : public ASTNode {
};

class Statement : public ASTNode {
};

class Type : public ASTNode {
    public:
    TypeKind kind;
    int arraySize = 0;
    std::unique_ptr<Type> elementType;
    std::vector<std::unique_ptr<Type>> paramTypes;
    std::unique_ptr<Type> returnType;

    Type(TypeKind kind) : kind(kind) {}
    void accept(ASTVisitor& visitor) override {visitor.visit(this);}
};

class Program : public ASTNode {
    public:
    std::pmr::vector<std::unique_ptr<Statement>> declarations;
    void accept(ASTVisitor& visitor) override {visitor.visit(this);};
};

class IntegerLiteral : public Expression {
public:
    int64_t value;
    explicit IntegerLiteral(int64_t v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class FloatLiteral : public Expression {
public:
    double value;
    explicit FloatLiteral(double v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BooleanLiteral : public Expression {
public:
    bool value;
    explicit BooleanLiteral(bool v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class CharLiteral : public Expression {
public:
    char value;
    explicit CharLiteral(char v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class StringLiteral : public Expression {
public:
    std::string value;
    explicit StringLiteral(const std::string& v) : value(v) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class Identifier : public Expression {
public:
    std::string name;
    explicit Identifier(const std::string& n) : name(n) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ListLiteral : public Expression {
public:
    std::vector<std::unique_ptr<Expression>> elements;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ArrayAccessExpr : public Expression {
public:
    std::unique_ptr<Expression> target;
    std::unique_ptr<Expression> index;
    ArrayAccessExpr(std::unique_ptr<Expression> t, std::unique_ptr<Expression> idx)
        : target(std::move(t)), index(std::move(idx)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class FunctionCallExpr : public Expression {
public:
    std::unique_ptr<Expression> target;
    std::vector<std::unique_ptr<Expression>> arguments;
    FunctionCallExpr(std::unique_ptr<Expression> t) : target(std::move(t)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class UnaryExpr : public Expression {
public:
    enum Op { Negate, Not };
    Op op;
    std::unique_ptr<Expression> operand;
    UnaryExpr(Op o, std::unique_ptr<Expression> e) : op(o), operand(std::move(e)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BinaryExpr : public Expression {
public:
    enum Op { Add, Sub, Mul, Div, Mod, Eq, Neq, Lt, Lte, Gt, Gte, And, Or };
    Op op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    BinaryExpr(Op o, std::unique_ptr<Expression> l, std::unique_ptr<Expression> r)
        : op(o), left(std::move(l)), right(std::move(r)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BlockStmt : public Statement {
public:
    std::vector<std::unique_ptr<Statement>> statements;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ExprStmt : public Statement {
public:
    std::unique_ptr<Expression> expr;
    ExprStmt(std::unique_ptr<Expression> e) : expr(std::move(e)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class VariableDecl : public Statement {
public:
    std::string name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Expression> initializer;
    bool isLet;
    VariableDecl(std::string n, std::unique_ptr<Type> t, std::unique_ptr<Expression> init, bool let = false)
        : name(std::move(n)), type(std::move(t)), initializer(std::move(init)), isLet(let) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class AssignmentStmt : public Statement {
public:
    std::unique_ptr<Expression> target;
    std::unique_ptr<Expression> value;

    AssignmentStmt(std::unique_ptr<Expression> t, std::unique_ptr<Expression> v)
        : target(std::move(t)), value(std::move(v)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ParamDecl : public ASTNode {
public:
    std::string name;
    std::unique_ptr<Type> type;
    ParamDecl(std::string n, std::unique_ptr<Type> t) : name(std::move(n)), type(std::move(t)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class FunctionDecl : public Statement {
public:
    std::string name;
    std::vector<std::unique_ptr<ParamDecl>> params;
    std::unique_ptr<Type> returnType;
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class LambdaExpr : public Expression {
public:
    std::vector<std::unique_ptr<ParamDecl>> params;
    std::unique_ptr<Type> returnType;
    std::unique_ptr<ASTNode> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class IfStmt : public Statement {
public:
    struct ElseIf {
        std::unique_ptr<Expression> condition;
        std::unique_ptr<BlockStmt> block;
    };
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStmt> trueBlock;
    std::vector<ElseIf> elseIfs;
    std::unique_ptr<BlockStmt> elseBlock;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class WhileStmt : public Statement {
public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ForRangeStmt : public Statement {
public:
    std::string iteratorName;
    std::unique_ptr<Expression> start;
    std::unique_ptr<Expression> end;
    std::unique_ptr<Expression> step;
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ForCollectionStmt : public Statement {
public:
    std::string iteratorName;
    std::unique_ptr<Expression> collection;
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class BreakStmt : public Statement {
public:
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ContinueStmt : public Statement {
public:
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

class ReturnStmt : public Statement {
public:
    std::unique_ptr<Expression> expr;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};





#endif //ASTNODE_H
