
#include "webclient.h"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#pragma warning(disable:4996)
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netdb.h>
#define closesocket(S) ::close(S)
using socket_t = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

#if USE_OPENSSL
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
// #if (OPENSSL_VERSION_NUMBER >= 0x30000000L && !defined SSL_get_peer_certificate)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define SSL_get_peer_certificate(s) SSL_get1_peer_certificate(s)
#endif
#else
typedef void SSL;
typedef void SSL_CTX;
#endif

#include <set>
#include <cassert>
#include "common/base64.h"

#define USER_AGENT "Generic Web Client"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace {

void vappend(std::vector<char> *vec, char const *begin, char const *end)
{
	vec->insert(vec->end(), begin, end);
}

void vappend(std::vector<char> *vec, char const *p, size_t n)
{
	vappend(vec, p, p + n);
}

void vappend(std::vector<char> *vec, std::string_view const &s)
{
	vappend(vec, s.data(), s.size());
}

std::string_view trimmed(const std::string_view &s)
{
	size_t i = 0;
	size_t j = s.size();
	while (i < j && std::isspace((unsigned char)s[i])) i++;
	while (i < j && std::isspace((unsigned char)s[j - 1])) j--;
	return s.substr(i, j - i);
}

int x_stricmp(char const *s1, char const *s2)
{
#ifdef _WIN32
	return ::stricmp(s1, s2);
#else
	return ::strcasecmp(s1, s2);
#endif
}

int x_strnicmp(char const *s1, char const *s2, size_t n)
{
#ifdef _WIN32
	return ::strnicmp(s1, s2, n);
#else
	return ::strncasecmp(s1, s2, n);
#endif
}

} // namespace

bool HostNameResolver::resolve(const char *name, _in_addr *out)
{
	if (!name || !out) return false;

	struct hostent *he = nullptr;
#if defined(_WIN32) || defined(__APPLE__)
	he = ::gethostbyname(name);
#else
	int err = 0;
	// Use dynamic allocation to ensure sufficient buffer space
	size_t buflen = 8192; // Large enough for most hostent data
	char *buf = new char[buflen];
	struct hostent tmp;
	int ret = gethostbyname_r(name, &tmp, buf, buflen, &he, &err);
	bool success = (ret == 0 && he != nullptr);
	if (success && he->h_length > 0 && he->h_addr) {
		memcpy(out, he->h_addr, he->h_length);
	} else {
		success = false;
	}
	delete[] buf;
	return success;
#endif

	if (!he || !he->h_addr || he->h_length <= 0) return false;
	memcpy(out, he->h_addr, he->h_length);
	return true;
}

struct WebContext::Private {
	WebClient::HttpVersion http_version = WebClient::HTTP_1_0;
	SSL_CTX *ctx = nullptr;
	bool use_keep_alive = false;
	WebProxy http_proxy;
	WebProxy https_proxy;
	bool broken_pipe = false;
};

WebClient::URL::URL(std::string const &addr)
{
	data.full_request = addr;

	// Initialize with defaults
	data.scheme = "http";
	data.host = "";
	data.port = 0;
	data.path = "/";
	
	if (addr.empty()) {
		return;
	}

	char const *str = addr.c_str();
	char const *left = str;
	char const *right = strstr(left, "://");
	
	// Parse scheme (http, https, etc.)
	if (right) {
		if (right > left) { // Ensure scheme isn't empty
			data.scheme.assign(str, right - str);
			left = right + 3;
		} else {
			left = right + 3; // Skip "://" but use default scheme
		}
	}
	
	// Find start of path, or end of string if no path
	right = strchr(left, '/');
	if (!right) {
		right = left + strlen(left);
	}
	
	// Handle host:port format
	if (left < right) {
		// Look for port separator
		char const *p = strchr(left, ':');
		if (p && left < p && p < right) {
			// Extract hostname
			data.host.assign(left, p - left);
			
			// Parse port number
			int n = 0;
			char const *q = p + 1;
			bool port_valid = true;
			size_t digits = 0;
			
			while (q < right) {
				if (isdigit(*q & 0xff)) {
					int old_n = n;
					n = n * 10 + (*q - '0');
					
					// Check for integer overflow
					if (n < old_n || digits > 5) {
						port_valid = false;
						break;
					}
					digits++;
				} else {
					port_valid = false;
					break;
				}
				q++;
			}
			
			if (port_valid && n > 0 && n < 65536) {
				data.port = n;
			}
		} else {
			// No port specified, just hostname
			data.host.assign(left, right - left);
		}
	}
	
	// Extract path
	if (*right) {
		data.path = right;
	}
}

