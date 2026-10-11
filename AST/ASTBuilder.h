//
// Created by Eminf on 18.07.2026.
//

#ifndef ASTBUILDER_H
#define ASTBUILDER_H
#include <minikBaseVisitor.h>
#include "ASTNode.h"


class ASTBuilder : public minikBaseVisitor {

    // Düğüme kaynak konumunu yazar (henüz yazılmamışsa).
    static void setLoc(ASTNode* node, antlr4::Token* tok) {
        if (!node || !tok || node->line != 0) return;
        node->line   = static_cast<int>(tok->getLine());
        node->column = static_cast<int>(tok->getCharPositionInLine()) + 1;
    }

    // "\n", "\t" gibi kaçış dizilerini gerçek karakterlere çevirir.
    static std::string unescape(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
            switch (s[++i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                default:  out += s[i]; break; // \\  \'  \"
            }
        }
        return out;
    }

    Expression* makeBinary(BinaryExpr::Op op, minikParser::ExpressionContext* l,
                           minikParser::ExpressionContext* r, antlr4::Token* opTok) {
        auto left = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(l)));
        auto right = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(r)));
        auto* node = new BinaryExpr(op, std::move(left), std::move(right));
        setLoc(node, opTok); // hata mesajları işleci göstersin
        return node;
    }

public:
    // Her kuralın ürettiği düğüme, o kuralın ilk token'ının konumunu yazar.
    // Böylece visitXxx metotlarının tek tek konum ataması gerekmez.
    std::any visit(antlr4::tree::ParseTree* tree) override {
        std::any result = minikBaseVisitor::visit(tree);
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(tree);
        if (!ctx || !result.has_value()) return result;

        if (auto* e = std::any_cast<Expression*>(&result)) setLoc(*e, ctx->getStart());
        else if (auto* s = std::any_cast<Statement*>(&result)) setLoc(*s, ctx->getStart());
        else if (auto* t = std::any_cast<Type*>(&result)) setLoc(*t, ctx->getStart());
        return result;
    }

    virtual std::any visitProgram(minikParser::ProgramContext *ctx) override {
        auto program = std::make_unique<Program>();
        for (auto* declCtx : ctx->declaration()) {
            auto declResult = visit(declCtx);
            if (declResult.has_value()) {
                program->declarations.push_back(
                    std::unique_ptr<Statement>(std::any_cast<Statement*>(declResult))
                );
            }
        }
        return static_cast<Program*>(program.release());
    }

    virtual std::any visitDeclaration(minikParser::DeclarationContext *ctx) override {
    if (ctx->functionDecl()) return visit(ctx->functionDecl());
    if (ctx->variableDefinition()) return visit(ctx->variableDefinition());
    return {};
}

// --- Statements ---
virtual std::any visitVariableDefStatement(minikParser::VariableDefStatementContext *ctx) override {
    return visit(ctx->variableDefinition());
}

virtual std::any visitIfStmt(minikParser::IfStmtContext *ctx) override {
    return visit(ctx->ifStatement());
}

virtual std::any visitWhileStmt(minikParser::WhileStmtContext *ctx) override {
    return visit(ctx->whileStatement());
}

virtual std::any visitForStmt(minikParser::ForStmtContext *ctx) override {
    return visit(ctx->forStatement());
}

virtual std::any visitBreakStatement(minikParser::BreakStatementContext *ctx) override {
    return static_cast<Statement*>(new BreakStmt());
}

virtual std::any visitContinueStatement(minikParser::ContinueStatementContext *ctx) override {
    return static_cast<Statement*>(new ContinueStmt());
}

virtual std::any visitReturnStatement(minikParser::ReturnStatementContext *ctx) override {
    auto ret = new ReturnStmt();
    if (ctx->expression()) {
        ret->expr = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    }
    return static_cast<Statement*>(ret);
}

virtual std::any visitExpressionStatement(minikParser::ExpressionStatementContext *ctx) override {
    auto expr = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Statement*>(new ExprStmt(std::move(expr)));
}

virtual std::any visitBlockStatement(minikParser::BlockStatementContext *ctx) override {
    return visit(ctx->block());
}

