#include "Unit.h"
#include "../Lexer/Lexer.h"
#include "../llvm_Module.h"
#include "../llvm_Linker.h"
#include "../Utils/Utils.h"
#include "../CommonDecl/CommonDecl.h"

#include <cstdio>

namespace dale
{
Unit::Unit(const char *path, Units *units, ErrorReporter *er, NativeTypes *nt,
           TypeRegister *tr, llvm::ExecutionEngine *ee, bool is_x86_64)
{
    FILE *mfp = fopen(path, "r");
    if (!mfp) {
        char buf[1024];
        sprintf(buf, "unable to open %s for reading", path);
        error(buf, true);
    }

    er->current_filename = path;
    dnc = new DNodeConverter(er);

    ctx = new Context(er, nt, tr);
    mp = new MacroProcessor(units, ctx, ee);
    fp = new FunctionProcessor(units);

    Lexer *lxr = new Lexer(mfp);
    parser = new Parser(lxr, er, path);

    module = new llvm::Module(path, llvm::getGlobalContext());

#if D_LLVM_VERSION_MINOR <= 2
    linker = new llvm::Linker(path, module, false);
#else
    linker = new llvm::Linker(module);
#endif

    this->ee = ee;
    this->is_x86_64 = is_x86_64;
    var_count = 0;
    fn_count  = 0;

    /* For now, the unused prefix name will be a randomly-generated
     * string. */
    for (int i = 0; i < 4; i++) {
        unused_name_prefix[i] = (rand() % 25 + 97);
    }
}

Unit::~Unit()
{
    delete ctx;
    delete parser;
}

bool
Unit::hasOnceTag()
{
    return (once_tag.size() ? true : false);
}

bool
Unit::setOnceTag(std::string new_once_tag)
{
    if (hasOnceTag()) {
        return false;
    }

    once_tag = new_once_tag;
    return true;
}

Function*
Unit::getGlobalFunction()
{
    return global_function;
}

llvm::BasicBlock*
Unit::getGlobalBlock()
{
    return global_block;
}

void
Unit::pushGlobalFunction(Function *fn)
{
    global_functions.push_back(fn);
    global_function = fn;
}

void
Unit::pushGlobalBlock(llvm::BasicBlock *block)
{
    global_blocks.push_back(block);
    global_block = block;
}

void
Unit::popGlobalFunction()
{
    global_functions.pop_back();
    if (global_functions.size()) {
        global_function = global_functions.back();
    } else {
        global_function = NULL;
    }
}

void
Unit::popGlobalBlock()
{
    global_blocks.pop_back();
    if (global_blocks.size()) {
        global_block = global_blocks.back();
    } else {
        global_block = NULL;
    }
}

void
Unit::makeTemporaryGlobalFunction()
{
    llvm::Type *llvm_return_type =
        ctx->toLLVMType(ctx->tr->type_int, NULL, false);

    std::vector<llvm::Type*> empty_args;
    llvm::FunctionType *ft =
        getFunctionType(llvm_return_type, empty_args, false);

    char buf[32];
    sprintf(buf, "_intro%d", fn_count++);

    std::string new_name;
    ctx->ns()->nameToSymbol(buf, &new_name);

    assert(!module->getFunction(llvm::StringRef(new_name.c_str())) &&
           "function already exists in module");

    llvm::Constant *llvm_fnc =
        module->getOrInsertFunction(new_name.c_str(), ft);
    assert(llvm_fnc && "unable to add function to module");

    llvm::Function *llvm_fn = llvm::dyn_cast<llvm::Function>(llvm_fnc);
    assert(llvm_fn && "unable to convert function constant to function");

    std::vector<Variable *> vars;
    Function *fn =
        new Function(ctx->tr->type_int, &vars, llvm_fn, 0,
                     new std::string(new_name), 0);
    fn->linkage = Linkage::Intern;

    llvm::BasicBlock *block =
        llvm::BasicBlock::Create(llvm::getGlobalContext(), "entry", llvm_fn);

    pushGlobalFunction(fn);
    pushGlobalBlock(block);

    ctx->activateAnonymousNamespace();
}

void
Unit::removeTemporaryGlobalFunction()
{
    ctx->deactivateAnonymousNamespace();
    Function *current = getGlobalFunction();
    popGlobalFunction();
    popGlobalBlock();
    current->llvm_function->eraseFromParent();
}

static bool added_common_declarations = false;

void
Unit::addCommonDeclarations()
{
    CommonDecl::addBasicTypes(this, is_x86_64);

    /* The basic math functions and the varargs functions are
     * added to every module, but the structs are not, because
     * they can merge backwards and forwards (the other stuff has
     * internal linkage). */

    if (added_common_declarations) {
        return;
    }
    added_common_declarations = true;

    CommonDecl::addVarargsTypes(this, is_x86_64);
    CommonDecl::addStandardVariables(this);

    return;
}

void
Unit::getUnusedVarName(std::string *buf)
{
    char ibuf[16];
    do {
        sprintf(ibuf, "_dv%c%c%c%c%d",
                unused_name_prefix[0],
                unused_name_prefix[1],
                unused_name_prefix[2],
                unused_name_prefix[3],
                var_count++);
    } while (module->getGlobalVariable(llvm::StringRef(ibuf)));

    buf->append(ibuf);
    return;
}

void
Unit::getUnusedFunctionName(std::string *buf)
{
    char ibuf[16];
    do {
        sprintf(ibuf, "_fn%c%c%c%c%d",
                unused_name_prefix[0],
                unused_name_prefix[1],
                unused_name_prefix[2],
                unused_name_prefix[3],
                fn_count++);
    } while (module->getFunction(llvm::StringRef(ibuf)));

    buf->append(ibuf);
    return;
}
}
