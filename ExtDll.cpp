#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdarg>

#pragma comment(lib, "winhttp.lib")

#pragma pack(push, 1)
struct CardInfo1164 {
	WORD	structureSize; // must be 1164, filled by caller
	BYTE	wasDeleted;    // 0/1
	BYTE	shouldSeize;   // 0/1
	BYTE	isExpired;     // 0/1
	BYTE	isInactiveNow; // 0/1
	BYTE	needManager;   // 0/1
	BYTE	isBlocked;     // 0/1
	char	blockReason[256]; // asciiz
	char	ownerName[40];    // asciiz
	int64_t	ownerId;
	DWORD	accountNumber;
	DWORD	nonPayerType;
	WORD	bonusNumber;
	WORD	discountNumber;
	int64_t	maxDiscountSumKopecks;
	int64_t	accountBalances[8]; // [0] available for payment, [1..7] accounts 2..8
	char	cardInfo[256];
	char	displayInfo[256];
	char	printInfo[256];
};

struct TransactionEx122 {
	WORD	structureSize; // must be 122
	int64_t	cardNumber;
	int64_t	ownerId;
	DWORD	accountNumber;
	BYTE	transactionType; // 0..3
	int64_t	amountKopecks;  // signed
	WORD	restaurantCode;
	DWORD	cashDate; // days from 30/12/1899
	BYTE	unitNumber;
	DWORD	checkNumber;
	// taxes A..H
	int64_t	taxSumA; WORD taxPercA;
	int64_t	taxSumB; WORD taxPercB;
	int64_t	taxSumC; WORD taxPercC;
	int64_t	taxSumD; WORD taxPercD;
	int64_t	taxSumE; WORD taxPercE;
	int64_t	taxSumF; WORD taxPercF;
	int64_t	taxSumG; WORD taxPercG;
	int64_t	taxSumH; WORD taxPercH;
};
#pragma pack(pop)

static const DWORD MAX_OUT_LEN = 150 * 1024; // 150 KB

// Logging globals
static CRITICAL_SECTION g_LogCs;
static bool g_LogInit = false;
static std::wstring g_LogPath;
static int g_LogLevel = 2; // 0=off,1=error,2=info,3=debug
static bool g_LogDebugOutput = false;

static std::wstring ToWide(const std::string &s) {
	if (s.empty()) return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring ws(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
	return ws;
}

static std::string FromWide(const std::wstring &ws) {
	if (ws.empty()) return std::string();
	int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
	std::string s(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, nullptr, nullptr);
	return s;
}

static std::wstring GetModuleDirectory() {
	wchar_t path[MAX_PATH] = {0};
	HMODULE hModule = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&GetModuleDirectory), &hModule)) {
		DWORD len = GetModuleFileNameW(hModule, path, MAX_PATH);
		for (DWORD i = len; i > 0; --i) {
			if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
				path[i - 1] = L'\0';
				break;
			}
		}
	}
	return std::wstring(path);
}

static std::wstring ReadIniString(const wchar_t *section, const wchar_t *key, const wchar_t *defVal, const std::wstring &iniPath) {
	wchar_t buf[512] = {0};
	GetPrivateProfileStringW(section, key, defVal, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), iniPath.c_str());
	return std::wstring(buf);
}

static int ReadIniInt(const wchar_t *section, const wchar_t *key, int defVal, const std::wstring &iniPath) {
	wchar_t buf[64] = {0};
	_snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", defVal);
	GetPrivateProfileStringW(section, key, buf, buf, (DWORD)_countof(buf), iniPath.c_str());
	return _wtoi(buf);
}

static void InitLoggingOnce() {
	if (g_LogInit) return;
	InitializeCriticalSection(&g_LogCs);
	std::wstring iniPath = GetModuleDirectory() + L"\\ExtDll.ini";
	g_LogPath = ReadIniString(L"Logging", L"LogFile", L"ExtDll.log", iniPath);
	if (g_LogPath.find(L"\\") == std::wstring::npos && g_LogPath.find(L"/") == std::wstring::npos) {
		g_LogPath = GetModuleDirectory() + L"\\" + g_LogPath;
	}
	g_LogLevel = ReadIniInt(L"Logging", L"LogLevel", 2, iniPath);
	g_LogDebugOutput = ReadIniInt(L"Logging", L"DebugOutput", 0, iniPath) != 0;
	g_LogInit = true;
}

