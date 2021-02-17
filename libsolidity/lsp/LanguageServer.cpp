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
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/interface/ReadFile.h>
#include <libsolidity/lsp/LanguageServer.h>
#include <libsolidity/lsp/ReferenceCollector.h>

#include <liblangutil/SourceReferenceExtractor.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/JSON.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <ostream>

#include <iostream>
#include <string>

using solidity::util::URI;

using namespace std;
using namespace std::placeholders;

using namespace solidity::langutil;
using namespace solidity::frontend;

namespace solidity::lsp {

namespace // {{{ helpers
{

// TODO: maybe use SimpleASTVisitor here, if that would be a simple free-fuunction :)
class ASTNodeLocator : public ASTConstVisitor
{
private:
	int m_pos = -1;
	ASTNode const* m_currentNode = nullptr;

public:
	explicit ASTNodeLocator(int _pos): m_pos{_pos}
	{
	}

	ASTNode const* closestMatch() const noexcept { return m_currentNode; }

	bool visitNode(ASTNode const& _node) override
	{
		if (_node.location().start <= m_pos && m_pos <= _node.location().end)
		{
			m_currentNode = &_node;
			return true;
		}
		return false;
	}
};

} // }}} end helpers

LanguageServer::LanguageServer(::lsp::Transport& _client, Logger _logger):
	::lsp::Server(_client, std::move(_logger)),
	m_vfs()
{
}

::lsp::ServerId LanguageServer::initialize(URI _rootUri, vector<::lsp::WorkspaceFolder> _workspaceFolders)
{
	(void) _workspaceFolders; // a list of root directories (usually one).
	if (_rootUri.scheme == "file://"sv)
	{
		auto const fspath = boost::filesystem::path(_rootUri.path);
		m_basePath = fspath;
		m_allowedDirectories.push_back(fspath);
	}

	return {"solc", string(solidity::frontend::VersionNumber)};
}

void LanguageServer::changeConfiguration(Json::Value const& _settings)
{
	if (_settings["evm"].isString())
		if (auto const evmVersionOpt = EVMVersion::fromString(_settings["evm"].asString()); evmVersionOpt.has_value())
			m_evmVersion = evmVersionOpt.value();

	if (_settings["remapping"].isArray())
	{
		for (auto const& element: _settings["remapping"])
		{
			if (element.isString())
			{
				if (auto remappingOpt = CompilerStack::parseRemapping(element.asString()); remappingOpt.has_value())
					m_remappings.emplace_back(move(remappingOpt.value()));
				else
					trace("Failed to parse remapping: '"s + element.asString() + "'");
			}
		}
	}
}

void LanguageServer::initialized()
{
	// NB: this means the client has finished initializing. Now we could maybe start sending
	// events to the client.
	log("LanguageServer: Client initialized");
}

void LanguageServer::documentOpened(URI const& _uri, string _languageId, int _documentVersion, std::string _contents)
{
	log("LanguageServer: Opening document: " + to_string(_uri));

	::lsp::vfs::File const& file = m_vfs.insert(
		_uri,
		_languageId,
		_documentVersion,
		_contents
	);

	validate(file);
}

void LanguageServer::documentContentUpdated(URI const& _uri, std::optional<int> _version, ::lsp::Range _range, std::string const& _text)
{
	// TODO: all this info is actually unrelated to solidity/lsp specifically except knowing that
	// the file has updated, so we can  abstract that away and only do the re-validation here.
	auto file = m_vfs.find(_uri);
	if (!file)
	{
		log("LanguageServer: File to be modified not opened \"" + to_string(_uri) + "\"");
		return;
	}

	if (_version.has_value())
		file->setVersion(_version.value());

#if !defined(NDEBUG)
	ostringstream str;
	str << "did change: " << _range << " for '" << _text << "'";
	trace(str.str());
#endif
	file->modify(_range, _text);

}

void LanguageServer::documentContentUpdated(URI const& _uri)
{
	auto file = m_vfs.find(_uri);
	if (!file)
		log("LanguageServer: File to be modified not opened \"" + to_string(_uri) + "\"");
	else
		validate(*file);
}

void LanguageServer::documentContentUpdated(URI const& _uri, optional<int> _version, string const& _fullContentChange)
{
	auto file = m_vfs.find(_uri);
	if (!file)
	{
		log("LanguageServer: File to be modified not opened \"" + to_string(_uri) + "\"");
		return;
	}

	if (_version.has_value())
		file->setVersion(_version.value());

	file->replace(_fullContentChange);

	validate(*file);
}

void LanguageServer::documentClosed(URI const& _uri)
{
	log("LanguageServer: didClose: " + to_string(_uri));
}

void LanguageServer::validateAll()
{
	for (reference_wrapper<::lsp::vfs::File const> const& file: m_vfs.files())
		validate(file.get());
}

frontend::ReadCallback::Result LanguageServer::readFile(string const& _kind, string const& _path)
{
	return m_fileReader->readFile(_kind, _path);
}

constexpr ::lsp::DiagnosticSeverity toDiagnosticSeverity(Error::Type _errorType)
{
	using Type = Error::Type;
	using Severity = ::lsp::DiagnosticSeverity;
	switch (_errorType)
	{
		case Type::CodeGenerationError:
		case Type::DeclarationError:
		case Type::DocstringParsingError:
		case Type::ParserError:
		case Type::SyntaxError:
		case Type::TypeError:
			return Severity::Error;
		case Type::Warning:
			return Severity::Warning;
	}
	// Should never be reached.
	return Severity::Error;
}

void LanguageServer::compile(::lsp::vfs::File const& _file)
{
	// TODO: optimize! do not recompile if nothing has changed (file(s) not flagged dirty).

	// always start fresh when compiling
	m_sourceCodes.clear();

	m_sourceCodes[_file.uri().path] = _file.contentString();

	m_fileReader = make_unique<FileReader>(m_basePath, m_allowedDirectories);

	m_compilerStack.reset();
	m_compilerStack = make_unique<CompilerStack>(bind(&FileReader::readFile, ref(*m_fileReader), _1, _2));

	// TODO: configure all compiler flags like in CommandLineInterface (TODO: refactor to share logic!)
	OptimiserSettings settings = OptimiserSettings::standard(); // TODO: get from config
	m_compilerStack->setOptimiserSettings(settings);
	m_compilerStack->setParserErrorRecovery(false);
	m_compilerStack->setRevertStringBehaviour(RevertStrings::Default); // TODO get from config
	m_compilerStack->setSources(m_sourceCodes);
	m_compilerStack->setRemappings(m_remappings);

	m_compilerStack->setEVMVersion(m_evmVersion);

	trace("compile: using EVM "s + m_evmVersion.name());

	m_compilerStack->compile();
}

void LanguageServer::validate(::lsp::vfs::File const& _file)
{
	compile(_file);

	std::vector<::lsp::Diagnostic> collectedDiagnostics{};

	for (shared_ptr<Error const> const& error: m_compilerStack->errors())
	{
		// Don't show this warning. "This is a pre-release compiler version."
		if (error->errorId().error == 3805)
			continue;

		auto const message = SourceReferenceExtractor::extract(*error);

		auto const severity = toDiagnosticSeverity(error->type());

		// global warnings don't have positions in the source code - TODO: default them to top of file?

		auto const startPosition = LineColumn{{
			max(message.primary.position.line, 0),
			max(message.primary.startColumn, 0)
		}};

		auto const endPosition = LineColumn{{
			max(message.primary.position.line, 0),
			max(message.primary.endColumn, 0)
		}};

		::lsp::Diagnostic diag{};
		diag.range.start.line = startPosition.line;
		diag.range.start.column = startPosition.column;
		diag.range.end.line = endPosition.line;
		diag.range.end.column = endPosition.column;
		diag.message = message.primary.message;
		diag.source = "solc";
		diag.severity = severity;

		for (SourceReference const& secondary: message.secondary)
		{
			auto related = ::lsp::DiagnosticRelatedInformation{};

			related.message = secondary.message;
			related.location.uri = URI::parse("file://" + secondary.sourceName).value(); // is the sourceName always a fully qualified path?
			related.location.range.start.line = secondary.position.line;
			related.location.range.start.column = secondary.startColumn;
			related.location.range.end.line = secondary.position.line; // what about multiline?
			related.location.range.end.column = secondary.endColumn;

			diag.relatedInformation.emplace_back(move(related));
		}

		if (message.errorId.has_value())
			diag.code = message.errorId.value().error;

		collectedDiagnostics.emplace_back(move(diag));
	}
	optional<int> version = nullopt; // TODO: do we need this? (AFAIK that's been the file version this diagnostics belongs to)
	pushDiagnostics(_file.uri(), version, collectedDiagnostics);
}

frontend::ASTNode const* LanguageServer::findASTNode(::lsp::Position const& _position, std::string const& _fileName)
{
	if (!m_compilerStack)
		return nullptr;

	frontend::ASTNode const& sourceUnit = m_compilerStack->ast(_fileName);
	auto const sourcePos = sourceUnit.location().source->translateLineColumnToPosition(_position.line + 1, _position.column + 1);

	ASTNodeLocator m{sourcePos};
	sourceUnit.accept(m);
	auto const closestMatch = m.closestMatch();

	trace(
		"findASTNode not found for "s +
		to_string(sourcePos) + ":" +
		to_string(_position.line) + ":" +
		to_string(_position.column)
	);

	return closestMatch;
}

std::vector<::lsp::Location> LanguageServer::gotoDefinition(::lsp::DocumentPosition _location)
{
	auto const file = m_vfs.find(_location.uri);
	if (!file)
	{
		// error(_params.requestId, ErrorCode::InvalidRequest, "File not found in VFS.");
		return {};
	}

	// source should be compiled already
	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceName = file->uri().path;
	auto const sourceNode = findASTNode(_location.position, sourceName);
	if (!sourceNode)
	{
		trace("Could not infer AST node from given source location.");
		return {};
	}

	if (auto const importDirective = dynamic_cast<ImportDirective const*>(sourceNode))
	{
		// When cursor is on an import directive, then we want to jump to the actual file that
		// is being imported.
		auto const fpm = m_fileReader->fullPathMapping().find(importDirective->path());
		if (fpm == m_fileReader->fullPathMapping().end())
		{
			trace("gotoDefinition: (importDirective) full path mapping not found\n");
			return {}; // definition not found
		}

		::lsp::Location output{};
		output.uri = URI::parse("file://" + fpm->second).value();
		return {output};
	}
	else if (auto const n = dynamic_cast<frontend::MemberAccess const*>(sourceNode))
	{
		// For scope members, jump to the naming symbol of the referencing declaration of this member.
		auto const declaration = n->annotation().referencedDeclaration;

		auto const location = declarationPosition(declaration);
		if (!location.has_value())
		{
			trace("gotoDefinition: declaration not found.");
			return {}; // definition not found
		}

		return {location.value()};
	}
	else if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		// For identifiers, jump to the naming symbol of the definition of this identifier.
		vector<::lsp::Location> output;

		if (auto location = declarationPosition(sourceIdentifier->annotation().referencedDeclaration); location.has_value())
			output.emplace_back(move(location.value()));

		for (auto const declaration: sourceIdentifier->annotation().candidateDeclarations)
			if (auto location = declarationPosition(declaration); location.has_value())
				output.emplace_back(move(location.value()));

		return output;
	}
	else
	{
		trace("gotoDefinition: Symbol is not an identifier. "s + typeid(*sourceIdentifier).name());
		return {};
	}
}

