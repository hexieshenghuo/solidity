/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
#include <libsolidity/lsp/ReferenceCollector.h>

#include <libsolidity/ast/AST.h>
#include <libsolidity/lsp/LanguageServer.h>

using std::move;
using std::vector;
using std::string;

using namespace std::string_literals;
using namespace solidity::frontend;

namespace solidity::lsp
{

ReferenceCollector::ReferenceCollector(
	frontend::Declaration const& _declaration,
	std::string const& _sourceIdentifierName
):
	m_declaration{_declaration},
	m_sourceIdentifierName{_sourceIdentifierName.empty() ? _declaration.name() : _sourceIdentifierName}
{
}

std::vector<::lsp::DocumentHighlight> ReferenceCollector::collect(
	frontend::Declaration const* _declaration,
	frontend::ASTNode const& _ast,
	std::string const& _sourceIdentifierName
)
{
	if (!_declaration)
		return {};

	// TODO if vardecl, just use decl's scope (for lower overhead).
	auto collector = ReferenceCollector(*_declaration, _sourceIdentifierName);
	_ast.accept(collector);
	return collector.take();
}

bool ReferenceCollector::visit(frontend::ImportDirective const& _import)
{
	for (auto const& symbolAlias: _import.symbolAliases())
		if (m_sourceIdentifierName == *symbolAlias.alias)
		{
			addReference(symbolAlias.location);
			return true;
		}

	return visitNode(_import);
}

bool ReferenceCollector::tryAddReference(frontend::Declaration const* _declaration, solidity::langutil::SourceLocation const& _location)
{
	if (&m_declaration != _declaration)
		return false;

	addReference(_location);
	return true;
}

bool ReferenceCollector::visit(frontend::Identifier const& _identifier)
{
	if (auto const* declaration = _identifier.annotation().referencedDeclaration)
		tryAddReference(declaration, _identifier.location());

	for (auto const* declaration: _identifier.annotation().candidateDeclarations)
		tryAddReference(declaration, _identifier.location());

	for (auto const* declaration: _identifier.annotation().overloadedDeclarations)
		tryAddReference(declaration, _identifier.location());

	return visitNode(_identifier);
}

bool ReferenceCollector::visit(frontend::MemberAccess const& _memberAccess)
{
	if (_memberAccess.annotation().referencedDeclaration == &m_declaration)
		addReference(_memberAccess.location());

	return visitNode(_memberAccess);
}

bool ReferenceCollector::visitNode(frontend::ASTNode const& _node)
{
	if (&_node == &m_declaration)
	{
		if (auto const* decl = dynamic_cast<Declaration const*>(&_node))
			addReference(decl->nameLocation());
		else
			addReference(_node.location());
	}

	return true;
}

void ReferenceCollector::addReference(solidity::langutil::SourceLocation const& _location)
{
	auto const [startLine, startColumn] = _location.source->translatePositionToLineColumn(_location.start);
	auto const [endLine, endColumn] = _location.source->translatePositionToLineColumn(_location.end);
	auto locationRange = ::lsp::Range{
		{startLine, startColumn},
		{endLine, endColumn}
	};

	m_result.emplace_back(::lsp::DocumentHighlight{move(locationRange), ::lsp::DocumentHighlightKind::Text});
}

}