static void WriteLogLineVA(int level, const wchar_t *fmt, va_list ap) {
	if (level <= 0 || g_LogLevel <= 0 || level > g_LogLevel) return;
	InitLoggingOnce();
	wchar_t line[1024];
	_vsnwprintf_s(line, _countof(line), _TRUNCATE, fmt, ap);
	SYSTEMTIME st; GetLocalTime(&st);
	wchar_t prefix[64];
	const wchar_t *lvl = (level==1?L"ERR":(level==2?L"INF":L"DBG"));
	_snwprintf_s(prefix, _countof(prefix), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, lvl);
	std::wstring wmsg = std::wstring(prefix) + line + L"\r\n";
	if (g_LogDebugOutput) OutputDebugStringW(wmsg.c_str());
	std::string msg = FromWide(wmsg);
	EnterCriticalSection(&g_LogCs);
	HANDLE h = CreateFileW(g_LogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(h, msg.data(), (DWORD)msg.size(), &written, NULL);
		CloseHandle(h);
	}
	LeaveCriticalSection(&g_LogCs);
}

static void LogInfo(const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); WriteLogLineVA(2, fmt, ap); va_end(ap);
}
static void LogError(const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); WriteLogLineVA(1, fmt, ap); va_end(ap);
}
static void LogDebug(const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); WriteLogLineVA(3, fmt, ap); va_end(ap);
}

// Minimal base64 for binary payloads
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const void *data, size_t len) {
	const unsigned char *bytes = reinterpret_cast<const unsigned char*>(data);
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t triple = 0;
		int chunk = (int)std::min<size_t>(3, len - i);
		for (int j = 0; j < chunk; ++j) triple |= (uint32_t)bytes[i + j] << (16 - 8 * j);
		for (int j = 0; j < 4; ++j) {
			if (j > chunk) {
				out.push_back('=');
			} else {
				int idx = (triple >> (18 - 6 * j)) & 0x3F;
				out.push_back(b64_table[idx]);
			}
		}
	}
	// Fix padding logic
	if (len % 3 == 1) {
		out[out.size() - 1] = '=';
		out[out.size() - 2] = '=';
	} else if (len % 3 == 2) {
		out[out.size() - 1] = '=';
	}
	return out;
}

static unsigned char b64_rev(char c) {
	if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A');
	if (c >= 'a' && c <= 'z') return (unsigned char)(c - 'a' + 26);
	if (c >= '0' && c <= '9') return (unsigned char)(c - '0' + 52);
	if (c == '+') return 62;
	if (c == '/') return 63;
	return 0xFF;
}

static bool Base64Decode(const std::string &in, std::vector<unsigned char> &out) {
	std::vector<unsigned char> tmp;
	tmp.reserve(in.size());
	uint32_t buf = 0;
	int bits = 0;
	for (char ch : in) {
		if (ch == '=') break;
		unsigned char v = b64_rev(ch);
		if (v == 0xFF) continue;
		buf = (buf << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			tmp.push_back((unsigned char)((buf >> bits) & 0xFF));
		}
	}
	out.swap(tmp);
	return true;
}

// Very small JSON helpers for predictable keys (not a full JSON parser)
static bool JsonFindString(const std::string &json, const std::string &key, std::string &value) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos);
	if (pos == std::string::npos) return false;
	pos = json.find('"', pos);
	if (pos == std::string::npos) return false;
	size_t end = json.find('"', pos + 1);
	if (end == std::string::npos) return false;
	value = json.substr(pos + 1, end - pos - 1);
	return true;
}

static bool JsonFindInt64(const std::string &json, const std::string &key, int64_t &value) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos);
	if (pos == std::string::npos) return false;
	// skip spaces
	while (pos + 1 < json.size() && (json[pos + 1] == ' ' || json[pos + 1] == '\t')) ++pos;
	size_t start = pos + 1;
	size_t end = start;
	while (end < json.size() && (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) ++end;
	if (end == start) return false;
	value = _strtoi64(json.substr(start, end - start).c_str(), nullptr, 10);
	return true;
}

static bool JsonFindUInt32(const std::string &json, const std::string &key, DWORD &value) {
	int64_t tmp = 0;
	if (!JsonFindInt64(json, key, tmp)) return false;
	value = (DWORD)tmp;
	return true;
}

static bool JsonFindUInt16(const std::string &json, const std::string &key, WORD &value) {
	int64_t tmp = 0;
	if (!JsonFindInt64(json, key, tmp)) return false;
	value = (WORD)tmp;
	return true;
}

static bool JsonFindUInt8(const std::string &json, const std::string &key, BYTE &value) {
	int64_t tmp = 0;
	if (!JsonFindInt64(json, key, tmp)) return false;
	value = (BYTE)tmp;
	return true;
}

