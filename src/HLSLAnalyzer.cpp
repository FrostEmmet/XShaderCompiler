/*
 * HLSLAnalyzer.cpp
 * 
 * This file is part of the "HLSL Translator" (Copyright (c) 2014 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include "HLSLAnalyzer.h"
#include "HLSLTree.h"


namespace HTLib
{


HLSLAnalyzer::HLSLAnalyzer(Logger* log) :
    log_{ log }
{
    EstablishMaps();
}

bool HLSLAnalyzer::DecorateAST(
    Program* program,
    const std::string& entryPoint,
    const ShaderTargets shaderTarget,
    const ShaderVersions shaderVersion)
{
    if (!program)
        return false;

    /* Store parameters */
    entryPoint_     = entryPoint;
    shaderTarget_   = shaderTarget;
    shaderVersion_  = shaderVersion;

    /* Decorate program AST */
    hasErrors_ = false;
    program_ = program;

    Visit(program);

    return !hasErrors_;
}


/*
 * ======= Private: =======
 */

void HLSLAnalyzer::EstablishMaps()
{
    intrinsicMap_ = std::map<std::string, IntrinsicClasses>
    {
        { "InterlockedAdd",             IntrinsicClasses::Interlocked },
        { "InterlockedAnd",             IntrinsicClasses::Interlocked },
        { "InterlockedOr",              IntrinsicClasses::Interlocked },
        { "InterlockedXor",             IntrinsicClasses::Interlocked },
        { "InterlockedMin",             IntrinsicClasses::Interlocked },
        { "InterlockedMax",             IntrinsicClasses::Interlocked },
        { "InterlockedCompareExchange", IntrinsicClasses::Interlocked },
        { "InterlockedExchange",        IntrinsicClasses::Interlocked },
    };
}

void HLSLAnalyzer::Error(const std::string& msg, const AST* ast)
{
    hasErrors_ = true;
    if (log_)
    {
        if (ast)
            log_->Error("context error (" + ast->pos.ToString() + ") : " + msg);
        else
            log_->Error("context error : " + msg);
    }
}

void HLSLAnalyzer::OpenScope()
{
    symTable_.OpenScope();
}

void HLSLAnalyzer::CloseScope()
{
    symTable_.CloseScope();
}

void HLSLAnalyzer::Register(const std::string& ident, AST* ast, const OnOverrideProc& overrideProc)
{
    try
    {
        symTable_.Register(ident, ast, overrideProc);
    }
    catch (const std::exception& err)
    {
        Error(err.what(), ast);
    }
}

AST* HLSLAnalyzer::Fetch(const std::string& ident) const
{
    return symTable_.Fetch(ident);
}

void HLSLAnalyzer::DecorateEntryInOut(VarDeclStmnt* ast, bool isInput)
{
    const auto structFlag = (isInput ? Structure::isShaderInput : Structure::isShaderOutput);

    /* Add flag to variable declaration statement */
    ast->flags << (isInput ? VarDeclStmnt::isShaderInput : VarDeclStmnt::isShaderOutput);

    /* Add flag to structure type */
    auto& varType = ast->varType;
    if (varType->structType)
        varType->structType->flags << structFlag;

    /* Add flag to optional symbol reference */
    auto& symbolRef = varType->symbolRef;
    if (symbolRef && symbolRef->Type() == AST::Types::Structure)
    {
        auto structType = dynamic_cast<Structure*>(symbolRef);
        if (structType)
        {
            structType->flags << structFlag;
            if (!ast->varDecls.empty())
                structType->aliasName = ast->varDecls.front()->name;
        }
    }
}

void HLSLAnalyzer::DecorateEntryInOut(VarType* ast, bool isInput)
{
    const auto structFlag = (isInput ? Structure::isShaderInput : Structure::isShaderOutput);

    /* Add flag to structure type */
    if (ast->structType)
        ast->structType->flags << structFlag;

    /* Add flag to optional symbol reference */
    auto& symbolRef = ast->symbolRef;
    if (symbolRef && symbolRef->Type() == AST::Types::Structure)
    {
        auto structType = dynamic_cast<Structure*>(symbolRef);
        if (structType)
            structType->flags << structFlag;
    }
}

/* ------- Visit functions ------- */

#define IMPLEMENT_VISIT_PROC(className) \
    void HLSLAnalyzer::Visit##className(className* ast, void* args)

IMPLEMENT_VISIT_PROC(Program)
{
    for (auto& globDecl : ast->globalDecls)
        Visit(globDecl);
}

IMPLEMENT_VISIT_PROC(CodeBlock)
{
    OpenScope();

    for (auto& stmnt : ast->stmnts)
        Visit(stmnt);

    CloseScope();
}

