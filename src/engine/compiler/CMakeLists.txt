add_executable(hepyc
    main.c
    lexer.c
    ast.c
    parser.c
    codegen.c
    arch.c
    mempool.c
    devicekind.c
)

target_include_directories(hepyc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
