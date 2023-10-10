# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/clang14
    contrib/libs/clang14/include
    contrib/libs/clang14/lib/AST
    contrib/libs/clang14/lib/Basic
    contrib/libs/clang14/lib/Edit
    contrib/libs/clang14/lib/Frontend
    contrib/libs/clang14/lib/Lex
    contrib/libs/clang14/lib/Rewrite
    contrib/libs/clang14/lib/Serialization
    contrib/libs/llvm14
    contrib/libs/llvm14/lib/Support
)

ADDINCL(
    contrib/libs/clang14/lib/Frontend/Rewrite
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    FixItRewriter.cpp
    FrontendActions.cpp
    HTMLPrint.cpp
    InclusionRewriter.cpp
    RewriteMacros.cpp
    RewriteModernObjC.cpp
    RewriteObjC.cpp
    RewriteTest.cpp
)

END()