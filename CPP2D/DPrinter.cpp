﻿//
// Copyright (c) 2016 Loïc HAMOT
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "DPrinter.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <locale>
#include <codecvt>
#include <ciso646>

#pragma warning(push, 0)
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/Support/Path.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/MacroArgs.h>
#include <clang/AST/Comment.h>
#pragma warning(pop)

#include "MatchContainer.h"
#include "CPP2DTools.h"

using namespace llvm;
using namespace clang;

std::vector<std::unique_ptr<std::stringstream> > outStack;

bool output_enabled = true;

std::stringstream& out()
{
	static std::stringstream empty_ss;
	empty_ss.str("");
	if(output_enabled)
		return *outStack.back();
	else
		return empty_ss;
}

void pushStream()
{
	outStack.emplace_back(std::make_unique<std::stringstream>());
}

std::string popStream()
{
	std::string const str = outStack.back()->str();
	outStack.pop_back();
	return str;
}

#define CHECK_LOC  if (checkFilename(Decl)) {} else return true

static std::map<std::string, std::string> type2type =
{
	{ "boost::optional", "std.typecons.Nullable" },
	{ "std::vector", "cpp_std.vector" },
	{ "std::set", "std.container.rbtree.RedBlackTree" },
	{ "boost::shared_mutex", "core.sync.rwmutex.ReadWriteMutex" },
	{ "boost::mutex", "core.sync.mutex.Mutex" },
	{ "std::allocator", "cpp_std.allocator" },
	{ "time_t", "core.stdc.time.time_t" },
	{ "intptr_t", "core.stdc.stdint.intptr_t" },
	{ "int8_t", "core.stdc.stdint.int8_t" },
	{ "uint8_t", "core.stdc.stdint.uint8_t" },
	{ "int16_t", "core.stdc.stdint.int16_t" },
	{ "uint16_t", "core.stdc.stdint.uint16_t" },
	{ "int32_t", "core.stdc.stdint.int32_t" },
	{ "uint32_t", "core.stdc.stdint.uint32_t" },
	{ "int64_t", "core.stdc.stdint.int64_t" },
	{ "uint64_t", "core.stdc.stdint.uint64_t" },
	{ "SafeInt", "std.experimental.safeint.SafeInt" },
	{ "RedBlackTree", "std.container.rbtree" },
	{ "std::map", "cpp_std.map" },
	{ "std::string", "string" },
	{ "std::ostream", "std.stdio.File" },
};


static const std::set<Decl::Kind> noSemiCommaDeclKind =
{
	Decl::Kind::CXXRecord,
	Decl::Kind::Function,
	Decl::Kind::CXXConstructor,
	Decl::Kind::CXXDestructor,
	Decl::Kind::CXXConversion,
	Decl::Kind::CXXMethod,
	Decl::Kind::Namespace,
	Decl::Kind::NamespaceAlias,
	Decl::Kind::UsingDirective,
	Decl::Kind::Empty,
	Decl::Kind::Friend,
	Decl::Kind::FunctionTemplate,
	Decl::Kind::Enum,
};

static const std::set<Stmt::StmtClass> noSemiCommaStmtKind =
{
	Stmt::StmtClass::ForStmtClass,
	Stmt::StmtClass::IfStmtClass,
	Stmt::StmtClass::CXXForRangeStmtClass,
	Stmt::StmtClass::WhileStmtClass,
	Stmt::StmtClass::CompoundStmtClass,
	Stmt::StmtClass::CXXCatchStmtClass,
	Stmt::StmtClass::CXXTryStmtClass,
	Stmt::StmtClass::NullStmtClass,
	//Stmt::StmtClass::DeclStmtClass,
};

bool needSemiComma(Stmt* stmt)
{
	auto const kind = stmt->getStmtClass();
	return noSemiCommaStmtKind.count(kind) == 0;
}

bool needSemiComma(Decl* decl)
{
	auto const kind = decl->getKind();
	if(kind == Decl::Kind::CXXRecord)
	{
		auto record = static_cast<CXXRecordDecl*>(decl);
		return !record->isCompleteDefinition();
	}
	else
		return noSemiCommaDeclKind.count(kind) == 0;
}

struct Spliter
{
	std::string const str;
	bool first = true;

	Spliter(std::string const& s) : str(s) {}

	void split()
	{
		if(first)
			first = false;
		else
			out() << str;
	}
};

std::string mangleName(std::string const& name)
{
	if(name == "version")
		return "version_";
	if(name == "out")
		return "out_";
	if(name == "in")
		return "in_";
	if(name == "ref")
		return "ref_";
	if(name == "debug")
		return "debug_";
	if(name == "function")
		return "function_";
	else if(name == "Exception")
		return "Exception_";
	else
		return name;
}

static char const* AccessSpecifierStr[] =
{
	"public",
	"protected",
	"private",
	"private" // ...the special value "none" which means different things in different contexts.
	//  (from the clang doxy)
};

void DPrinter::setIncludes(std::set<std::string> const& includes)
{
	includesInFile = includes;
}

void DPrinter::includeFile(std::string const& declInc, std::string const& typeName)
{
	if(isInMacro)
		return;
	for(std::string include : includesInFile)
	{
		auto const pos = declInc.find(include);
		// TODO : Use llvm::path
		if(pos != std::string::npos &&
		   pos == (declInc.size() - include.size()) &&
		   (pos == 0 || declInc[pos - 1] == '/' || declInc[pos - 1] == '\\'))
		{
			if(include.find(".h") == include.size() - 2)
				include = include.substr(0, include.size() - 2);
			if(include.find(".hpp") == include.size() - 4)
				include = include.substr(0, include.size() - 4);
			std::transform(std::begin(include), std::end(include),
			               std::begin(include),
				           [](char c) {return static_cast<char>(tolower(c)); });
			std::replace(std::begin(include), std::end(include), '/', '.');
			std::replace(std::begin(include), std::end(include), '\\', '.');
			externIncludes[include].insert(typeName);
			break;
		}
	}
}

std::string DPrinter::mangleType(NamedDecl const* decl)
{
	NamedDecl const* canDecl = nullptr;
	std::string const& name = decl->getNameAsString();
	std::string qualName = name;
	if(Decl const* canDeclNotype = decl->getCanonicalDecl())
	{
		auto const kind = canDeclNotype->getKind();
		if(Decl::Kind::firstNamed <= kind && kind <= Decl::Kind::lastNamed)
		{
			canDecl = static_cast<NamedDecl const*>(canDeclNotype);
			qualName = canDecl->getQualifiedNameAsString();
		}
	}

	auto qualTypeToD = type2type.find(qualName);
	if(qualTypeToD != type2type.end())
	{
		//There is a convertion to D
		auto const& dQualType = qualTypeToD->second;
		auto const dotPos = dQualType.find_last_of('.');
		auto const module = dotPos == std::string::npos ?
		                    std::string() :
		                    dQualType.substr(0, dotPos);
		if(not module.empty())  //Need an import
			externIncludes[module].insert(qualName);
		return dQualType.substr(dotPos + 1);
	}
	else
	{
		NamedDecl const* usedDecl = canDecl ? canDecl : decl;
		includeFile(CPP2DTools::getFile(Context->getSourceManager(), usedDecl), qualName);
		return mangleName(name);
	}
}

std::string getName(DeclarationName const& dn)
{
	std::string name = dn.getAsString();
	if(name.empty())
	{
		std::stringstream ss;
		ss << "var" << dn.getAsOpaqueInteger();
		name = ss.str();
	}
	return name;
}

std::string DPrinter::mangleVar(DeclRefExpr* expr)
{
	std::string name = getName(expr->getNameInfo().getName());
	if(char const* filename = CPP2DTools::getFile(Context->getSourceManager(), expr->getDecl()))
		includeFile(filename, name);
	return mangleName(name);
}

std::string DPrinter::replace(std::string str, std::string const& in, std::string const& out)
{
	size_t pos = 0;
	auto iter = std::find(std::begin(str) + static_cast<intptr_t>(pos), std::end(str), '\r');
	while(iter != std::end(str))
	{
		pos = static_cast<size_t>(iter - std::begin(str));
		if((pos + 1) < str.size() && str[pos + 1] == '\n')
		{
			str = str.substr(0, pos) + out + str.substr(pos + in.size());
			++pos;
		}
		else
			++pos;
	}
	return str;
}

void DPrinter::printCommentBefore(Decl* t)
{
	SourceManager& sm = Context->getSourceManager();
	const RawComment* rc = Context->getRawCommentForDeclNoCache(t);
	if(rc && not rc->isTrailingComment())
	{
		using namespace std;
		out() << std::endl << indentStr();
		string const comment = rc->getRawText(sm).str();
		out() << replace(comment, "\r\n", "\n") << std::endl << indentStr();
	}
	else
		out() << std::endl << indentStr();
}

void DPrinter::printCommentAfter(Decl* t)
{
	SourceManager& sm = Context->getSourceManager();
	const RawComment* rc = Context->getRawCommentForDeclNoCache(t);
	if(rc && rc->isTrailingComment())
		out() << '\t' << rc->getRawText(sm).str();
}

// trim from both ends
std::string DPrinter::trim(std::string const& s)
{
	auto const pos1 = s.find_first_not_of("\r\n\t ");
	auto const pos2 = s.find_last_not_of("\r\n\t ");
	return pos1 == std::string::npos ?
	       std::string() :
	       s.substr(pos1, (pos2 - pos1) + 1);
}

std::vector<std::string> DPrinter::split(std::string const& instr)
{
	std::vector<std::string> result;
	auto prevIter = std::begin(instr);
	auto iter = std::begin(instr);
	do
	{
		iter = std::find(prevIter, std::end(instr), '\n');
		result.push_back(trim(std::string(prevIter, iter)));
		if(iter != std::end(instr))
			prevIter = iter + 1;
	}
	while(iter != std::end(instr));
	return result;
}

void DPrinter::printStmtComment(SourceLocation& locStart,
                                SourceLocation const& locEnd,
                                SourceLocation const& nextStart)
{
	if(locStart.isInvalid() || locEnd.isInvalid() || locStart.isMacroID() || locEnd.isMacroID())
	{
		locStart = nextStart;
		out() << std::endl;
		return;
	}
	auto& sm = Context->getSourceManager();
	std::string comment =
	  Lexer::getSourceText(CharSourceRange(SourceRange(locStart, locEnd), true),
	                       sm,
	                       LangOptions()
	                      ).str();
	std::vector<std::string> comments = split(comment);
	//if (comments.back() == std::string())
	comments.pop_back();
	Spliter split(indentStr());
	if(comments.empty())
		out() << std::endl;
	if(not comments.empty())
	{
		auto& firstComment = comments.front();
		auto commentPos1 = firstComment.find("//");
		auto commentPos2 = firstComment.find("/*");
		size_t trimPos = 0;
		if(commentPos1 != std::string::npos)
		{
			if(commentPos2 != std::string::npos)
				trimPos = std::min(commentPos1, commentPos2);
			else
				trimPos = commentPos1;
		}
		else if(commentPos2 != std::string::npos)
			trimPos = commentPos2;
		else
			firstComment = "";

		if(not firstComment.empty())
			firstComment = firstComment.substr(trimPos);

		out() << " ";
		size_t index = 0;
		for(auto const& c : comments)
		{
			split.split();
			out() << c;
			out() << std::endl;
			++index;
		}
	}
	locStart = nextStart;
}

void DPrinter::printMacroArgs(CallExpr* macro_args)
{
	Spliter split(", ");
	for(Expr* arg : macro_args->arguments())
	{
		split.split();
		out() << "q{";
		if(auto* callExpr = dyn_cast<CallExpr>(arg))
		{
			if(auto* impCast = dyn_cast<ImplicitCastExpr>(callExpr->getCallee()))
			{
				if(auto* func = dyn_cast<DeclRefExpr>(impCast->getSubExpr()))
				{
					std::string const func_name = func->getNameInfo().getAsString();
					if(func_name == "cpp2d_type")
					{
						TraverseTemplateArgument(func->getTemplateArgs()->getArgument());
					}
					else if(func_name == "cpp2d_name")
					{
						auto* impCast2 = dyn_cast<ImplicitCastExpr>(callExpr->getArg(0));
						auto* str = dyn_cast<StringLiteral>(impCast2->getSubExpr());
						out() << str->getString().str();
					}
					else
						TraverseStmt(arg);
				}
				else
					TraverseStmt(arg);
			}
			else
				TraverseStmt(arg);
		}
		else
			TraverseStmt(arg);
		out() << "}";
	}
}

void DPrinter::printStmtMacro(std::string const& varName, Expr* init)
{
	if(varName.find("CPP2D_MACRO_STMT_END") == 0)
		--isInMacro;
	else if(varName.find("CPP2D_MACRO_STMT") == 0)
	{
		auto get_binop = [](Expr * paren)
		{
			return dyn_cast<BinaryOperator>(dyn_cast<ParenExpr>(paren)->getSubExpr());
		};
		BinaryOperator* name_and_args = get_binop(init);
		auto* macro_name = dyn_cast<StringLiteral>(name_and_args->getLHS());
		auto* macro_args = dyn_cast<CallExpr>(name_and_args->getRHS());
		out() << "mixin(" << macro_name->getString().str() << "!(";
		printMacroArgs(macro_args);
		out() << "))";
		++isInMacro;
	}
}

DPrinter::DPrinter(
  ASTContext* Context,
  MatchContainer const& receiver,
  StringRef file)
	: Context(Context)
	, receiver(receiver)
	, modulename(llvm::sys::path::stem(file))
{
}

