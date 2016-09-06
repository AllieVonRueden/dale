#include "Alignmentof.h"

#include <cstdio>

namespace dale
{
namespace Operation
{
static int alignment_structs = 0;

bool
Alignmentof(Context *ctx, llvm::BasicBlock *block, Type *type,
            ParseResult *pr)
{
    std::vector<llvm::Type*> elements_llvm;
    elements_llvm.push_back(
        ctx->toLLVMType(ctx->tr->type_char, NULL, false)
    );

    llvm::Type *llvm_type = ctx->toLLVMType(type, NULL, false);
    if (!llvm_type) {
        return false;
    }
    elements_llvm.push_back(llvm_type);

    llvm::StructType *llvm_as_struct =
        llvm::StructType::get(llvm::getGlobalContext(),
                              elements_llvm, false);

    char as_name[32];
    sprintf(as_name, "_as%d", alignment_structs++);
    std::string as_name_symbol;
    ctx->ns()->nameToSymbol(as_name, &as_name_symbol);
    std::string final_name;
    final_name.append("struct_");
    final_name.append(as_name_symbol);

    llvm_as_struct->setName(as_name_symbol.c_str());

    llvm::IRBuilder<> builder(block);
    llvm::PointerType *lpt = llvm::PointerType::getUnqual(llvm_as_struct);

    std::vector<llvm::Value *> values;
    values.push_back(ctx->nt->getLLVMZero());
    values.push_back(ctx->nt->getLLVMOne());
    llvm::Value *pointer =
        builder.CreateGEP(
            llvm::ConstantPointerNull::get(lpt),
            llvm::ArrayRef<llvm::Value *>(values)
        );

    llvm::Value *res =
        builder.CreatePtrToInt(pointer, ctx->nt->getNativeSizeType());

    pr->set(block, ctx->tr->type_size, res);
    return true;
}
}
}