virtual std::any visitBlock(minikParser::BlockContext *ctx) override {
    auto block = new BlockStmt();
    for (auto* stmtCtx : ctx->statement()) {
        auto stmtRes = visit(stmtCtx);
        if (stmtRes.has_value()) {
            block->statements.push_back(std::unique_ptr<Statement>(std::any_cast<Statement*>(stmtRes)));
        }
    }
    return static_cast<Statement*>(block);
}

virtual std::any visitFunctionDecl(minikParser::FunctionDeclContext *ctx) override {
    auto fn = new FunctionDecl();
    fn->name = ctx->IDENTIFIER()->getText();
    if (ctx->annotation()) fn->annotation = ctx->annotation()->IDENTIFIER()->getText();
    if (ctx->paramList()) {
        for (auto* p : ctx->paramList()->param()) {
            auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(p->variableType())));
            auto param = new ParamDecl(p->IDENTIFIER()->getText(), std::move(type));
            setLoc(param, p->getStart());
            fn->params.push_back(std::unique_ptr<ParamDecl>(param));
        }
    }
    fn->returnType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    fn->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    return static_cast<Statement*>(fn);
}

virtual std::any visitVariableTypeDefinitionWithExpression(minikParser::VariableTypeDefinitionWithExpressionContext *ctx) override {
    std::string name = ctx->IDENTIFIER()->getText();
    auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    auto init = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Statement*>(new VariableDecl(name, std::move(type), std::move(init), false));
}

virtual std::any visitVariableLetDefinitionWithExpression(minikParser::VariableLetDefinitionWithExpressionContext *ctx) override {
    std::string name = ctx->IDENTIFIER()->getText();
    auto init = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Statement*>(new VariableDecl(name, nullptr, std::move(init), true));
}

virtual std::any visitIfStatement(minikParser::IfStatementContext *ctx) override {
    auto ifNode = new IfStmt();
    ifNode->condition = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(0))));
    ifNode->trueBlock = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block(0)))));
    
    int blockIdx = 1;
    for (size_t i = 1; i < ctx->expression().size(); ++i) {
        IfStmt::ElseIf elif;
        elif.condition = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(i))));
        elif.block = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block(blockIdx++)))));
        ifNode->elseIfs.push_back(std::move(elif));
    }

    if (ctx->block().size() > ctx->expression().size()) {
        ifNode->elseBlock = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block(blockIdx)))));
    }

    return static_cast<Statement*>(ifNode);
}

virtual std::any visitWhileStatement(minikParser::WhileStatementContext *ctx) override {
    auto w = new WhileStmt();
    w->condition = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    w->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    return static_cast<Statement*>(w);
}

virtual std::any visitForRangeStatement(minikParser::ForRangeStatementContext *ctx) override {
    auto f = new ForRangeStmt();
    f->isParallel = ctx->parallel != nullptr;
    f->iteratorName = ctx->IDENTIFIER()->getText();
    f->start = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(0))));
    f->end = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(1))));
    if (ctx->expression().size() > 2) {
        f->step = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(2))));
    }
    f->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    return static_cast<Statement*>(f);
}

virtual std::any visitForCollectionStatement(minikParser::ForCollectionStatementContext *ctx) override {
    auto f = new ForCollectionStmt();
    f->isParallel = ctx->parallel != nullptr;
    f->iteratorName = ctx->IDENTIFIER()->getText();
    f->collection = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    f->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    return static_cast<Statement*>(f);
}

// --- Expressions ---

virtual std::any visitNumberExpr(minikParser::NumberExprContext *ctx) override {
    std::string text = ctx->NUMBER()->getText();
    if (text.find('.') != std::string::npos) return static_cast<Expression*>(new FloatLiteral(std::stod(text)));
    return static_cast<Expression*>(new IntegerLiteral(std::stoll(text)));
}

virtual std::any visitStringExpr(minikParser::StringExprContext *ctx) override {
    std::string text = ctx->STRING()->getText();
    text = unescape(text.substr(1, text.length() - 2)); // remove quotes
    return static_cast<Expression*>(new StringLiteral(text));
}

virtual std::any visitCharExpr(minikParser::CharExprContext *ctx) override {
    std::string text = ctx->CHAR()->getText();
    text = unescape(text.substr(1, text.length() - 2));
    char val = text.empty() ? '\0' : text[0];
    return static_cast<Expression*>(new CharLiteral(val));
}

virtual std::any visitTrueExpr(minikParser::TrueExprContext *ctx) override {
    return static_cast<Expression*>(new BooleanLiteral(true));
}