std::string DPrinter::indentStr() const
{
	return std::string(indent * 4, ' ');
}

bool DPrinter::TraverseTranslationUnitDecl(TranslationUnitDecl* Decl)
{
	if(passDecl(Decl)) return true;

	outStack.clear();
	outStack.emplace_back(std::make_unique<std::stringstream>());

	for(auto c : Decl->decls())
	{
		if(CPP2DTools::checkFilename(Context->getSourceManager(), modulename, c))
		{
			pushStream();
			TraverseDecl(c);
			std::string const decl = popStream();
			if(not decl.empty())
			{
				printCommentBefore(c);
				out() << indentStr() << decl;
				if(needSemiComma(c))
					out() << ';';
				printCommentAfter(c);
				out() << std::endl << std::endl;
			}
			output_enabled = (isInMacro == 0);
		}
	}

	return true;
}

bool DPrinter::TraverseTypedefDecl(TypedefDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "alias " << mangleName(Decl->getNameAsString()) << " = ";
	printType(Decl->getUnderlyingType());
	return true;
}

bool DPrinter::TraverseTypeAliasDecl(TypeAliasDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "alias " << mangleName(Decl->getNameAsString()) << " = ";
	printType(Decl->getUnderlyingType());
	return true;
}

bool DPrinter::TraverseTypeAliasTemplateDecl(TypeAliasTemplateDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "alias " << mangleName(Decl->getNameAsString());
	printTemplateParameterList(Decl->getTemplateParameters(), "");
	out() << " = ";
	printType(Decl->getTemplatedDecl()->getUnderlyingType());
	return true;
}

bool DPrinter::TraverseFieldDecl(FieldDecl* Decl)
{
	if(passDecl(Decl)) return true;
	std::string const varName = Decl->getNameAsString();
	if(varName.find("CPP2D_MACRO_STMT") == 0)
	{
		printStmtMacro(varName, Decl->getInClassInitializer());
		return true;
	}

	if(passDecl(Decl)) return true;
	if(Decl->isMutable())
		out() << "/*mutable*/";
	if(Decl->isBitField())
	{
		out() << "\t";
		printType(Decl->getType());
		out() << ", \"" << mangleName(varName) << "\", ";
		TraverseStmt(Decl->getBitWidth());
		out() << ',';
		externIncludes["std.bitmanip"].insert("bitfields");
	}
	else
	{
		printType(Decl->getType());
		out() << " " << mangleName(Decl->getNameAsString());
	}
	if(Decl->hasInClassInitializer())
	{
		out() << " = ";
		TraverseStmt(Decl->getInClassInitializer());
	}
	else if(getSemantic(Decl->getType()) == Reference)
	{
		out() << " = new ";
		printType(Decl->getType());
	}
	return true;
}

bool DPrinter::TraverseDependentNameType(DependentNameType* Type)
{
	if(passType(Type)) return false;
	TraverseNestedNameSpecifier(Type->getQualifier());
	out() << Type->getIdentifier()->getName().str();
	return true;
}

bool DPrinter::TraverseAttributedType(AttributedType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getEquivalentType());
	return true;
}

bool DPrinter::TraverseDecayedType(DecayedType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getOriginalType());
	return true;
}

bool DPrinter::TraverseElaboratedType(ElaboratedType* Type)
{
	if(passType(Type)) return false;
	if(Type->getQualifier())
		TraverseNestedNameSpecifier(Type->getQualifier());
	printType(Type->getNamedType());
	return true;
}

bool DPrinter::TraverseInjectedClassNameType(InjectedClassNameType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getInjectedSpecializationType());
	return true;
}

bool DPrinter::TraverseSubstTemplateTypeParmType(SubstTemplateTypeParmType* Type)
{
	if(passType(Type)) return false;
	return true;
}

bool DPrinter::TraverseNestedNameSpecifier(NestedNameSpecifier* NNS)
{
	if(NNS->getPrefix())
		TraverseNestedNameSpecifier(NNS->getPrefix());

	NestedNameSpecifier::SpecifierKind const kind = NNS->getKind();
	switch(kind)
	{
	//case NestedNameSpecifier::Namespace:
	//case NestedNameSpecifier::NamespaceAlias:
	//case NestedNameSpecifier::Global:
	//case NestedNameSpecifier::Super:
	case NestedNameSpecifier::TypeSpec:
	case NestedNameSpecifier::TypeSpecWithTemplate:
		printType(QualType(NNS->getAsType(), 0));
		out() << ".";
		break;
	case NestedNameSpecifier::Identifier:
		out() << NNS->getAsIdentifier()->getName().str() << ".";
		break;
	}
	return true;
}

void DPrinter::printTmpArgList(std::string const& tmpArgListStr)
{
	out() << '!';
	out() << '(' << tmpArgListStr << ')';
}

bool DPrinter::TraverseTemplateSpecializationType(TemplateSpecializationType* Type)
{
	if(passType(Type)) return false;
	if(isStdArray(Type->desugar()))
	{
		printTemplateArgument(Type->getArg(0));
		out() << '[';
		printTemplateArgument(Type->getArg(1));
		out() << ']';
		return true;
	}
	else if(isStdUnorderedMap(Type->desugar()))
	{
		printTemplateArgument(Type->getArg(1));
		out() << '[';
		printTemplateArgument(Type->getArg(0));
		out() << ']';
		return true;
	}
	out() << mangleType(Type->getTemplateName().getAsTemplateDecl());
	auto const argNum = Type->getNumArgs();
	Spliter spliter(", ");
	pushStream();
	for(unsigned int i = 0; i < argNum; ++i)
	{
		spliter.split();
		printTemplateArgument(Type->getArg(i));
	}
	printTmpArgList(popStream());
	return true;
}

bool DPrinter::TraverseTypedefType(TypedefType* Type)
{
	if(passType(Type)) return false;
	out() << mangleType(Type->getDecl());
	return true;
}

template<typename InitList>
bool DPrinter::traverseCompoundStmtImpl(CompoundStmt* Stmt, InitList initList)
{
	SourceLocation locStart = Stmt->getLBracLoc().getLocWithOffset(1);
	out() << "{";
	++indent;
	initList();
	for(auto child : Stmt->children())
	{
		printStmtComment(locStart,
		                 child->getLocStart().getLocWithOffset(-1),
		                 child->getLocEnd());
		out() << indentStr();
		TraverseStmt(child);
		if(needSemiComma(child))
			out() << ";";
		output_enabled = (isInMacro == 0);
	}
	printStmtComment(locStart, Stmt->getRBracLoc().getLocWithOffset(-1));
	--indent;
	out() << indentStr();
	out() << "}";
	return true;
}

bool DPrinter::TraverseCompoundStmt(CompoundStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	return traverseCompoundStmtImpl(Stmt, [] {});
}

template<typename InitList>
bool DPrinter::traverseCXXTryStmtImpl(CXXTryStmt* Stmt, InitList initList)
{
	out() << "try" << std::endl << indentStr();
	auto tryBlock = Stmt->getTryBlock();
	traverseCompoundStmtImpl(tryBlock, initList);
	auto handlerCount = Stmt->getNumHandlers();
	for(decltype(handlerCount) i = 0; i < handlerCount; ++i)
	{
		out() << std::endl << indentStr();
		TraverseStmt(Stmt->getHandler(i));
	}
	return true;
}

bool DPrinter::TraverseCXXTryStmt(CXXTryStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	return traverseCXXTryStmtImpl(Stmt, [] {});
}

bool DPrinter::passDecl(Decl* decl)
{
	auto printer = receiver.getPrinter(decl);
	if(printer)
	{
		printer(*this, decl);
		return true;
	}
	else
		return false;
}

bool DPrinter::passStmt(Stmt* stmt)
{
	auto printer = receiver.getPrinter(stmt);
	if(printer)
	{
		printer(*this, stmt);
		return true;
	}
	else
		return false;
}

bool DPrinter::passType(Type* type)
{
	auto printer = receiver.getPrinter(type);
	if(printer)
	{
		printer(*this, type);
		return true;
	}
	else
		return false;
}


bool DPrinter::TraverseNamespaceDecl(NamespaceDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "// -> module " << mangleName(Decl->getNameAsString()) << ';' << std::endl;
	for(auto decl : Decl->decls())
	{
		pushStream();
		TraverseDecl(decl);
		std::string const declstr = popStream();
		if(not declstr.empty())
		{
			printCommentBefore(decl);
			out() << indentStr() << declstr;
			if(needSemiComma(decl))
				out() << ';';
			printCommentAfter(decl);
			out() << std::endl << std::endl;
		}
		output_enabled = (isInMacro == 0);
	}
	out() << "// <- module " << mangleName(Decl->getNameAsString()) << " end" << std::endl;
	return true;
}

bool DPrinter::TraverseCXXCatchStmt(CXXCatchStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "catch";
	if(Stmt->getExceptionDecl())
	{
		out() << '(';
		traverseVarDeclImpl(Stmt->getExceptionDecl());
		out() << ')';
	}
	out() << std::endl;
	out() << indentStr();
	TraverseStmt(Stmt->getHandlerBlock());
	return true;
}

bool DPrinter::TraverseAccessSpecDecl(AccessSpecDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return true;
}

void DPrinter::printBasesClass(CXXRecordDecl* decl)
{
	Spliter splitBase(", ");
	if(decl->getNumBases() + decl->getNumVBases() != 0)
	{
		out() << " : ";
		auto printBaseSpec = [&](CXXBaseSpecifier & base)
		{
			splitBase.split();
			AccessSpecifier const as = base.getAccessSpecifier();
			if(as != AccessSpecifier::AS_public)
			{
				llvm::errs()
				    << "error : class " << decl->getNameAsString()
				    << " use of base class protection private and protected is no supported\n";
				out() << "/*" << AccessSpecifierStr[as] << "*/ ";
			}
			printType(base.getType());
		};
		for(CXXBaseSpecifier& base : decl->bases())
			printBaseSpec(base);
		for(CXXBaseSpecifier& base : decl->vbases())
			printBaseSpec(base);
	}
}

bool DPrinter::TraverseCXXRecordDecl(CXXRecordDecl* decl)
{
	if(passDecl(decl)) return true;
	if(decl->isClass())
	{
		for(auto* ctor : decl->ctors())
		{
			if(ctor->isImplicit() && ctor->isCopyConstructor())
			{
				llvm::errs() << "error : class " << decl->getNameAsString() <<
				             " is copy constructible which is not dlang compatible.\n";
				break;
			}
		}
	}
	return traverseCXXRecordDeclImpl(decl, [] {}, [this, decl] {printBasesClass(decl); });
}

bool DPrinter::TraverseRecordDecl(RecordDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseCXXRecordDeclImpl(Decl, [] {}, [] {});
}

template<typename TmpSpecFunc, typename PrintBasesClass>
bool DPrinter::traverseCXXRecordDeclImpl(
  RecordDecl* decl,
  TmpSpecFunc traverseTmpSpecs,
  PrintBasesClass printBasesClass)
{
	if(decl->isImplicit())
		return true;
	if(decl->isCompleteDefinition() == false && decl->getDefinition() != nullptr)
		return true;

	const bool isClass = decl->isClass();
	char const* struct_class =
	  decl->isClass() ? "class" :
	  decl->isUnion() ? "union" :
	  "struct";
	out() << struct_class << " " << mangleName(decl->getNameAsString());
	traverseTmpSpecs();
	if(decl->isCompleteDefinition() == false)
		return true;
	printBasesClass();
	out() << std::endl << indentStr() << "{";
	++indent;

	auto isBitField = [this](Decl * decl2) -> int
	{
		if(FieldDecl* field = llvm::dyn_cast<FieldDecl>(decl2))
		{
			if(field->isBitField())
				return static_cast<int>(field->getBitWidthValue(*Context));
			else
				return - 1;
		}
		else
			return -1;
	};

	auto roundPow2 = [](int bit_count)
	{
		return
		  bit_count <= 0 ? 0 :
		  bit_count <= 8 ? 8 :
		  bit_count <= 16 ? 16 :
		  bit_count <= 32 ? 32 :
		  64;
	};

	int bit_count = 0;
	bool inBitField = false;
	AccessSpecifier access = isClass ?
	                         AccessSpecifier::AS_private :
	                         AccessSpecifier::AS_public;
	for(Decl* decl2 : decl->decls())
	{
		pushStream();
		int const bc = isBitField(decl2);
		bool const nextIsBitField = (bc >= 0);
		if(nextIsBitField)
			bit_count += bc;
		else if(bit_count != 0)
			out() << "\tuint, \"\", " << (roundPow2(bit_count) - bit_count) << "));\n"
			      << indentStr();
		TraverseDecl(decl2);
		std::string const declstr = popStream();
		if(not declstr.empty())
		{
			AccessSpecifier newAccess = decl2->getAccess();
			if(newAccess == AccessSpecifier::AS_none)
				newAccess = AccessSpecifier::AS_public;
			if(newAccess != access && (isInMacro == 0))
			{
				--indent;
				out() << std::endl << indentStr() << AccessSpecifierStr[newAccess] << ":";
				++indent;
				access = newAccess;
			}
			printCommentBefore(decl2);
			if(inBitField == false && nextIsBitField && (isInMacro == 0))
				out() << "mixin(bitfields!(\n" << indentStr();
			out() << declstr;
			if(needSemiComma(decl2) && nextIsBitField == false)
				out() << ";";
			printCommentAfter(decl2);
		}
		inBitField = nextIsBitField;
		if(nextIsBitField == false)
			bit_count = 0;
		output_enabled = (isInMacro == 0);
	}
	if(inBitField)
		out() << "\n" << indentStr() << "\tuint, \"\", "
		      << (roundPow2(bit_count) - bit_count) << "));";
	out() << std::endl;

	//Print all free operator inside the class scope
	auto record_name = decl->getTypeForDecl()->getCanonicalTypeInternal().getAsString();
	for(auto rng = receiver.freeOperator.equal_range(record_name);
	    rng.first != rng.second;
	    ++rng.first)
	{
		out() << indentStr();
		traverseFunctionDeclImpl(const_cast<FunctionDecl*>(rng.first->second), 0);
		out() << std::endl;
	}
	for(auto rng = receiver.freeOperatorRight.equal_range(record_name);
	    rng.first != rng.second;
	    ++rng.first)
	{
		out() << indentStr();
		traverseFunctionDeclImpl(const_cast<FunctionDecl*>(rng.first->second), 1);
		out() << std::endl;
	}

	// print the opCmd operator
	if(auto* cxxRecordDecl = dyn_cast<CXXRecordDecl>(decl))
	{
		ClassInfo const& classInfo = classInfoMap[cxxRecordDecl];
		for(auto && type_info : classInfoMap[cxxRecordDecl].relations)
		{
			Type const* type = type_info.first;
			RelationInfo& info = type_info.second;
			if(info.hasOpLess and info.hasOpEqual)
			{
				out() << indentStr() << "int opCmp(ref in ";
				printType(type->getPointeeType());
				out() << " other)";
				if(portConst)
					out() << " const";
				out() << "\n";
				out() << indentStr() << "{\n";
				++indent;
				out() << indentStr() << "return _opLess(other) ? -1: ((this == other)? 0: 1);\n";
				--indent;
				out() << indentStr() << "}\n";
			}
		}

		if(classInfo.hasOpExclaim and not classInfo.hasBoolConv)
		{
			out() << indentStr() << "bool opCast(T : bool)()";
			if(portConst)
				out() << " const";
			out() << "\n";
			out() << indentStr() << "{\n";
			++indent;
			out() << indentStr() << "return !_opExclaim();\n";
			--indent;
			out() << indentStr() << "}\n";
		}
	}

	--indent;
	out() << indentStr() << "}";

	return true;
}

