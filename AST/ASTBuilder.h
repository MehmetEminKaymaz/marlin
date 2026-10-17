//
// Created by Eminf on 18.07.2026.
//

#ifndef ASTBUILDER_H
#define ASTBUILDER_H
#include <marlinBaseVisitor.h>
#include "ASTNode.h"


class ASTBuilder : public marlinBaseVisitor {

    int module; // ayrıştırılan dosyanın Program::modules içindeki sırası

    // Düğüme kaynak konumunu yazar (henüz yazılmamışsa).
    void setLoc(ASTNode* node, antlr4::Token* tok) const {
        if (!node || !tok || node->line != 0) return;
        node->line   = static_cast<int>(tok->getLine());
        node->column = static_cast<int>(tok->getCharPositionInLine()) + 1;
        node->module = module;
    }

    // Görünürlük yazılmadıysa tanım private'tır.
    static bool isPublic(marlinParser::VisibilityContext* ctx) {
        return ctx && ctx->getText() == "public";
    }

    ImportDecl* makeImport(marlinParser::ImportDeclContext* ctx) const {
        auto* decl = new ImportDecl();
        setLoc(decl, ctx->getStart());
        if (auto* whole = dynamic_cast<marlinParser::ImportModuleContext*>(ctx)) {
            decl->modulePath = whole->qualifiedName()->getText();
            if (whole->IDENTIFIER()) decl->alias = whole->IDENTIFIER()->getText();
        } else if (auto* some = dynamic_cast<marlinParser::ImportNamesContext*>(ctx)) {
            decl->isFrom = true;
            decl->modulePath = some->qualifiedName()->getText();
            for (auto* item : some->importName()) {
                ImportDecl::Name name{item->IDENTIFIER(0)->getText(), ""};
                if (item->IDENTIFIER().size() > 1) name.alias = item->IDENTIFIER(1)->getText();
                decl->names.push_back(std::move(name));
            }
        }
        return decl;
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

    Expression* makeBinary(BinaryExpr::Op op, marlinParser::ExpressionContext* l,
                           marlinParser::ExpressionContext* r, antlr4::Token* opTok) {
        auto left = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(l)));
        auto right = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(r)));
        auto* node = new BinaryExpr(op, std::move(left), std::move(right));
        setLoc(node, opTok); // hata mesajları işleci göstersin
        return node;
    }

public:
    explicit ASTBuilder(int module = 0) : module(module) {}

    // Her kuralın ürettiği düğüme, o kuralın ilk token'ının konumunu yazar.
    // Böylece visitXxx metotlarının tek tek konum ataması gerekmez.
    std::any visit(antlr4::tree::ParseTree* tree) override {
        std::any result = marlinBaseVisitor::visit(tree);
        auto* ctx = dynamic_cast<antlr4::ParserRuleContext*>(tree);
        if (!ctx || !result.has_value()) return result;

        if (auto* e = std::any_cast<Expression*>(&result)) setLoc(*e, ctx->getStart());
        else if (auto* s = std::any_cast<Statement*>(&result)) setLoc(*s, ctx->getStart());
        else if (auto* t = std::any_cast<Type*>(&result)) setLoc(*t, ctx->getStart());
        return result;
    }

    // Tek bir dosyanın ağacı: `modules` yalnızca bu dosyanın import'larını
    // taşıyan tek bir kayıt içerir. Dosyaları ModuleLoader birleştirir.
    virtual std::any visitProgram(marlinParser::ProgramContext *ctx) override {
        auto program = std::make_unique<Program>();
        program->modules.emplace_back();
        for (auto* importCtx : ctx->importDecl())
            program->modules[0].imports.emplace_back(makeImport(importCtx));
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

    virtual std::any visitDeclaration(marlinParser::DeclarationContext *ctx) override {
    if (ctx->functionDecl()) return visit(ctx->functionDecl());
    if (ctx->externDecl()) return visit(ctx->externDecl());
    if (ctx->structDecl()) return visit(ctx->structDecl());
    if (ctx->variableDefinition()) {
        std::any result = visit(ctx->variableDefinition());
        if (auto* var = dynamic_cast<VariableDecl*>(std::any_cast<Statement*>(result)))
            var->isPublic = isPublic(ctx->visibility());
        return result;
    }
    return {};
}

virtual std::any visitExternDecl(marlinParser::ExternDeclContext *ctx) override {
    auto fn = new FunctionDecl();
    fn->isExtern = true;
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
    return static_cast<Statement*>(fn);
}

virtual std::any visitStructDecl(marlinParser::StructDeclContext *ctx) override {
    auto decl = new StructDecl();
    decl->name = ctx->IDENTIFIER()->getText();
    decl->isPublic = isPublic(ctx->visibility());
    for (auto* f : ctx->structField()) {
        auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(f->variableType())));
        decl->fields.push_back({f->IDENTIFIER()->getText(), std::move(type)});
    }
    return static_cast<Statement*>(decl);
}