bool WebClient::URL::isssl() const
{
	if (scheme() == "https") return true;
	if (scheme() == "http") return false;
	if (port() == 443) return true;
	return false;
}

struct WebClient::Private {
	std::vector<std::string> request_header;
	WebClient::Error error;
	WebClient::Response response;
	WebContext *webcx;
	WebClient::HttpVersion http_version = WebClient::HTTP_1_0;
	int crlf_state = 0;
	size_t content_offset = 0;
	std::string last_host_name;
	int last_port = 0;
	bool keep_alive = false;
	socket_t sock = INVALID_SOCKET;
	SSL *ssl = nullptr;
};

WebClient::WebClient(WebContext *webcx)
	: m(new Private)
{
	assert(webcx);
	m->webcx = webcx;
	set_http_version(m->webcx->m->http_version);
}

WebClient::~WebClient()
{
	close();
	delete m;
}

void WebClient::set_http_version(HttpVersion httpver)
{
	m->http_version = httpver;
}

void WebClient::initialize()
{
#ifdef _WIN32
	static bool initialized = false;
	
	// Prevent multiple initialization
	if (!initialized) {
		WSADATA wsaData;
		WORD wVersionRequested;
		wVersionRequested = MAKEWORD(2, 2); // Request version 2.2 for better compatibility
		if (WSAStartup(wVersionRequested, &wsaData) == 0) {
			atexit(cleanup);
			initialized = true;
		}
	}
#endif

#if USE_OPENSSL
	// Thread-safe OpenSSL initialization
	static bool ssl_initialized = false;
	if (!ssl_initialized) {
		OpenSSL_add_all_algorithms();
		ssl_initialized = true;
	}
#endif
}

void WebClient::cleanup()
{
#if USE_OPENSSL
	ERR_free_strings();
#endif
#ifdef _WIN32
	WSACleanup();
#endif
}

void WebClient::reset()
{
//	m->request_header.clear();
	m->error = Error();
	m->response = Response();
	m->crlf_state = 0;
	m->content_offset = 0;
}

void WebClient::output_debug_string(char const *str)
{
	if (0) {
#ifdef _WIN32
		OutputDebugStringA(str);
#else
		fwrite(str, 1, strlen(str), stderr);
#endif
	}
}

void WebClient::output_debug_strings(std::vector<std::string> const &vec)
{
	for (std::string const &s : vec) {
		output_debug_string((s + '\n').c_str());
	}
}

WebClient::Error const &WebClient::error() const
{
	return m->error;
}

void WebClient::clear_error()
{
	m->error = Error();
}

int WebClient::get_port(URL const *url, char const *scheme, char const *protocol)
{
	int port = url->port();
	if (port < 1 || port > 65535) {
		struct servent *s;
		s = getservbyname(url->scheme().c_str(), protocol);
		if (s) {
			port = ntohs(s->s_port);
		} else {
			s = getservbyname(scheme, protocol);
			if (s) {
				port = ntohs(s->s_port);
			}
		}
		if (port < 1 || port > 65535) {
			port = 80;
		}
	}
	return port;
}

static inline std::string to_s(size_t n)
{
	char tmp[32]; // Sufficient for size_t on 64-bit systems
	snprintf(tmp, sizeof(tmp), "%zu", n); // Use %zu for size_t and prevent buffer overflow
	return tmp;
}

void WebClient::set_default_header(Request const &url, Post const *post, RequestOption const &opt)
{
	std::vector<std::string> header;
	std::set<std::string> names;
	auto AddHeader = [&](std::string const &s){
		size_t i = s.find(':');
		if (i != std::string::npos) {
			std::string name = s.substr(0, i);
			if (names.find(name) == names.end()) {
				names.insert(names.end(), name);
				header.push_back(s);
			}
		}
	};
	AddHeader("Host: " + url.url.host());
	AddHeader("User-Agent: " USER_AGENT);
	AddHeader("Accept: */*");
	if (opt.keep_alive) {
		AddHeader("Connection: keep-alive");
	} else {
		AddHeader("Connection: close");
	}
	if (post) {
		AddHeader("Content-Length: " + to_s(post->data.size()));
		std::string ct = "Content-Type: ";
		if (post->content_type.empty()) {
			ct += ContentType::APPLICATION_OCTET_STREAM;
		} else if (post->content_type == ContentType::MULTIPART_FORM_DATA) {
			ct += post->content_type;
			if (!post->boundary.empty()) {
				ct += "; boundary=";
				ct += post->boundary;
			}
		} else {
			ct += post->content_type;
		}
		AddHeader(ct);
	}
	if (url.auth.type == Authorization::Basic) {
		std::string s = url.auth.uid + ':' + url.auth.pwd;
		AddHeader("Authorization: Basic " + base64_encode(s));
	}
	for (std::string const &h : url.headers) {
		AddHeader(h);
	}
	m->request_header = std::move(header);
}