void DPrinter::printTemplateParameterList(TemplateParameterList* tmpParams,
    std::string const& prevTmplParmsStr)
{
	out() << "(";
	Spliter spliter1(", ");
	if(prevTmplParmsStr.empty() == false)
	{
		spliter1.split();
		out() << prevTmplParmsStr;
	}
	for(unsigned int i = 0, size = tmpParams->size(); i != size; ++i)
	{
		spliter1.split();
		NamedDecl* param = tmpParams->getParam(i);
		TraverseDecl(param);
		// Print default template arguments
		if(auto* FTTP = dyn_cast<TemplateTypeParmDecl>(param))
		{
			if(FTTP->hasDefaultArgument())
			{
				out() << " = ";
				printType(FTTP->getDefaultArgument());
			}
		}
		else if(auto* FNTTP = dyn_cast<NonTypeTemplateParmDecl>(param))
		{
			if(FNTTP->hasDefaultArgument())
			{
				out() << " = ";
				TraverseStmt(FNTTP->getDefaultArgument());
			}
		}
		else if(auto* FTTTP = dyn_cast<TemplateTemplateParmDecl>(param))
		{
			if(FTTTP->hasDefaultArgument())
			{
				out() << " = ";
				printTemplateArgument(FTTTP->getDefaultArgument().getArgument());
			}
		}
	}
	out() << ')';
}

bool DPrinter::TraverseClassTemplateDecl(ClassTemplateDecl* Decl)
{
	if(passDecl(Decl)) return true;
	traverseCXXRecordDeclImpl(Decl->getTemplatedDecl(), [Decl, this]
	{
		printTemplateParameterList(Decl->getTemplateParameters(), "");
	},
	[this, Decl] {printBasesClass(Decl->getTemplatedDecl()); });
	return true;
}

TemplateParameterList* DPrinter::getTemplateParameters(ClassTemplateSpecializationDecl*)
{
	return nullptr;
}

TemplateParameterList* DPrinter::getTemplateParameters(
  ClassTemplatePartialSpecializationDecl* Decl)
{
	return Decl->getTemplateParameters();
}

bool DPrinter::TraverseClassTemplatePartialSpecializationDecl(
  ClassTemplatePartialSpecializationDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseClassTemplateSpecializationDeclImpl(Decl);
}

bool DPrinter::TraverseClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseClassTemplateSpecializationDeclImpl(Decl);
}

void DPrinter::printTemplateArgument(TemplateArgument const& ta)
{
	switch(ta.getKind())
	{
	case TemplateArgument::Null: break;
	case TemplateArgument::Declaration: TraverseDecl(ta.getAsDecl()); break;
	case TemplateArgument::Integral: out() << ta.getAsIntegral().toString(10); break;
	case TemplateArgument::NullPtr: out() << "null"; break;
	case TemplateArgument::Type: printType(ta.getAsType()); break;
	default: TraverseTemplateArgument(ta);
	}
}

void DPrinter::printTemplateSpec_TmpArgsAndParms(
  TemplateParameterList& primaryTmpParams,
  TemplateArgumentList const& tmpArgs,
  TemplateParameterList* newTmpParams,
  std::string const& prevTmplParmsStr
)
{
	assert(tmpArgs.size() == primaryTmpParams.size());
	out() << '(';
	Spliter spliter2(", ");
	if(prevTmplParmsStr.empty() == false)
	{
		spliter2.split();
		out() << prevTmplParmsStr;
	}
	if(newTmpParams)
	{
		for(decltype(newTmpParams->size()) i = 0, size = newTmpParams->size(); i != size; ++i)
		{
			NamedDecl* parmDecl = newTmpParams->getParam(i);
			IdentifierInfo* info = parmDecl->getIdentifier();
			std::string name = info->getName().str() + "_";
			renamedIdentifiers[info] = name;
		}
	}

	for(decltype(tmpArgs.size()) i = 0, size = tmpArgs.size(); i != size; ++i)
	{
		spliter2.split();
		renameIdentifiers = false;
		TraverseDecl(primaryTmpParams.getParam(i));
		renameIdentifiers = true;
		out() << " : ";
		printTemplateArgument(tmpArgs.get(i));
	}
	if(newTmpParams)
	{
		for(decltype(newTmpParams->size()) i = 0, size = newTmpParams->size(); i != size; ++i)
		{
			spliter2.split();
			TraverseDecl(newTmpParams->getParam(i));
		}
	}
	out() << ')';
}

template<typename D>
bool DPrinter::traverseClassTemplateSpecializationDeclImpl(D* Decl)
{
	if(passDecl(Decl)) return true;
	if(Decl->getSpecializationKind() == TSK_ExplicitInstantiationDeclaration
	   || Decl->getSpecializationKind() == TSK_ExplicitInstantiationDefinition
	   || Decl->getSpecializationKind() == TSK_ImplicitInstantiation)
		return true;

	templateArgsStack.emplace_back();
	TemplateParameterList* tmpParams = getTemplateParameters(Decl);
	if(tmpParams)
	{
		auto& template_args = templateArgsStack.back();
		for(decltype(tmpParams->size()) i = 0, size = tmpParams->size(); i != size; ++i)
			template_args.push_back(tmpParams->getParam(i));
	}
	TemplateParameterList& specializedTmpParams =
	  *Decl->getSpecializedTemplate()->getTemplateParameters();
	TemplateArgumentList const& tmpArgs = Decl->getTemplateArgs();
	traverseCXXRecordDeclImpl(Decl, [&]
	{
		printTemplateSpec_TmpArgsAndParms(specializedTmpParams, tmpArgs, tmpParams, "");
	},
	[this, Decl] {printBasesClass(Decl); });
	templateArgsStack.pop_back();
	return true;
}

bool DPrinter::TraverseCXXConversionDecl(CXXConversionDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseFunctionDeclImpl(Decl);
}

bool DPrinter::TraverseCXXConstructorDecl(CXXConstructorDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseFunctionDeclImpl(Decl);
}

bool DPrinter::TraverseCXXDestructorDecl(CXXDestructorDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseFunctionDeclImpl(Decl);
}

bool DPrinter::TraverseCXXMethodDecl(CXXMethodDecl* Decl)
{
	if(passDecl(Decl)) return true;
	if(Decl->getLexicalParent() == Decl->getParent())
		return traverseFunctionDeclImpl(Decl);
	else
		return true;
}

bool DPrinter::TraversePredefinedExpr(PredefinedExpr* expr)
{
	if(passStmt(expr)) return true;
	out() << "__PRETTY_FUNCTION__";
	return true;
}

bool DPrinter::TraverseCXXDefaultArgExpr(CXXDefaultArgExpr* expr)
{
	if(passStmt(expr)) return true;
	TraverseStmt(expr->getExpr());
	return true;
}

bool DPrinter::TraverseCXXUnresolvedConstructExpr(CXXUnresolvedConstructExpr*  Expr)
{
	if(passStmt(Expr)) return true;
	printType(Expr->getTypeAsWritten());
	Spliter spliter(", ");
	out() << "(";
	for(decltype(Expr->arg_size()) i = 0; i < Expr->arg_size(); ++i)
	{
		auto arg = Expr->getArg(i);
		if(arg->getStmtClass() != Stmt::StmtClass::CXXDefaultArgExprClass)
		{
			spliter.split();
			TraverseStmt(arg);
		}
	}
	out() << ")";
	return true;
}

bool DPrinter::TraverseUnresolvedLookupExpr(UnresolvedLookupExpr*  Expr)
{
	if(passStmt(Expr)) return true;
	out() << mangleName(Expr->getName().getAsString());
	if(Expr->hasExplicitTemplateArgs())
	{
		size_t const argNum = Expr->getNumTemplateArgs();
		Spliter spliter(", ");
		pushStream();
		for(size_t i = 0; i < argNum; ++i)
		{
			spliter.split();
			auto tmpArg = Expr->getTemplateArgs()[i];
			printTemplateArgument(tmpArg.getArgument());
		}
		printTmpArgList(popStream());
	}
	return true;
}

bool DPrinter::TraverseCXXForRangeStmt(CXXForRangeStmt*  Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "foreach(";
	refAccepted = true;
	inForRangeInit = true;
	traverseVarDeclImpl(dyn_cast<VarDecl>(Stmt->getLoopVarStmt()->getSingleDecl()));
	inForRangeInit = false;
	refAccepted = false;
	out() << "; ";
	Expr* rangeInit = Stmt->getRangeInit();
	TraverseStmt(rangeInit);
	if(TagDecl* rangeInitDecl = rangeInit->getType()->getAsTagDecl())
	{
		std::string const name = rangeInitDecl->getQualifiedNameAsString();
		if(name.find("std::unordered_map") != std::string::npos)
			out() << ".byKeyValue";
	}

	out() << ")" << std::endl;
	TraverseCompoundStmtOrNot(Stmt->getBody());
	return true;
}

bool DPrinter::TraverseDoStmt(DoStmt*  Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "do" << std::endl;
	TraverseCompoundStmtOrNot(Stmt->getBody());
	out() << "while(";
	TraverseStmt(Stmt->getCond());
	out() << ")";
	return true;
}

bool DPrinter::TraverseSwitchStmt(SwitchStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "switch(";
	TraverseStmt(Stmt->getCond());
	out() << ")" << std::endl << indentStr();
	TraverseStmt(Stmt->getBody());
	return true;
}

bool DPrinter::TraverseCaseStmt(CaseStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "case ";
	TraverseStmt(Stmt->getLHS());
	out() << ":" << std::endl;
	++indent;
	out() << indentStr();
	TraverseStmt(Stmt->getSubStmt());
	--indent;
	return true;
}

bool DPrinter::TraverseBreakStmt(BreakStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "break";
	return true;
}

bool DPrinter::TraverseStaticAssertDecl(StaticAssertDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "static assert(";
	TraverseStmt(Decl->getAssertExpr());
	out() << ", ";
	TraverseStmt(Decl->getMessage());
	out() << ")";
	return true;
}

bool DPrinter::TraverseDefaultStmt(DefaultStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "default:" << std::endl;
	++indent;
	out() << indentStr();
	TraverseStmt(Stmt->getSubStmt());
	--indent;
	return true;
}

bool DPrinter::TraverseCXXDeleteExpr(CXXDeleteExpr* Expr)
{
	if(passStmt(Expr)) return true;
	TraverseStmt(Expr->getArgument());
	out() << " = null";
	return true;
}

bool DPrinter::TraverseCXXNewExpr(CXXNewExpr* Expr)
{
	if(passStmt(Expr)) return true;
	out() << "new ";
	if(Expr->isArray())
	{
		printType(Expr->getAllocatedType());
		out() << '[';
		TraverseStmt(Expr->getArraySize());
		out() << ']';
	}
	else
	{
		switch(Expr->getInitializationStyle())
		{
		case CXXNewExpr::NoInit:
			printType(Expr->getAllocatedType());
			break;
		case CXXNewExpr::CallInit:
			printType(Expr->getAllocatedType());
			out() << '(';
			TraverseStmt(const_cast<CXXConstructExpr*>(Expr->getConstructExpr()));
			out() << ')';
			break;
		case CXXNewExpr::ListInit:
			TraverseStmt(Expr->getInitializer());
			break;
		}
	}
	return true;
}

