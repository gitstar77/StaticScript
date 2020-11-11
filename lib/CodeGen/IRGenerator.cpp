#include "CodeGen/IRGenerator.h"
#include "CodeGen/Pass.h"
#include "Entity/Scope.h"

IRGenerator::IRGenerator() : llvmIRBuilder(llvmContext) {}

void IRGenerator::resolve(const SharedPtr<ModuleNode> &module) {
    llvmModule = makeShared<LLVMModule>(module->filename, llvmContext);
    Builtin::initialize(*llvmModule, llvmContext);
    ASTVisitor::resolve(module);
    LLVMVerifyModule(*llvmModule);
    runPasses(*llvmModule);
}

void IRGenerator::visit(const SharedPtr<ModuleNode> &module) {
    LLVMFunctionType *mainFnType = LLVMFunctionType::get(llvmIRBuilder.getInt64Ty(), false);
    mainFn = LLVMFunction::Create(mainFnType, LLVMFunction::ExternalLinkage, "main", llvmModule.get());
    curFn = mainFn;
    LLVMBasicBlock *mainEntryBlock = LLVMBasicBlock::Create(llvmContext, "entry", mainFn);
    llvmIRBuilder.SetInsertPoint(mainEntryBlock);
    ASTVisitor::visit(module);
    setFuncInsertPoint(mainFn);
    llvmIRBuilder.CreateRet(llvmIRBuilder.getInt64(0));
    LLVMVerifyFunction(*mainFn);
}

// delete
void IRGenerator::visit(const SharedPtr<BuiltinTypeNode> &builtinType) {
    ASTVisitor::visit(builtinType);
}

void IRGenerator::visit(const SharedPtr<VarDeclNode> &varDecl) {
    ASTVisitor::visit(varDecl);
    LLVMType *type = getType(varDecl->type);
    // 变量有初始值
    bool hasInitVal = bool(varDecl->initVal);
    // 区分全局变量和局部变量
    if (varDecl->scope->isTopLevel()) {
        // 变量初始值为字面量
        bool isLiteralInit = bool(dynPtrCast<LiteralExprNode>(varDecl->initVal));
        // 是否为字符串类型变量
        bool isStringVar = varDecl->type == BuiltinTypeNode::STRING_TYPE;
        llvm::Constant *initializer;
        if (isStringVar) {
            initializer = llvm::ConstantPointerNull::getNullValue(type);
        } else {
            if (isLiteralInit) {
                initializer = LLVMCast<LLVMConstantInt>(varDecl->initVal->code);
            } else {
                initializer = llvm::ConstantInt::get(type, 0);
            }
        }
        auto *gVar = new LLVMGlobalVariable(
                *llvmModule,
                type,
                false,
                LLVMGlobalValue::ExternalLinkage,
                initializer,
                varDecl->name
        );
        uint64_t alignment = 8;
        if (varDecl->type == BuiltinTypeNode::BOOLEAN_TYPE) {
            alignment = 1;
        }
#if LLVM_VERSION_MAJOR < 10
        gVar->setAlignment(alignment);
#else
        gVar->setAlignment(llvm::MaybeAlign(alignment));
#endif
        // 如果为字符串或者有非字面量的初始值
        if (isStringVar || (hasInitVal && !isLiteralInit)) {
            llvmIRBuilder.CreateStore(varDecl->initVal->code, gVar);
        }
        varDecl->code = gVar;
    } else {
        LLVMValue *alloca = llvmIRBuilder.CreateAlloca(type);
        if (hasInitVal) {
            llvmIRBuilder.CreateStore(varDecl->initVal->code, alloca);
        }
        varDecl->code = alloca;
    }
}

// delete
void IRGenerator::visit(const SharedPtr<ParmVarDeclNode> &paramVarDecl) {
    ASTVisitor::visit(paramVarDecl);
}

void IRGenerator::visit(const SharedPtr<FunctionDeclNode> &funcDecl) {
    Vector<LLVMType *> argsType;
    for (const SharedPtr<ParmVarDeclNode> &param: funcDecl->params) {
        argsType.push_back(getType(param->type));
    }
    LLVMType *returnType = getType(funcDecl->returnType);
    LLVMFunctionType *funcType = LLVMFunctionType::get(returnType, argsType, false);
    LLVMFunction *func = LLVMFunction::Create(funcType, LLVMFunction::ExternalLinkage, funcDecl->name, llvmModule.get());
    curFn = func;

    LLVMBasicBlock *entryBlock = createBasicBlock("entry", func);
    llvmIRBuilder.SetInsertPoint(entryBlock);

    {
        size_t i = 0;
        for (llvm::Argument &arg : func->args()) {
            arg.setName(funcDecl->params[i]->name);
            LLVMValue *paramAlloca = llvmIRBuilder.CreateAlloca(getType(funcDecl->params[i]->type));
            llvmIRBuilder.CreateStore(&arg, paramAlloca);
            funcDecl->params[i]->code = paramAlloca;
            i += 1;
        }
    }

    funcDecl->body->accept(shared_from_this());

    // 如果返回类型为void而且没有显式return的话则添加ret指令
    if (funcDecl->returnType == nullptr) {
        LLVMBasicBlock *curBB = llvmIRBuilder.GetInsertBlock();
        if (!curBB->getTerminator()) {
            llvmIRBuilder.CreateRetVoid();
        }
    }
    LLVMVerifyFunction(*func);
}

