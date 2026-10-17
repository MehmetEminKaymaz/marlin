#include "NestedParallel.h"

#include "../MLIRBackend/CodegenContext.h" // kGpuFunctionAttr

#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

// Çalışma zamanındaki sayaç (bkz. runtime/marlin_rt.c).
constexpr const char* kEnter = "marlin_parallel_enter";
constexpr const char* kExit = "marlin_parallel_exit";
constexpr const char* kNested = "marlin_parallel_nested";

func::FuncOp declare(ModuleOp module, StringRef name, TypeRange results) {
    if (auto existing = module.lookupSymbol<func::FuncOp>(name)) return existing;
    OpBuilder builder(module.getContext());
    builder.setInsertionPointToStart(module.getBody());
    auto function = builder.create<func::FuncOp>(module.getLoc(), name,
                                                 builder.getFunctionType({}, results));
    function.setPrivate();
    return function;
}

// Gövde çalışırken başka bir paralel döngüye girilebilir mi? İçinde paralel
// döngü varsa ya da gövdesi bu modülde olan (marlin) bir fonksiyon
// çağrılıyorsa evet. Çalışma zamanının C fonksiyonları (yalnızca bildirimi
// olanlar) paralel döngü çalıştırmaz.
bool mayReachParallel(scf::ParallelOp loop, ModuleOp module) {
    WalkResult result = loop.getBody()->walk([&](Operation* op) {
        if (isa<scf::ParallelOp, func::CallIndirectOp>(op)) return WalkResult::interrupt();
        if (auto call = dyn_cast<func::CallOp>(op)) {
            auto callee = module.lookupSymbol<FunctionOpInterface>(call.getCallee());
            if (!callee || !callee.isExternal()) return WalkResult::interrupt();
        }
        return WalkResult::advance();
    });
    return result.wasInterrupted();
}

// Döngünün sıralı kopyası: her boyut için bir scf.for, en içte gövde.
void buildSequential(OpBuilder& builder, scf::ParallelOp loop) {
    IRMapping mapping;
    for (unsigned d = 0; d < loop.getNumLoops(); ++d) {
        auto forOp = builder.create<scf::ForOp>(loop.getLoc(), loop.getLowerBound()[d],
                                                loop.getUpperBound()[d], loop.getStep()[d]);
        mapping.map(loop.getInductionVars()[d], forOp.getInductionVar());
        builder.setInsertionPointToStart(forOp.getBody());
    }
    for (Operation& op : loop.getBody()->without_terminator()) builder.clone(op, mapping);
}

class NestedParallelPass : public PassWrapper<NestedParallelPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NestedParallelPass)

    StringRef getArgument() const final { return "marlin-nested-parallel"; }
    StringRef getDescription() const final {
        return "Paralel dongu icinden girilen paralel donguleri calisirken sirali yapar";
    }
    void getDependentDialects(DialectRegistry& registry) const override {
        registry.insert<scf::SCFDialect, arith::ArithDialect, func::FuncDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();

        // İçteki döngüler önce (walk sonradan-sırayla gezer): dıştakinin sıralı
        // kopyası, içtekinin hazır hâlini kopyalar.
        llvm::SmallVector<scf::ParallelOp, 8> loops;
        for (FunctionOpInterface function : module.getOps<FunctionOpInterface>()) {
            if (function.isExternal() || function->hasAttr(kGpuFunctionAttr)) continue;
            function.walk([&](scf::ParallelOp loop) {
                if (loop.getNumResults() == 0) loops.push_back(loop);
            });
        }
        if (loops.empty()) return;

        MLIRContext* context = module.getContext();
        const Type i32 = IntegerType::get(context, 32);
        func::FuncOp enter = declare(module, kEnter, {});
        func::FuncOp exit = declare(module, kExit, {});
        func::FuncOp nested = declare(module, kNested, {i32});

        OpBuilder builder(context);
        for (scf::ParallelOp loop : loops) {
            const Location loc = loop.getLoc();
            const bool mark = mayReachParallel(loop, module);

            // if (marlin_parallel_nested() != 0) { sıralı } else { paralel }
            builder.setInsertionPoint(loop);
            Value depth = builder.create<func::CallOp>(loc, nested, ValueRange{}).getResult(0);
            Value zero = builder.create<arith::ConstantIntOp>(loc, 0, 32);
            Value inside = builder.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, depth, zero);
            auto branch = builder.create<scf::IfOp>(loc, inside, /*withElseRegion=*/true);

            builder.setInsertionPointToStart(branch.thenBlock());
            buildSequential(builder, loop);
            loop->moveBefore(branch.elseBlock()->getTerminator());

            // Paralel hâlin gövdesi: içinden girilen döngüler sıralı çalışsın.
            if (mark) {
                Block* body = loop.getBody();
                builder.setInsertionPointToStart(body);
                builder.create<func::CallOp>(loc, enter, ValueRange{});
                builder.setInsertionPoint(body->getTerminator());
                builder.create<func::CallOp>(loc, exit, ValueRange{});
            }
        }
    }
};

} // namespace

std::unique_ptr<Pass> marlin::createNestedParallelPass() {
    return std::make_unique<NestedParallelPass>();
}
