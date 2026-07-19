//
// Created by Eminf on 19.07.2026.
//

#ifndef ASTPRINTER_H
#define ASTPRINTER_H



#include <iostream>
#include <string>
#include "ASTVisitor.h"
#include "ASTNode.h"

class ASTPrinter : public ASTVisitor {
    int indentLevel = 0;

    void printIndent() const {
        for (int i = 0; i < indentLevel; ++i) std::cout << "  ";
    }

    void printNode(const std::string& name) {
        printIndent();
        std::cout << name << "\n";
    }

public:
    // Program
    void visit(Program* node) override {
        printNode("Program");
        indentLevel++;
        for (auto& decl : node->declarations) {
            if (decl) decl->accept(*this);
        }
        indentLevel--;
    }

    // Expressions
    void visit(IntegerLiteral* node) override {
        printNode("IntegerLiteral: " + std::to_string(node->value));
    }
    void visit(FloatLiteral* node) override {
        printNode("FloatLiteral: " + std::to_string(node->value));
    }
    void visit(BooleanLiteral* node) override {
        printNode(std::string("BooleanLiteral: ") + (node->value ? "true" : "false"));
    }
    void visit(CharLiteral* node) override {
        printNode(std::string("CharLiteral: '") + node->value + "'");
    }
    void visit(StringLiteral* node) override {
        printNode("StringLiteral: \"" + node->value + "\"");
    }
    void visit(Identifier* node) override {
        printNode("Identifier: " + node->name);
    }
    void visit(ListLiteral* node) override {
        printNode("ListLiteral");
        indentLevel++;
        for (auto& elem : node->elements) {
            if (elem) elem->accept(*this);
        }
        indentLevel--;
    }
    void visit(ArrayAccessExpr* node) override {
        printNode("ArrayAccess");
        indentLevel++;
        if (node->target) node->target->accept(*this);
        if (node->index) node->index->accept(*this);
        indentLevel--;
    }
    void visit(FunctionCallExpr* node) override {
        printNode("FunctionCall");
        indentLevel++;
        if (node->target) node->target->accept(*this);
        printIndent(); std::cout << "Arguments:\n";
        indentLevel++;
        for (auto& arg : node->arguments) {
            if (arg) arg->accept(*this);
        }
        indentLevel--;
        indentLevel--;
    }
    void visit(UnaryExpr* node) override {
        std::string op = (node->op == UnaryExpr::Negate) ? "Negate" : "Not";
        printNode("UnaryExpr: " + op);
        indentLevel++;
        if (node->operand) node->operand->accept(*this);
        indentLevel--;
    }
    void visit(BinaryExpr* node) override {
        static const char* opNames[] = {"Add","Sub","Mul","Div","Mod","Eq","Neq","Lt","Lte","Gt","Gte","And","Or"};
        printNode(std::string("BinaryExpr: ") + opNames[node->op]);
        indentLevel++;
        if (node->left) node->left->accept(*this);
        if (node->right) node->right->accept(*this);
        indentLevel--;
    }
    void visit(LambdaExpr* node) override {
        printNode("LambdaExpr");
        indentLevel++;
        for (auto& p : node->params) {
            if (p) p->accept(*this);
        }
        if (node->returnType) node->returnType->accept(*this);
        if (node->body) node->body->accept(*this);
        indentLevel--;
    }