IMPLEMENT_VISIT_PROC(FunctionCall)
{
    auto name = FullVarIdent(ast->name);

    /* Check if a specific intrinsic is used */
    if (name == "mul")
        program_->flags << Program::mulIntrinsicUsed;
    else
    {
        auto it = intrinsicMap_.find(name);
        if (it != intrinsicMap_.end())
        {
            switch (it->second)
            {
                case IntrinsicClasses::Interlocked:
                    program_->flags << Program::interlockedIntrinsicsUsed;
                    break;
            }
        }
    }

    /* Analyze function arguments */
    for (auto& arg : ast->arguments)
        Visit(arg);
}

IMPLEMENT_VISIT_PROC(Structure)
{
    if (!ast->name.empty())
    {
        Register(
            ast->name, ast,
            [](AST* symbol) -> bool
            {
                return symbol->Type() == AST::Types::StructDecl;//!TODO! Types::StructForwardDecl !!!
            }
        );
    }

    for (auto& varDecl : ast->members)
        Visit(varDecl);
}

/* --- Global declarations --- */

IMPLEMENT_VISIT_PROC(FunctionDecl)
{
    /* Register symbol name */
    Register(
        ast->name, ast,
        [](AST* symbol) -> bool
        {
            return symbol->Type() == AST::Types::FunctionDecl;//!TODO! Types::FunctionForwardDecl !!!
        }
    );

    /* Visit function header */
    for (auto& attrib : ast->attribs)
        Visit(attrib);

    Visit(ast->returnType);
    for (auto& param : ast->parameters)
        Visit(param);

    /* Mark function as used when it's the main entry point */
    const auto isEntryPoint = (ast->name == entryPoint_);

    if (isEntryPoint)
    {
        /* Add flags */
        ast->flags << FunctionDecl::isEntryPoint;
        ast->flags << FunctionDecl::isUsed;

        /* Add flags to input- and output parameters of the main entry point */
        DecorateEntryInOut(ast->returnType.get(), false);
        for (auto& param : ast->parameters)
            DecorateEntryInOut(param.get(), true);
    }

    /* Visit function body */
    isInsideEntryPoint_ = isEntryPoint;
    {
        Visit(ast->codeBlock);
    }
    isInsideEntryPoint_ = false;
}

IMPLEMENT_VISIT_PROC(BufferDecl)
{
    if (ast->bufferType != "cbuffer")
        Error("buffer type \"" + ast->bufferType + "\" currently not supported", ast);

    for (auto& member : ast->members)
        Visit(member);
}

IMPLEMENT_VISIT_PROC(StructDecl)
{
    Visit(ast->structure);
}

/* --- Statements --- */

IMPLEMENT_VISIT_PROC(VarDeclStmnt)
{
    Visit(ast->varType);
    for (auto& varDecl : ast->varDecls)
        Visit(varDecl);

    /* Decorate variable type */
    if (isInsideEntryPoint_ && ast->varDecls.empty())
    {
        auto symbolRef = ast->varType->symbolRef;
        if (symbolRef && symbolRef->Type() == AST::Types::Structure)
        {
            auto structType = dynamic_cast<Structure*>(symbolRef);
            if (structType && structType->flags(Structure::isShaderOutput) && structType->aliasName.empty())
            {
                /* Store alias name for shader output interface block */
                structType->aliasName = ast->varDecls.front()->name;
            }
        }
    }
}

IMPLEMENT_VISIT_PROC(CtrlTransferStmnt)
{
    // do nothing
}

/* --- Expressions --- */

//...

/* --- Variables --- */

IMPLEMENT_VISIT_PROC(PackOffset)
{
    // do nothing
}

IMPLEMENT_VISIT_PROC(VarSemantic)
{
    // do nothing
}

IMPLEMENT_VISIT_PROC(VarType)
{
    if (!ast->baseType.empty())
    {
        /* Decorate variable type */
        auto symbol = Fetch(ast->baseType);
        if (symbol)
            ast->symbolRef = symbol;
    }
    else if (ast->structType)
        Visit(ast->structType);
    else
        Error("missing variable type", ast);
}

IMPLEMENT_VISIT_PROC(VarIdent)
{
    for (auto& index : ast->arrayIndices)
        Visit(index);
    Visit(ast->next);
}

IMPLEMENT_VISIT_PROC(VarDecl)
{
    for (auto& dim : ast->arrayDims)
        Visit(dim);
    for (auto& semantic : ast->semantics)
        Visit(semantic);
    Visit(ast->initializer);
}

#undef IMPLEMENT_VISIT_PROC


} // /namespace HTLib



// ================================================================================