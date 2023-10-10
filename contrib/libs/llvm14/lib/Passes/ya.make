# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm14
    contrib/libs/llvm14/include
    contrib/libs/llvm14/lib/Analysis
    contrib/libs/llvm14/lib/IR
    contrib/libs/llvm14/lib/Support
    contrib/libs/llvm14/lib/Target
    contrib/libs/llvm14/lib/Transforms/AggressiveInstCombine
    contrib/libs/llvm14/lib/Transforms/Coroutines
    contrib/libs/llvm14/lib/Transforms/IPO
    contrib/libs/llvm14/lib/Transforms/InstCombine
    contrib/libs/llvm14/lib/Transforms/Instrumentation
    contrib/libs/llvm14/lib/Transforms/ObjCARC
    contrib/libs/llvm14/lib/Transforms/Scalar
    contrib/libs/llvm14/lib/Transforms/Utils
    contrib/libs/llvm14/lib/Transforms/Vectorize
)

ADDINCL(
    contrib/libs/llvm14/lib/Passes
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    OptimizationLevel.cpp
    PassBuilder.cpp
    PassBuilderBindings.cpp
    PassBuilderPipelines.cpp
    PassPlugin.cpp
    StandardInstrumentations.cpp
)

END()