optional<::lsp::Location> LanguageServer::declarationPosition(frontend::Declaration const* _declaration)
{
	if (!_declaration)
		return nullopt;

	auto const location = _declaration->nameLocation();
	auto const [startLine, startColumn] = location.source->translatePositionToLineColumn(location.start);
	auto const [endLine, endColumn] = location.source->translatePositionToLineColumn(location.end);

	auto const sourceName = _declaration->location().source->name();

	auto output = ::lsp::Location{};
	if (auto fullPath = m_fileReader->fullPathMapping().find(sourceName); fullPath != m_fileReader->fullPathMapping().end())
		output.uri = URI::parse("file://" + fullPath->second).value();
	else
		output.uri = URI::parse("file://" + sourceName).value();

	output.range = {
		{startLine, startColumn},
		{endLine, endColumn}
	};

	return output;
}

std::vector<::lsp::DocumentHighlight> LanguageServer::findAllReferences(
	frontend::Declaration const* _declaration,
	string const& _sourceIdentifierName,
	SourceUnit const& _sourceUnit
)
{
	if (!_declaration)
		return {};

	// The SourceUnit should be the root scope unless we're looking for simple variable identifier.

	// TODO if vardecl, just use decl's scope (for lower overhead).
	return ReferenceCollector::collect(*_declaration, _sourceUnit, _sourceIdentifierName);
}

