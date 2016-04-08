#include "MatchContainer.h"

#include <iostream>

using namespace clang;

clang::ast_matchers::MatchFinder MatchContainer::getMatcher()
{
	using namespace clang::ast_matchers;
	MatchFinder finder;


	DeclarationMatcher hash_trait =
	  namespaceDecl(
	    allOf(
	      hasName("std"),
	      hasDescendant(
	        classTemplateSpecializationDecl(
	          allOf(
	            templateArgumentCountIs(1),
	            hasName("hash"),
	            hasMethod(cxxMethodDecl(hasName("operator()")).bind("hash_method"))
	          )
	        ).bind("dont_print_this_decl")
	      )
	    )
	  );
	finder.addMatcher(hash_trait, this);
	declPrinters.emplace("dont_print_this_decl", [this](Decl const*) {});
	on_decl_match.emplace("hash_method", [this](Decl const * d)
	{
		if(auto* methDecl = dyn_cast<CXXMethodDecl>(d))
		{
			auto* tmplClass = dyn_cast<ClassTemplateSpecializationDecl>(methDecl->getParent());
			TemplateArgumentList const& tmpArgs = tmplClass->getTemplateArgs();
			if(tmpArgs.size() == 1)
			{
				auto const type_name = tmpArgs[0].getAsType().getCanonicalType().getAsString();
				hash_traits.emplace(type_name, methDecl);
			}
		}
	});


	DeclarationMatcher out_stream_op =
	  functionDecl(
	    unless(hasDeclContext(recordDecl())),
	    matchesName("operator[\\+-\\*\\^\\[\\(\\!\\&\\|\\~\\=\\/\\%\\<\\>]")
	  ).bind("free_operator");
	finder.addMatcher(out_stream_op, this);
	declPrinters.emplace("free_operator", [this](Decl const*) {});
	on_decl_match.emplace("free_operator", [this](Decl const * d)
	{
		if(auto* funcDecl = dyn_cast<FunctionDecl>(d))
		{
			auto getParamTypeName = [](ParmVarDecl const * typeParam)
			{
				QualType canType = typeParam->getType().getCanonicalType()
				                   .getUnqualifiedType().getNonReferenceType();
				canType.removeLocalConst();
				return canType.getAsString();
			};

			if(funcDecl->getNumParams() > 0)
			{
				std::string const left_name = getParamTypeName(funcDecl->getParamDecl(0));
				free_operator.emplace(left_name, funcDecl);
				if(funcDecl->getNumParams() > 1)
				{
					std::string const right_name = getParamTypeName(funcDecl->getParamDecl(1));
					if(right_name != left_name)
						free_operator_right.emplace(right_name, funcDecl);
				}
			}
		}
	});

	// printf
	//finder.addMatcher(declRefExpr(hasDeclaration(functionDecl(hasName("::printf")))).bind("printf"), this);
	//stmtPrinters.emplace("printf", [this](Stmt const*) {llvm::errs() << "printf!"; });

	return finder;
}

void MatchContainer::run(const ast_matchers::MatchFinder::MatchResult& Result)
{
	// To call printers during the D print
	for(auto const& tag_n_func : typePrinters)
	{
		if(auto* type = Result.Nodes.getNodeAs<Type>(tag_n_func.first))
			typeTags.emplace(type, tag_n_func.first);
	}
	for(auto const& tag_n_func : stmtPrinters)
	{
		if(auto* stmt = Result.Nodes.getNodeAs<Stmt>(tag_n_func.first))
			stmtTags.emplace(stmt, tag_n_func.first);
	}
	for(auto const& tag_n_func : declPrinters)
	{
		if(auto* decl = Result.Nodes.getNodeAs<Decl>(tag_n_func.first))
			declTags.emplace(decl, tag_n_func.first);
	}

	// To call a special handling now
	for(auto const& name_n_func : on_decl_match)
	{
		if(auto* decl = Result.Nodes.getNodeAs<Decl>(name_n_func.first))
			name_n_func.second(decl);
	}
	for(auto const& name_n_func : on_stmt_match)
	{
		if(auto* stmt = Result.Nodes.getNodeAs<Stmt>(name_n_func.first))
			name_n_func.second(stmt);
	}
	for(auto const& name_n_func : on_type_match)
	{
		if(auto* type = Result.Nodes.getNodeAs<Type>(name_n_func.first))
			name_n_func.second(type);
	}
}


std::function<void(clang::Stmt const*)> MatchContainer::getPrinter(clang::Stmt const* node) const
{
	auto iter_pair = stmtTags.equal_range(node);
	if(iter_pair.first != iter_pair.second)
	{
		auto iter2 = stmtPrinters.find(iter_pair.first->second);
		if(iter2 != stmtPrinters.end())
			return iter2->second;
	}

	return std::function<void(clang::Stmt const*)>();
}

std::function<void(clang::Decl const*)> MatchContainer::getPrinter(clang::Decl const* node) const
{
	auto iter_pair = declTags.equal_range(node);
	if(iter_pair.first != iter_pair.second)
	{
		auto iter2 = declPrinters.find(iter_pair.first->second);
		if(iter2 != declPrinters.end())
			return iter2->second;
	}

	return std::function<void(clang::Decl const*)>();
}

std::function<void(clang::Type const*)> MatchContainer::getPrinter(clang::Type const* node) const
{
	auto iter_pair = typeTags.equal_range(node);
	if(iter_pair.first != iter_pair.second)
	{
		auto iter2 = typePrinters.find(iter_pair.first->second);
		if(iter2 != typePrinters.end())
			return iter2->second;
	}

	return std::function<void(clang::Type const*)>();
}
