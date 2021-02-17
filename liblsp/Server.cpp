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
#include <liblsp/Server.h>
#include <liblsp/Transport.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/JSON.h>

#include <functional>
#include <ostream>
#include <string>

#include <boost/algorithm/string/predicate.hpp>

using namespace std;
using namespace std::placeholders;
using namespace std::string_literals;

namespace lsp {

namespace
{
	optional<string> extractPathFromFileURI(std::string const& _uri)
	{
		if (!boost::starts_with(_uri, "file://"))
			return nullopt;

		return _uri.substr(7);
	}

	string toFileURI(std::string const& _path)
	{
		return "file://" + _path;
	}

	void loadTextDocumentPosition(DocumentPosition& _params, Json::Value const& _json)
	{
		_params.path = extractPathFromFileURI(_json["textDocument"]["uri"].asString()).value();
		_params.position.line = _json["position"]["line"].asInt();
		_params.position.column = _json["position"]["character"].asInt();
	}

	Json::Value toJson(Range const& _range)
	{
		Json::Value json;
		json["start"]["line"] = _range.start.line;
		json["start"]["character"] = _range.start.column;
		json["end"]["line"] = _range.end.line;
		json["end"]["character"] = _range.end.column;
		return json;
	}
}

// LSP specification can be found at:
//
// https://microsoft.github.io/language-server-protocol/specifications/specification-current/

Server::Server(Transport& _client, std::function<void(std::string_view)> _logger):
	m_client{_client},
	m_handlers{
		{"cancelRequest", [](auto, auto) {/*don't do anything for now, as we're synchronous*/}},
		{"initialize", bind(&Server::handle_initializeRequest, this, _1, _2)},
		{"initialized", [this](auto, auto) { initialized(); }},
		{"shutdown", [this](auto, auto) { m_shutdownRequested = true; }},
		{"workspace/didChangeConfiguration", bind(&Server::handle_workspace_didChangeConfiguration, this, _1, _2)},
		{"textDocument/didOpen", bind(&Server::handle_textDocument_didOpen, this, _1, _2)},
		{"textDocument/didChange", bind(&Server::handle_textDocument_didChange, this, _1, _2)},
		{"textDocument/didClose", bind(&Server::handle_textDocument_didClose, this, _1, _2)},
		{"textDocument/definition", bind(&Server::handle_textDocument_definition, this, _1, _2)},
		{"textDocument/documentHighlight", bind(&Server::handle_textDocument_highlight, this, _1, _2)},
		{"textDocument/references", bind(&Server::handle_textDocument_references, this, _1, _2)},
	},
	m_logger{std::move(_logger)}
{
}

bool Server::run()
{
	while (!m_exitRequested && !m_client.closed())
	{
		// TODO: receive() must return a variant<> to also return on <Transport::TimeoutEvent>,
		// so that we can perform some idle tasks in the meantime, such as
		// - lazy validation runs
		// - check for results of asynchronous runs (in case we want to support threaded background jobs)
		// Also, EOF should be noted properly as a <Transport::ClosedEvent>.
		optional<Json::Value> const jsonMessage = m_client.receive();
		if (jsonMessage.has_value())
		{
			try
			{
				handleMessage(*jsonMessage);
			}
			catch (std::exception const& e)
			{
				log("Unhandled exception caught when handling message. "s + e.what());
			}
		}
		else
			log("Could not read RPC request.");
	}

	if (m_shutdownRequested)
		return true;
	else
		return false;
}

void Server::handleMessage(Json::Value const& _jsonMessage)
{
	string const methodName = _jsonMessage["method"].asString();

	MessageId const id = _jsonMessage["id"].isInt()
		? MessageId{_jsonMessage["id"].asInt()}
		: _jsonMessage["id"].isString()
			? MessageId{_jsonMessage["id"].asString()}
			: MessageId{};

	if (auto const handler = m_handlers.find(methodName); handler != m_handlers.end())
	{
		Json::Value const& jsonArgs = _jsonMessage["params"];
		handler->second(id, jsonArgs);
	}
	else
		error(id, ErrorCode::MethodNotFound, "Unknown method " + methodName);
}

void Server::handle_initializeRequest(MessageId _id, Json::Value const& _args)
{
	string rootPath;
	if (Json::Value uri = _args["rootUri"])
		rootPath = extractPathFromFileURI(uri.asString()).value();
	else if (Json::Value rootPath = _args["rootPath"]; rootPath)
		rootPath = rootPath.asString();

	if (Json::Value value = _args["trace"]; value)
	{
		string const name = value.asString();
		if (name == "messages")
			m_trace = Trace::Messages;
		else if (name == "verbose")
			m_trace = Trace::Verbose;
		else if (name == "off")
			m_trace = Trace::Off;
	}

	std::vector<WorkspaceFolder> workspaceFolders; // initial configured workspace folders
	if (Json::Value folders = _args["workspaceFolders"]; folders)
	{
		for (Json::Value folder: folders)
		{
			WorkspaceFolder wsFolder{};
			wsFolder.name = folder["name"].asString();
			wsFolder.path = extractPathFromFileURI(folder["uri"].asString()).value();
			workspaceFolders.emplace_back(move(wsFolder));
		}
	}

	// TODO: ClientCapabilities
	// ... Do we actually care? Not in the initial PR.

	auto const info = initialize(move(rootPath), move(workspaceFolders));

	if (_args["initializationOptions"].isObject())
		changeConfiguration(_args["initializationOptions"]);

	// {{{ encoding
	Json::Value replyArgs;

	if (!info.serverName.empty())
		replyArgs["serverInfo"]["name"] = info.serverName;

	if (!info.serverVersion.empty())
		replyArgs["serverInfo"]["version"] = info.serverVersion;

	replyArgs["hoverProvider"] = true;
	replyArgs["capabilities"]["hoverProvider"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["openClose"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["change"] = 2; // 0=none, 1=full, 2=incremental
	replyArgs["capabilities"]["definitionProvider"] = true;
	replyArgs["capabilities"]["documentHighlightProvider"] = true;
	replyArgs["capabilities"]["referencesProvider"] = true;

	m_client.reply(_id, replyArgs);
	// }}}
}

void Server::handle_workspace_didChangeConfiguration(MessageId, Json::Value const& _args)
{
	if (_args["settings"].isObject())
		changeConfiguration(_args["settings"]);
}

void Server::handle_exit(MessageId _id, Json::Value const& /*_args*/)
{
	m_exitRequested = true;
	auto const exitCode = m_shutdownRequested ? 0 : 1;

	Json::Value replyArgs = Json::intValue;
	replyArgs = exitCode;

	m_client.reply(_id, replyArgs);
}

void Server::handle_textDocument_didOpen(MessageId /*_id*/, Json::Value const& _args)
{
	// decoding
	if (!_args["textDocument"])
		return;

	auto const path = extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value();
	auto const languageId = _args["textDocument"]["languageId"].asString();
	auto const version = _args["textDocument"]["version"].asInt();
	auto const text = _args["textDocument"]["text"].asString();

	documentOpened(path, languageId, version, text);

	// no encoding
}

void Server::handle_textDocument_didChange(MessageId /*_id*/, Json::Value const& _args)
{
	auto const version = _args["textDocument"]["version"].asInt();
	auto const path = extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value();

	// TODO: in the longer run, I'd like to try moving the VFS handling into Server class, so
	// the specific Solidity LSP implementation doesn't need to care about that.

	auto const contentChanges = _args["contentChanges"];
	for (Json::Value jsonContentChange: contentChanges)
	{
		if (!jsonContentChange.isObject())
			// Protocol error, will only happen on broken clients, so silently ignore it.
			continue;

		auto const text = jsonContentChange["text"].asString();

		if (jsonContentChange["range"].isObject())
		{
			Json::Value jsonRange = jsonContentChange["range"];
			Range range{};
			range.start.line = jsonRange["start"]["line"].asInt();
			range.start.column = jsonRange["start"]["character"].asInt();
			range.end.line = jsonRange["end"]["line"].asInt();
			range.end.column = jsonRange["end"]["character"].asInt();

			documentContentUpdated(path, version, range, text);
		}
		else
		{
			// full content update
			documentContentUpdated(path, version, text);
		}
	}

	if (!contentChanges.empty())
		documentContentUpdated(path); // tell LSP impl we're done with content updates.
}

void Server::handle_textDocument_didClose(MessageId /*_id*/, Json::Value const& _args)
{
	documentClosed(extractPathFromFileURI(_args["textDocument"]["uri"].asString()).value());
}

void Server::handle_textDocument_definition(MessageId _id, Json::Value const& _args)
{
	DocumentPosition dpos{};
	loadTextDocumentPosition(dpos, _args);

	Json::Value reply = Json::arrayValue;
	for (::lsp::Location const& target: gotoDefinition(dpos))
	{
		Json::Value json = Json::objectValue;
		json["uri"] = toFileURI(target.path);
		json["range"]["start"]["line"] = target.range.start.line;
		json["range"]["start"]["character"] = target.range.start.column;
		json["range"]["end"]["line"] = target.range.end.line;
		json["range"]["end"]["character"] = target.range.end.column;
		reply.append(json);
	}
	m_client.reply(_id, reply);
}

void Server::handle_textDocument_highlight(MessageId _id, Json::Value const& _args)
{
	DocumentPosition dpos{};
	loadTextDocumentPosition(dpos, _args);

	Json::Value replyArgs = Json::arrayValue;
	for (DocumentHighlight const& highlight: semanticHighlight(dpos))
	{
		Json::Value item = Json::objectValue;

		item["range"]["start"]["line"] = highlight.range.start.line;
		item["range"]["start"]["character"] = highlight.range.start.column;
		item["range"]["end"]["line"] = highlight.range.end.line;
		item["range"]["end"]["character"] = highlight.range.end.column;

		if (highlight.kind != DocumentHighlightKind::Unspecified)
			item["kind"] = static_cast<int>(highlight.kind);

		replyArgs.append(item);
	}
	m_client.reply(_id, replyArgs);
}

void Server::handle_textDocument_references(MessageId _id, Json::Value const& _args)
{
	DocumentPosition dpos{};
	loadTextDocumentPosition(dpos, _args);

	Json::Value replyArgs = Json::arrayValue;
	for (Location const& location: references(dpos))
	{
		Json::Value item = Json::objectValue;

		item["range"]["start"]["line"] = location.range.start.line;
		item["range"]["start"]["character"] = location.range.start.column;
		item["range"]["end"]["line"] = location.range.end.line;
		item["range"]["end"]["character"] = location.range.end.column;
		item["uri"] = toFileURI(location.path);

		replyArgs.append(item);
	}
	m_client.reply(_id, replyArgs);
}

void Server::error(MessageId const& _id, ErrorCode _code, string  const& _message)
{
	m_client.error(_id, _code, _message);
}

void Server::log(string const& _message)
{
	if (m_trace < Trace::Messages)
		return;

	Json::Value json = Json::objectValue;
	json["type"] = static_cast<int>(Trace::Messages);
	json["message"] = _message;

	m_client.notify("window/logMessage", json);

	if (m_logger)
		m_logger(_message);
}

void Server::trace(string const& _message)
{
	if (m_trace < Trace::Verbose)
		return;

	Json::Value json = Json::objectValue;
	json["type"] = static_cast<int>(Trace::Verbose);
	json["message"] = _message;

	m_client.notify("window/logMessage", json);

	if (m_logger)
		m_logger(_message);
}

void Server::pushDiagnostics(string const& _path, optional<int> _version, vector<Diagnostic> const& _diagnostics)
{
	Json::Value params;

	params["uri"] = toFileURI(_path);

	if (_version)
		params["version"] = _version.value();

	params["diagnostics"] = Json::arrayValue;
	for (Diagnostic const& diag: _diagnostics)
	{
		Json::Value jsonDiag;

		jsonDiag["range"] = toJson(diag.range);

		if (diag.severity.has_value())
			jsonDiag["severity"] = static_cast<int>(diag.severity.value());

		if (diag.code.has_value())
			jsonDiag["code"] = Json::UInt64{diag.code.value()};

		if (diag.source.has_value())
			jsonDiag["source"] = diag.source.value();

		jsonDiag["message"] = diag.message;

		if (!diag.diagnosticTag.empty())
			for (DiagnosticTag tag: diag.diagnosticTag)
				jsonDiag["diagnosticTag"].append(static_cast<int>(tag));

		if (!diag.relatedInformation.empty())
		{
			for (DiagnosticRelatedInformation const& related: diag.relatedInformation)
			{
				Json::Value json;
				json["message"] = related.message;
				json["location"]["uri"] = toFileURI(related.location.path);
				json["location"]["range"] = toJson(related.location.range);
				jsonDiag["relatedInformation"].append(json);
			}
		}

		params["diagnostics"].append(jsonDiag);
	}

	m_client.notify("textDocument/publishDiagnostics", params);
}

} // end namespace