void DPrinter::printCXXConstructExprParams(CXXConstructExpr* Init)
{
	if(Init->getNumArgs() == 1)   //Handle Copy ctor
	{
		QualType recordType = Init->getType();
		recordType.addConst();
		if(recordType == Init->getArg(0)->getType())
		{
			TraverseStmt(Init->getArg(0));
			return;
		}
	}
	printType(Init->getType());
	out() << '(';
	Spliter spliter(", ");
	size_t counter = 0;
	Semantic const sem = getSemantic(Init->getType());
	for(auto arg : Init->arguments())
	{
		if(arg->getStmtClass() == Stmt::StmtClass::CXXDefaultArgExprClass
		   && ((counter != 0) || sem != Semantic::Value))
			break;
		spliter.split();
		TraverseStmt(arg);
		++counter;
	}
	out() << ')';
}

bool DPrinter::TraverseCXXConstructExpr(CXXConstructExpr* Init)
{
	if(passStmt(Init)) return true;
	if(Init->isListInitialization() && !Init->isStdInitListInitialization())
		out() << '{';

	Spliter spliter(", ");
	size_t count = 0;
	for(unsigned i = 0, e = Init->getNumArgs(); i != e; ++i)
	{
		if(isa<CXXDefaultArgExpr>(Init->getArg(i)) && (count != 0))
			break; // Don't print any defaulted arguments

		spliter.split();
		TraverseStmt(Init->getArg(i));
		++count;
	}
	if(Init->isListInitialization() && !Init->isStdInitListInitialization())
		out() << '}';

	return true;
}

void DPrinter::printType(QualType const& type)
{
	if(type.getTypePtr()->getTypeClass() == Type::TypeClass::Auto)
	{
		if(type.isConstQualified() && portConst)
			out() << "const ";
		TraverseType(type);
	}
	else
	{
		bool printConst = portConst || isa<BuiltinType>(type->getCanonicalTypeUnqualified());
		if(type.isConstQualified() && printConst)
			out() << "const(";
		TraverseType(type);
		if(type.isConstQualified() && printConst)
			out() << ')';
	}
}

bool DPrinter::TraverseConstructorInitializer(CXXCtorInitializer* Init)
{
	if(Init->isAnyMemberInitializer())
	{
		if(Init->getInit()->getStmtClass() == Stmt::StmtClass::CXXDefaultInitExprClass)
			return true;

		FieldDecl* fieldDecl = Init->getAnyMember();
		Semantic const sem = getSemantic(fieldDecl->getType());
		out() << fieldDecl->getNameAsString();
		out() << " = ";
		if(sem == Semantic::Value)
		{
			Expr* init = Init->getInit();
			if(auto* parenListExpr = dyn_cast<ParenListExpr>(init))
			{
				if(parenListExpr->getNumExprs() > 1)
				{
					printType(fieldDecl->getType());
					out() << '(';
				}
				TraverseStmt(Init->getInit());
				if(parenListExpr->getNumExprs() > 1)
					out() << ')';
			}
			else if(auto* ctorExpr = dyn_cast<CXXConstructExpr>(init))
			{
				if(ctorExpr->getNumArgs() > 1)
				{
					printType(fieldDecl->getType());
					out() << '(';
				}
				TraverseStmt(Init->getInit());
				if(ctorExpr->getNumArgs() > 1)
					out() << ')';
			}
			else
				TraverseStmt(Init->getInit());
		}
		else
		{
			isThisFunctionUsefull = true;
			if(auto* ctorExpr = dyn_cast<CXXConstructExpr>(Init->getInit()))
			{
				if(ctorExpr->getNumArgs() == 1)
				{
					QualType initType = ctorExpr->getArg(0)->getType().getCanonicalType();
					QualType fieldType = fieldDecl->getType().getCanonicalType();
					initType.removeLocalConst();
					fieldType.removeLocalConst();
					if(fieldType == initType)
					{
						TraverseStmt(Init->getInit());
						out() << ".dup()";
						return true;
					}
				}
				else if(ctorExpr->getNumArgs() == 0 && sem == Semantic::AssocArray)
					return true;
			}
			out() << "new ";
			printType(fieldDecl->getType());
			out() << '(';
			TraverseStmt(Init->getInit());
			out() << ')';
		}
	}
	else if(Init->isWritten())
	{
		out() << "super(";
		TraverseStmt(Init->getInit());
		out() << ")";
	}
	return true;
}

void DPrinter::startCtorBody(FunctionDecl*) {}

void DPrinter::startCtorBody(CXXConstructorDecl* Decl)
{
	auto ctor_init_count = Decl->getNumCtorInitializers();
	if(ctor_init_count != 0)
	{
		for(CXXCtorInitializer* init : Decl->inits())
		{
			pushStream();
			TraverseConstructorInitializer(init);
			std::string const initStr = popStream();
			if(initStr.empty() == false)
			{
				out() << std::endl << indentStr();
				// If nothing to print, default init is enought.
				if(initStr.substr(initStr.size() - 2) != "= ")
				{
					out() << initStr;
					out() << ";";
				}
			}
		}
	}
}

void DPrinter::printFuncEnd(CXXMethodDecl* Decl)
{
	if(Decl->isConst() && portConst)
		out() << " const";
}

void DPrinter::printFuncEnd(FunctionDecl*) {}

void DPrinter::printSpecialMethodAttribute(CXXMethodDecl* Decl)
{
	if(Decl->isStatic())
		out() << "static ";
	CXXRecordDecl* record = Decl->getParent();
	if(record->isClass())
	{
		if(Decl->isPure())
			out() << "abstract ";
		if(Decl->size_overridden_methods() != 0)
			out() << "override ";
		if(Decl->isVirtual() == false)
			out() << "final ";
	}
	else
	{
		if(Decl->isPure())
		{
			llvm::errs() << "struct " << record->getName()
			             << " has abstract function, which is forbiden.\n";
			out() << "abstract ";
		}
		if(Decl->isVirtual())
		{
			llvm::errs() << "struct " << record->getName()
			             << " has virtual function, which is forbiden.\n";
			out() << "virtual ";
		}
		if(Decl->size_overridden_methods() != 0)
			out() << "override ";
	}
}

bool DPrinter::printFuncBegin(CXXMethodDecl* Decl, std::string& tmpParams, int arg_become_this)
{
	if (not Decl->isPure() && Decl->getBody() == nullptr)
		return false;
	if(Decl->isImplicit())
		return false;
	if(Decl->isMoveAssignmentOperator())
		return false;
	if(Decl->isOverloadedOperator()
	   && Decl->getOverloadedOperator() == OverloadedOperatorKind::OO_ExclaimEqual)
		return false;
	printSpecialMethodAttribute(Decl);
	printFuncBegin((FunctionDecl*)Decl, tmpParams, arg_become_this);
	return true;
}

bool DPrinter::printFuncBegin(FunctionDecl* Decl, std::string& tmpParams, int arg_become_this)
{
	if(Decl->isImplicit())
		return false;
	if(Decl->isOverloadedOperator()
	   && Decl->getOverloadedOperator() == OverloadedOperatorKind::OO_ExclaimEqual)
		return false;
	std::string const name = Decl->getNameAsString();
	if(name == "cpp2d_dummy_variadic")
		return false;
	printType(Decl->getReturnType());
	out() << " ";
	if(Decl->isOverloadedOperator())
	{
		QualType arg1Type;
		QualType arg2Type;
		CXXRecordDecl* arg1Record = nullptr;
		CXXRecordDecl* arg2Record = nullptr;
		auto getRecordType = [](QualType qt)
		{
			if(auto const* lval = dyn_cast<LValueReferenceType>(qt.getTypePtr()))
			{
				return lval->getPointeeType()->getAsCXXRecordDecl();
			}
			else
			{
				return qt->getAsCXXRecordDecl();
			}
		};
		if(auto* methodDecl = dyn_cast<CXXMethodDecl>(Decl))
		{
			arg1Type = methodDecl->getThisType(*Context);
			arg1Record = methodDecl->getParent();
			if(methodDecl->getNumParams() > 0)
			{
				arg2Type = methodDecl->getParamDecl(0)->getType();
				arg2Record = getRecordType(arg2Type);
			}
		}
		else
		{
			if(Decl->getNumParams() > 0)
			{
				arg1Type = Decl->getParamDecl(0)->getType();
				arg1Record = getRecordType(arg1Type);
			}
			if(Decl->getNumParams() > 1)
			{
				arg2Type = Decl->getParamDecl(1)->getType();
				arg2Record = getRecordType(arg2Type);
			}
		}
		auto const nbArgs = (arg_become_this == -1 ? 1 : 0) + Decl->getNumParams();
		std::string const right = (arg_become_this == 1) ? "Right" : "";
		OverloadedOperatorKind const opKind = Decl->getOverloadedOperator();
		if(opKind == OverloadedOperatorKind::OO_EqualEqual)
		{
			out() << "opEquals" + right;
			if(arg1Record)
				classInfoMap[arg1Record].relations[arg2Type.getTypePtr()].hasOpEqual = true;
			if(arg2Record)
				classInfoMap[arg2Record].relations[arg1Type.getTypePtr()].hasOpEqual = true;
		}
		else if(opKind == OverloadedOperatorKind::OO_Exclaim)
		{
			out() << "_opExclaim" + right;
			if(arg1Record)
				classInfoMap[arg1Record].hasOpExclaim = true;
		}
		else if(opKind == OverloadedOperatorKind::OO_Call)
			out() << "opCall" + right;
		else if(opKind == OverloadedOperatorKind::OO_Subscript)
			out() << "opIndex" + right;
		else if(opKind == OverloadedOperatorKind::OO_Equal)
			out() << "opAssign" + right;
		else if(opKind == OverloadedOperatorKind::OO_Less)
		{
			out() << "_opLess" + right;
			if(arg1Record)
				classInfoMap[arg1Record].relations[arg2Type.getTypePtr()].hasOpLess = true;
		}
		else if(opKind == OverloadedOperatorKind::OO_LessEqual)
			out() << "_opLessEqual" + right;
		else if(opKind == OverloadedOperatorKind::OO_Greater)
			out() << "_opGreater" + right;
		else if(opKind == OverloadedOperatorKind::OO_GreaterEqual)
			out() << "_opGreaterEqual" + right;
		else if(opKind == OverloadedOperatorKind::OO_PlusPlus && nbArgs == 2)
			out() << "_opPostPlusplus" + right;
		else if(opKind == OverloadedOperatorKind::OO_MinusMinus && nbArgs == 2)
			out() << "_opPostMinusMinus" + right;
		else
		{
			std::string spelling = getOperatorSpelling(opKind);
			if(nbArgs == 1)
				out() << "opUnary" + right;
			else  // Two args
			{
				if(spelling.back() == '=')  //Handle self assign operators
				{
					out() << "opOpAssign";
					spelling.resize(spelling.size() - 1);
				}
				else
					out() << "opBinary" + right;
			}
			tmpParams = "string op: \"" + spelling + "\"";
		}
	}
	else
		out() << mangleName(name);
	return true;
}

bool DPrinter::printFuncBegin(CXXConversionDecl* Decl, std::string& tmpParams, int)
{
	printSpecialMethodAttribute(Decl);
	printType(Decl->getConversionType());
	out() << " opCast";
	pushStream();
	out() << "T : ";
	if(Decl->getConversionType().getAsString() == "bool")
		classInfoMap[Decl->getParent()].hasBoolConv = true;
	printType(Decl->getConversionType());
	tmpParams = popStream();
	return true;
}

bool DPrinter::printFuncBegin(CXXConstructorDecl* Decl,
                              std::string&,	//tmpParams
                              int			//arg_become_this = -1
                             )
{
	if(Decl->isMoveConstructor() || Decl->getBody() == nullptr)
		return false;

	CXXRecordDecl* record = Decl->getParent();
	if(record->isStruct() || record->isUnion())
	{
		if(Decl->isDefaultConstructor() && Decl->getNumParams() == 0)
		{
			if(Decl->isExplicit() && Decl->isDefaulted() == false)
			{
				llvm::errs() << "error : " << Decl->getNameAsString()
				             << " struct has an explicit default ctor.\n";
				llvm::errs() << "\tThis is illegal in D language.\n";
				llvm::errs() << "\tRemove it, default it or replace it by a factory method.\n";
			}
			return false; //If default struct ctor : don't print
		}
	}
	else
	{
		if(Decl->isImplicit() && !Decl->isDefaultConstructor())
			return false;
	}
	out() << "this";
	return true;
}

bool DPrinter::printFuncBegin(CXXDestructorDecl* decl,
                              std::string&,	//tmpParams,
                              int				//arg_become_this = -1
                             )
{
	if(decl->isImplicit() || decl->getBody() == nullptr)
		return false;
	//if(Decl->isPure() && !Decl->hasBody())
	//	return false; //ctor and dtor can't be abstract
	//else
	out() << "~this";
	return true;
}

template<typename Decl>
DPrinter::Semantic getThisSemantic(Decl* decl, ASTContext& context)
{
	if(decl->isStatic())
		return DPrinter::Semantic::Reference;
	auto* recordPtrType = dyn_cast<PointerType>(decl->getThisType(context));
	return DPrinter::getSemantic(recordPtrType->getPointeeType());
}

DPrinter::Semantic getThisSemantic(FunctionDecl*, ASTContext&)
{
	return DPrinter::Semantic::Reference;
}

