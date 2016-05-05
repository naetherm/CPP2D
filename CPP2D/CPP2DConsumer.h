﻿//
// Copyright (c) 2016 Loïc HAMOT
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#pragma warning(push, 0)
#include <clang/AST/ASTConsumer.h>
#pragma warning(disable: 4265)
#include <clang/ASTMatchers/ASTMatchers.h>
#pragma warning(pop)

#include "MatchContainer.h"
#include "DPrinter.h"

namespace clang
{
class CompilerInstance;
}

class VisitorToDConsumer : public clang::ASTConsumer
{
public:
	explicit VisitorToDConsumer(
	  clang::CompilerInstance& Compiler,
	  llvm::StringRef InFile
	);

	void HandleTranslationUnit(clang::ASTContext& Context) override;

private:
	clang::CompilerInstance& Compiler;
	MatchContainer receiver;
	clang::ast_matchers::MatchFinder finder;
	std::unique_ptr<clang::ASTConsumer> finderConsumer;
	std::string InFile;
	DPrinter Visitor;
};