void IRGenerator::visit(const SharedPtr<BooleanLiteralExprNode> &boolLiteralExpr) {
    boolLiteralExpr->code = boolLiteralExpr->literal ?
                            llvmIRBuilder.getTrue() :
                            llvmIRBuilder.getFalse();
}

void IRGenerator::visit(const SharedPtr<IntegerLiteralExprNode> &intLiteralExpr) {
    intLiteralExpr->code = LLVMConstantInt::get(
            llvmIRBuilder.getInt64Ty(),
            intLiteralExpr->literal,
            true
    );
}

void IRGenerator::visit(const SharedPtr<StringLiteralExprNode> &strLiteralExpr) {
    llvm::Constant *literal = llvmIRBuilder.CreateGlobalString(strLiteralExpr->literal);
    llvm::Value *argLiteral = llvmIRBuilder.CreatePointerCast(literal, llvmIRBuilder.getInt8PtrTy());
    strLiteralExpr->code = llvmIRBuilder.CreateCall(BuiltinString::createFunc, argLiteral);
}

void IRGenerator::visit(const SharedPtr<IdentifierExprNode> &varExpr) {
    // 按右值处理
    varExpr->code = llvmIRBuilder.CreateLoad(varExpr->refVarDecl->code);
}

void IRGenerator::visit(const SharedPtr<CallExprNode> &callExpr) {
    ASTVisitor::visit(callExpr);
    llvm::Function *calleeFunc = llvmModule->getFunction(callExpr->calleeName);
    if (!calleeFunc) {
        throw CodeGenException("没有找到函数");
    }
    if (calleeFunc->arg_size() != callExpr->args.size()) {
        throw CodeGenException("函数参数传递不正确");
    }
    Vector<LLVMValue *> argsV;
    for (size_t i = 0, e = callExpr->args.size(); i != e; ++i) {
        argsV.push_back(callExpr->args[i]->code);
    }
    callExpr->code = llvmIRBuilder.CreateCall(calleeFunc, argsV);
}

void IRGenerator::visit(const SharedPtr<UnaryOperatorExprNode> &uopExpr) {
    ASTVisitor::visit(uopExpr);
    switch (uopExpr->opCode) {
        case StaticScriptLexer::Minus: {
            uopExpr->code = llvmIRBuilder.CreateNSWNeg(uopExpr->subExpr->code);
            break;
        }
        default: {
            throw CodeGenException("不支持的一元操作符");
        }
    }
}

