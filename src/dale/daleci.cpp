#include "REPL/REPL.h"

#include "Config.h"
#include "Utils/Utils.h"
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <getopt.h>
#include <cstdio>

/*! daleci

    The REPL executable.
*/

using namespace dale;

static bool
isEndingOn(const char *string, const char *ending)
{
    size_t sl = strlen (string);
    size_t el = strlen (ending);

    return (sl >= el) && (strcmp (string + (sl - el), ending) == 0);
}

static bool
appearsToBeLib(const char *str)
{
    return isEndingOn(str, ".o") || isEndingOn(str, ".a");
}

std::string
joinWithPrefix(std::vector<const char*> strings,
               const std::string prefix, std::string buffer)
{
    for (std::vector<const char*>::iterator b = strings.begin(),
                                            e = strings.end();
            b != e;
            buffer += " " + prefix + " " + (*b++));

    return buffer;
}

int
main(int argc, char **argv)
{
    srand(time(NULL) + getpid());

    progname = argv[0];

    std::vector<const char*> compile_libs;
    std::vector<const char*> run_libs;
    std::vector<const char*> include_paths;
    std::vector<const char*> run_paths;
    std::vector<const char*> bitcode_paths;
    std::vector<const char*> module_paths;

    const char *output_path_arg = NULL;
    const char *module_name     = NULL;


    int produce_set      = 0;
    int no_linking       = 0;
    int debug            = 0;
    int no_dale_stdlib   = 0;
    int no_stdlib        = 0;
    int no_common        = 0;
    int found_sm         = 0;
    int version          = 0;
    int print_expansions = 0;

    int option_index         = 0;
    int forced_remove_macros = 0;

    static const char *options = "M:a:I:d";
    static struct option long_options[] = {
        { "no-dale-stdlib",   no_argument,       &no_dale_stdlib,   1 },
        { "no-common",        no_argument,       &no_common,        1 },
        { "no-stdlib",        no_argument,       &no_stdlib,        1 },
        { "version",          no_argument,       &version,          1 },
        { "print-expansions", no_argument,       &print_expansions, 1 },
        { 0, 0, 0, 0 }
    };

    for (int opt; (opt = getopt_long(argc, argv, options, long_options,
                                     &option_index)) != -1; ) {
        switch ((char) opt) {
            case 'd': debug = 1;                                   break;
            case 'I': include_paths.push_back(optarg);             break;
            case 'a': compile_libs.push_back(optarg);              break;
            case 'M': module_paths.push_back(optarg);              break;
        };
    }

    if (version) {
        printf("%d.%d\n", DALE_VERSION_MAJOR, DALE_VERSION_MINOR);
        exit(0);
    }

    REPL repl; 
    repl.run(&compile_libs,
             &include_paths,
             &module_paths,
             debug,
             no_common,
             no_dale_stdlib,
             print_expansions);
}
