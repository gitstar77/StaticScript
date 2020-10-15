#include <iostream>
#include <antlr4-runtime.h>
#include "StaticScriptLexer.h"
#include "StaticScriptParser.h"
#include "AST/AST.h"
#include "Util/Output.h"
#include "Sema/ReferenceResolver.h"
#include "Sema/ScopeScanner.h"

int main(int argc, char *argv[]) {
    if (argc > 1) {
        const String &codeFilename = argv[1];
        std::ifstream fin(codeFilename);
        if (!fin) {
            errPrintln("Can not open ", argv[1]);
            return 1;
        }
        antlr4::ANTLRInputStream inputStream(fin);
        StaticScriptLexer lexer(&inputStream);
        antlr4::CommonTokenStream tokenStream(&lexer);
        tokenStream.fill();
        StaticScriptParser parser(&tokenStream);
        antlr4::tree::ParseTree *tree = parser.module();
        ASTBuilder builder(codeFilename);
        SharedPtr<ModuleNode> module = builder.visit(tree);
        SharedPtr<ScopeScanner> scanner = makeShared<ScopeScanner>();
        scanner->resolve(module);
        SharedPtr<ReferenceResolver> resolver = makeShared<ReferenceResolver>();
        resolver->resolve(module);
    } else {
        errPrintln("At least one parameter is required.");
    }
}