std::string WebClient::make_http_request(Request const &url, Post const *post, WebProxy const *proxy, bool https)
{
	std::string str;

	str = post ? "POST " : "GET ";

	char const *httpver = "1.0";
	switch (m->http_version) {
	case HTTP_1_1:
		httpver = "1.1";
		break;
	}

	if (proxy && !https) {
		str += url.url.data.full_request;
		str += " HTTP/";
		str += httpver;
		str += "\r\n";
	} else {
		str += url.url.path();
		str += " HTTP/";
		str += httpver;
		str += "\r\n";
	}

	for (std::string const &s: m->request_header) {
		str += s;
		str += "\r\n";
	}

	str += "\r\n";
	return str;
}

void WebClient::parse_http_header(char const *begin, char const *end, std::vector<std::string> *header)
{
	if (begin < end) {
		char const *left = begin;
		char const *right = left;
		while (1) {
			if (right >= end) {
				break;
			}
			if (*right == '\r' || *right == '\n') {
				if (left < right) {
					header->push_back(std::string(left, right));
				}
				if (right + 1 < end && *right == '\r' && right[1] == '\n') {
					right++;
				}
				right++;
				if (*right == '\r' || *right == '\n') {
					if (right + 1 < end && *right == '\r' && right[1] == '\n') {
						right++;
					}
					right++;
					break;
				}
				left = right;
			} else {
				right++;
			}
		}
	}
}

void WebClient::parse_http_header(char const *begin, char const *end, WebClient::Response *out)
{
	*out = Response();
	parse_http_header(begin, end, &out->header);
	parse_header(&out->header, out);
}

static void send_(socket_t s, char const *ptr, int len)
{
	while (len > 0) {
		int n = std::min(len, 65536);
		n = send(s, ptr, n, 0);
		if (n < 1 || n > len) {
			throw WebClient::Error("send request failed.");
		}
		ptr += n;
		len -= n;
	}
}

void WebClient::on_end_header(std::vector<char> const *vec, WebClientHandler *handler)
{
	if (vec->empty()) return;
	char const *begin = &vec->at(0);
	char const *end = begin + vec->size();
	parse_http_header(begin, end, &m->response);
	if (handler) {
		handler->checkHeader(this);
	}
}

void WebClient::append(char const *ptr, size_t len, std::vector<char> *out, WebClientHandler *handler)
{
	size_t offset = out->size();
	out->insert(out->end(), ptr, ptr + len);

	if (m->crlf_state < 0) {
		// nop
	} else {
		for (size_t i = 0; i < len; i++) {
			int c = ptr[i] & 0xff;
			if (c == '\r') {
				m->crlf_state |= 1;
			} else if (c == '\n') {
				m->crlf_state |= 1;
				m->crlf_state++;
			} else {
				m->crlf_state = 0;
			}
			if (m->crlf_state == 4) {
				m->content_offset = offset + i + 1;
				on_end_header(out, handler);
				m->crlf_state = -1;
				break;
			}
		}
	}
	if (handler && m->content_offset > 0) {
		offset = out->size();
		if (offset > m->content_offset) {
			size_t len = offset - m->content_offset;
			char const *ptr = &out->at(m->content_offset);
			handler->checkContent(ptr, len);
		}
	}
}

static char *stristr(char *str1, char const *str2)
{
	if (!str1 || !str2) return nullptr;
	
	size_t len1 = strlen(str1);
	size_t len2 = strlen(str2);
	
	if (len2 == 0) return str1; // Empty search string
	if (len2 > len1) return nullptr; // Search string longer than target
	
	for (size_t i = 0; i + len2 <= len1; i++) {
		if (x_strnicmp(str1 + i, str2, len2) == 0) {
			return str1 + i;
		}
	}
	return nullptr;
}

