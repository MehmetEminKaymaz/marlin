//
// Created by Eminf on 19.07.2026.
//

#ifndef ASTVALIDATOR_H
#define ASTVALIDATOR_H

#include <vector>
#include <string>
#include "ASTNode.h"

class ASTValidator : public ASTVisitor {
    std::vector<std::string> errors;

public:
    bool hasErrors() const { return !errors.empty(); }
    const std::vector<std::string>& getErrors() const { return errors; }

    // ------------------------------------------------------------
    //  Ziyaretçi metotları – şimdilik yalnızca Program ilgileniyor
    // ------------------------------------------------------------

    void visit(Program* node) override {
        bool hasMain = false;
        for (auto& decl : node->declarations) {
            if (auto* fn = dynamic_cast<FunctionDecl*>(decl.get())) {
                if (fn->name == "main") {
                    hasMain = true;
                    break;
                }
            }
        }
        if (!hasMain) {
            errors.push_back("Program does not contain a 'main' function");
        }
    }

    // Diğer tüm düğümler için boş implementasyonlar
    void visit(Type* node) override {}
    void visit(IntegerLiteral* node) override {}
    void visit(FloatLiteral* node) override {}
    void visit(BooleanLiteral* node) override {}
    void visit(CharLiteral* node) override {}
    void visit(StringLiteral* node) override {}
    void visit(Identifier* node) override {}
    void visit(ListLiteral* node) override {}
    void visit(ArrayAccessExpr* node) override {}
    void visit(FunctionCallExpr* node) override {}
    void visit(UnaryExpr* node) override {}
    void visit(BinaryExpr* node) override {}
    void visit(BlockStmt* node) override {}
    void visit(ExprStmt* node) override {}
    void visit(VariableDecl* node) override {}
    void visit(ParamDecl* node) override {}
    void visit(FunctionDecl* node) override {}
    void visit(LambdaExpr* node) override {}
    void visit(IfStmt* node) override {}
    void visit(WhileStmt* node) override {}
    void visit(ForRangeStmt* node) override {}
    void visit(ForCollectionStmt* node) override {}
    void visit(BreakStmt* node) override {}
    void visit(ContinueStmt* node) override {}
    void visit(ReturnStmt* node) override {}
    void visit(AssignmentStmt* node) override {}
};



#endif //ASTVALIDATOR_H