template<typename D>
bool DPrinter::traverseFunctionDeclImpl(
  D* Decl,
  int arg_become_this)
{
	if(Decl->isDeleted())
		return true;
	if(Decl->isImplicit() && Decl->getBody() == nullptr)
		return true;

	if(Decl != Decl->getCanonicalDecl() &&
	   not(Decl->getTemplatedKind() == FunctionDecl::TK_FunctionTemplateSpecialization
	       && Decl->isThisDeclarationADefinition()))
		return true;

	pushStream();
	refAccepted = true;
	std::string tmplParamsStr;
	if(printFuncBegin(Decl, tmplParamsStr, arg_become_this) == false)
	{
		refAccepted = false;
		popStream();
		return true;
	}
	bool tmplPrinted = false;
	switch(Decl->getTemplatedKind())
	{
	case FunctionDecl::TK_MemberSpecialization:
	case FunctionDecl::TK_NonTemplate:
		break;
	case FunctionDecl::TK_FunctionTemplate:
		if(FunctionTemplateDecl* tDecl = Decl->getDescribedFunctionTemplate())
		{
			printTemplateParameterList(tDecl->getTemplateParameters(), tmplParamsStr);
			tmplPrinted = true;
		}
		break;
	case FunctionDecl::TK_FunctionTemplateSpecialization:
	case FunctionDecl::TK_DependentFunctionTemplateSpecialization:
		if(FunctionTemplateDecl* tDecl = Decl->getPrimaryTemplate())
		{
			TemplateParameterList* primaryTmpParams = tDecl->getTemplateParameters();
			TemplateArgumentList const* tmpArgs = Decl->getTemplateSpecializationArgs();
			assert(primaryTmpParams && tmpArgs);
			printTemplateSpec_TmpArgsAndParms(*primaryTmpParams, *tmpArgs, nullptr, tmplParamsStr);
			tmplPrinted = true;
		}
		break;
	default: assert(false && "Inconststent clang::FunctionDecl::TemplatedKind");
	}
	if(not tmplPrinted and not tmplParamsStr.empty())
		out() << '(' << tmplParamsStr << ')';
	out() << "(";
	inFuncArgs = true;
	bool isConstMethod = false;
	auto* ctorDecl = dyn_cast<CXXConstructorDecl>(Decl);
	bool const isCopyCtor = ctorDecl && ctorDecl->isCopyConstructor();
	Semantic const sem = getThisSemantic(Decl, *Context);
	if(Decl->getNumParams() != 0)
	{
		TypeSourceInfo* declSourceInfo = Decl->getTypeSourceInfo();
		FunctionTypeLoc funcTypeLoc;
		SourceLocation locStart;
		if(declSourceInfo)
		{
			TypeLoc declTypeLoc = declSourceInfo->getTypeLoc();
			TypeLoc::TypeLocClass tlClass = declTypeLoc.getTypeLocClass();
			if(tlClass == TypeLoc::TypeLocClass::FunctionProto)
			{
				funcTypeLoc = declTypeLoc.castAs<FunctionTypeLoc>();
				locStart = funcTypeLoc.getLParenLoc().getLocWithOffset(1);
			}
		}

		auto isConst = [](QualType type)
		{
			if(auto* ref = dyn_cast<LValueReferenceType>(type.getTypePtr()))
				return ref->getPointeeType().isConstQualified();
			else
				return type.isConstQualified();
		};

		++indent;
		size_t index = 0;
		size_t const numParam =
		  Decl->getNumParams() +
		  (Decl->isVariadic() ? 1 : 0) +
		  ((arg_become_this == -1) ? 0 : -1);
		for(ParmVarDecl* decl : Decl->params())
		{
			if(arg_become_this == static_cast<int>(index))
				isConstMethod = isConst(decl->getType());
			else
			{
				if(numParam != 1)
				{
					printStmtComment(locStart,
					                 decl->getLocStart().getLocWithOffset(-1),
					                 decl->getLocEnd().getLocWithOffset(1));
					out() << indentStr();
				}
				if(isCopyCtor && sem == Semantic::Value)
					out() << "this";
				else
				{
					if(index == 0
					   && sem == Semantic::Value
					   && (ctorDecl != nullptr))
						printDefaultValue = false;
					TraverseDecl(decl);
					printDefaultValue = true;
				}
				if(index < numParam - 1)
					out() << ',';
			}
			++index;
		}
		if(Decl->isVariadic())
		{
			if(numParam != 1)
				out() << "\n" << indentStr();
			out() << "...";
		}
		pushStream();
		if(funcTypeLoc.isNull() == false)
			printStmtComment(locStart, funcTypeLoc.getRParenLoc());
		std::string const comment = popStream();
		--indent;
		if(comment.size() > 2)
			out() << comment << indentStr();
	}
	out() << ")";
	if(isConstMethod && portConst)
		out() << " const";
	printFuncEnd(Decl);
	refAccepted = false;
	inFuncArgs = false;
	isThisFunctionUsefull = false;
	if(Stmt* body = Decl->getBody())
	{
		//Stmt* body = Decl->getBody();
		out() << std::endl << std::flush;
		if(isCopyCtor && sem == Semantic::Value)
			arg_become_this = 0;
		auto alias_this = [Decl, arg_become_this, this]
		{
			if(arg_become_this >= 0)
			{
				ParmVarDecl* param = *(Decl->param_begin() + arg_become_this);
				out() << std::endl;
				std::string const this_name = getName(param->getDeclName());
				if(this_name.empty() == false)
					out() << indentStr() << "alias " << this_name << " = this;";
			}
		};
		if(body->getStmtClass() == Stmt::CXXTryStmtClass)
		{
			out() << indentStr() << '{' << std::endl;
			++indent;
			out() << indentStr();
			traverseCXXTryStmtImpl(static_cast<CXXTryStmt*>(body),
			                       [&] {alias_this(); startCtorBody(Decl); });
			out() << std::endl;
			--indent;
			out() << indentStr() << '}';
		}
		else
		{
			out() << indentStr();
			assert(body->getStmtClass() == Stmt::CompoundStmtClass);
			traverseCompoundStmtImpl(static_cast<CompoundStmt*>(body),
			                         [&] {alias_this(); startCtorBody(Decl); });
		}
	}
	else
		out() << ";";
	std::string printedFunction = popStream();
	if(not Decl->isImplicit() || isThisFunctionUsefull)
		out() << printedFunction;
	return true;
}

bool DPrinter::TraverseUsingDecl(UsingDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "//using " << Decl->getNameAsString();
	return true;
}

bool DPrinter::TraverseFunctionDecl(FunctionDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return traverseFunctionDeclImpl(Decl);
}

bool DPrinter::TraverseUsingDirectiveDecl(UsingDirectiveDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return true;
}


bool DPrinter::TraverseFunctionTemplateDecl(FunctionTemplateDecl* Decl)
{
	if(passDecl(Decl)) return true;
	FunctionDecl* FDecl = Decl->getTemplatedDecl();
	switch(FDecl->getKind())
	{
	case Decl::Function:
		return traverseFunctionDeclImpl(FDecl);
	case Decl::CXXMethod:
		return traverseFunctionDeclImpl(llvm::cast<CXXMethodDecl>(FDecl));
	case Decl::CXXConstructor:
		return traverseFunctionDeclImpl(llvm::cast<CXXConstructorDecl>(FDecl));
	case Decl::CXXConversion:
		return traverseFunctionDeclImpl(llvm::cast<CXXConversionDecl>(FDecl));
	case Decl::CXXDestructor:
		return traverseFunctionDeclImpl(llvm::cast<CXXDestructorDecl>(FDecl));
	default: assert(false && "Inconsistent FunctionDecl kind in FunctionTemplateDecl");
		return true;
	}
}

bool DPrinter::TraverseBuiltinType(BuiltinType* Type)
{
	if(passType(Type)) return false;
	out() << [Type]
	{
		BuiltinType::Kind k = Type->getKind();
		switch(k)
		{
		case BuiltinType::Void: return "void";
		case BuiltinType::Bool: return "bool";
		case BuiltinType::Char_S: return "char";
		case BuiltinType::Char_U: return "char";
		case BuiltinType::SChar: return "char";
		case BuiltinType::Short: return "short";
		case BuiltinType::Int: return "int";
		case BuiltinType::Long: return "long";
		case BuiltinType::LongLong: return "long";
		case BuiltinType::Int128: return "cent";
		case BuiltinType::UChar: return "ubyte";
		case BuiltinType::UShort: return "ushort";
		case BuiltinType::UInt: return "uint";
		case BuiltinType::ULong: return "ulong";
		case BuiltinType::ULongLong: return "ulong";
		case BuiltinType::UInt128: return "ucent";
		case BuiltinType::Half: return "half";
		case BuiltinType::Float: return "float";
		case BuiltinType::Double: return "double";
		case BuiltinType::LongDouble: return "real";
		case BuiltinType::WChar_S:
		case BuiltinType::WChar_U: return "wchar";
		case BuiltinType::Char16: return "wchar";
		case BuiltinType::Char32: return "dchar";
		case BuiltinType::NullPtr: return "nullptr_t";
		case BuiltinType::Overload: return "<overloaded function type>";
		case BuiltinType::BoundMember: return "<bound member function type>";
		case BuiltinType::PseudoObject: return "<pseudo-object type>";
		case BuiltinType::Dependent: return "<dependent type>";
		case BuiltinType::UnknownAny: return "<unknown type>";
		case BuiltinType::ARCUnbridgedCast: return "<ARC unbridged cast type>";
		case BuiltinType::BuiltinFn: return "<builtin fn type>";
		case BuiltinType::ObjCId: return "id";
		case BuiltinType::ObjCClass: return "Class";
		case BuiltinType::ObjCSel: return "SEL";
		case BuiltinType::OCLImage1d: return "image1d_t";
		case BuiltinType::OCLImage1dArray: return "image1d_array_t";
		case BuiltinType::OCLImage1dBuffer: return "image1d_buffer_t";
		case BuiltinType::OCLImage2d: return "image2d_t";
		case BuiltinType::OCLImage2dArray: return "image2d_array_t";
		case BuiltinType::OCLImage2dDepth: return "image2d_depth_t";
		case BuiltinType::OCLImage2dArrayDepth: return "image2d_array_depth_t";
		case BuiltinType::OCLImage2dMSAA: return "image2d_msaa_t";
		case BuiltinType::OCLImage2dArrayMSAA: return "image2d_array_msaa_t";
		case BuiltinType::OCLImage2dMSAADepth: return "image2d_msaa_depth_t";
		case BuiltinType::OCLImage2dArrayMSAADepth: return "image2d_array_msaa_depth_t";
		case BuiltinType::OCLImage3d: return "image3d_t";
		case BuiltinType::OCLSampler: return "sampler_t";
		case BuiltinType::OCLEvent: return "event_t";
		case BuiltinType::OCLClkEvent: return "clk_event_t";
		case BuiltinType::OCLQueue: return "queue_t";
		case BuiltinType::OCLNDRange: return "ndrange_t";
		case BuiltinType::OCLReserveID: return "reserve_id_t";
		case BuiltinType::OMPArraySection: return "<OpenMP array section type>";
		default: assert(false && "invalid Type->getKind()");
		}
		return "";
	}();
	return true;
}

DPrinter::Semantic DPrinter::getSemantic(QualType qt)
{
	Type const* type = qt.getTypePtr();
	std::string empty;
	raw_string_ostream os(empty);
	LangOptions lo;
	PrintingPolicy pp(lo);
	qt.getCanonicalType().getUnqualifiedType().print(os, pp);
	std::string const name = os.str();
	// TODO : Externalize the semantic customization
	if(name.find("class SafeInt<") == 0)
		return Value;
	if(name.find("class boost::array<") == 0)
		return Value;
	if(name.find("class std::basic_string<") == 0)
		return Value;
	if(name.find("class boost::optional<") == 0)
		return Value;
	if(name.find("class boost::property_tree::basic_ptree<") == 0)
		return Value;
	if(name.find("class std::vector<") == 0)
		return Value;
	if(name.find("class std::shared_ptr<") == 0)
		return Value;
	if(name.find("class std::scoped_ptr<") == 0)
		return Value;
	if(name.find("class std::unordered_map<") == 0)
		return AssocArray;
	Type::TypeClass const cla = type->getTypeClass();
	return
	  cla == Type::TypeClass::Auto ? Value :
	  (type->isClassType() || type->isFunctionType()) ? Reference :
	  Value;
}

template<typename PType>
bool DPrinter::traversePointerTypeImpl(PType* Type)
{
	QualType const pointee = Type->getPointeeType();
	Type::TypeClass const tc = pointee->getTypeClass();
	if(tc == Type::Paren)  //function pointer do not need '*'
	{
		auto innerType = static_cast<ParenType const*>(pointee.getTypePtr())->getInnerType();
		if(innerType->getTypeClass() == Type::FunctionProto)
			return TraverseType(innerType);
	}
	printType(pointee);
	out() << ((getSemantic(pointee) == Value) ? "[]" : ""); //'*';
	return true;
}

bool DPrinter::TraverseMemberPointerType(MemberPointerType* Type)
{
	if(passType(Type)) return false;
	return traversePointerTypeImpl(Type);
}
bool DPrinter::TraversePointerType(PointerType* Type)
{
	if(passType(Type)) return false;
	return traversePointerTypeImpl(Type);
}

bool DPrinter::TraverseCXXNullPtrLiteralExpr(CXXNullPtrLiteralExpr* Expr)
{
	if(passStmt(Expr)) return true;
	out() << "null";
	return true;
}