    // Statements
    void visit(BlockStmt* node) override {
        printNode("Block");
        indentLevel++;
        for (auto& stmt : node->statements) {
            if (stmt) stmt->accept(*this);
        }
        indentLevel--;
    }
    void visit(ExprStmt* node) override {
        printNode("ExprStmt");
        indentLevel++;
        if (node->expr) node->expr->accept(*this);
        indentLevel--;
    }
    void visit(VariableDecl* node) override {
        printNode("VariableDecl: " + node->name + (node->isLet ? " (let)" : ""));
        indentLevel++;
        if (node->type) node->type->accept(*this);
        if (node->initializer) node->initializer->accept(*this);
        indentLevel--;
    }
    void visit(ParamDecl* node) override {
        printNode("Param: " + node->name);
        indentLevel++;
        if (node->type) node->type->accept(*this);
        indentLevel--;
    }
    void visit(FunctionDecl* node) override {
        printNode("FunctionDecl: " + node->name);
        indentLevel++;
        for (auto& p : node->params) {
            if (p) p->accept(*this);
        }
        if (node->returnType) node->returnType->accept(*this);
        if (node->body) node->body->accept(*this);
        indentLevel--;
    }
    void visit(IfStmt* node) override {
        printNode("IfStmt");
        indentLevel++;
        if (node->condition) node->condition->accept(*this);
        printIndent(); std::cout << "Then:\n";
        indentLevel++;
        if (node->trueBlock) node->trueBlock->accept(*this);
        indentLevel--;
        for (auto& elif : node->elseIfs) {
            printIndent(); std::cout << "ElseIf:\n";
            indentLevel++;
            if (elif.condition) elif.condition->accept(*this);
            if (elif.block) elif.block->accept(*this);
            indentLevel--;
        }
        if (node->elseBlock) {
            printIndent(); std::cout << "Else:\n";
            indentLevel++;
            node->elseBlock->accept(*this);
            indentLevel--;
        }
        indentLevel--;
    }
    void visit(WhileStmt* node) override {
        printNode("WhileStmt");
        indentLevel++;
        if (node->condition) node->condition->accept(*this);
        if (node->body) node->body->accept(*this);
        indentLevel--;
    }
    void visit(ForRangeStmt* node) override {
        printNode("ForRangeStmt: " + node->iteratorName);
        indentLevel++;
        if (node->start) { printIndent(); std::cout << "Start:\n"; indentLevel++; node->start->accept(*this); indentLevel--; }
        if (node->end)   { printIndent(); std::cout << "End:\n";   indentLevel++; node->end->accept(*this);   indentLevel--; }
        if (node->step)  { printIndent(); std::cout << "Step:\n";  indentLevel++; node->step->accept(*this);  indentLevel--; }
        if (node->body) node->body->accept(*this);
        indentLevel--;
    }
    void visit(ForCollectionStmt* node) override {
        printNode("ForCollectionStmt: " + node->iteratorName);
        indentLevel++;
        if (node->collection) { printIndent(); std::cout << "Collection:\n"; indentLevel++; node->collection->accept(*this); indentLevel--; }
        if (node->body) node->body->accept(*this);
        indentLevel--;
    }
    void visit(BreakStmt* node) override {
        printNode("BreakStmt");
    }
    void visit(ContinueStmt* node) override {
        printNode("ContinueStmt");
    }
    void visit(ReturnStmt* node) override {
        printNode("ReturnStmt");
        indentLevel++;
        if (node->expr) node->expr->accept(*this);
        indentLevel--;
    }

    // Type
    void visit(Type* node) override {
        std::string typeStr;
        switch (node->kind) {
            case TypeKind::Int8:    typeStr = "int8"; break;
            case TypeKind::Int16:   typeStr = "int16"; break;
            case TypeKind::Int32:   typeStr = "int"; break;
            case TypeKind::Int64:   typeStr = "int64"; break;
            case TypeKind::UInt8:   typeStr = "uint8"; break;
            case TypeKind::UInt16:  typeStr = "uint16"; break;
            case TypeKind::UInt32:  typeStr = "uint"; break;
            case TypeKind::UInt64:  typeStr = "uint64"; break;
            case TypeKind::Float32: typeStr = "float"; break;
            case TypeKind::Float64: typeStr = "float64"; break;
            case TypeKind::Bool:    typeStr = "bool"; break;
            case TypeKind::Char:    typeStr = "char"; break;
            case TypeKind::String:  typeStr = "string"; break;
            case TypeKind::Void:    typeStr = "void"; break;
            case TypeKind::Array:   typeStr = "Array[" + std::to_string(node->arraySize) + "]"; break;
            case TypeKind::List:    typeStr = "List"; break;
            case TypeKind::Lambda:  typeStr = "Lambda"; break;
            default: typeStr = "Unknown"; break;
        }
        printNode("Type: " + typeStr);
        indentLevel++;
        if (node->elementType) node->elementType->accept(*this);
        for (auto& pt : node->paramTypes) {
            if (pt) pt->accept(*this);
        }
        if (node->returnType) node->returnType->accept(*this);
        indentLevel--;
    }

    void visit(AssignmentStmt* node) override {
        printNode("AssignmentStmt");
        indentLevel++;

        // Hedefi yazdır
        printIndent(); std::cout << "Target:\n";
        indentLevel++;
        if (node->target) node->target->accept(*this);
        indentLevel--;

        // Atanan Değeri yazdır
        printIndent(); std::cout << "Value:\n";
        indentLevel++;
        if (node->value) node->value->accept(*this);
        indentLevel--;

        indentLevel--;
    }
};



#endif //ASTPRINTER_H