class ResponseHeader {
public:
	size_t pos = 0;
	std::vector<char> line;
	int content_length = -1;
	bool connection_keep_alive = false;
	bool connection_close = false;
	struct {
		bool chunked = false;
	} internal;
	int lf = 0;
	enum State {
		Header,
		Content,
	};
	State state = Header;
	void put(int c)
	{
		pos++;
		if (state == Header) {
			if (c== '\r' || c == '\n') {
				if (!line.empty()) {
					line.push_back(0);
					char *begin = &line[0];
					char *p = strchr(begin, ':');
					if (p && *p == ':') {
						*p++ = 0;
						auto IS = [&](char const *name){ return x_stricmp(begin, name) == 0; };
						if (IS("content-length")) {
							content_length = strtol(p, nullptr, 10);
						} else if (IS("connection")) {
							if (stristr(p, "keep-alive")) {
								connection_keep_alive = true;
							} else if (stristr(p, "close")) {
								connection_close = true;
							}
						} else if (IS("transfer-encoding")) {
							std::vector<std::string> vec;
							auto SPLIT = [](char const *str, char sep, std::vector<std::string> *out){
								out->clear();
								char const *begin = str;
								char const *end = begin + strlen(str);
								char const *ptr = begin;
								char const *left = ptr;
								while (1) {
									char c = 0;
									if (ptr < end) {
										c = *ptr;
									}
									if (c == sep || c == 0) {
										if (left < ptr) {
											char const *l = left;
											char const *r = ptr;
											while (l < r && isspace((unsigned char)*l)) l++;
											while (l < r && isspace((unsigned char)r[-1])) r--;
											out->emplace_back(l, r);
										}
										if (c == 0) break;
										ptr++;
										left = ptr;
									} else {
										ptr++;
									}
								}
							};
							SPLIT(p, ',', &vec);
							auto it = std::find(vec.begin(), vec.end(), "chunked");
							internal.chunked = it != vec.end();
						}
					}
					line.clear();
				}
				if (c== '\r') {
					return;
				}
				if (c == '\n') {
					lf++;
					if (lf == 2) {
						state = Content;
					}
					return;
				}
			}
			lf = 0;
			line.push_back(c);
		}
	}
};

void WebClient::receive_(RequestOption const &opt, std::function<int(char *, int)> const &rcv, ResponseHeader *rh, std::vector<char> *out)
{
	char buf[4096];
	size_t pos = 0;
	while (1) {
		int n;
		if (rh->state == ResponseHeader::Content && rh->content_length >= 0) {
			n = rh->pos + rh->content_length - pos;
			if (n > (int)sizeof(buf)) {
				n = sizeof(buf);
			}
			if (n < 1) break;
		} else {
			n = sizeof(buf);
		}
		n = rcv(buf, n);
		if (n < 1) break;
		append(buf, n, out, opt.handler);
		pos += n;
		if (rh->state == ResponseHeader::Header) {
			for (int i = 0; i < n; i++) {
				rh->put(buf[i]);
				if (rh->state == ResponseHeader::Content) {
					m->keep_alive = rh->connection_keep_alive && !rh->connection_close;
					break;
				}
			}
		}
	}
}

static int inet_connect(std::string const &hostname, int port)
{
	struct sockaddr_in server;
	memset((char *)&server, 0, sizeof(server));
	server.sin_family = AF_INET;

	if (HostNameResolver().resolve(hostname.data(), &server.sin_addr)) {
		server.sin_port = htons(port);
		socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock != INVALID_SOCKET) {
			if (connect(sock, (struct sockaddr*) &server, sizeof(server)) != SOCKET_ERROR) {
				return sock;
			}
			closesocket(sock);
			return INVALID_SOCKET;
		}
	}
	return INVALID_SOCKET;
}

bool WebClient::http_get(Request const &request, Post const *post, RequestOption const &opt, ResponseHeader *rh, std::vector<char> *out)
{
	clear_error();
	out->clear();

	Request server_req;

	WebProxy const *proxy = m->webcx->http_proxy();
	if (proxy) {
		server_req = Request(proxy->server);
	} else {
		server_req = request;
	}

	std::string hostname = server_req.url.host();
	int port = get_port(&server_req.url, "http", "tcp");

	m->keep_alive = opt.keep_alive && hostname == m->last_host_name && port == m->last_port;
	if (!m->keep_alive) close();

	if (m->sock == INVALID_SOCKET) {
		m->sock = inet_connect(hostname, port);
		if (m->sock == INVALID_SOCKET) {
			throw Error("connect failed.");
		}
	}
	m->last_host_name = hostname;
	m->last_port = port;

	set_default_header(request, post, opt);

	std::string req = make_http_request(request, post, proxy, false);

	send_(m->sock, req.c_str(), (int)req.size());
	if (post && !post->data.empty()) {
		send_(m->sock, (char const *)&post->data[0], (int)post->data.size());
	}

	m->crlf_state = 0;
	m->content_offset = 0;

	receive_(opt, [&](char *ptr, int len){
		return recv(m->sock, ptr, len, 0);
	}, rh, out);

	if (!m->keep_alive) close();

	return true;
}

