// ExtDll.cpp
// ExtDLL for R-Keeper FarCards. Implements GetCardInfoEx and TransactionsEx.
// Build: cl /LD ExtDll.cpp /Fe:ExtDll.dll /link winhttp.lib

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <sstream>
#include <cstdint>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

// --- Типы данных по спецификации (с pack 1) ---
#pragma pack(push,1)
typedef uint16_t Word;
typedef uint8_t  Byte;
typedef uint32_t DWORDu;
typedef int64_t  Int64;

struct CardInfoStruct {
    Word    StructSize;              // =1164 (заполнено при вызове)
    Byte    Deleted;                 // 0/1
    Byte    NeedToTakeCard;          // 0/1
    Byte    Expired;                 // 0/1
    Byte    CurrentlyNotActive;      // 0/1
    Byte    NeedManagerConfirm;      // 0/1
    Byte    CardBlocked;             // 0/1
    char    BlockReason[256];        // asciiz
    char    HolderName[40];          // asciiz
    Int64   OwnerId;
    DWORDu  AccountNumber;
    DWORDu  NonPayerType;
    Word    BonusNumber;
    Word    DiscountNumber;
    Int64   MaxDiscountAmount;       // копейки
    Int64   AvailableToPay;          // копейки
    Int64   Account2;                // копейк
    Int64   Account3;
    Int64   Account4;
    Int64   Account5;
    Int64   Account6;
    Int64   Account7;
    Int64   Account8;
    char    ArbitraryInfo[256];
    char    DisplayInfo[256];
    char    PrintInfo[256];
    // total size should be 1164 bytes
};
#pragma pack(pop)

#pragma pack(push,1)
struct TransactionRecord {
    Word    StructSize;   // =122
    Int64   Card;
    Int64   OwnerId;
    DWORDu  AccountNumber;
    Byte    TxType;       // 0 payment,1 discount,2 bonus,3 guest pay
    Int64   Amount;       // in kopecks (see spec)
    Word    RestaurantCode;
    DWORDu  CashDate;     // (0 -> 30/12/1899)
    Byte    CashRegisterNo;
    DWORDu  CheckNumber;
    // taxes: 8 blocks
    Int64   TaxAmountA; Word TaxPercentA;
    Int64   TaxAmountB; Word TaxPercentB;
    Int64   TaxAmountC; Word TaxPercentC;
    Int64   TaxAmountD; Word TaxPercentD;
    Int64   TaxAmountE; Word TaxPercentE;
    Int64   TaxAmountF; Word TaxPercentF;
    Int64   TaxAmountG; Word TaxPercentG;
    Int64   TaxAmountH; Word TaxPercentH;
};
#pragma pack(pop)

// --- Конфигурация (можно подправить в Extdll.ini) ---
static std::wstring g_getCardPath = L"/get_card_info";
static std::wstring g_transactionsPath = L"/transactionsex";
static std::wstring g_host = L"complete-relieved-martin.ngrok-free.app";
static INTERNET_PORT g_port = INTERNET_DEFAULT_HTTPS_PORT;

// --- Небольшие утилиты ---

static void SafeFreeOut(void* p) {
    if (!p) return;
    // Мы выделяем через GlobalAlloc(GMEM_FIXED), поэтому освобождать нужно GlobalFree.
    GlobalFree(p);
}

// Простой HTTP POST (WinHTTP). Возвращает строку ответа (utf-8).
static std::string HttpPostJson(const std::wstring& host, INTERNET_PORT port, const std::wstring& path, const std::string& jsonBody) {
    std::string result;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    hSession = WinHttpOpen(L"ExtDll-FarCards/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto cleanup;

    hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) goto cleanup;

    hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE);
    if (!hRequest) goto cleanup;

    // SSL: ignore cert errors for ngrok/test environments (WARNING: remove/adjust for prod)
    DWORD dwFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwFlags, sizeof(dwFlags));

    std::wstring headers = L"Content-Type: application/json\r\n";

    BOOL bResult = WinHttpSendRequest(hRequest,
                                      headers.c_str(), (DWORD)headers.length(),
                                      (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(),
                                      (DWORD)jsonBody.length(), 0);
    if (!bResult) goto cleanup;

    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

    // Читаем ответ
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> buffer(dwSize + 1);
        if (!WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded)) break;
        buffer[dwDownloaded] = 0;
        result.append(buffer.data(), dwDownloaded);
    } while (dwSize > 0);

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

// Простой поиск числового поля в JSON-ответе вида "... "balance": 12345 ..."
static bool ExtractJsonInt64(const std::string& json, const std::string& key, Int64& outValue) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find_first_of(":", pos);
    if (pos == std::string::npos) return false;
    pos++;
    // пропускаем пробелы
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos]=='\n' || json[pos]=='\r')) pos++;
    // считываем число (возможно в кавычках)
    bool inQuotes = false;
    if (json[pos] == '"') { inQuotes = true; pos++; }
    size_t start = pos;
    bool neg = false;
    if (pos < json.size() && json[pos]=='-') { neg = true; pos++; }
    while (pos < json.size() && (isdigit((unsigned char)json[pos]))) pos++;
    std::string numstr = json.substr(start, pos - start);
    if (numstr.empty()) {
        if (inQuotes) {
            // может быть "null" or empty
            return false;
        }
    }
    try {
        outValue = std::stoll(numstr);
    } catch (...) { return false; }
    return true;
}