void IRGenerator::visit(const SharedPtr<BinaryOperatorExprNode> &bopExpr) {
    // Plus...NotEquals
    if (bopExpr->opCode >= 22 && bopExpr->opCode <= 31) {
        ASTVisitor::visit(bopExpr);
    }
    const SharedPtr<BuiltinTypeNode> &leftType = bopExpr->lhs->inferType;
    switch (bopExpr->opCode) {
        case StaticScriptLexer::Assign: {
            bopExpr->rhs->accept(shared_from_this());
            // 在语义阶段保证lhs类型为IdentifierExprNode
            // 按左值处理
            SharedPtr<IdentifierExprNode> varExpr = staticPtrCast<IdentifierExprNode>(bopExpr->lhs);
            llvmIRBuilder.CreateStore(bopExpr->rhs->code, varExpr->refVarDecl->code);
            break;
        }
        case StaticScriptLexer::Plus: {
            if (leftType == BuiltinTypeNode::STRING_TYPE) {
                Vector<LLVMValue *> argsV{bopExpr->lhs->code, bopExpr->rhs->code};
                bopExpr->code = llvmIRBuilder.CreateCall(BuiltinString::concatFunc, argsV);
            } else if (leftType == BuiltinTypeNode::INTEGER_TYPE) {
                bopExpr->code = llvmIRBuilder.CreateNSWAdd(bopExpr->lhs->code, bopExpr->rhs->code);
            }
            break;
        }
        case StaticScriptLexer::Minus: {
            bopExpr->code = llvmIRBuilder.CreateNSWSub(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::Multiply: {
            bopExpr->code = llvmIRBuilder.CreateNSWMul(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::Divide: {
            bopExpr->code = llvmIRBuilder.CreateSDiv(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::LessThan: {
            bopExpr->code = llvmIRBuilder.CreateICmpSLT(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::GreaterThan: {
            bopExpr->code = llvmIRBuilder.CreateICmpSGT(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::LessThanEquals: {
            bopExpr->code = llvmIRBuilder.CreateICmpSLE(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::GreaterThanEquals: {
            bopExpr->code = llvmIRBuilder.CreateICmpSGE(bopExpr->lhs->code, bopExpr->rhs->code);
            break;
        }
        case StaticScriptLexer::Equals: {
            if (leftType == BuiltinTypeNode::STRING_TYPE) {
                Vector<LLVMValue *> argsV{bopExpr->lhs->code, bopExpr->rhs->code};
                LLVMValue *relationship = llvmIRBuilder.CreateCall(BuiltinString::equalsFunc, argsV);
                bopExpr->code = llvmIRBuilder.CreateICmpEQ(relationship, llvmIRBuilder.getInt32(0));
            } else {
                bopExpr->code = llvmIRBuilder.CreateICmpEQ(bopExpr->lhs->code, bopExpr->rhs->code);
            }
            break;
        }
        case StaticScriptLexer::NotEquals: {
            if (leftType == BuiltinTypeNode::STRING_TYPE) {
                Vector<LLVMValue *> argsV{bopExpr->lhs->code, bopExpr->rhs->code};
                LLVMValue *relationship = llvmIRBuilder.CreateCall(BuiltinString::equalsFunc, argsV);
                bopExpr->code = llvmIRBuilder.CreateICmpNE(relationship, llvmIRBuilder.getInt32(0));
            } else {
                bopExpr->code = llvmIRBuilder.CreateICmpNE(bopExpr->lhs->code, bopExpr->rhs->code);
            }
            break;
        }
        default: {
            throw CodeGenException("不支持的二元操作符");
        }
    }
}

void IRGenerator::visit(const SharedPtr<ExprStmtNode> &exprStmt) {
    ASTVisitor::visit(exprStmt);
}

void IRGenerator::visit(const SharedPtr<CompoundStmtNode> &compStmt) {
    ASTVisitor::visit(compStmt);
}

void IRGenerator::visit(const SharedPtr<VarDeclStmtNode> &varDeclStmt) {
    ASTVisitor::visit(varDeclStmt);
}

void IRGenerator::visit(const SharedPtr<FunctionDeclStmtNode> &funcDeclStmt) {
    ASTVisitor::visit(funcDeclStmt);
    // 复位插入点
    setFuncInsertPoint(mainFn);
}

void IRGenerator::visit(const SharedPtr<IfStmtNode> &ifStmt) {
    ifStmt->condition->accept(shared_from_this());
    ifStmt->thenBB = createBasicBlock("if.then");
    ifStmt->endBB = createBasicBlock("if.end");
    ifStmt->elseBB = ifStmt->endBB;
    if (ifStmt->elseBody) {
        ifStmt->elseBB = createBasicBlock("if.else");
    }

    llvmIRBuilder.CreateCondBr(ifStmt->condition->code, ifStmt->thenBB, ifStmt->elseBB);

    emitBlock(ifStmt->thenBB);
    ifStmt->thenBody->accept(shared_from_this());
    emitBranch(ifStmt->endBB);

    if (ifStmt->elseBody) {
        emitBlock(ifStmt->elseBB);
        ifStmt->elseBody->accept(shared_from_this());
        emitBranch(ifStmt->endBB);
    }
    emitBlock(ifStmt->endBB, true);
}

// TODO: break和continue入栈出栈
void IRGenerator::visit(const SharedPtr<WhileStmtNode> &whileStmt) {
    whileStmt->condBB = createBasicBlock("while.cond");
    whileStmt->bodyBB = createBasicBlock("while.body");
    whileStmt->endBB = createBasicBlock("while.end");
    emitBlock(whileStmt->condBB);
    whileStmt->condition->accept(shared_from_this());
    llvmIRBuilder.CreateCondBr(whileStmt->condition->code, whileStmt->bodyBB, whileStmt->endBB);

    emitBlock(whileStmt->bodyBB);
    whileStmt->body->accept(shared_from_this());
    emitBranch(whileStmt->condBB);

    emitBlock(whileStmt->endBB, true);
}

// TODO: break和continue入栈出栈
void IRGenerator::visit(const SharedPtr<ForStmtNode> &forStmt) {
    if (forStmt->initVarStmt) {
        forStmt->initVarStmt->accept(shared_from_this());
    } else {
        for (const SharedPtr<ExprNode> &initExpr: forStmt->initExprs) {
            initExpr->accept(shared_from_this());
        }
    }
    forStmt->condBB = createBasicBlock("for.cond");
    forStmt->endBB = createBasicBlock("for.end");
    emitBlock(forStmt->condBB);

    if (!forStmt->updates.empty()) {
        forStmt->updateBB = createBasicBlock("for.update");
    }

    if (forStmt->condition) {
        forStmt->bodyBB = createBasicBlock("for.body");
        forStmt->condition->accept(shared_from_this());
        llvmIRBuilder.CreateCondBr(forStmt->condition->code, forStmt->bodyBB, forStmt->endBB);
        emitBlock(forStmt->bodyBB);
    } else {
        // 如果没有condition, 则把condBB看做bodyBB
    }

    forStmt->body->accept(shared_from_this());

    if (!forStmt->updates.empty()) {
        emitBlock(forStmt->updateBB);
        for (const SharedPtr<ExprNode> &updateExpr: forStmt->updates) {
            updateExpr->accept(shared_from_this());
        }
    }

    emitBranch(forStmt->condBB);

    emitBlock(forStmt->endBB, true);
}

void IRGenerator::visit(const SharedPtr<ContinueStmtNode> &continueStmt) {
    SharedPtr<WhileStmtNode> whileStmt = dynPtrCast<WhileStmtNode>(continueStmt->refIterationStmt);
    if (whileStmt) {
        llvmIRBuilder.CreateBr(whileStmt->condBB);
    } else {
        // 语义分析阶段保证了refIterationStmt不是WhileStmtNode类型就是ForStmtNode类型
        SharedPtr<ForStmtNode> forStmt = dynPtrCast<ForStmtNode>(continueStmt->refIterationStmt);
        llvmIRBuilder.CreateBr(forStmt->updateBB ? forStmt->updateBB : forStmt->condBB);
    }
}

void IRGenerator::visit(const SharedPtr<BreakStmtNode> &breakStmt) {
    SharedPtr<WhileStmtNode> whileStmt = dynPtrCast<WhileStmtNode>(breakStmt->refIterationStmt);
    if (whileStmt) {
        llvmIRBuilder.CreateBr(whileStmt->endBB);
    } else {
        SharedPtr<ForStmtNode> forStmt = dynPtrCast<ForStmtNode>(breakStmt->refIterationStmt);
        llvmIRBuilder.CreateBr(forStmt->endBB);
    }
}

// TODO: 处理函数中间的return语句
void IRGenerator::visit(const SharedPtr<ReturnStmtNode> &returnStmt) {
    ASTVisitor::visit(returnStmt);
    if (returnStmt->returnExpr) {
        llvmIRBuilder.CreateRet(returnStmt->returnExpr->code);
    } else {
        llvmIRBuilder.CreateRetVoid();
    }
}

LLVMType *IRGenerator::getType(const SharedPtr<BuiltinTypeNode> &builtinType) {
    LLVMType *type = llvmIRBuilder.getVoidTy();
    if (builtinType == BuiltinTypeNode::BOOLEAN_TYPE) {
        type = llvmIRBuilder.getInt1Ty();
    } else if (builtinType == BuiltinTypeNode::INTEGER_TYPE) {
        type = llvmIRBuilder.getInt64Ty();
    } else if (builtinType == BuiltinTypeNode::STRING_TYPE) {
        type = BuiltinString::type;
    }
    return type;
}

void IRGenerator::emitBlock(LLVMBasicBlock *bb, bool isFinished) {
    LLVMBasicBlock *curBB = llvmIRBuilder.GetInsertBlock();
    emitBranch(bb);
    if (isFinished && bb->use_empty()) {
        delete bb;
        return;
    }
    if (curBB && curBB->getParent()) {
        curFn->getBasicBlockList().insertAfter(curBB->getIterator(), bb);
    } else {
        curFn->getBasicBlockList().push_back(bb);
    }
    llvmIRBuilder.SetInsertPoint(bb);
}

void IRGenerator::emitBranch(LLVMBasicBlock *targetBB) {
    LLVMBasicBlock *curBB = llvmIRBuilder.GetInsertBlock();
    if (!curBB || curBB->getTerminator()) {

    } else {
        llvmIRBuilder.CreateBr(targetBB);
    }
    llvmIRBuilder.ClearInsertionPoint();
}