bool DPrinter::TraverseEnumConstantDecl(EnumConstantDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << mangleName(Decl->getNameAsString());
	if(Decl->getInitExpr())
	{
		out() << " = ";
		TraverseStmt(Decl->getInitExpr());
	}
	return true;
}

bool DPrinter::TraverseEnumDecl(EnumDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "enum " << mangleName(Decl->getNameAsString());
	if(Decl->isFixed())
	{
		out() << " : ";
		TraverseType(Decl->getIntegerType());
	}
	out() << std::endl << indentStr() << "{" << std::endl;
	++indent;
	size_t count = 0;
	for(auto e : Decl->enumerators())
	{
		++count;
		out() << indentStr();
		TraverseDecl(e);
		out() << "," << std::endl;
	}
	if(count == 0)
		out() << indentStr() << "Default" << std::endl;
	--indent;
	out() << indentStr() << "}";
	return true;
}

bool DPrinter::TraverseEnumType(EnumType* Type)
{
	if(passType(Type)) return false;
	out() << mangleName(Type->getDecl()->getNameAsString());
	return true;
}

bool DPrinter::TraverseIntegerLiteral(IntegerLiteral* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << Stmt->getValue().toString(10, true);
	return true;
}

bool DPrinter::TraverseDecltypeType(DecltypeType* Type)
{
	if(passType(Type)) return false;
	out() << "typeof(";
	TraverseStmt(Type->getUnderlyingExpr());
	out() << ')';
	return true;
}

bool DPrinter::TraverseAutoType(AutoType* Type)
{
	if(passType(Type)) return false;
	if(not inForRangeInit)
		out() << "auto";
	return true;
}

bool DPrinter::TraverseLinkageSpecDecl(LinkageSpecDecl* Decl)
{
	if(passDecl(Decl)) return true;
	switch(Decl->getLanguage())
	{
	case LinkageSpecDecl::LanguageIDs::lang_c: out() << "extern (C) "; break;
	case LinkageSpecDecl::LanguageIDs::lang_cxx: out() << "extern (C++) "; break;
	default: assert(false && "Inconsistant LinkageSpecDecl::LanguageIDs");
	}
	DeclContext* declContext = LinkageSpecDecl::castToDeclContext(Decl);;
	if(Decl->hasBraces())
	{
		out() << "\n" << indentStr() << "{\n";
		++indent;
		for(auto* decl : declContext->decls())
		{
			out() << indentStr();
			TraverseDecl(decl);
			if(needSemiComma(decl))
				out() << ";";
			out() << "\n";
		}
		--indent;
		out() << indentStr() << "}";
	}
	else
		TraverseDecl(*declContext->decls_begin());
	return true;
}

bool DPrinter::TraverseFriendDecl(FriendDecl* Decl)
{
	if(passDecl(Decl)) return true;
	out() << "//friend ";
	if(Decl->getFriendType())
		TraverseType(Decl->getFriendType()->getType());
	else
		TraverseDecl(Decl->getFriendDecl());
	return true;
}

bool DPrinter::TraverseParmVarDecl(ParmVarDecl* Decl)
{
	if(passDecl(Decl)) return true;
	printType(Decl->getType());
	std::string const name = getName(Decl->getDeclName());//getNameAsString();
	if(name.empty() == false)
		out() <<  " " << mangleName(name);
	if(Decl->hasDefaultArg())
	{
		if(not printDefaultValue)
			out() << "/*";
		out() << " = ";
		TraverseStmt(
		  Decl->hasUninstantiatedDefaultArg() ?
		  Decl->getUninstantiatedDefaultArg() :
		  Decl->getDefaultArg());
		if(not printDefaultValue)
			out() << "*/";
	}
	return true;
}

bool DPrinter::TraverseRValueReferenceType(RValueReferenceType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getPointeeType());
	out() << "/*&&*/";
	return true;
}

bool DPrinter::TraverseLValueReferenceType(LValueReferenceType* Type)
{
	if(passType(Type)) return false;
	if(refAccepted)
	{
		if(getSemantic(Type->getPointeeType()) == Value)
		{
			if(inFuncArgs)
			{
				// In D, we can't take a rvalue by const ref. So we need to pass by copy.
				// (But the copy will be elided when possible)
				if(Type->getPointeeType().isConstant(*Context) == false)
					out() << "ref ";
			}
			else
				out() << "ref ";
		}
		printType(Type->getPointeeType());
	}
	else
	{
		printType(Type->getPointeeType());
		if(getSemantic(Type->getPointeeType()) == Value)
			out() << "[]";
	}
	return true;
}

bool DPrinter::TraverseTemplateTypeParmType(TemplateTypeParmType* Type)
{
	if(passType(Type)) return false;
	if(Type->getDecl())
		TraverseDecl(Type->getDecl());
	else
	{
		IdentifierInfo* identifier = Type->getIdentifier();
		if(identifier == nullptr)
		{
			if(Type->getDepth() >= templateArgsStack.size())
				out() << "/* getDepth : " << Type->getDepth() << "*/";
			else if(Type->getIndex() >= templateArgsStack[Type->getDepth()].size())
				out() << "/* getIndex : " << Type->getIndex() << "*/";
			else
			{
				auto param = templateArgsStack[Type->getDepth()][Type->getIndex()];
				identifier = param->getIdentifier();
				if(identifier == nullptr)
					TraverseDecl(param);
			}
		}
		auto iter = renamedIdentifiers.find(identifier);
		if(iter != renamedIdentifiers.end())
			out() << iter->second;
		else if(identifier != nullptr)
			out() << identifier->getName().str();
		else
			out() << "cant_find_name";
	}
	return true;
}

bool DPrinter::TraverseTemplateTypeParmDecl(TemplateTypeParmDecl* Decl)
{
	if(passDecl(Decl)) return true;
	IdentifierInfo* identifier = Decl->getIdentifier();
	if(identifier)
	{
		auto iter = renamedIdentifiers.find(identifier);
		if(renameIdentifiers && iter != renamedIdentifiers.end())
			out() << iter->second;
		else
			out() << identifier->getName().str();
	}
	// A template type without name is a auto param of a lambda
	// Add "else" to handle it
	return true;
}

bool DPrinter::TraverseNonTypeTemplateParmDecl(NonTypeTemplateParmDecl* Decl)
{
	if(passDecl(Decl)) return true;
	printType(Decl->getType());
	out() << " ";
	IdentifierInfo* identifier = Decl->getIdentifier();
	if(identifier)
		out() << mangleName(identifier->getName());
	return true;
}

bool DPrinter::TraverseDeclStmt(DeclStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	if(Stmt->isSingleDecl()) //May be in for or catch
		TraverseDecl(Stmt->getSingleDecl());
	else
	{
		if(splitMultiLineDecl)
		{
			auto declCount = Stmt->decl_end() - Stmt->decl_begin();
			decltype(declCount) count = 0;
			for(auto d : Stmt->decls())
			{
				TraverseDecl(d);
				++count;
				if(count != declCount)
					out() << ";\n" << indentStr();
			}
		}
		else
		{
			Spliter split(", ");
			for(auto d : Stmt->decls())
			{
				doPrintType = split.first;
				split.split();
				TraverseDecl(d);
				if(isa<RecordDecl>(d))
				{
					out() << "\n" << indentStr();
					split.first = true;
				}
				doPrintType = true;
			}
		}
	}
	return true;
}

bool DPrinter::TraverseNamespaceAliasDecl(NamespaceAliasDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return true;
}

bool DPrinter::TraverseReturnStmt(ReturnStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "return";
	if(Stmt->getRetValue())
	{
		out() << " ";
		TraverseStmt(Stmt->getRetValue());
	}
	return true;
	out() << "return";
	if(Stmt->getRetValue())
	{
		out() << ' ';
		TraverseStmt(Stmt->getRetValue());
	}
	return true;
}

bool DPrinter::TraverseCXXOperatorCallExpr(CXXOperatorCallExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	auto const numArgs = Stmt->getNumArgs();
	const OverloadedOperatorKind kind = Stmt->getOperator();
	char const* opStr = getOperatorSpelling(kind);
	if(kind == OverloadedOperatorKind::OO_Call || kind == OverloadedOperatorKind::OO_Subscript)
	{
		auto iter = Stmt->arg_begin(), end = Stmt->arg_end();
		TraverseStmt(*iter);
		Spliter spliter(", ");
		out() << opStr[0];
		for(++iter; iter != end; ++iter)
		{
			if((*iter)->getStmtClass() != Stmt::StmtClass::CXXDefaultArgExprClass)
			{
				spliter.split();
				TraverseStmt(*iter);
			}
		}
		out() << opStr[1];
	}
	else if(kind == OverloadedOperatorKind::OO_Arrow)
		TraverseStmt(*Stmt->arg_begin());
	else if(kind == OverloadedOperatorKind::OO_Equal)
	{
		Expr* lo = *Stmt->arg_begin();
		Expr* ro = *(Stmt->arg_end() - 1);

		bool const lo_ptr = lo->getType()->isPointerType();
		bool const ro_ptr = ro->getType()->isPointerType();

		Semantic const lo_sem = getSemantic(lo->getType());
		Semantic const ro_sem = getSemantic(ro->getType());

		bool const dup = //both operands will be transformed to pointer
		  (ro_ptr == false && ro_sem != Semantic::Value) &&
		  (lo_ptr == false && lo_sem != Semantic::Value);

		if(dup)
		{
			// Always use dup, because
			//  - It is OK on hashmap
			//  - opAssign is not possible on classes
			//  - copy ctor is possible but can cause slicing
			TraverseStmt(lo);
			out() << " = ";
			TraverseStmt(ro);
			out() << ".dup()";
			isThisFunctionUsefull = true;
		}
		else
		{
			TraverseStmt(lo);
			out() << " = ";
			TraverseStmt(ro);
		}
	}
	else if(kind == OverloadedOperatorKind::OO_PlusPlus ||
	        kind == OverloadedOperatorKind::OO_MinusMinus)
	{
		if(numArgs == 2)
		{
			TraverseStmt(*Stmt->arg_begin());
			out() << opStr;
		}
		else
		{
			out() << opStr;
			TraverseStmt(*Stmt->arg_begin());
		}
	}
	else
	{
		if(numArgs == 2)
		{
			TraverseStmt(*Stmt->arg_begin());
			out() << " ";
		}
		out() << opStr;
		if(numArgs == 2)
			out() << " ";
		TraverseStmt(*(Stmt->arg_end() - 1));
	}
	return true;
}

bool DPrinter::TraverseExprWithCleanups(ExprWithCleanups* Stmt)
{
	if(passStmt(Stmt)) return true;
	TraverseStmt(Stmt->getSubExpr());
	return true;
}

void DPrinter::TraverseCompoundStmtOrNot(Stmt* Stmt)  //Impl
{
	if(Stmt->getStmtClass() == Stmt::StmtClass::CompoundStmtClass)
	{
		out() << indentStr();
		TraverseStmt(Stmt);
	}
	else
	{
		++indent;
		out() << indentStr();
		if(isa<NullStmt>(Stmt))
			out() << "{}";
		TraverseStmt(Stmt);
		if(needSemiComma(Stmt))
			out() << ";";
		--indent;
	}
}

bool DPrinter::TraverseArraySubscriptExpr(ArraySubscriptExpr* Expr)
{
	if(passStmt(Expr)) return true;
	TraverseStmt(Expr->getLHS());
	out() << '[';
	TraverseStmt(Expr->getRHS());
	out() << ']';
	return true;
}

bool DPrinter::TraverseFloatingLiteral(FloatingLiteral* Expr)
{
	if(passStmt(Expr)) return true;
	const llvm::fltSemantics& sem = Expr->getSemantics();
	llvm::SmallString<1000> str;
	if(APFloat::semanticsSizeInBits(sem) < 64)
	{
		Expr->getValue().toString(str, std::numeric_limits<float>::digits10);
		out() << str.c_str() << 'f';
	}
	else if(APFloat::semanticsSizeInBits(sem) > 64)
	{
		Expr->getValue().toString(str, std::numeric_limits<long double>::digits10);
		out() << str.c_str() << 'l';
	}
	else
	{
		Expr->getValue().toString(str, std::numeric_limits<double>::digits10);
		out() << str.c_str();
	}
	return true;
}

bool DPrinter::TraverseForStmt(ForStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "for(";
	splitMultiLineDecl = false;
	TraverseStmt(Stmt->getInit());
	splitMultiLineDecl = true;
	out() << "; ";
	TraverseStmt(Stmt->getCond());
	out() << "; ";
	TraverseStmt(Stmt->getInc());
	out() << ")" << std::endl;
	TraverseCompoundStmtOrNot(Stmt->getBody());
	return true;
}

bool DPrinter::TraverseWhileStmt(WhileStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "while(";
	TraverseStmt(Stmt->getCond());
	out() << ")" << std::endl;
	TraverseCompoundStmtOrNot(Stmt->getBody());
	return true;
}

bool DPrinter::TraverseIfStmt(IfStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	out() << "if(";
	TraverseStmt(Stmt->getCond());
	out() << ")" << std::endl;
	TraverseCompoundStmtOrNot(Stmt->getThen());
	if(Stmt->getElse())
	{
		out() << std::endl << indentStr() << "else ";
		if(Stmt->getElse()->getStmtClass() == Stmt::IfStmtClass)
			TraverseStmt(Stmt->getElse());
		else
		{
			out() << std::endl;
			TraverseCompoundStmtOrNot(Stmt->getElse());
		}
	}
	return true;
}

bool DPrinter::TraverseCXXBindTemporaryExpr(CXXBindTemporaryExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	TraverseStmt(Stmt->getSubExpr());
	return true;
}

