#include "win_http_client.h"
#include "common.h"
#include "win_http_api.h"
#include <windows.h>
#include <winhttp.h>
#include <urlmon.h>
#include <string>
#include <memory>
#include <sstream>
#include <regex>

#define HTTP_VER L"HTTP/1.1"
#define ACCEPT_TYPES L"*/*"

static win_http_api winHttpApi;

namespace mixer_internal
{

void
hinternet_deleter::operator()(void* internet)
{	
	winHttpApi.win_http_close_handle(internet);
}

std::string GetDebugError(DWORD errorCode)
{
	PSTR errorText = nullptr;
	FormatMessageA(
		// use system message tables to retrieve error text
		FORMAT_MESSAGE_FROM_SYSTEM
		// allocate buffer on local heap for error text
		| FORMAT_MESSAGE_ALLOCATE_BUFFER
		// Important! will fail otherwise, since we're not 
		// (and CANNOT) pass insertion parameters
		| FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,    // unused with FORMAT_MESSAGE_FROM_SYSTEM
		errorCode,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(PSTR)&errorText,  // output 
		0, // minimum size for output buffer
		NULL);   // arguments - see note 
	std::string error(errorText);
	LocalFree(errorText);
	return error;
}

win_http_client::win_http_client()
{
	m_internet.reset(winHttpApi.win_http_open(L"Mixer C++ SDK - Windows", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
};

win_http_client::~win_http_client()
{
}

int win_http_client::make_request(const std::string& uri, const std::string& verb, const http_headers* headers, const std::string& body, _Out_ http_response& response, unsigned long timeoutMs) const
{
	DEBUG_TRACE(verb + " " + uri + " " + body);
	// Crack the URI.
	// Parse the url with regex in accordance with RFC 3986.
	std::regex url_regex(R"(^(([^:/?#]+):)?(//([^:/?#]*):?([0-9]*)?)?([^?#]*)(\?([^#]*))?(#(.*))?)", std::regex::ECMAScript);
	std::smatch url_match_result;

	if (!std::regex_match(uri, url_match_result, url_regex))
	{
		return E_INVALIDARG;
	}

	std::string protocol = url_match_result[2];
	std::string host = url_match_result[4];
	std::string port = url_match_result[5];
	std::string path = url_match_result[6];

	if (port.empty())
	{
		if (0 == protocol.compare("https"))
		{
			port = "443";
		}
		else if (0 == protocol.compare("http"))
		{
			port = "80";
		}
		else
		{
			return E_INVALIDARG;
		}
	}

	// Determine if there is already a session open for this hostname.
	HINTERNET session;
	auto search = m_sessionsByHostname.find(host);
	if (search != m_sessionsByHostname.end())
	{
		session = search->second.get();
	}
	else
	{
		// No cached session, establish a new one.
		session = winHttpApi.win_http_connect(m_internet.get(), utf8_to_wstring(host).c_str(), (INTERNET_PORT)atoi(port.c_str()), 0);
		if (!session)
		{
			return GetLastError();
		}

		// Cache the session to this hostname for future use.
		m_sessionsByHostname[host].reset(session);
	}

	// Session established, make the request.
	const wchar_t* acceptTypes[] = { ACCEPT_TYPES, NULL };
	HINTERNET hRequest = winHttpApi.win_http_open_request(session, utf8_to_wstring(verb).c_str(), utf8_to_wstring(path).c_str(), HTTP_VER, nullptr, acceptTypes, 0 == protocol.compare("https") ? WINHTTP_FLAG_SECURE : 0);
	if (!hRequest)
	{
		return GetLastError();
	}

	hinternet_ptr request(hRequest);

	if (nullptr != headers)
	{
		for (const auto& header : *headers)
		{
			std::wstring str = utf8_to_wstring(header.first + ": " + header.second);
			if (!winHttpApi.win_http_add_request_headers(request.get(), str.c_str(), (DWORD)str.length(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
			{
				return GetLastError();
			}
		}
	}

	if (!winHttpApi.win_http_set_timeouts(request.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs))
	{
		return GetLastError();
	}
	
	if (!winHttpApi.win_http_send_request(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void*)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0))
	{
		return GetLastError();
	}

	if (!winHttpApi.win_http_receive_response(request.get(), nullptr))
	{
		return GetLastError();
	}

	DWORD httpStatus = 0;
	DWORD httpStatusSize = sizeof(httpStatus);

	if (!winHttpApi.win_http_query_headers(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &httpStatus, &httpStatusSize, nullptr))
	{
		return GetLastError();
	}

	std::stringstream responseStream;
	char buffer[1024];
	memset(buffer, 0, ARRAYSIZE(buffer));
	DWORD bytesRead = 0;
	while (winHttpApi.win_http_read_data(request.get(), buffer, ARRAYSIZE(buffer) - 1, &bytesRead) && bytesRead)
	{
		buffer[bytesRead] = '\0';
		responseStream << buffer;
		bytesRead = 0;
		memset(buffer, 0, ARRAYSIZE(buffer));
	}

	response.statusCode = httpStatus;
	response.body = responseStream.str();

	return 0;
}

}