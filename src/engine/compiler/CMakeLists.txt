add_executable(hepyc
    main.c
    lexer.c
    ast.c
    parser.c
    codegen.c
    arch.c
    mempool.c
    devicekind.c
    module.c
    corelib.c
    repo.c
)

target_include_directories(hepyc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# hepyc finds the repo (src/ and modules/) through this unless --repo-dir is given.
target_compile_definitions(hepyc PRIVATE HERMES_DEFAULT_REPO_DIR="${CMAKE_SOURCE_DIR}")