virtual std::any visitNamedType(marlinParser::NamedTypeContext *ctx) override {
    auto type = new Type(TypeKind::Named);
    type->name = ctx->qualifiedName()->getText();
    return type;
}

virtual std::any visitMemberAccessExpr(marlinParser::MemberAccessExprContext *ctx) override {
    auto base = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    auto* node = new MemberExpr(std::move(base), ctx->IDENTIFIER()->getText());
    setLoc(node, ctx->IDENTIFIER()->getSymbol()); // hata mesajları alan adını göstersin
    return static_cast<Expression*>(node);
}

// --- Statements ---
virtual std::any visitVariableDefStatement(marlinParser::VariableDefStatementContext *ctx) override {
    return visit(ctx->variableDefinition());
}

virtual std::any visitIfStmt(marlinParser::IfStmtContext *ctx) override {
    return visit(ctx->ifStatement());
}

virtual std::any visitWhileStmt(marlinParser::WhileStmtContext *ctx) override {
    return visit(ctx->whileStatement());
}

virtual std::any visitForStmt(marlinParser::ForStmtContext *ctx) override {
    return visit(ctx->forStatement());
}

virtual std::any visitBreakStatement(marlinParser::BreakStatementContext *ctx) override {
    return static_cast<Statement*>(new BreakStmt());
}

virtual std::any visitContinueStatement(marlinParser::ContinueStatementContext *ctx) override {
    return static_cast<Statement*>(new ContinueStmt());
}

virtual std::any visitReturnStatement(marlinParser::ReturnStatementContext *ctx) override {
    auto ret = new ReturnStmt();
    if (ctx->expression()) {
        ret->expr = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    }
    return static_cast<Statement*>(ret);
}

virtual std::any visitExpressionStatement(marlinParser::ExpressionStatementContext *ctx) override {
    auto expr = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Statement*>(new ExprStmt(std::move(expr)));
}

virtual std::any visitBlockStatement(marlinParser::BlockStatementContext *ctx) override {
    return visit(ctx->block());
}

virtual std::any visitBlock(marlinParser::BlockContext *ctx) override {
    auto block = new BlockStmt();
    for (auto* stmtCtx : ctx->statement()) {
        auto stmtRes = visit(stmtCtx);
        if (stmtRes.has_value()) {
            block->statements.push_back(std::unique_ptr<Statement>(std::any_cast<Statement*>(stmtRes)));
        }
    }
    return static_cast<Statement*>(block);
}