static bool JsonFindBool(const std::string &json, const std::string &key, bool &value) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos);
	if (pos == std::string::npos) return false;
	// skip spaces
	while (pos + 1 < json.size() && (json[pos + 1] == ' ' || json[pos + 1] == '\t')) ++pos;
	size_t start = pos + 1;
	if (json.compare(start, 4, "true") == 0) { value = true; return true; }
	if (json.compare(start, 5, "false") == 0) { value = false; return true; }
	return false;
}

static bool JsonFindArrayInt64(const std::string &json, const std::string &key, std::vector<int64_t> &values) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find('[', pos);
	if (pos == std::string::npos) return false;
	size_t end = json.find(']', pos);
	if (end == std::string::npos) return false;
	values.clear();
	std::string body = json.substr(pos + 1, end - pos - 1);
	std::stringstream ss(body);
	std::string item;
	while (std::getline(ss, item, ',')) {
		item.erase(std::remove_if(item.begin(), item.end(), [](char c){ return c==' ' || c=='\t' || c=='\r' || c=='\n'; }), item.end());
		if (!item.empty()) {
			int64_t v = _strtoi64(item.c_str(), nullptr, 10);
			values.push_back(v);
		}
	}
	return true;
}

static bool HttpPostJson(const std::wstring &host, const std::wstring &path, const std::string &body, std::string &responseBody, const std::wstring &apiKey = L"") {
	responseBody.clear();
	HINTERNET hSession = WinHttpOpen(L"LyttiDll/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) { LogError(L"WinHttpOpen failed: %lu", GetLastError()); return false; }
	HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) { LogError(L"WinHttpConnect failed: %lu", GetLastError()); WinHttpCloseHandle(hSession); return false; }
	DWORD flags = WINHTTP_FLAG_SECURE;
	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!hRequest) { LogError(L"WinHttpOpenRequest failed: %lu", GetLastError()); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
	std::wstring headers = L"Content-Type: application/json\r\n";
	if (!apiKey.empty()) {
		headers += L"Authorization: Bearer ";
		headers += apiKey;
		headers += L"\r\n";
	}
	BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(), (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
	if (!sent) {
		LogError(L"WinHttpSendRequest failed: %lu", GetLastError());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}
	if (!WinHttpReceiveResponse(hRequest, NULL)) {
		LogError(L"WinHttpReceiveResponse failed: %lu", GetLastError());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}
	DWORD statusCode = 0, statusSize = sizeof(statusCode);
	WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
	LogDebug(L"HTTP %s %s -> %lu", host.c_str(), path.c_str(), statusCode);
	DWORD dwSize = 0;
	do {
		DWORD dwDownloaded = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
		if (dwSize == 0) break;
		std::vector<char> buf(dwSize);
		if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) break;
		if (dwDownloaded == 0) break;
		responseBody.append(buf.data(), buf.data() + dwDownloaded);
	} while (dwSize > 0);
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	LogDebug(L"HTTP response length: %u", (UINT)responseBody.size());
	return true;
}

static void ZeroMemorySafe(void *ptr, size_t size) {
	if (ptr && size) std::memset(ptr, 0, size);
}

static void CopyAsciiz(char *dst, size_t dstSize, const std::string &src) {
	ZeroMemorySafe(dst, dstSize);
	if (dstSize == 0) return;
	std::string s = src;
	if (s.size() >= dstSize) s.resize(dstSize - 1);
	std::memcpy(dst, s.data(), s.size());
}

static void EnsureLeadingSlash(std::wstring &path) {
	if (path.empty() || path[0] != L'/') path = L"/" + path;
}

static void LoadConfig(std::wstring &host, std::wstring &getCardPath, std::wstring &transactionsPath, std::wstring &apiKey) {
	std::wstring iniPath = GetModuleDirectory() + L"\\ExtDll.ini";
	host = ReadIniString(L"Main", L"Host", L"complete-relieved-martin.ngrok-free.app", iniPath);
	getCardPath = ReadIniString(L"Main", L"GetCardPath", L"/get_card_info", iniPath);
	transactionsPath = ReadIniString(L"Main", L"TransactionsPath", L"/transactionsex", iniPath);
	apiKey = ReadIniString(L"Security", L"ApiKey", L"", iniPath);
	EnsureLeadingSlash(getCardPath);
	EnsureLeadingSlash(transactionsPath);
}

static void MaybeAppendAuthHeader(std::wstring &headers, const std::wstring &apiKey) {
	if (!apiKey.empty()) {
		headers += L"Authorization: Bearer ";
		headers += apiKey;
		headers += L"\r\n";
	}
}