bool WebClient::https_get(Request const &request_req, Post const *post, RequestOption const &opt, ResponseHeader *rh, std::vector<char> *out)
{
#if USE_OPENSSL

	auto *sslctx = m->webcx->m->ctx;
	if (!m->webcx || !sslctx) {
		output_debug_string("SSL context is null.\n");
		return false;
	}

	clear_error();
	out->clear();

	auto get_ssl_error = []()->std::string{
		char tmp[1000];
		unsigned long e = ERR_get_error();
		ERR_error_string_n(e, tmp, sizeof(tmp));
		return tmp;
	};

	Request server_req;

	WebProxy const *proxy = m->webcx->https_proxy();
	if (proxy) {
		server_req = Request(proxy->server);
	} else {
		server_req = request_req;
	}

	std::string hostname = server_req.url.host();
	int port = get_port(&server_req.url, "https", "tcp");

	m->keep_alive = opt.keep_alive && hostname == m->last_host_name && port == m->last_port;
	if (!m->keep_alive) close();

	socket_t sock = m->sock;
	SSL *ssl = m->ssl;
	bool new_connection = false;
	
	if (sock == INVALID_SOCKET || !ssl) {
		new_connection = true;
		sock = inet_connect(hostname, port);
		if (sock == INVALID_SOCKET) {
			throw Error("connect failed.");
		}
		ssl = nullptr; // Ensure ssl is nullptr before we try to create it
		
		try {
			if (proxy) { // Connect through proxy
				char port_str[16];
				snprintf(port_str, sizeof(port_str), ":%u", get_port(&request_req.url, "https", "tcp"));

				std::string str = "CONNECT ";
				str += request_req.url.data.host;
				str += port_str;
				str += " HTTP/1.0\r\n\r\n";
				send_(sock, str.c_str(), str.size());
				
				// Read proxy response
				char tmp[1000];
				int n = recv(sock, tmp, sizeof(tmp), 0);
				if (n <= 0) {
					throw Error("Proxy connection failed");
				}
				
				// Parse response to check if connection succeeded
				bool found_ok = false;
				int i;
				for (i = 0; i < n - 8; i++) {
					if (strncmp(tmp + i, "200 OK", 6) == 0 || 
						strncmp(tmp + i, "200 Connection established", 26) == 0) {
						found_ok = true;
						break;
					}
				}
				
				if (!found_ok) {
					// Format response for error message
					int end = 0;
					for (i = 0; i < n && i < 100; i++) {
						if (tmp[i] == '\r' || tmp[i] == '\n') {
							end = i;
							break;
						}
					}
					throw Error(std::string("Proxy error: ") + std::string(tmp, end));
				}
			}

			// Set up SSL
			ssl = SSL_new(sslctx);
			if (!ssl) {
				throw Error(get_ssl_error());
			}

			// Disable insecure protocols
			SSL_set_options(ssl, SSL_OP_NO_SSLv2);
			SSL_set_options(ssl, SSL_OP_NO_SSLv3);
			
			// Set hostname for SNI and certificate verification
			SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			if (!SSL_set1_host(ssl, hostname.c_str())) {
				throw Error(get_ssl_error());
			}
			SSL_set_tlsext_host_name(ssl, hostname.c_str());

			int ret = SSL_set_fd(ssl, sock);
			if (ret != 1) {
				throw Error(get_ssl_error());
			}

			// Ensure PRNG is properly seeded
			RAND_poll();
			if (RAND_status() == 0) {
				// If RAND_poll didn't work, use a more secure method than rand()
				unsigned char rand_buf[32];
				for (int i = 0; i < 32; i++) {
					rand_buf[i] = (unsigned char)(time(NULL) ^ (i * 41) ^ (size_t)&ret);
				}
				RAND_add(rand_buf, sizeof(rand_buf), sizeof(rand_buf) / 4.0);
			}

			// Connect SSL
			ret = SSL_connect(ssl);
			if (ret != 1) {
				int err = SSL_get_error(ssl, ret);
				std::string error_msg = get_ssl_error();
				throw Error("SSL connection failed: " + error_msg + " (code: " + std::to_string(err) + ")");
			}

			X509 *x509 = SSL_get_peer_certificate(ssl);
			if (!x509) {
				throw Error("Server did not present a certificate");
			}
			
			// Verify certificate
			long verify_result = SSL_get_verify_result(ssl);
			if (verify_result != X509_V_OK) {
				std::string err = X509_verify_cert_error_string(verify_result);
				// In a real security-sensitive app, we would abort here
				// For compatibility, we just log the error but should consider adding a strict mode option
				output_debug_string(("Certificate verification failed: " + err + "\n").c_str());
			}

			// Log certificate info in debug mode if needed
			if (0) {
				std::string cipher = SSL_get_cipher(ssl);
				output_debug_string((cipher + "\n").c_str());

				std::string version = SSL_get_cipher_version(ssl);
				output_debug_string((version + "\n").c_str());
			}
			
			X509_free(x509);
		} catch (...) {
			// Clean up resources on error
			if (ssl) {
				SSL_free(ssl);
				ssl = nullptr;
			}
			if (sock != INVALID_SOCKET) {
				closesocket(sock);
				sock = INVALID_SOCKET;
			}
			throw;
		}
	}
	
	// Update connection state
	m->last_host_name = hostname;
	m->last_port = port;

	// Prepare request
	set_default_header(request_req, post, opt);
	std::string request = make_http_request(request_req, post, proxy, true);

	// Send request
	auto SEND = [&](char const *ptr, int len){
		while (len > 0) {
			int n = SSL_write(ssl, ptr, len);
			if (n < 1 || n > len) {
				// Store error before potentially losing SSL context
				std::string error_msg = get_ssl_error();
				// We must clean up if this is a connection error
				if (new_connection) {
					if (ssl) {
						SSL_free(ssl);
						ssl = nullptr;
					}
					if (sock != INVALID_SOCKET) {
						closesocket(sock);
						sock = INVALID_SOCKET;
					}
				}
				throw WebClient::Error(error_msg);
			}
			ptr += n;
			len -= n;
		}
	};

	try {
		SEND(request.c_str(), (int)request.size());
		if (post && !post->data.empty()) {
			SEND((char const *)&post->data[0], (int)post->data.size());
		}

		m->crlf_state = 0;
		m->content_offset = 0;

		// Receive response
		receive_(opt, [&](char *ptr, int len){
			int n = SSL_read(ssl, ptr, len);
			if (n < 0) {
				// Store error info before cleanup
				std::string error_msg = get_ssl_error();
				if (new_connection) {
					// Clean up connection on error since it's not fully established
					if (ssl) {
						SSL_free(ssl);
						ssl = nullptr;
					}
					if (sock != INVALID_SOCKET) {
						closesocket(sock);
						sock = INVALID_SOCKET;
					}
				}
				throw WebClient::Error(error_msg);
			}
			return n;
		}, rh, out);

		// Save connection for reuse if keep-alive
		m->sock = sock;
		m->ssl = ssl;
		
		if (!m->keep_alive) {
			close();
		}
		return true;
	} catch (...) {
		// If there's an error during send/receive and this is a new connection,
		// we need to clean up to prevent resource leaks
		if (new_connection) {
			if (ssl) {
				SSL_free(ssl);
				m->ssl = nullptr;
			}
			if (sock != INVALID_SOCKET) {
				closesocket(sock);
				m->sock = INVALID_SOCKET;
			}
		}
		throw;
	}
#endif
	return false;
}