virtual std::any visitFunctionDecl(marlinParser::FunctionDeclContext *ctx) override {
    auto fn = new FunctionDecl();
    fn->name = ctx->IDENTIFIER()->getText();
    fn->isPublic = isPublic(ctx->visibility());
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

virtual std::any visitVariableTypeDefinitionWithExpression(marlinParser::VariableTypeDefinitionWithExpressionContext *ctx) override {
    std::string name = ctx->IDENTIFIER()->getText();
    auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    auto init = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Statement*>(new VariableDecl(name, std::move(type), std::move(init), false));
}

virtual std::any visitVariableLetDefinitionWithExpression(marlinParser::VariableLetDefinitionWithExpressionContext *ctx) override {
    std::string name = ctx->IDENTIFIER()->getText();
    auto init = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Statement*>(new VariableDecl(name, nullptr, std::move(init), true));
}

virtual std::any visitIfStatement(marlinParser::IfStatementContext *ctx) override {
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

virtual std::any visitWhileStatement(marlinParser::WhileStatementContext *ctx) override {
    auto w = new WhileStmt();
    w->condition = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    w->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    return static_cast<Statement*>(w);
}

virtual std::any visitForRangeStatement(marlinParser::ForRangeStatementContext *ctx) override {
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

virtual std::any visitForCollectionStatement(marlinParser::ForCollectionStatementContext *ctx) override {
    auto f = new ForCollectionStmt();
    f->isParallel = ctx->parallel != nullptr;
    f->iteratorName = ctx->IDENTIFIER()->getText();
    f->collection = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    f->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(std::any_cast<Statement*>(visit(ctx->block()))));
    return static_cast<Statement*>(f);
}

// --- Expressions ---

virtual std::any visitNumberExpr(marlinParser::NumberExprContext *ctx) override {
    std::string text = ctx->NUMBER()->getText();
    if (text.find('.') != std::string::npos) return static_cast<Expression*>(new FloatLiteral(std::stod(text)));
    return static_cast<Expression*>(new IntegerLiteral(std::stoll(text)));
}

virtual std::any visitStringExpr(marlinParser::StringExprContext *ctx) override {
    std::string text = ctx->STRING()->getText();
    text = unescape(text.substr(1, text.length() - 2)); // remove quotes
    return static_cast<Expression*>(new StringLiteral(text));
}

virtual std::any visitCharExpr(marlinParser::CharExprContext *ctx) override {
    std::string text = ctx->CHAR()->getText();
    text = unescape(text.substr(1, text.length() - 2));
    char val = text.empty() ? '\0' : text[0];
    return static_cast<Expression*>(new CharLiteral(val));
}

virtual std::any visitTrueExpr(marlinParser::TrueExprContext *ctx) override {
    return static_cast<Expression*>(new BooleanLiteral(true));
}

virtual std::any visitFalseExpr(marlinParser::FalseExprContext *ctx) override {
    return static_cast<Expression*>(new BooleanLiteral(false));
}

virtual std::any visitIdentifierExpr(marlinParser::IdentifierExprContext *ctx) override {
    return static_cast<Expression*>(new Identifier(ctx->qualifiedName()->getText()));
}

virtual std::any visitListLiteralExpr(marlinParser::ListLiteralExprContext *ctx) override {
    auto list = new ListLiteral();
    if (ctx->exprList()) {
        for (auto* e : ctx->exprList()->expression()) {
            list->elements.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
        }
    }
    return static_cast<Expression*>(list);
}

virtual std::any visitArrayAccessExpr(marlinParser::ArrayAccessExprContext *ctx) override {
    auto target = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    auto access = new ArrayAccessExpr(std::move(target));
    for (auto* e : ctx->exprList()->expression())
        access->indices.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
    return static_cast<Expression*>(access);
}

virtual std::any visitFunctionCallExpr(marlinParser::FunctionCallExprContext *ctx) override {
    auto target = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    auto call = new FunctionCallExpr(std::move(target));
    if (ctx->exprList()) {
        for (auto* e : ctx->exprList()->expression()) {
            call->arguments.push_back(std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(e))));
        }
    }
    return static_cast<Expression*>(call);
}

virtual std::any visitParenExpr(marlinParser::ParenExprContext *ctx) override {
    return visit(ctx->expression());
}

virtual std::any visitMarlExpr(marlinParser::MarlExprContext *ctx) override {
    auto call = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Expression*>(new MarlExpr(std::move(call)));
}

virtual std::any visitUnaryMinusExpr(marlinParser::UnaryMinusExprContext *ctx) override {
    auto op = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Expression*>(new UnaryExpr(UnaryExpr::Negate, std::move(op)));
}

virtual std::any visitNotExpr(marlinParser::NotExprContext *ctx) override {
    auto op = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));
    return static_cast<Expression*>(new UnaryExpr(UnaryExpr::Not, std::move(op)));
}

