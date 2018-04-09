#include "REPL.h"
#include "Config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <unistd.h>
#include <setjmp.h>
#include <float.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#if D_LLVM_VERSION_ORD <= 34
#include "llvm/Support/system_error.h"
#else
#include "llvm/Object/Error.h"
#endif
#if D_LLVM_VERSION_ORD >= 33
#include "llvm/Support/SourceMgr.h"
#include "llvm/IRReader/IRReader.h"
#endif

#if D_LLVM_VERSION_ORD >= 36
#include "llvm/Transforms/Utils/Cloning.h"
#endif

#include "../llvm_LLVMContext.h"
#include "../llvm_Module.h"
#include "../llvm_Linker.h"
#include "../llvm_Function.h"
#include "../llvm_CallingConv.h"
#include "../llvm_AssemblyPrintModulePass.h"
#include "../llvm_ValueSymbolTable.h"
#include "../llvm_AnalysisVerifier.h"
#include "../llvm_IRBuilder.h"
#include "../llvm_PassManager.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#if D_LLVM_VERSION_ORD <= 39
#include "llvm/Bitcode/ReaderWriter.h"
#else
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#endif
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#if D_LLVM_VERSION_ORD <= 35
#include "llvm/ExecutionEngine/JIT.h"
#else
#include "llvm/ExecutionEngine/MCJIT.h"
#endif
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Signals.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "../Units/Units.h"
#include "../Utils/Utils.h"
#include "../Context/Context.h"
#include "../Node/Node.h"
#include "../ErrorReporter/ErrorReporter.h"
#include "../TypeRegister/TypeRegister.h"
#include "../ErrorType/ErrorType.h"
#include "../STL/STL.h"
#include "../NativeTypes/NativeTypes.h"
#include "../Module/Writer/Writer.h"
#include "../Form/TopLevel/Inst/Inst.h"
#include "../Unit/Unit.h"
#include "../CoreForms/CoreForms.h"
#include "../CommonDecl/CommonDecl.h"

static const char *x86_64_layout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128";
static const char *x86_32_layout = "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-f32:32:32-f64:32:64-v64:64:64-v128:128:128-a0:0:64-f80:32:32";

extern "C" {
    void init_introspection_functions();
    void *find_introspection_function(const char *);
}