virtual std::any visitFalseExpr(minikParser::FalseExprContext *ctx) override {
    return static_cast<Expression*>(new BooleanLiteral(false));
}

virtual std::any visitIdentifierExpr(minikParser::IdentifierExprContext *ctx) override {
    return static_cast<Expression*>(new Identifier(ctx->IDENTIFIER()->getText()));
}

virtual std::any visitListLiteralExpr(minikParser::ListLiteralExprContext *ctx) override {
    auto list = new ListLiteral();
    if (ctx->exprList()) {
        for (auto* e : ctx->exprList()->expression()) {
            list->elements.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
        }
    }
    return static_cast<Expression*>(list);
}

virtual std::any visitArrayAccessExpr(minikParser::ArrayAccessExprContext *ctx) override {
    auto target = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    auto access = new ArrayAccessExpr(std::move(target));
    for (auto* e : ctx->exprList()->expression())
        access->indices.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
    return static_cast<Expression*>(access);
}

virtual std::any visitFunctionCallExpr(minikParser::FunctionCallExprContext *ctx) override {
    auto target = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    auto call = new FunctionCallExpr(std::move(target));
    if (ctx->exprList()) {
        for (auto* e : ctx->exprList()->expression()) {
            call->arguments.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
        }
    }
    return static_cast<Expression*>(call);
}

virtual std::any visitParenExpr(minikParser::ParenExprContext *ctx) override {
    return visit(ctx->expression());
}

virtual std::any visitUnaryMinusExpr(minikParser::UnaryMinusExprContext *ctx) override {
    auto op = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Expression*>(new UnaryExpr(UnaryExpr::Negate, std::move(op)));
}

virtual std::any visitNotExpr(minikParser::NotExprContext *ctx) override {
    auto op = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Expression*>(new UnaryExpr(UnaryExpr::Not, std::move(op)));
}