void LanguageServer::findAllReferences(
	frontend::Declaration const* _declaration,
	string const& _sourceIdentifierName,
	frontend::SourceUnit const& _sourceUnit,
	URI const& _sourceUnitUri,
	std::vector<::lsp::Location>& _output
)
{
	for (auto const& highlight: findAllReferences(_declaration, _sourceIdentifierName, _sourceUnit))
	{
		auto location = ::lsp::Location{};
		location.range = highlight.range;
		location.uri = _sourceUnitUri;
		_output.emplace_back(location);
	}
}


vector<::lsp::Location> LanguageServer::references(::lsp::DocumentPosition _documentPosition)
{
	trace(
		"find all references: "s +
		to_string(_documentPosition.uri) + ":" +
		to_string(_documentPosition.position.line) + ":" +
		to_string(_documentPosition.position.column)
	);

	auto const file = m_vfs.find(_documentPosition.uri);
	if (!file)
	{
		trace("File does not exist. "s + to_string(_documentPosition.uri));
		return {};
	}

	if (!m_compilerStack)
		compile(*file);

	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceName = file->uri().path;

	auto const sourceNode = findASTNode(_documentPosition.position, sourceName);
	if (!sourceNode)
	{
		trace("AST node not found");
		return {};
	}

	auto output = vector<::lsp::Location>{};
	if (auto const sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.uri.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

		if (auto decl = sourceIdentifier->annotation().referencedDeclaration)
			findAllReferences(decl, sourceUnit, _documentPosition.uri, output);
		else
			trace("references: referencedDeclaration == NULL");

		for (auto const decl: sourceIdentifier->annotation().candidateDeclarations)
			findAllReferences(decl, sourceUnit, _documentPosition.uri, output);
	}
	else if (auto const varDecl = dynamic_cast<VariableDeclaration const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.uri.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		findAllReferences(varDecl, sourceUnit, _documentPosition.uri, output);
	}
	else if (auto const memberAccess = dynamic_cast<MemberAccess const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.uri.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		findAllReferences(varDecl, sourceUnit, _documentPosition.uri, output);
	}
	else
		trace("references: not an identifier");

	return output;
}

