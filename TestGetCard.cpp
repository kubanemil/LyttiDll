// Minimal console harness to test ExtDll.dll:GetCardInfoEx
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct CardInfo1164 {
	WORD	structureSize;
	BYTE	wasDeleted;
	BYTE	shouldSeize;
	BYTE	isExpired;
	BYTE	isInactiveNow;
	BYTE	needManager;
	BYTE	isBlocked;
	char	blockReason[256];
	char	ownerName[40];
	__int64	ownerId;
	DWORD	accountNumber;
	DWORD	nonPayerType;
	WORD	bonusNumber;
	WORD	discountNumber;
	__int64	maxDiscountSumKopecks;
	__int64	accountBalances[8];
	char	cardInfo[256];
	char	displayInfo[256];
	char	printInfo[256];
};
#pragma pack(pop)

typedef int(__stdcall *GetCardInfoExFn)(
	__int64 Card,
	DWORD Restaurant,
	DWORD UnitNo,
	void *Info,
	void *InpBuf,
	DWORD InpLen,
	WORD InpKind,
	void* &OutBuf,
	DWORD &OutLen,
	WORD &OutKind);

static void PrintCardInfo(const CardInfo1164 &ci) {
	std::cout << "wasDeleted=" << (int)ci.wasDeleted
		<< " shouldSeize=" << (int)ci.shouldSeize
		<< " expired=" << (int)ci.isExpired
		<< " inactive=" << (int)ci.isInactiveNow
		<< " needManager=" << (int)ci.needManager
		<< " blocked=" << (int)ci.isBlocked << "\n";
	std::cout << "ownerName='" << ci.ownerName << "' ownerId=" << (long long)ci.ownerId << "\n";
	std::cout << "accountNumber=" << ci.accountNumber << " debtorType=" << ci.nonPayerType << "\n";
	std::cout << "bonusNo=" << ci.bonusNumber << " discountNo=" << ci.discountNumber << "\n";
	std::cout << "maxDiscount=" << (long long)ci.maxDiscountSumKopecks << "\n";
	std::cout << "balances:";
	for (int i = 0; i < 8; ++i) std::cout << " " << (long long)ci.accountBalances[i];
	std::cout << "\ncardInfo='" << ci.cardInfo << "'\n";
	std::cout << "displayInfo='" << ci.displayInfo << "'\n";
	std::cout << "printInfo='" << ci.printInfo << "'\n";
}

int main(int argc, char** argv) {
	const char* dllPath = (argc > 1) ? argv[1] : "ExtDll.dll"; // allow passing full path
	HMODULE h = LoadLibraryA(dllPath);
	if (!h) {
		std::cerr << "LoadLibrary failed: " << GetLastError() << "\n";
		return 1;
	}
	// Call Init to initialize logging and prove logging works
	typedef void (__stdcall *InitFn)();
	if (auto pInit = reinterpret_cast<InitFn>(GetProcAddress(h, "Init"))) {
		pInit();
	}
	auto pGet = reinterpret_cast<GetCardInfoExFn>(GetProcAddress(h, "GetCardInfoEx"));
	if (!pGet) {
		std::cerr << "GetProcAddress(GetCardInfoEx) failed\n";
		FreeLibrary(h);
		return 1;
	}

	CardInfo1164 ci{};
	ci.structureSize = (WORD)sizeof(CardInfo1164);
	void* outBuf = nullptr;
	DWORD outLen = 0;
	WORD outKind = 0;

	// Example inputs (adjust as needed)
	__int64 card = 1234567890; // test card
	DWORD restaurant = 1;
	DWORD unitNo = 1;
	void* inpBuf = nullptr;
	DWORD inpLen = 0;
	WORD inpKind = 0;

	int rc = pGet(card, restaurant, unitNo, &ci, inpBuf, inpLen, inpKind, outBuf, outLen, outKind);
	std::cout << "GetCardInfoEx rc=" << rc << " outLen=" << outLen << " outKind=" << outKind << "\n";
	if (rc == 0) {
		PrintCardInfo(ci);
	}
	if (outBuf) {
		// Print first bytes as text (may be JSON)
		DWORD toShow = (outLen < 512 ? outLen : 512);
		std::string s((const char*)outBuf, (const char*)outBuf + toShow);
		std::cout << "OutBuf (first " << toShow << "):\n" << s << "\n";
		GlobalFree(outBuf);
	}
	FreeLibrary(h);
	return 0;
}