namespace dale
{
REPL::REPL()
{
    CoreForms::init();

    llvm::InitializeNativeTarget();
    llvm::InitializeAllAsmPrinters();
}

REPL::~REPL()
{
}

void *
lazyFunctionCreator(const std::string &name)
{
    void *fn_pointer = find_introspection_function(name.c_str());
    if (fn_pointer) {
        return fn_pointer;
    }

    fn_pointer =
        llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(name.c_str());
    if (fn_pointer) {
        return fn_pointer;
    }

    if (name[0] != '_') {
        /* Try for one beginning with an underscore (OS X-specific). */
        std::string osx_name;
        osx_name.append("_");
        osx_name.append(name);

        fn_pointer =
            llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(osx_name);
        if (fn_pointer) {
            return fn_pointer;
        }
    }

    fprintf(stderr, "Unable to find symbol (%s) in LFC\n", name.c_str());
    abort();
    return NULL;
}

std::string
getTriple()
{
#if D_LLVM_VERSION_ORD >= 32
    return llvm::sys::getDefaultTargetTriple();
#else
    return llvm::sys::getHostTriple();
#endif
}

void
linkModule(llvm::Linker *linker, llvm::Module *mod)
{
    std::string error;
    bool result;
#if D_LLVM_VERSION_ORD <= 32
    result = linker->LinkInModule(mod, &error);
#elif D_LLVM_VERSION_ORD <= 35
    result = linker->linkInModule(mod, &error);
#elif D_LLVM_VERSION_ORD <= 37
    result = linker->linkInModule(mod);
#else
    std::unique_ptr<llvm::Module> module_ptr(llvm::CloneModule(mod));
    result = linker->linkInModule(move(module_ptr));
#endif
    assert(!result && "unable to link module");
    _unused(result);
}

void
addDataLayout(PassManager *pass_manager, llvm::Module *mod)
{
#if D_LLVM_VERSION_ORD >= 37
#elif D_LLVM_VERSION_ORD >= 36
    pass_manager->add(new llvm::DataLayoutPass());
#elif D_LLVM_VERSION_ORD >= 35
    pass_manager->add(new llvm::DataLayoutPass(mod));
#elif D_LLVM_VERSION_ORD >= 32
    pass_manager->add(new llvm::DataLayout(mod));
#else
    pass_manager->add(new llvm::TargetData(mod));
#endif
}

void
addPrintModulePass(PassManager *pass_manager,
                   llvm::raw_fd_ostream *ostream)
{
#if D_LLVM_VERSION_ORD <= 34
    pass_manager->add(llvm::createPrintModulePass(ostream));
#else
    pass_manager->add(llvm::createPrintModulePass(*ostream));
#endif
}

#if D_LLVM_VERSION_ORD <= 34
std::auto_ptr<llvm::TargetMachine> target_sp;
#else
std::shared_ptr<llvm::TargetMachine> target_sp;
#endif
llvm::TargetMachine *
getTargetMachine(llvm::Module *last_module)
{
    llvm::Triple triple(last_module->getTargetTriple());
    if (triple.getTriple().empty()) {
        triple.setTriple(getTriple());
    }

    std::string Err;
    const llvm::Target *target =
        llvm::TargetRegistry::lookupTarget(triple.getTriple(), Err);
    assert(target && "cannot auto-select target for module");

#if D_LLVM_VERSION_ORD >= 32
    llvm::TargetOptions target_options;
#endif

    std::string Features;
    target_sp =
#if D_LLVM_VERSION_ORD <= 34
        std::auto_ptr<llvm::TargetMachine>
#else
        std::shared_ptr<llvm::TargetMachine>
#endif
        (target->createTargetMachine(
            triple.getTriple(), llvm::sys::getHostCPUName(),
            Features
#if D_LLVM_VERSION_ORD >= 32
            , target_options
#endif
#if D_LLVM_VERSION_ORD >= 39
            , llvm::Optional<llvm::Reloc::Model>()
#endif
        ));

    return target_sp.get();
}

int
REPL::run(std::vector<const char *> *compile_lib_paths,
          std::vector<const char *> *include_paths,
          std::vector<const char *> *module_paths,
          int debug,
          int no_common,
          int no_dale_stdlib,
          int print_expansions) {
{
    NativeTypes nt;
    TypeRegister tr;
    llvm::ExecutionEngine *ee = NULL;

    /* On OS X, SYSTEM_PROCESSOR is i386 even when the underlying
     * processor is x86-64, hence the extra check here. */
    bool is_x86_64 =
        ((!strcmp(SYSTEM_PROCESSOR, "x86_64"))
         || ((!strcmp(SYSTEM_PROCESSOR, "amd64")))
         || ((!strcmp(SYSTEM_NAME, "Darwin"))
             && (sizeof(char *) == 8)));

    init_introspection_functions();

    llvm::Module *last_module = NULL;

    std::vector<std::string> shared_object_paths;  
    std::vector<const char *> static_module_names; 
    Module::Reader mr(module_paths, &shared_object_paths, include_paths,
                      &static_module_names, false, false);
    for (std::vector<const char*>::iterator b = compile_lib_paths->begin(),
                                            e = compile_lib_paths->end();
            b != e;
            ++b) {
        mr.addDynamicLibrary((*b), false, false);
    }

    const char *libdrt_path = NULL;
    if (!no_dale_stdlib) {
        FILE *drt_file = NULL;
        if ((drt_file = fopen(DALE_LIBRARY_PATH "/libdrt.so", "r"))) {
            libdrt_path = DALE_LIBRARY_PATH "/libdrt.so";
        } else if ((drt_file = fopen("./libdrt.so", "r"))) {
            libdrt_path = "./libdrt.so";
        } else {
            error("unable to find libdrt.so");
        }
        mr.addDynamicLibrary(libdrt_path, false, false);
        int res = fclose(drt_file);
        if (res != 0) {
            error("unable to close %s", libdrt_path, true);
        }
    }
    if (libdrt_path) {
        shared_object_paths.push_back(libdrt_path);
    }

    Units units(&mr);
    units.cto              = false;
    units.no_common        = no_common;
    units.no_dale_stdlib   = no_dale_stdlib;
    units.print_expansions = print_expansions;
    units.debug            = debug;

    Context *ctx         = NULL;
    llvm::Module *mod    = NULL;
    llvm::Linker *linker = NULL;

    ErrorReporter er("");

    Unit *unit = new Unit("/dev/stdin", &units, &er, &nt, &tr, NULL,
                          is_x86_64, NULL, NULL, NULL, NULL, NULL,
                          true);
    units.push(unit);
    ctx    = unit->ctx;
    mod    = unit->module;
    linker = unit->linker;

    llvm::Triple triple(mod->getTargetTriple());
    if (triple.getTriple().empty()) {
        triple.setTriple(getTriple());
    }

#if D_LLVM_VERSION_ORD <= 36
    mod->setDataLayout((is_x86_64) ? x86_64_layout : x86_32_layout);
#else
    llvm::TargetMachine *target_machine = getTargetMachine(mod);
    mod->setDataLayout(target_machine->createDataLayout());
#endif

#if D_LLVM_VERSION_ORD <= 35
    llvm::EngineBuilder eb = llvm::EngineBuilder(mod);
#else
    std::unique_ptr<llvm::Module> module_ptr(llvm::CloneModule(mod));
    llvm::EngineBuilder eb(move(module_ptr));
#endif

    eb.setEngineKind(llvm::EngineKind::JIT);
    std::string error;
    eb.setErrorStr(&error);
    ee = eb.create();
    if (!ee) {
        fprintf(stderr, "Unable to create execution engine: %s\n",
                error.c_str());
        abort();
    }
    ee->InstallLazyFunctionCreator(&lazyFunctionCreator);

    unit->ee = ee;
    unit->mp->ee = ee;

    CommonDecl::addVarargsFunctions(unit);

    if (!no_common) {
        if (no_dale_stdlib) {
            unit->addCommonDeclarations();
        } else {
            std::vector<const char*> import_forms;
            mr.run(ctx, linker, mod, nullNode(), "drt", &import_forms);
            units.top()->mp->setPoolfree();
        }
    }

    std::vector<Node*> nodes;
    for (;;) {
        int error_count = er.getErrorTypeCount(ErrorType::Error);
        fprintf(stdout, "> ");
        fflush(stdout);

        Node *top = units.top()->parser->getNextList();
        if (top) {
            nodes.push_back(top);
        }

        if (er.getErrorTypeCount(ErrorType::Error) > error_count) {
            er.flush();
            continue;
        }
        if (!top) {
            er.flush();
            break;
        }

        if (!top->is_token && !top->is_list) {
            units.pop();
            if (!units.empty()) {
                Unit *unit = units.top();
                ctx    = unit->ctx;
                mod    = unit->module;
                linker = unit->linker;
                continue;
            }
            break;
        }
        FormTopLevelInstParse(&units, top);
        er.flush();
    }
}
}
}
