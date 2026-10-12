// Sema — @GPU fonksiyonların kuralları (kurallar: Sema.h)

#include "Sema.h"

bool Sema::gpuTypeAllowed(const SemType& type) {
    return type.isScalar() || type.isMatrix();
}

// Fonksiyonun başındaki @CPU / @GPU.
void Sema::resolveTarget(FunctionDecl* fn) {
    if (fn->annotation.empty() || fn->annotation == "CPU") return;
    if (fn->annotation != "GPU") {
        error(fn, "bilinmeyen hedef '@" + fn->annotation + "' (gecerli olanlar: @CPU, @GPU)");
        return;
    }
    fn->isGpu = true;
}

void Sema::checkGpuType(const SemType& type, const ASTNode* at) {
    if (type.isError() || type.isVoid() || gpuTypeAllowed(type)) return;
    error(at, "@GPU fonksiyonda yalnizca sayisal/bool/char degerler ve matrisler kullanilabilir "
              "(bulunan: " + type.str() + ")");
}

// Fonksiyon GPU'ya iş veriyor: paralel döngünün içinden çağrılamaz (kural 5).
void Sema::noteGpuWork(const std::string& reason) {
    if (gpuFunction && currentEffects && currentEffects->gpuWorkReason.empty())
        currentEffects->gpuWorkReason = "@GPU fonksiyon ve " + reason;
}

// Her ifadeden sonra çağrılır (bkz. check).
void Sema::checkGpuExpr(Expression* expr) {
    if (!gpuFunction) return;
    const SemType& type = expr->type;
    auto* ident = dynamic_cast<Identifier*>(expr);

    // Fonksiyon isimleri checkGpuIdentifier'da ele alınır.
    if (!(ident && type.isFunction())) checkGpuType(type, expr);

    if (!type.isMatrix()) return;
    noteGpuWork("matris kullaniyor");
    // Matrisin adı (elemanına erişmek ya da boyutunu sormak için) serbest;
    // yeni matris üreten her ifade yasak.
    if (inGpuKernel() && !ident)
        error(expr, "GPU'da calisan paralel dongunun icinde matris islemi yapilamaz; yalnizca "
                    "matris elemanlarina erisilebilir (or. m[i, j])");
}

void Sema::checkGpuIdentifier(Identifier* ident, const Symbol& sym, bool isCallee) {
    if (sym.kind == Symbol::Function && !isCallee) {
        if (sym.isGpu)
            error(ident, "@GPU fonksiyon '" + ident->name + "' deger olarak kullanilamaz; "
                         "yalnizca adiyla cagrilabilir");
        else if (gpuFunction)
            error(ident, "@GPU fonksiyonda fonksiyon degeri kullanilamaz");
    }
    if (gpuFunction && sym.kind == Symbol::Global)
        error(ident, "@GPU fonksiyon global degisken kullanamaz ('" + ident->name +
                     "'); degeri parametre olarak verin");
}

void Sema::checkGpuElementAccess(ArrayAccessExpr* access) {
    if (!gpuFunction || !access->target->type.isMatrix() || inParallel()) return;
    error(access, "@GPU fonksiyonda matris elemanina yalnizca parallel for icinde erisilebilir "
                  "(matris GPU belleginde durur; dongunun disindaki kod CPU'da calisir)");
}

void Sema::checkGpuBuiltin(FunctionCallExpr* call, const std::string& name) {
    if (!gpuFunction) return;
    switch (call->builtin) {
        case Builtin::Print:
            error(call, "@GPU fonksiyonda print kullanilamaz; degeri dondurup @CPU tarafinda yazdirin");
            return;
        case Builtin::Time:
            // Döngünün dışında (CPU'da çalışan kısımda) serbesttir.
            if (inGpuKernel())
                error(call, "GPU'da calisan paralel dongunun icinde '" + name + "' kullanilamaz");
            return;
        case Builtin::Zeros: case Builtin::Ones: case Builtin::Fill:
        case Builtin::Matmul: case Builtin::Transpose: case Builtin::Sum:
            noteGpuWork("matris kullaniyor");
            // Sonucu matris olanları checkGpuExpr raporlar; sum sayı döndürür.
            if (inGpuKernel() && call->builtin == Builtin::Sum)
                error(call, "GPU'da calisan paralel dongunun icinde '" + name + "' kullanilamaz");
            return;
        default:
            return;
    }
}

// Kullanıcı fonksiyonuna çağrı.
void Sema::checkGpuCall(FunctionCallExpr* call) {
    auto* ident = dynamic_cast<Identifier*>(call->target.get());
    Symbol* sym = ident ? lookup(ident) : nullptr;
    const bool direct = sym && sym->kind == Symbol::Function;

    if (!gpuFunction) return;
    if (!direct) {
        error(call, "@GPU fonksiyonda yalnizca adiyla tanimli fonksiyonlar cagrilabilir");
        return;
    }
    if (!sym->isGpu)
        error(call, "@GPU fonksiyondan @CPU fonksiyon '" + ident->name + "' cagrilamaz "
                    "(GPU'da calismasi gerekiyorsa basina @GPU yazin)");
}

void Sema::checkGpuParallelFor(ForRangeStmt* loop) {
    if (!gpuFunction) return;
    noteGpuWork("parallel for iceriyor");
    // parallelLoops, bu döngü eklenmeden önce çağrılır.
    if (inParallel())
        error(loop, "@GPU fonksiyonda ic ice parallel for desteklenmiyor; icteki donguyu "
                    "siradan for olarak yazin");
}