bool DPrinter::TraverseCXXThrowExpr(CXXThrowExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << "throw ";
	TraverseStmt(Stmt->getSubExpr());
	return true;
}

bool DPrinter::TraverseMaterializeTemporaryExpr(MaterializeTemporaryExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	TraverseStmt(Stmt->GetTemporaryExpr());
	return true;
}

bool DPrinter::TraverseCXXFunctionalCastExpr(CXXFunctionalCastExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	QualType qt = Stmt->getTypeInfoAsWritten()->getType();
	if(getSemantic(qt) == Semantic::Reference)
		out() << "new ";
	printType(qt);
	out() << '(';
	TraverseStmt(Stmt->getSubExpr());
	out() << ')';
	return true;
}

bool DPrinter::TraverseParenType(ParenType* Type)
{
	if(passType(Type)) return false;
	// Parenthesis are useless (and illegal) on function types
	printType(Type->getInnerType());
	return true;
}

bool DPrinter::TraverseFunctionProtoType(FunctionProtoType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getReturnType());
	out() << " function(";
	Spliter spliter(", ");
	for(auto const& p : Type->getParamTypes())
	{
		spliter.split();
		printType(p);
	}
	if(Type->isVariadic())
	{
		spliter.split();
		out() << "...";
	}
	out() << ')';
	return true;
}

bool DPrinter::TraverseCXXTemporaryObjectExpr(CXXTemporaryObjectExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	printType(Stmt->getType());
	out() << '(';
	TraverseCXXConstructExpr(Stmt);
	out() << ')';
	return true;
}

bool DPrinter::TraverseNullStmt(NullStmt* Stmt)
{
	if(passStmt(Stmt)) return false;
	return true;
}

bool DPrinter::TraverseCharacterLiteral(CharacterLiteral* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << '\'';
	auto c = Stmt->getValue();
	switch(c)
	{
	case '\0': out() << "\\0"; break;
	case '\n': out() << "\\n"; break;
	case '\t': out() << "\\t"; break;
	case '\r': out() << "\\r"; break;
	default: out() << (char)c;
	}
	out() << '\'';
	return true;
}

bool DPrinter::TraverseStringLiteral(StringLiteral* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << "\"";
	std::string literal;
	auto str = Stmt->getString();
	if(Stmt->isUTF16() || Stmt->isWide())
	{
		typedef unsigned short ushort;
		static_assert(sizeof(ushort) == 2, "sizeof(unsigned short) == 2 expected");
		std::basic_string<unsigned short> literal16(
			reinterpret_cast<ushort const*>(str.data()), 
			str.size() / 2);
		std::wstring_convert<std::codecvt_utf8<ushort>, ushort> cv;
		literal = cv.to_bytes(literal16);
	}
	else if(Stmt->isUTF32())
	{
		static_assert(sizeof(unsigned int) == 4, "sizeof(unsigned int) == 4 required");
		std::basic_string<unsigned int> literal32(
			reinterpret_cast<unsigned int const*>(str.data()), 
			str.size() / 4);
		std::wstring_convert<std::codecvt_utf8<unsigned int>, unsigned int> cv;
		literal = cv.to_bytes(literal32);
	}
	else
		literal = std::string(str.data(), str.size());
	size_t pos = 0;
	while((pos = literal.find('\\', pos)) != std::string::npos)
	{
		literal = literal.substr(0, pos) + "\\\\" + literal.substr(pos + 1);
		pos += 2;
	}
	pos = std::string::npos;
	while((pos = literal.find('\n')) != std::string::npos)
		literal = literal.substr(0, pos) + "\\n" + literal.substr(pos + 1);
	pos = 0;
	while((pos = literal.find('"', pos)) != std::string::npos)
	{
		if(pos > 0 && literal[pos - 1] == '\\')
			++pos;
		else
		{
			literal = literal.substr(0, pos) + "\\\"" + literal.substr(pos + 1);
			pos += 2;
		}
	}
	out() << literal;
	out() << "\\0\"";
	return true;
}

bool DPrinter::TraverseCXXBoolLiteralExpr(CXXBoolLiteralExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << (Stmt->getValue() ? "true" : "false");
	return true;
}

bool DPrinter::TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr* Expr)
{
	if(passStmt(Expr)) return true;
	if(Expr->isArgumentType())
		printType(Expr->getArgumentType());
	else
		TraverseStmt(Expr->getArgumentExpr());
	UnaryExprOrTypeTrait const kind = Expr->getKind();
	out() << (
	        kind == UETT_AlignOf					? ".alignof"					:
	        kind == UETT_SizeOf						? ".sizeof"						:
	        kind == UETT_OpenMPRequiredSimdAlign	? ".OpenMPRequiredSimdAlign"	:
	        kind == UETT_VecStep					? ".VecStep"					:
	        "");
	return true;
}

bool DPrinter::TraverseEmptyDecl(EmptyDecl* Decl)
{
	if(passDecl(Decl)) return true;
	return true;
}


bool DPrinter::TraverseLambdaExpr(LambdaExpr* Node)
{
	if(passStmt(Node)) return true;
	CXXMethodDecl* Method = Node->getCallOperator();

	// Has some auto type?
	bool hasAuto = false;
	if(Node->hasExplicitParameters())
	{
		for(ParmVarDecl* P : Method->params())
		{
			if(P->getType()->getTypeClass() == Type::TypeClass::TemplateTypeParm)
			{
				hasAuto = true;
				break;
			}
		}
	}

	if(hasAuto)
	{
		externIncludes["cpp_std"].insert("toFunctor");
		out() << "toFunctor!(";
	}

	const FunctionProtoType* Proto = Method->getType()->getAs<FunctionProtoType>();

	if(Node->hasExplicitResultType())
	{
		out() << "function ";
		printType(Proto->getReturnType());
	}

	if(Node->hasExplicitParameters())
	{
		out() << "(";
		inFuncArgs = true;
		refAccepted = true;
		Spliter split(", ");
		for(ParmVarDecl* P : Method->params())
		{
			split.split();
			TraverseDecl(P);
		}
		if(Method->isVariadic())
		{
			split.split();
			out() << "...";
		}
		out() << ')';
		inFuncArgs = false;
		refAccepted = false;
	}

	// Print the body.
	out() << "\n" << indentStr();
	CompoundStmt* Body = Node->getBody();
	TraverseStmt(Body);
	if(hasAuto)
		out() << ")()";
	return true;
}

void DPrinter::printCallExprArgument(CallExpr* Stmt)
{
	out() << "(";
	Spliter spliter(", ");
	for(Expr* arg : Stmt->arguments())
	{
		if(arg->getStmtClass() == Stmt::StmtClass::CXXDefaultArgExprClass)
			break;
		spliter.split();
		TraverseStmt(arg);
	}
	out() << ")";
}

bool DPrinter::TraverseCallExpr(CallExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	Expr* func = Stmt->getCallee();
	dontTakePtr.insert(func);
	TraverseStmt(func);
	dontTakePtr.erase(func);
	// Are parentezis on zero argument needed?
	//if (Stmt->getNumArgs() == 0)
	//	return true;
	printCallExprArgument(Stmt);
	return true;
}

bool DPrinter::TraverseImplicitCastExpr(ImplicitCastExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	if(Stmt->getCastKind() == CK_FunctionToPointerDecay && dontTakePtr.count(Stmt) == 0)
		out() << "&";
	if(Stmt->getCastKind() == CK_ConstructorConversion)
	{
		QualType const type = Stmt->getType();
		if(getSemantic(type) == Semantic::Reference)
			out() << "new ";
		printType(type);
		out() << '(';
	}
	TraverseStmt(Stmt->getSubExpr());
	if(Stmt->getCastKind() == CK_ConstructorConversion)
		out() << ')';
	return true;
}

bool DPrinter::TraverseCXXThisExpr(CXXThisExpr* expr)
{
	if(passStmt(expr)) return true;
	QualType pointee = expr->getType()->getPointeeType();
	if(getSemantic(pointee) == Semantic::Value)
		out() << "(&this)[0..1]";
	else
		out() << "this";
	return true;
}

bool DPrinter::isStdArray(QualType const& type)
{
	QualType const rawType = type.isCanonical() ?
	                         type :
	                         type.getCanonicalType();
	std::string const name = rawType.getAsString();
	static std::string const boost_array = "class boost::array<";
	static std::string const std_array = "class std::array<";
	return
	  name.substr(0, boost_array.size()) == boost_array ||
	  name.substr(0, std_array.size()) == std_array;
}

bool DPrinter::isStdUnorderedMap(QualType const& type)
{
	QualType const rawType = type.isCanonical() ?
	                         type :
	                         type.getCanonicalType();
	std::string const name = rawType.getAsString();
	static std::string const std_unordered_map = "class std::unordered_map<";
	return name.substr(0, std_unordered_map.size()) == std_unordered_map;
}

bool DPrinter::TraverseCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr* expr)
{
	if(passStmt(expr)) return true;
	return traverseMemberExprImpl(expr);
}

bool DPrinter::TraverseMemberExpr(MemberExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	return traverseMemberExprImpl(Stmt);
}

template<typename ME>
bool DPrinter::traverseMemberExprImpl(ME* Stmt)
{
	DeclarationName const declName = Stmt->getMemberNameInfo().getName();
	auto const kind = declName.getNameKind();
	std::string const memberName = Stmt->getMemberNameInfo().getName().getAsString();
	Expr* base = Stmt->isImplicitAccess() ? nullptr : Stmt->getBase();
	bool const isThis = not(base && base->getStmtClass() != Stmt::StmtClass::CXXThisExprClass);
	if(not isThis)
		TraverseStmt(base);
	if(kind == DeclarationName::NameKind::CXXConversionFunctionName)
	{
		if(memberName.empty() == false && not isThis)
			out() << '.';
		out() << "opCast!(";
		printType(declName.getCXXNameType());
		out() << ')';
	}
	else if(kind == DeclarationName::NameKind::CXXOperatorName)
	{
		out() << " " << memberName.substr(8) << " "; //8 is size of "operator"
	}
	else
	{
		if(memberName.empty() == false && not isThis)
			out() << '.';
		out() << memberName;
	}
	auto TAL = Stmt->getTemplateArgs();
	auto const tmpArgCount = Stmt->getNumTemplateArgs();
	Spliter spliter(", ");
	if(tmpArgCount != 0)
	{
		pushStream();
		for(size_t I = 0; I < tmpArgCount; ++I)
		{
			spliter.split();
			printTemplateArgument(TAL[I].getArgument());
		}
		printTmpArgList(popStream());
	}

	return true;
}

bool DPrinter::TraverseCXXMemberCallExpr(CXXMemberCallExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	TraverseStmt(Stmt->getCallee());
	printCallExprArgument(Stmt);
	return true;
}

bool DPrinter::TraverseCXXStaticCastExpr(CXXStaticCastExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << "cast(";
	printType(Stmt->getTypeInfoAsWritten()->getType());
	out() << ')';
	TraverseStmt(Stmt->getSubExpr());
	return true;
}

bool DPrinter::TraverseCStyleCastExpr(CStyleCastExpr* Stmt)
{
	if(passStmt(Stmt)) return true;
	out() << "cast(";
	printType(Stmt->getTypeInfoAsWritten()->getType());
	out() << ')';
	TraverseStmt(Stmt->getSubExpr());
	return true;
}

bool DPrinter::TraverseConditionalOperator(ConditionalOperator* op)
{
	if(passStmt(op)) return true;
	TraverseStmt(op->getCond());
	out() << "? ";
	TraverseStmt(op->getTrueExpr());
	out() << ": ";
	TraverseStmt(op->getFalseExpr());
	return true;
}

bool DPrinter::TraverseCompoundAssignOperator(CompoundAssignOperator* op)
{
	if(passStmt(op)) return true;
	DPrinter::TraverseBinaryOperator(op);
	return true;
}

bool DPrinter::TraverseBinAddAssign(CompoundAssignOperator* expr)
{
	if(passStmt(expr)) return true;
	if(expr->getLHS()->getType()->isPointerType())
	{
		TraverseStmt(expr->getLHS());
		out() << ".popFrontN(";
		TraverseStmt(expr->getRHS());
		out() << ')';
		externIncludes["std.range.primitives"].insert("popFrontN");
		return true;
	}
	else
		return TraverseCompoundAssignOperator(expr);
}


#define OPERATOR(NAME)                                        \
	bool DPrinter::TraverseBin##NAME##Assign(CompoundAssignOperator *S) \
	{if (passStmt(S)) return true; return TraverseCompoundAssignOperator(S);}
OPERATOR(Mul) OPERATOR(Div) OPERATOR(Rem) OPERATOR(Sub)
OPERATOR(Shl) OPERATOR(Shr) OPERATOR(And) OPERATOR(Or) OPERATOR(Xor)
#undef OPERATOR


bool DPrinter::TraverseSubstNonTypeTemplateParmExpr(SubstNonTypeTemplateParmExpr* Expr)
{
	if(passStmt(Expr)) return true;
	TraverseStmt(Expr->getReplacement());
	return true;
}