extern "C" __declspec(dllexport) int __stdcall GetCardInfoEx(
	int64_t Card,
	DWORD Restaurant,
	DWORD UnitNo,
	void *Info,
	void *InpBuf,
	DWORD InpLen,
	WORD InpKind,
	void* &OutBuf,
	DWORD &OutLen,
	WORD &OutKind
)
{
	OutBuf = nullptr;
	OutLen = 0;
	OutKind = 0;
	if (Info == nullptr) return 1;
	CardInfo1164 *ci = reinterpret_cast<CardInfo1164*>(Info);
	if (ci->structureSize != sizeof(CardInfo1164)) {
		return 1;
	}
	// Build request JSON
	std::ostringstream os;
	os << "{";
	os << "\"card\":" << Card << ",";
	os << "\"restaurant\":" << Restaurant << ",";
	os << "\"unitNo\":" << UnitNo << ",";
	os << "\"inpKind\":" << InpKind << ",";
	if (InpBuf != nullptr && InpLen > 0) {
		std::string b64 = Base64Encode(InpBuf, InpLen);
		os << "\"inpBufBase64\":\"" << b64 << "\",";
	} else {
		os << "\"inpBufBase64\":\"\",";
	}
	os << "\"client\":\"LyttiDll\"";
	os << "}";
	std::wstring host, path, txPath, apiKey;
	LoadConfig(host, path, txPath, apiKey);
	std::string response;
	LogInfo(L"GetCardInfoEx card=%lld rest=%u unit=%u inpLen=%u", (long long)Card, Restaurant, UnitNo, InpLen);
	if (!HttpPostJson(host, path, os.str(), response, apiKey)) {
		LogError(L"GetCardInfoEx request failed");
		return 1; // treat as not found
	}
	// Optionally pass-through response to OutBuf if present
	if (!response.empty()) {
		size_t toCopy = std::min<size_t>(response.size(), MAX_OUT_LEN);
		HGLOBAL h = GlobalAlloc(GMEM_FIXED, toCopy);
		if (h) {
			std::memcpy(h, response.data(), toCopy);
			OutBuf = h;
			OutLen = (DWORD)toCopy;
			// try find outKind from json
			DWORD kind = 0;
			JsonFindUInt32(response, "outKind", kind);
			OutKind = (WORD)kind;
		}
	}
	// Parse JSON into CardInfo1164
	bool exists = false;
	JsonFindBool(response, "exists", exists);
	if (!exists) {
		LogInfo(L"GetCardInfoEx: card not found");
		return 1;
	}
	ZeroMemorySafe(ci, sizeof(CardInfo1164));
	ci->structureSize = (WORD)sizeof(CardInfo1164);
	BYTE b = 0;
	bool bv = false;
	if (JsonFindBool(response, "deleted", bv)) ci->wasDeleted = bv ? 1 : 0;
	if (JsonFindBool(response, "seize", bv)) ci->shouldSeize = bv ? 1 : 0;
	if (JsonFindBool(response, "expired", bv)) ci->isExpired = bv ? 1 : 0;
	if (JsonFindBool(response, "inactive", bv)) ci->isInactiveNow = bv ? 1 : 0;
	if (JsonFindBool(response, "needManagerConfirm", bv)) ci->needManager = bv ? 1 : 0;
	if (JsonFindBool(response, "blocked", bv)) ci->isBlocked = bv ? 1 : 0;
	std::string s;
	if (JsonFindString(response, "blockReason", s)) CopyAsciiz(ci->blockReason, sizeof(ci->blockReason), s);
	if (JsonFindString(response, "ownerName", s)) CopyAsciiz(ci->ownerName, sizeof(ci->ownerName), s);
	int64_t i64 = 0;
	DWORD u32 = 0;
	WORD u16 = 0;
	if (JsonFindInt64(response, "ownerId", i64)) ci->ownerId = i64;
	if (JsonFindUInt32(response, "accountNumber", u32)) ci->accountNumber = u32;
	if (JsonFindUInt32(response, "debtorType", u32)) ci->nonPayerType = u32;
	if (JsonFindUInt16(response, "bonusNo", u16)) ci->bonusNumber = u16;
	if (JsonFindUInt16(response, "discountNo", u16)) ci->discountNumber = u16;
	if (JsonFindInt64(response, "discountLimit", i64)) ci->maxDiscountSumKopecks = i64;
	std::vector<int64_t> balances;
	if (JsonFindArrayInt64(response, "accountBalances", balances)) {
		for (size_t i = 0; i < balances.size() && i < 8; ++i) ci->accountBalances[i] = balances[i];
	}
	if (JsonFindString(response, "infoArbitrary", s)) CopyAsciiz(ci->cardInfo, sizeof(ci->cardInfo), s);
	if (JsonFindString(response, "infoDisplay", s)) CopyAsciiz(ci->displayInfo, sizeof(ci->displayInfo), s);
	if (JsonFindString(response, "infoPrint", s)) CopyAsciiz(ci->printInfo, sizeof(ci->printInfo), s);
	LogInfo(L"GetCardInfoEx: OK");
	return 0;
}