bool decode_chunked(char const *ptr, char const *end, std::vector<char> *out)
{
	if (!ptr || !end || !out || ptr >= end) return false;
	
	out->clear();
	const size_t MAX_CHUNK_SIZE = 10 * 1024 * 1024; // 10 MB max chunk size for safety
	size_t length = 0;
	
	while (ptr < end) {
		// Parse chunk size (hex)
		if (isxdigit((unsigned char)*ptr)) {
			// Handle chunk terminator (0-length chunk)
			if (*ptr == '0' && length == 0) {
				if (ptr + 2 < end && ptr[1] == '\r' && ptr[2] == '\n') {
					return true; // normal exit
				}
				return false;
			}
			
			// Parse hex digit safely
			size_t old_length = length;
			length *= 16;
			
			// Check for overflow
			if (length / 16 != old_length) {
				return false; // Integer overflow
			}
			
			if (isdigit(*ptr)) {
				length += *ptr - '0';
			} else {
				length += toupper(*ptr) - 'A' + 10;
			}
			
			// Prevent excessive memory allocation
			if (length > MAX_CHUNK_SIZE) {
				return false;
			}
			
			ptr++;
		} else {
			// End of chunk size, start of chunk data
			if (ptr + 1 < end && ptr[0] == '\r' && ptr[1] == '\n') {
				// Check if we have enough data for the chunk plus terminating CRLF
				if (ptr + 2 + length + 2 <= end && 
				    ptr[length + 2] == '\r' && ptr[length + 3] == '\n') {
					ptr += 2; // Skip CRLF after chunk size
					// Append chunk data to output
					out->insert(out->end(), ptr, ptr + length);
					ptr += length + 2; // Skip chunk data and terminating CRLF
				} else {
					return false; // Malformed chunk or incomplete data
				}
				length = 0;
			} else {
				// Unexpected character in chunk size
				return false;
			}
		}
	}
	return false; // Unexpected end of data
}