bool DPrinter::TraverseBinaryOperator(BinaryOperator* Stmt)
{
	if(passStmt(Stmt)) return true;
	Expr* lhs = Stmt->getLHS();
	Expr* rhs = Stmt->getRHS();
	Type const* typeL = lhs->getType().getTypePtr();
	Type const* typeR = rhs->getType().getTypePtr();
	if(typeL->isPointerType() and typeR->isPointerType())
	{
		TraverseStmt(Stmt->getLHS());
		switch(Stmt->getOpcode())
		{
		case BinaryOperatorKind::BO_EQ: out() << " is "; break;
		case BinaryOperatorKind::BO_NE: out() << " !is "; break;
		default: out() << " " << Stmt->getOpcodeStr().str() << " ";
		}
		TraverseStmt(Stmt->getRHS());
	}
	else
	{
		TraverseStmt(lhs);
		out() << " " << Stmt->getOpcodeStr().str() << " ";
		TraverseStmt(rhs);
	}
	return true;
}

bool DPrinter::TraverseBinAdd(BinaryOperator* expr)
{
	if(passStmt(expr)) return true;
	if(expr->getLHS()->getType()->isPointerType())
	{
		TraverseStmt(expr->getLHS());
		out() << '[';
		TraverseStmt(expr->getRHS());
		out() << "..$]";
		return true;
	}
	else
		return TraverseBinaryOperator(expr);
}

#define OPERATOR(NAME) \
	bool DPrinter::TraverseBin##NAME(BinaryOperator* Stmt) \
	{if (passStmt(Stmt)) return true; return TraverseBinaryOperator(Stmt);}
OPERATOR(PtrMemD) OPERATOR(PtrMemI) OPERATOR(Mul) OPERATOR(Div)
OPERATOR(Rem) OPERATOR(Sub) OPERATOR(Shl) OPERATOR(Shr)
OPERATOR(LT) OPERATOR(GT) OPERATOR(LE) OPERATOR(GE) OPERATOR(EQ)
OPERATOR(NE) OPERATOR(And) OPERATOR(Xor) OPERATOR(Or) OPERATOR(LAnd)
OPERATOR(LOr) OPERATOR(Assign) OPERATOR(Comma)
#undef OPERATOR

bool DPrinter::TraverseUnaryOperator(UnaryOperator* Stmt)
{
	if(passStmt(Stmt)) return true;
	if(Stmt->isIncrementOp())
	{
		if(Stmt->getSubExpr()->getType()->isPointerType())
		{
			TraverseStmt(Stmt->getSubExpr());
			out() << ".popFront";
			externIncludes["std.range.primitives"].insert("popFront");
			return true;
		}
	}

	if(Stmt->isPostfix())
	{
		TraverseStmt(Stmt->getSubExpr());
		out() << Stmt->getOpcodeStr(Stmt->getOpcode()).str();
	}
	else
	{
		std::string preOp = Stmt->getOpcodeStr(Stmt->getOpcode()).str();
		std::string postOp = "";
		if(Stmt->getOpcode() == UnaryOperatorKind::UO_AddrOf)
		{
			preOp = "(&";
			postOp = ")[0..1]";
		}
		else if(Stmt->getOpcode() == UnaryOperatorKind::UO_Deref)
		{
			if(auto* t = dyn_cast<CXXThisExpr>(Stmt->getSubExpr()))
			{
				// (*this) in C++ mean (this) in D
				out() << "this";
				return true;
			}

			preOp.clear();
			postOp = "[0]";
		}

		// Avoid to deref struct this
		Expr* expr = static_cast<Expr*>(*Stmt->child_begin());
		bool showOp = true;

		QualType exprType = expr->getType();
		Semantic operSem =
		  exprType->hasPointerRepresentation() ?
		  getSemantic(exprType->getPointeeType()) :
		  getSemantic(exprType);

		if(operSem != Value)
		{
			if(Stmt->getOpcode() == UnaryOperatorKind::UO_AddrOf
			   || Stmt->getOpcode() == UnaryOperatorKind::UO_Deref)
				showOp = false;
		}
		if(showOp)
			out() << preOp;
		for(auto c : Stmt->children())
			TraverseStmt(c);
		if(showOp)
			out() << postOp;
	}
	return true;
}
#define OPERATOR(NAME) \
	bool DPrinter::TraverseUnary##NAME(UnaryOperator* Stmt) \
	{if (passStmt(Stmt)) return true; return TraverseUnaryOperator(Stmt);}
OPERATOR(PostInc) OPERATOR(PostDec) OPERATOR(PreInc) OPERATOR(PreDec)
OPERATOR(AddrOf) OPERATOR(Deref) OPERATOR(Plus) OPERATOR(Minus)
OPERATOR(Not) OPERATOR(LNot) OPERATOR(Real) OPERATOR(Imag)
OPERATOR(Extension) OPERATOR(Coawait)
#undef OPERATOR

template<typename TDeclRefExpr>
bool DPrinter::traverseDeclRefExprImpl(TDeclRefExpr* Expr)
{
	if(passStmt(Expr)) return true;
	size_t const argNum = Expr->getNumTemplateArgs();
	if(argNum != 0)
	{
		TemplateArgumentLoc const* tmpArgs = Expr->getTemplateArgs();
		Spliter split(", ");
		pushStream();
		for(size_t i = 0; i < argNum; ++i)
		{
			split.split();
			printTemplateArgument(tmpArgs[i].getArgument());
		}
		printTmpArgList(popStream());
	}

	return true;
}

bool DPrinter::TraverseDeclRefExpr(DeclRefExpr* Expr)
{
	if(passStmt(Expr)) return true;
	QualType nnsQualType;
	if(Expr->hasQualifier())
	{
		NestedNameSpecifier* nns = Expr->getQualifier();
		if(nns->getKind() == NestedNameSpecifier::SpecifierKind::TypeSpec)
			nnsQualType = nns->getAsType()->getCanonicalTypeUnqualified();
		TraverseNestedNameSpecifier(nns);
	}
	auto decl = Expr->getDecl();
	if(decl->getKind() == Decl::Kind::EnumConstant)
	{
		if(nnsQualType != decl->getType().getUnqualifiedType())
		{
			printType(decl->getType());
			out() << '.';
		}
	}
	out() << mangleVar(Expr);
	return traverseDeclRefExprImpl(Expr);
}

bool DPrinter::TraverseDependentScopeDeclRefExpr(DependentScopeDeclRefExpr* expr)
{
	if(passStmt(expr)) return true;
	NestedNameSpecifier* nns = expr->getQualifier();
	TraverseNestedNameSpecifier(nns);
	out() << expr->getDeclName().getAsString();
	return traverseDeclRefExprImpl(expr);
}

bool DPrinter::TraverseRecordType(RecordType* Type)
{
	if(passType(Type)) return false;
	out() << mangleType(Type->getDecl());
	RecordDecl* decl = Type->getDecl();
	switch(decl->getKind())
	{
	case Decl::Kind::Record:	break;
	case Decl::Kind::CXXRecord:	break;
	case Decl::Kind::ClassTemplateSpecialization:
	{
		// Print template arguments in template type of template specialization
		auto* tmpSpec = llvm::dyn_cast<ClassTemplateSpecializationDecl>(decl);
		TemplateArgumentList const& tmpArgsSpec = tmpSpec->getTemplateInstantiationArgs();
		pushStream();
		Spliter spliter2(", ");
		for(unsigned int i = 0, size = tmpArgsSpec.size(); i != size; ++i)
		{
			spliter2.split();
			TemplateArgument const& tmpArg = tmpArgsSpec.get(i);
			printTemplateArgument(tmpArg);
		}
		printTmpArgList(popStream());
		break;
	}
	default: assert(false && "Unconsustent RecordDecl kind");
	}
	return true;
}

bool DPrinter::TraverseConstantArrayType(ConstantArrayType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getElementType());
	out() << '[' << Type->getSize().toString(10, false) << ']';
	return true;
}

bool DPrinter::TraverseIncompleteArrayType(IncompleteArrayType* Type)
{
	if(passType(Type)) return false;
	printType(Type->getElementType());
	out() << "[]";
	return true;
}

bool DPrinter::TraverseInitListExpr(InitListExpr* expr)
{
	if(passStmt(expr)) return true;
	Expr* expr2 = expr->IgnoreImplicit();
	if(expr2 != expr)
		return TraverseStmt(expr2);

	bool isExplicitBracket = true;
	if(expr->getNumInits() == 1)
		isExplicitBracket = not isa<InitListExpr>(expr->getInit(0));

	bool const isArray =
	  (expr->ClassifyLValue(*Context) == Expr::LV_ArrayTemporary);
	if(isExplicitBracket)
		out() << (isArray ? '[' : '{') << " " << std::endl;
	++indent;
	size_t argIndex = 0;
	for(Expr* c : expr->inits())
	{
		++argIndex;
		pushStream();
		TraverseStmt(c);
		std::string const valInit = popStream();
		if(valInit.empty() == false)
		{
			out() << indentStr() << valInit;
			if(isExplicitBracket)
				out() << ',' << std::endl;
		}
		output_enabled = (isInMacro == 0);
	}
	--indent;
	if(isExplicitBracket)
		out() << indentStr() << (isArray ? ']' : '}');
	return true;
}

bool DPrinter::TraverseParenExpr(ParenExpr* expr)
{
	if(passStmt(expr)) return true;
	if(auto* binOp = dyn_cast<BinaryOperator>(expr->getSubExpr()))
	{
		Expr* lhs = binOp->getLHS();
		Expr* rhs = binOp->getRHS();
		StringLiteral const* strLit = dyn_cast<StringLiteral>(lhs);
		if(strLit && (binOp->getOpcode() == BinaryOperatorKind::BO_Comma))
		{
			StringRef const str = strLit->getString();
			if(str == "CPP2D_MACRO_EXPR")
			{
				auto get_binop = [](Expr * paren)
				{
					return dyn_cast<BinaryOperator>(dyn_cast<ParenExpr>(paren)->getSubExpr());
				};
				BinaryOperator* macro_and_cpp = get_binop(rhs);
				BinaryOperator* macro_name_and_args = get_binop(macro_and_cpp->getLHS());
				auto* macro_name = dyn_cast<StringLiteral>(macro_name_and_args->getLHS());
				auto* macro_args = dyn_cast<CallExpr>(macro_name_and_args->getRHS());
				out() << "(mixin(" << macro_name->getString().str() << "!(";
				printMacroArgs(macro_args);
				out() << ")))";
				pushStream();
				TraverseStmt(macro_and_cpp->getRHS()); //Add the required import
				popStream();
				return true;
			}
		}
	}
	out() << '(';
	TraverseStmt(expr->getSubExpr());
	out() << ')';
	return true;
}

bool DPrinter::TraverseImplicitValueInitExpr(ImplicitValueInitExpr* expr)
{
	if(passStmt(expr)) return true;
	return true;
}

bool DPrinter::TraverseParenListExpr(clang::ParenListExpr* expr)
{
	if(passStmt(expr)) return true;
	Spliter split(", ");
	for(Expr* arg : expr->exprs())
	{
		split.split();
		TraverseStmt(arg);
	}
	return true;
}


void DPrinter::traverseVarDeclImpl(VarDecl* Decl)
{
	std::string const varName = Decl->getNameAsString();
	if(varName.find("CPP2D_MACRO_STMT") == 0)
	{
		printStmtMacro(varName, Decl->getInit());
		return;
	}

	if(passDecl(Decl)) return;

	if(Decl->isOutOfLine())
		return;
	else if(Decl->getOutOfLineDefinition())
		Decl = Decl->getOutOfLineDefinition();
	QualType varType = Decl->getType();
	if(doPrintType)
	{
		if(Decl->isStaticDataMember() || Decl->isStaticLocal())
			out() << "static ";
		if(!Decl->isOutOfLine())
		{
			if(auto qualifier = Decl->getQualifier())
				TraverseNestedNameSpecifier(qualifier);
		}
		printType(varType);
		out() << " ";
	}
	out() << mangleName(Decl->getNameAsString());
	bool const in_foreach_decl = inForRangeInit;
	if(Decl->hasInit() && !in_foreach_decl)
	{
		Expr* init = Decl->getInit();
		if(Decl->isDirectInit())
		{
			if(auto* constr = dyn_cast<CXXConstructExpr>(init))
			{
				if(getSemantic(varType) != Reference)
				{
					if(constr->getNumArgs() != 0)
					{
						out() << " = ";
						printCXXConstructExprParams(constr);
					}
				}
				else
				{
					out() << " = new ";
					printCXXConstructExprParams(constr);
				}
			}
			else
			{
				out() << " = ";
				TraverseStmt(init);
			}
		}
		else
		{
			out() << " = ";
			TraverseStmt(init);
		}
	}
}

bool DPrinter::TraverseVarDecl(VarDecl* Decl)
{
	if(passDecl(Decl)) return true;
	traverseVarDeclImpl(Decl);
	return true;
}

bool DPrinter::VisitDecl(Decl* Decl)
{
	out() << indentStr() << "/*" << Decl->getDeclKindName() << " Decl*/";
	return true;
}

bool DPrinter::VisitStmt(Stmt* Stmt)
{
	out() << indentStr() << "/*" << Stmt->getStmtClassName() << " Stmt*/";
	return true;
}

bool DPrinter::VisitType(Type* Type)
{
	out() << indentStr() << "/*" << Type->getTypeClassName() << " Type*/";
	return true;
}

void DPrinter::addExternInclude(std::string const& include, std::string const& typeName)
{
	externIncludes[include].insert(typeName);
}

std::ostream& DPrinter::stream()
{
	return out();
}

std::map<std::string, std::set<std::string> > const& DPrinter::getExternIncludes() const
{
	return externIncludes;
}


std::string DPrinter::getDCode() const
{
	return out().str();
}

bool DPrinter::shouldVisitImplicitCode() const
{
	return true;
}