// Простой поиск строкового поля в JSON-ответе вида ... "name":"Ivan" ...
static bool ExtractJsonString(const std::string& json, const std::string& key, std::string& outStr) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find_first_of(":", pos);
    if (pos == std::string::npos) return false;
    pos++;
    // пропускаем пробелы
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos]=='\n' || json[pos]=='\r')) pos++;
    // ищем начало кавычек
    if (pos >= json.size() || json[pos] != '"') return false;
    pos++;
    size_t start = pos;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) pos += 2; // пропускаем escaped
        else pos++;
    }
    if (pos >= json.size()) return false;
    outStr = json.substr(start, pos - start);
    return true;
}

// Helper: allocate OutBuf via GlobalAlloc and copy data (utf-8)
static bool SetOutBuffer(void** OutBuf, DWORDu* OutLen, Word* OutKind, const std::string& content, Word kind = 2) {
    if (!OutBuf || !OutLen || !OutKind) return false;
    SIZE_T len = content.size();
    if (len == 0) {
        *OutBuf = NULL;
        *OutLen = 0;
        *OutKind = kind;
        return true;
    }
    // ограничение: не более 150 KB (~153600). Проверяем.
    const SIZE_T MAX_LEN = 150 * 1024;
    if (len > MAX_LEN) len = MAX_LEN;
    HGLOBAL h = GlobalAlloc(GMEM_FIXED, len + 1);
    if (!h) return false;
    memcpy((void*)h, content.data(), len);
    ((char*)h)[len] = 0;
    *OutBuf = (void*)h;
    *OutLen = (DWORDu)len;
    *OutKind = kind;
    return true;
}

// --- Экспортируемые функции ---

extern "C" __declspec(dllexport)
int __stdcall GetCardInfoEx (
    Int64 Card,
    DWORDu Restaurant,
    DWORDu UnitNo,
    void* InfoPtr,
    void* InpBuf,
    DWORDu InpLen,
    Word InpKind,
    void** OutBuf,
    DWORDu* OutLen,
    Word* OutKind
) {
    if (!InfoPtr) return 1;
    CardInfoStruct* info = (CardInfoStruct*)InfoPtr;

    // Инициализация структуры (заполняем StructSize и обнуляем)
    ZeroMemory(info, sizeof(CardInfoStruct));
    info->StructSize = 1164;

    // Подготовим JSON-запрос
    std::ostringstream ss;
    ss << "{";
    ss << "\"card\":" << Card;
    ss << ",\"restaurant\":" << Restaurant;
    ss << ",\"unit_no\":" << UnitNo;
    // если в InpBuf передана дополнительная информация (XML), отправим её как строку (utf-8)
    if (InpBuf && InpLen > 0) {
        std::string inp((char*)InpBuf, InpLen);
        // экранируем простым способом
        ss << ",\"inp\":\"";
        for (char c : inp) {
            if (c == '"' || c == '\\') ss << '\\' << c;
            else if (c == '\n') ss << "\\n";
            else if (c == '\r') ss << "\\r";
            else ss << c;
        }
        ss << "\"";
    }
    ss << "}";

    std::string requestBody = ss.str();

    // Отправляем запрос к /get_card_info
    std::string response = HttpPostJson(g_host, g_port, g_getCardPath, requestBody);
    if (response.empty()) {
        // не удалось получить ответ — считаем карту не найденной
        SetOutBuffer(OutBuf, OutLen, OutKind, std::string("{\"error\":\"no response\"}"), 2);
        return 1;
    }

    // Простой разбор JSON-ответа (ожидаем поля: found (bool/int), balance (int), name (string), owner_id (int) )
    // Примеры ожидаемого JSON: {"found":true, "balance":350, "name":"Ivan Ivanov", "owner_id":12345, "display":"..."}
    bool found = false;
    // ищем "found"
    size_t posFound = response.find("\"found\"");
    if (posFound != std::string::npos) {
        size_t p = response.find(":", posFound);
        if (p!=std::string::npos) {
            p++;
            while (p < response.size() && isspace((unsigned char)response[p])) p++;
            if (response.compare(p, 4, "true") == 0) found = true;
            else if (response.compare(p, 4, "True") == 0) found = true;
            else if (response.compare(p, 1, "1") == 0) found = true;
        }
    } else {
        // если нет поля found — будем считать карту найденной, если есть поле balance или name
        if (response.find("\"balance\"") != std::string::npos || response.find("\"name\"") != std::string::npos) {
            found = true;
        }
    }

    if (!found) {
        SetOutBuffer(OutBuf, OutLen, OutKind, response, 2);
        return 1; // карта не существует (внешняя система ответила что нет)
    }

    // Заполняем поля структуры (как минимум name, owner, balance)
    Int64 balance = 0;
    ExtractJsonInt64(response, "balance", balance);

    std::string name;
    ExtractJsonString(response, "name", name);

    Int64 ownerId = 0;
    ExtractJsonInt64(response, "owner_id", ownerId);

    // Заполняем структуру: конвертируем name в ASCII (ограниченно)
    if (!name.empty()) {
        size_t copyLen = std::min(sizeof(info->HolderName) - 1, name.size());
        memcpy(info->HolderName, name.c_str(), copyLen);
        info->HolderName[copyLen] = 0;
    }

    info->OwnerId = ownerId;
    // по умолчанию: не заблокирована
    info->CardBlocked = 0;
    // записываем баланс в одно из полей: AvailableToPay (в копейках)
    info->AvailableToPay = balance; // важно: ожидается в копейках — сервер должен прислать в копейках

    // заполним ArbitraryInfo или DisplayInfo дополнительными данными (копируем полный ответ)
    {
        std::string shortResp = response.substr(0, 250); // ограничим
        size_t c = std::min((size_t)sizeof(info->DisplayInfo) - 1, shortResp.size());
        memcpy(info->DisplayInfo, shortResp.c_str(), c);
        info->DisplayInfo[c] = 0;
    }

    // Заполняем OutBuf ответом сервера (полезно для логики на кассе)
    SetOutBuffer(OutBuf, OutLen, OutKind, response, 2);

    return 0; // OK
}