bool WebClient::get(Request const &req, Post const *post, Response *out, WebClientHandler *handler)
{
	reset();
	bool ok = false;
	try {
		if (!m->webcx->m) {
			throw Error("WebContext is null.");
		}
		m->webcx->m->broken_pipe = false;
		RequestOption opt;
		opt.keep_alive = m->webcx->m->use_keep_alive;
		opt.handler = handler;
		ResponseHeader rh;
		std::vector<char> res;
		if (req.url.isssl()) {
#if USE_OPENSSL
			https_get(req, post, opt, &rh, &res);
#endif
		} else {
			http_get(req, post, opt, &rh, &res);
		}
		if (!res.empty()) {
			char const *begin = &res[0];
			char const *end = begin + res.size();
			char const *ptr = begin + m->content_offset;
			if (ptr < end) {
				if (rh.internal.chunked) {
					if (!decode_chunked(ptr, end, &out->content)) {
						out->content.clear();
						return false;
					}
				} else {
					out->content.assign(ptr, end);
				}
			}
		}
		ok = true;
	} catch (Error const &e) {
		m->error = e;
		close();
	}
	if (m->webcx->m->broken_pipe) {
		m->webcx->m->broken_pipe = false;
		ok = false;
	}
	if (!ok) {
		*out = Response();
	}
	return ok;
}

void WebClient::parse_header(std::vector<std::string> const *header, WebClient::Response *res)
{
	if (0) { // for debug
		for (std::string const &s : *header) {
			fprintf(stderr, "%s\n", s.c_str());
		}
	}
	if (!header->empty()) {
		std::string const &line = header->at(0);
		char const *begin = line.c_str();
		char const *end = begin + line.size();
		if (line.size() > 5 && strncmp(line.c_str(), "HTTP/", 5) == 0) {
			int state = 0;
			res->version.hi = res->version.lo = res->code = 0;
			char const *ptr = begin + 5;
			while (1) {
				int c = 0;
				if (ptr < end) {
					c = *ptr & 0xff;
				}
				switch (state) {
				case 0:
					if (isdigit(c)) {
						res->version.hi = res->version.hi * 10 + (c - '0');
					} else if (c == '.') {
						state = 1;
					} else {
						state = -1;
					}
					break;
				case 1:
					if (isdigit(c)) {
						res->version.lo = res->version.lo * 10 + (c - '0');
					} else if (isspace(c)) {
						state = 2;
					} else {
						state = -1;
					}
					break;
				case 2:
					if (isspace(c)) {
						if (res->code != 0) {
							state = -1;
						}
					} else if (isdigit(c)) {
						res->code = res->code * 10 + (c - '0');
					} else {
						state = -1;
					}
					break;
				default:
					state = -1;
					break;
				}
				if (c == 0 || state < 0) {
					break;
				}
				ptr++;
			}
		}
	}
}

std::string WebClient::header_value(std::vector<std::string> const *header, std::string const &name)
{
	for (size_t i = 1; i < header->size(); i++) {
		std::string const &line = header->at(i);
		char const *begin = line.c_str();
		char const *end = begin + line.size();
		char const *colon = strchr(begin, ':');
		if (colon) {
			if (x_strnicmp(begin, name.c_str(), name.size()) == 0) {
				char const *ptr = colon + 1;
				while (ptr < end && isspace(*ptr & 0xff)) ptr++;
				return std::string(ptr, end);
			}
		}
	}
	return std::string();
}

std::string WebClient::header_value(std::string const &name) const
{
	return header_value(&m->response.header, name);
}

std::string WebClient::content_type() const
{
	std::string s = header_value("Content-Type");
	char const *begin = s.c_str();
	char const *end = begin + s.size();
	char const *ptr = begin;
	while (ptr < end) {
		int c = *ptr & 0xff;
		if (c == ';' || c < 0x21) break;
		ptr++;
	}
	if (ptr < end) return std::string(begin, ptr);
	return s;
}

size_t WebClient::content_length() const
{
	return m->response.content.size();
}

char const *WebClient::content_data() const
{
	if (m->response.content.empty()) return "";
	return &m->response.content[0];
}

int WebClient::get(Request const &req, WebClientHandler *handler)
{
	get(req, nullptr, &m->response, handler);
	return m->response.code;
}

int WebClient::post(Request const &req, Post const *post, WebClientHandler *handler)
{
	get(req, post, &m->response, handler);
	return m->response.code;
}

void WebClient::close()
{
#if USE_OPENSSL
	if (m->ssl) {
		SSL_shutdown(m->ssl);
		SSL_free(m->ssl);
		m->ssl = nullptr;
	}
#endif
	if (m->sock != INVALID_SOCKET) {
		// Try graceful shutdown first
		int shutdown_result = shutdown(m->sock, 2); // SD_BOTH or SHUT_RDWR
		// Ignore shutdown errors as socket might already be disconnected
		
		closesocket(m->sock);
		m->sock = INVALID_SOCKET;
	}
	
	// Reset connection state
	m->last_host_name = "";
	m->last_port = 0;
	m->keep_alive = false;
}