extern "C" __declspec(dllexport) int __stdcall TransactionsEx(
	DWORD Count,
	void *List,
	void *InpBuf,
	DWORD InpLen,
	WORD InpKind,
	void* &OutBuf,
	DWORD &OutLen,
	WORD &OutKind
)
{
	OutBuf = nullptr;
	OutLen = 0;
	OutKind = 0;
	if (Count > 0 && List == nullptr) return 1;
	// Build JSON
	std::ostringstream os;
	os << "{";
	os << "\"count\":" << Count << ",";
	os << "\"inpKind\":" << InpKind << ",";
	if (InpBuf != nullptr && InpLen > 0) {
		std::string b64 = Base64Encode(InpBuf, InpLen);
		os << "\"inpBufBase64\":\"" << b64 << "\",";
	} else {
		os << "\"inpBufBase64\":\"\",";
	}
	os << "\"transactions\":[";
	void **ptrs = reinterpret_cast<void**>(List);
	for (DWORD i = 0; i < Count; ++i) {
		if (i > 0) os << ",";
		TransactionEx122 *tr = reinterpret_cast<TransactionEx122*>(ptrs[i]);
		if (!tr || tr->structureSize != sizeof(TransactionEx122)) {
			// malformed entry
			os << "{}";
			continue;
		}
		os << "{";
		os << "\"card\":" << tr->cardNumber << ",";
		os << "\"ownerId\":" << tr->ownerId << ",";
		os << "\"accountNumber\":" << tr->accountNumber << ",";
		os << "\"type\":" << (unsigned)tr->transactionType << ",";
		os << "\"amount\":" << tr->amountKopecks << ",";
		os << "\"restaurant\":" << tr->restaurantCode << ",";
		os << "\"cashDate\":" << tr->cashDate << ",";
		os << "\"unitNo\":" << (unsigned)tr->unitNumber << ",";
		os << "\"checkNo\":" << tr->checkNumber << ",";
		os << "\"taxes\":[";
		struct TaxPair { int64_t sum; WORD perc; };
		TaxPair taxes[8] = {
			{tr->taxSumA, tr->taxPercA},{tr->taxSumB, tr->taxPercB},{tr->taxSumC, tr->taxPercC},{tr->taxSumD, tr->taxPercD},
			{tr->taxSumE, tr->taxPercE},{tr->taxSumF, tr->taxPercF},{tr->taxSumG, tr->taxPercG},{tr->taxSumH, tr->taxPercH}
		};
		for (int t = 0; t < 8; ++t) {
			if (t > 0) os << ",";
			os << "{";
			os << "\"sum\":" << taxes[t].sum << ",";
			os << "\"percentX100\":" << taxes[t].perc;
			os << "}";
		}
		os << "]"; // taxes
		os << "}"; // transaction
	}
	os << "]"; // transactions
	os << "}";

	std::wstring host, path, txPath, apiKey;
	LoadConfig(host, path, txPath, apiKey);
	std::string response;
	LogInfo(L"TransactionsEx count=%u inpLen=%u", Count, InpLen);
	if (!HttpPostJson(host, txPath, os.str(), response, apiKey)) {
		LogError(L"TransactionsEx request failed");
		return 1;
	}
	// Out data passthrough if any
	if (!response.empty()) {
		size_t toCopy = std::min<size_t>(response.size(), MAX_OUT_LEN);
		HGLOBAL h = GlobalAlloc(GMEM_FIXED, toCopy);
		if (h) {
			std::memcpy(h, response.data(), toCopy);
			OutBuf = h;
			OutLen = (DWORD)toCopy;
			DWORD kind = 0;
			JsonFindUInt32(response, "outKind", kind);
			OutKind = (WORD)kind;
		}
	}
	bool allProcessed = false;
	JsonFindBool(response, "allProcessed", allProcessed);
	int rc = allProcessed ? 0 : 1;
	LogInfo(L"TransactionsEx: %s", rc==0?L"OK":L"FAILED");
	return rc;
}

// Optional DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		InitLoggingOnce();
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}