virtual std::any visitMulDivExpr(marlinParser::MulDivExprContext *ctx) override {
    BinaryExpr::Op op;
    if (ctx->op->getText() == "*") op = BinaryExpr::Mul;
    else if (ctx->op->getText() == "/") op = BinaryExpr::Div;
    else op = BinaryExpr::Mod;
    return makeBinary(op, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitAddSubExpr(marlinParser::AddSubExprContext *ctx) override {
    BinaryExpr::Op op = ctx->op->getText() == "+" ? BinaryExpr::Add : BinaryExpr::Sub;
    return makeBinary(op, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitRelationalExpr(marlinParser::RelationalExprContext *ctx) override {
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

virtual std::any visitLogicalAndExpr(marlinParser::LogicalAndExprContext *ctx) override {
    return makeBinary(BinaryExpr::And, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitLogicalOrExpr(marlinParser::LogicalOrExprContext *ctx) override {
    return makeBinary(BinaryExpr::Or, ctx->expression(0), ctx->expression(1), ctx->op);
}

virtual std::any visitLambdaExpr(marlinParser::LambdaExprContext *ctx) override {
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

virtual std::any visitLambdaExpression(marlinParser::LambdaExpressionContext *ctx) override {
    return visit(ctx->lambdaExpr());
}

// --- Types ---

virtual std::any visitArrayOrBaseType(marlinParser::ArrayOrBaseTypeContext *ctx) override {
    auto baseType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->baseType())));
    if (ctx->NUMBER()) {
        auto arrType = new Type(TypeKind::Array);
        arrType->elementType = std::move(baseType);
        arrType->arraySize = std::stoi(ctx->NUMBER()->getText());
        return arrType;
    }
    return baseType.release();
}

virtual std::any visitListType(marlinParser::ListTypeContext *ctx) override {
    auto listType = new Type(TypeKind::List);
    listType->elementType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    return listType;
}

virtual std::any visitMatrixType(marlinParser::MatrixTypeContext *ctx) override {
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

virtual std::any visitMapType(marlinParser::MapTypeContext *ctx) override {
    auto mapType = new Type(TypeKind::Map);
    mapType->keyType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType(0))));
    mapType->elementType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType(1))));
    return mapType;
}

virtual std::any visitTaskType(marlinParser::TaskTypeContext *ctx) override {
    auto taskType = new Type(TypeKind::Task);
    taskType->elementType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    return taskType;
}

virtual std::any visitLambdaType(marlinParser::LambdaTypeContext *ctx) override {
    auto lamType = new Type(TypeKind::Lambda);
    if (ctx->typeList()) {
        for (auto* t : ctx->typeList()->variableType()) {
            lamType->paramTypes.push_back(std::unique_ptr<Type>(std::any_cast<Type*>(visit(t))));
        }
    }
    lamType->returnType = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));
    return lamType;
}

virtual std::any visitBaseType(marlinParser::BaseTypeContext *ctx) override {
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
    virtual std::any visitAssignmentStmt(marlinParser::AssignmentStmtContext *ctx) override {
        return visit(ctx->assignmentStatement());
    }

    // 2. Düz Atama: x = 5;
    virtual std::any visitDirectAssignment(marlinParser::DirectAssignmentContext *ctx) override {
        // Sol taraf (hedef) doğrudan bir Identifier
        auto target = std::unique_ptr<Expression>(new Identifier(ctx->qualifiedName()->getText()));
        setLoc(target.get(), ctx->getStart());
        // Sağ taraf (atanacak değer)
        auto value = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression())));

        return static_cast<Statement*>(new AssignmentStmt(std::move(target), std::move(value)));
    }

    // 3. Dizi Elemanına Atama: dizi[0] = 5;
    virtual std::any visitArrayElementAssignment(marlinParser::ArrayElementAssignmentContext *ctx) override {
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

    // Liste elemanının alanına atama: l[i].x = 5;
    virtual std::any visitMemberAssignment(marlinParser::MemberAssignmentContext *ctx) override {
        auto base = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(0))));
        auto target = std::unique_ptr<Expression>(
            new MemberExpr(std::move(base), ctx->IDENTIFIER()->getText()));
        setLoc(target.get(), ctx->IDENTIFIER()->getSymbol());
        auto value = std::unique_ptr<Expression>(std::any_cast<Expression*>(visit(ctx->expression(1))));

        return static_cast<Statement*>(new AssignmentStmt(std::move(target), std::move(value)));
    }

    // 4. Boş Değişken Tanımı: int x;
    virtual std::any visitVariableTypeDefinitionEmpty(marlinParser::VariableTypeDefinitionEmptyContext *ctx) override {
        std::string name = ctx->IDENTIFIER()->getText();
        auto type = std::unique_ptr<Type>(std::any_cast<Type*>(visit(ctx->variableType())));

        // Değer (initializer) yok, bu yüzden nullptr geçiyoruz.
        return static_cast<Statement*>(new VariableDecl(name, std::move(type), nullptr, false));
    }

};





#endif //ASTBUILDER_H