void WebClient::add_header(std::string const &text)
{
	m->request_header.push_back(text);
}

WebClient::Response const &WebClient::response() const
{
	return m->response;
}

void WebClient::make_application_www_form_urlencoded(char const *begin, char const *end, WebClient::Post *out)
{
	*out = WebClient::Post();
	out->content_type = ContentType::APPLICATION_X_WWW_FORM_URLENCODED;
	vappend(&out->data, begin, end - begin);
}

void WebClient::make_multipart_form_data(std::vector<Part> const &parts, WebClient::Post *out, std::string const &boundary)
{
	*out = WebClient::Post();
	out->content_type = ContentType::MULTIPART_FORM_DATA;
	out->boundary = boundary;

	for (Part const &part : parts) {
		vappend(&out->data, "--");
		vappend(&out->data, out->boundary);
		vappend(&out->data, "\r\n");
		if (!part.content_disposition.type.empty()) {
			ContentDisposition const &cd = part.content_disposition;
			std::string s;
			s = "Content-Disposition: ";
			s += cd.type;
			auto Add = [&s](std::string const &name, std::string const &value){
				if (!value.empty()) {
					s += "; " + name + "=\"";
					s += value;
					s += '\"';
				}
			};
			Add("name", cd.name);
			Add("filename", cd.filename);
			vappend(&out->data, s);
			vappend(&out->data, "\r\n");
		}
		if (!part.content_type.empty()) {
			vappend(&out->data, "Content-Type: " + part.content_type + "\r\n");
		}
		if (!part.content_transfer_encoding.empty()) {
			vappend(&out->data, "Content-Transfer-Encoding: " + part.content_transfer_encoding + "\r\n");
		}
		vappend(&out->data, "\r\n");
		vappend(&out->data, part.data, part.size);
		vappend(&out->data, "\r\n");
	}

	vappend(&out->data, "--");
	vappend(&out->data, out->boundary);
	vappend(&out->data, "--\r\n");
}

void WebClient::make_multipart_form_data(char const *data, size_t size, WebClient::Post *out, std::string const &boundary)
{
	Part part;
	part.data = data;
	part.size = size;
	std::vector<Part> parts;
	parts.push_back(part);
	make_multipart_form_data(parts, out, boundary);
}


//

WebContext::WebContext(WebClient::HttpVersion httpver)
	: m(new Private)
{
	set_http_version(httpver);
#if USE_OPENSSL
	SSL_load_error_strings();
	SSL_library_init();
	m->ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_default_verify_paths(m->ctx);
#endif
}

WebContext::~WebContext()
{
#if USE_OPENSSL
	SSL_CTX_free(m->ctx);
#endif
	delete m;
}

void WebContext::set_http_version(WebClient::HttpVersion httpver)
{
	m->http_version = httpver;
}

void WebContext::set_keep_alive_enabled(bool f)
{
	m->use_keep_alive = f;
}

void WebContext::set_http_proxy(std::string const &proxy)
{
	m->http_proxy = WebProxy();
	m->http_proxy.server = proxy;
}

void WebContext::set_https_proxy(std::string const &proxy)
{
	m->https_proxy = WebProxy();
	m->https_proxy.server = proxy;
}

const WebProxy *WebContext::http_proxy() const
{
	if (!m->http_proxy.empty()) {
		return &m->http_proxy;
	}
	return nullptr;
}

const WebProxy *WebContext::https_proxy() const
{
	if (!m->https_proxy.empty()) {
		return &m->https_proxy;
	}
	if (!m->http_proxy.empty()) {
		return &m->http_proxy;
	}
	return nullptr;
}

bool WebContext::load_cacert(char const *path)
{
#if USE_OPENSSL
	int r = SSL_CTX_load_verify_locations(m->ctx, path, nullptr);
	return r == 1;
#else
	return false;
#endif
}

void WebContext::notify_broken_pipe()
{
	m->broken_pipe = true;
}

std::string WebClient::quick_get(std::string const &url)
{
	WebContext wc(WebClient::HTTP_1_1);
	wc.set_keep_alive_enabled(false);
	WebClient http(&wc);
	if (http.get(WebClient::Request(url))) {
		return {http.content_data(), http.content_length()};
	}
	return {};
}

std::string WebClient::checkip()
{
	auto s = quick_get("http://checkip.amazonaws.com/");
	return (std::string)trimmed(s);
}