virtual std::any visitMulDivExpr(minikParser::MulDivExprContext *ctx) override {
    BinaryExpr::Op op;
    if (ctx->op->getText() == "*") op = BinaryExpr::Mul;
    else if (ctx->op->getText() == "/") op = BinaryExpr::Div;
    else op = BinaryExpr::Mod;
    return makeBinary(op, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitAddSubExpr(minikParser::AddSubExprContext *ctx) override {
    BinaryExpr::Op op = ctx->op->getText() == "+" ? BinaryExpr::Add : BinaryExpr::Sub;
    return makeBinary(op, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitRelationalExpr(minikParser::RelationalExprContext *ctx) override {
    std::string optxt = ctx->op->getText();
    BinaryExpr::Op op;
    if (optxt == "<") op = BinaryExpr::Lt;
    else if (optxt == "<=") op = BinaryExpr::Lte;
    else if (optxt == ">") op = BinaryExpr::Gt;
    else if (optxt == ">=") op = BinaryExpr::Gte;
    else if (optxt == "==") op = BinaryExpr::Eq;
    else op = BinaryExpr::Neq;
    return makeBinary(op, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitLogicalAndExpr(minikParser::LogicalAndExprContext *ctx) override {
    return makeBinary(BinaryExpr::And, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitLogicalOrExpr(minikParser::LogicalOrExprContext *ctx) override {
    return makeBinary(BinaryExpr::Or, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitLambdaExpr(minikParser::LambdaExprContext *ctx) override {
    auto lam = new LambdaExpr();
    if (ctx->paramList()) {
        for (auto* p : ctx->paramList()->param()) {
            auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(p->variableType())));
            auto param = new ParamDecl(p->IDENTIFIER()->getText(), std::move(type));
            setLoc(param, p->getStart());
            lam->params.push_back(std::unique_ptr<ParamDecl>(param));
        }
    }
    lam->returnType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    if (ctx->expression()) {
        lam->exprBody = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    } else {
        lam->blockBody = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    }
    return static_cast<Expression*>(lam);
}

virtual std::any visitLambdaExpression(minikParser::LambdaExpressionContext *ctx) override {
    return visit(ctx->lambdaExpr());
}

// --- Types ---

virtual std::any visitArrayOrBaseType(minikParser::ArrayOrBaseTypeContext *ctx) override {
    auto baseType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->baseType())));
    if (ctx->NUMBER()) {
        auto arrType = new Type(TypeKind::Array);
        arrType->elementType = std::move(baseType);
        arrType->arraySize = std::stoi(ctx->NUMBER()->getText());
        return arrType;
    }
    return baseType.release();
}

virtual std::any visitListType(minikParser::ListTypeContext *ctx) override {
    auto listType = new Type(TypeKind::List);
    listType->elementType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    return listType;
}

virtual std::any visitMatrixType(minikParser::MatrixTypeContext *ctx) override {
    auto matrixType = new Type(TypeKind::Matrix);
    matrixType->elementType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    matrixType->rank = 2;
    if (ctx->NUMBER()) {
        std::string text = ctx->NUMBER()->getText();
        const bool isInteger = text.find('.') == std::string::npos && text.size() <= 3;
        matrixType->rank = isInteger ? std::stoi(text) : -1;
    }
    return matrixType;
}

virtual std::any visitLambdaType(minikParser::LambdaTypeContext *ctx) override {
    auto lamType = new Type(TypeKind::Lambda);
    if (ctx->typeList()) {
        for (auto* t : ctx->typeList()->variableType()) {
            lamType->paramTypes.push_back(std::unique_ptr<Type>(std::any_cast<Type*>(visit(t))));
        }
    }
    lamType->returnType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    return lamType;
}

virtual std::any visitBaseType(minikParser::BaseTypeContext *ctx) override {
    std::string t = ctx->getText();
    TypeKind kind;
    if (t == "int8") kind = TypeKind::Int8;
    else if (t == "int16") kind = TypeKind::Int16;
    else if (t == "int") kind = TypeKind::Int32;
    else if (t == "int64") kind = TypeKind::Int64;
    else if (t == "uint8") kind = TypeKind::UInt8;
    else if (t == "uint16") kind = TypeKind::UInt16;
    else if (t == "uint") kind = TypeKind::UInt32;
    else if (t == "uint64") kind = TypeKind::UInt64;
    else if (t == "float") kind = TypeKind::Float32;
    else if (t == "float64") kind = TypeKind::Float64;
    else if (t == "bool") kind = TypeKind::Bool;
    else if (t == "char") kind = TypeKind::Char;
    else if (t == "string") kind = TypeKind::String;
    else kind = TypeKind::Void;
    
    return new Type(kind);
}


    // 1. Üst Kural: Statement'ın içinden Assignment geldiğinde doğrudan içeriye yönlendirir.
    virtual std::any visitAssignmentStmt(minikParser::AssignmentStmtContext *ctx) override {
        return visit(ctx->assignmentStatement());
    }

    // 2. Düz Atama: x = 5;
    virtual std::any visitDirectAssignment(minikParser::DirectAssignmentContext *ctx) override {
        // Sol taraf (hedef) doğrudan bir Identifier
        auto target = std::unique_ptr<Expression>(new Identifier(ctx->IDENTIFIER()->getText()));
        setLoc(target.get(), ctx->getStart());
        // Sağ taraf (atanacak değer)
        auto value = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));

        return static_cast<Statement*>(new AssignmentStmt(std::move(target), std::move(value)));
    }

    // 3. Dizi Elemanına Atama: dizi[0] = 5;
    virtual std::any visitArrayElementAssignment(minikParser::ArrayElementAssignmentContext *ctx) override {
        // Sol tarafı (hedef) bir ArrayAccessExpr olarak oluşturuyoruz
        auto arrayTarget = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(0))));
        auto access = new ArrayAccessExpr(std::move(arrayTarget));
        for (auto* e : ctx->exprList()->expression())
            access->indices.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
        auto target = std::unique_ptr<Expression>(access);
        setLoc(target.get(), ctx->getStart());

        // Sağ taraf (atanacak değer): '=' işaretinden sonraki ifade
        auto value = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(1))));

        return static_cast<Statement*>(new AssignmentStmt(std::move(target), std::move(value)));
    }

    // 4. Boş Değişken Tanımı: int x;
    virtual std::any visitVariableTypeDefinitionEmpty(minikParser::VariableTypeDefinitionEmptyContext *ctx) override {
        std::string name = ctx->IDENTIFIER()->getText();
        auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));

        // Değer (initializer) yok, bu yüzden nullptr geçiyoruz.
        return static_cast<Statement*>(new VariableDecl(name, std::move(type), nullptr, false));
    }

};





#endif //ASTBUILDER_H
