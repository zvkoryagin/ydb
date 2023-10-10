# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm14
    contrib/libs/llvm14/lib/DebugInfo/DWARF
    contrib/libs/llvm14/lib/MC
    contrib/libs/llvm14/lib/Object
    contrib/libs/llvm14/lib/Support
)

ADDINCL(
    contrib/libs/llvm14/lib/DebugInfo/GSYM
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    DwarfTransformer.cpp
    FileWriter.cpp
    FunctionInfo.cpp
    GsymCreator.cpp
    GsymReader.cpp
    Header.cpp
    InlineInfo.cpp
    LineTable.cpp
    LookupResult.cpp
    ObjectFileTransformer.cpp
    Range.cpp
)

END()