vector<::lsp::DocumentHighlight> LanguageServer::semanticHighlight(::lsp::DocumentPosition _documentPosition)
{
	auto const file = m_vfs.find(_documentPosition.uri);
	if (!file)
	{
		// reply(_params.requestId, output);
		// error(_documentPosition.requestId, ErrorCode::RequestCancelled, "not implemented yet.");
		return {};
	}

	compile(*file);
	solAssert(m_compilerStack.get() != nullptr, "");

	auto const sourceName = file->uri().path;

	auto const sourceNode = findASTNode(_documentPosition.position, sourceName);
	if (!sourceNode)
	{
		trace("semanticHighlight: AST node not found");
		// error(_documentPosition.requestId, ErrorCode::InvalidParams, "Symbol not found.");
		return {};
	}

	trace(
		"semanticHighlight: Source Node("s + typeid(*sourceNode).name() + "): " +
		sourceNode->location().text()
	);

	auto output = vector<::lsp::DocumentHighlight>{};

	// TODO: ImportDirective: hovering a symbol of an import directive should highlight all uses of that symbol.
	if (auto const* sourceIdentifier = dynamic_cast<Identifier const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.uri.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

		vector<::lsp::DocumentHighlight> output;

		if (sourceIdentifier->annotation().referencedDeclaration)
			output += findAllReferences(sourceIdentifier->annotation().referencedDeclaration, sourceIdentifier->name(), sourceUnit);

		for (Declaration const* declaration: sourceIdentifier->annotation().candidateDeclarations)
			output += findAllReferences(declaration, sourceIdentifier->name(), sourceUnit);

		for (Declaration const* declaration: sourceIdentifier->annotation().overloadedDeclarations)
			output += findAllReferences(declaration, sourceIdentifier->name(), sourceUnit);

		return output;
	}
	else if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(sourceNode))
	{
		auto const sourceName = _documentPosition.uri.path;
		frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		output = findAllReferences(varDecl, sourceUnit);
	}
	else if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(sourceNode))
	{
		TypePointer const type = memberAccess->expression().annotation().type;
		if (auto const ttype = dynamic_cast<TypeType const*>(type))
		{
			auto const memberName = memberAccess->memberName();

			auto const sourceName = _documentPosition.uri.path;
			frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);

			if (auto const* enumType = dynamic_cast<EnumType const*>(ttype->actualType()))
			{
				auto const& enumMembers = enumType->enumDefinition().members();
				if (enumMembers.empty())
					trace("enumType members are empty");

				// find the definition
				for (auto const& enumMember: enumMembers)
					if (enumMember->name() == memberName)
						output += findAllReferences(enumMember.get(), sourceUnit);

				// find uses of the enum value
			}
			else
				trace("semanticHighlight: not an EnumType");
		}
		else
			trace("semanticHighlight: member type is NULL");

		// TODO: If the cursor os positioned on top of a type name, then all other symbols matching
		// this type should be highlighted (clangd does so, too).
		//
		// if (auto const tt = dynamic_cast<TypeType const*>(type))
		// {
		// 	auto const sourceName = _documentPosition.uri.substr(7); // strip "file://"
		// 	frontend::SourceUnit const& sourceUnit = m_compilerStack->ast(sourceName);
		// 	output = findAllReferences(declaration, sourceUnit);
		// }
	}
	else
		trace("semanticHighlight: not an identifier");

	return output;
}

} // namespace solidity