// TransactionsEx: Count — количество транзакций. List — указатель на массив указателей на структуры TransactionRecord
extern "C" __declspec(dllexport)
int __stdcall TransactionsEx(
    DWORDu Count,
    void* ListPtr,
    void* InpBuf,
    DWORDu InpLen,
    Word InpKind,
    void** OutBuf,
    DWORDu* OutLen,
    Word* OutKind
) {
    if (Count == 0 || !ListPtr) return 1;

    // Преобразуем ListPtr (Pointer на массив указателей)
    void** array = (void**)ListPtr;

    // Сформируем JSON-массив транзакций
    std::ostringstream ss;
    ss << "{ \"transactions\": [";
    for (DWORDu i = 0; i < Count; ++i) {
        void* recptr = array[i];
        if (!recptr) continue;
        TransactionRecord* tr = (TransactionRecord*)recptr;

        // Собираем данные транзакции в JSON
        ss << "{";
        ss << "\"card\":" << tr->Card;
        ss << ",\"owner_id\":" << tr->OwnerId;
        ss << ",\"tx_type\":" << (int)tr->TxType;
        ss << ",\"amount\":" << tr->Amount;
        ss << ",\"account_number\":" << tr->AccountNumber;
        ss << ",\"restaurant\":" << tr->RestaurantCode;
        ss << ",\"cashdate\":" << tr->CashDate;
        ss << ",\"cashregister\":" << (int)tr->CashRegisterNo;
        ss << ",\"check_number\":" << tr->CheckNumber;
        // можно добавить данные по налогам при необходимости
        ss << "}";
        if (i + 1 < Count) ss << ",";
    }
    ss << "]";

    // Прикрепляем InpBuf если есть
    if (InpBuf && InpLen > 0) {
        std::string inp((char*)InpBuf, InpLen);
        ss << ",\"inp\":\"";
        for (char c : inp) {
            if (c == '"' || c == '\\') ss << '\\' << c;
            else if (c == '\n') ss << "\\n";
            else if (c == '\r') ss << "\\r";
            else ss << c;
        }
        ss << "\"";
    }
    ss << "}";

    std::string body = ss.str();

    // Отправляем запрос на /transactionsex
    std::string response = HttpPostJson(g_host, g_port, g_transactionsPath, body);
    if (response.empty()) {
        SetOutBuffer(OutBuf, OutLen, OutKind, std::string("{\"error\":\"no response\"}"), 2);
        return 1;
    }

    // Ожидаем в ответе поле success: true/false или code==0
    bool ok = false;
    size_t pos = response.find("\"success\"");
    if (pos != std::string::npos) {
        size_t p = response.find(":", pos);
        if (p != std::string::npos) {
            p++;
            while (p < response.size() && isspace((unsigned char)response[p])) p++;
            if (response.compare(p, 4, "true") == 0 || response.compare(p, 1, "1") == 0) ok = true;
        }
    } else {
        // fallback: если в ответе есть "ok" или "result":"OK"
        if (response.find("\"ok\"") != std::string::npos || response.find("OK") != std::string::npos) ok = true;
    }

    // Вернём ответ сервера в OutBuf (для дебага/логики)
    SetOutBuffer(OutBuf, OutLen, OutKind, response, 2);

    return ok ? 0 : 1;
}

// Минимальный DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    UNREFERENCED_PARAMETER(hModule);
    UNREFERENCED_PARAMETER(lpReserved);
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        // могу загрузить конфиг из Extdll.ini при необходимости
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
