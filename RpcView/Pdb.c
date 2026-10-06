#define _CRT_SECURE_NO_WARNINGS
#include "Pdb.h"
#include <conio.h>
#include <Strsafe.h>
#include <Dbghelp.h>
#include <winhttp.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>

#define RSDS_SIGNATURE 'SDSR'
#define PDB_MAX_SYMBOL_SIZE	1000

//Only for PDB2.0 format!
struct CV_INFO_PDB20{
  DWORD	CvSignature;
  DWORD	Offset;
  DWORD Signature;
  DWORD Age;
  BYTE PdbFileName[MAX_PATH];
};

//Only for PDB7.0 format!
typedef struct _CV_INFO_PDB70{
	DWORD	CvSignature;
	GUID	Signature;
	DWORD	Age;
	BYTE	PdbFileName[MAX_PATH];
} CV_INFO_PDB70;


typedef struct _PdbCtxt_T{
	HANDLE	hProcess;
	void*	pModuleBase;
	ULONG	ModuleSize;
}PdbCtxt_T;


//------------------------------------------------------------------------------
BOOL WINAPI GetModulePdbInfo(HANDLE hProcess, VOID* pModuleBase, CV_INFO_PDB70* pPdb70Info)
{
	UCHAR*					pBase = (UCHAR*)pModuleBase;
	IMAGE_DOS_HEADER		ImageDosHeader;
	IMAGE_NT_HEADERS		ImageNtHeaders;
	IMAGE_DEBUG_DIRECTORY	ImageDebugDirectory;
	BOOL					bResult = FALSE;
#ifdef _WIN64
	BOOL					bWow64;
	IMAGE_NT_HEADERS32		ImageNtHeaders32;

	if (hProcess==NULL) hProcess = GetCurrentProcess();

	if (!IsWow64Process(hProcess, &bWow64)) goto End;
	if (bWow64==TRUE)
	{
		if (!ReadProcessMemory(hProcess, pModuleBase, &ImageDosHeader, sizeof(ImageDosHeader), NULL)) goto End;
		if (!ReadProcessMemory(hProcess, pBase + ImageDosHeader.e_lfanew, &ImageNtHeaders32, sizeof(ImageNtHeaders32), NULL)) goto End;
		if (!ReadProcessMemory(hProcess, pBase + ImageNtHeaders32.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress, &ImageDebugDirectory, sizeof(ImageDebugDirectory), NULL)) goto End;
		if (!ReadProcessMemory(hProcess, pBase + ImageDebugDirectory.AddressOfRawData, pPdb70Info, sizeof(*pPdb70Info), NULL)) goto End;
	}
	else
#endif
	{
		if (!ReadProcessMemory(hProcess, pModuleBase, &ImageDosHeader, sizeof(ImageDosHeader), NULL)) goto End;
		if (!ReadProcessMemory(hProcess, pBase + ImageDosHeader.e_lfanew, &ImageNtHeaders, sizeof(ImageNtHeaders), NULL)) goto End;
		if (!ReadProcessMemory(hProcess, pBase + ImageNtHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress, &ImageDebugDirectory, sizeof(ImageDebugDirectory), NULL)) goto End;
		if (!ReadProcessMemory(hProcess, pBase + ImageDebugDirectory.AddressOfRawData, pPdb70Info, sizeof(*pPdb70Info), NULL)) goto End;
	}
	pPdb70Info->PdbFileName[MAX_PATH - 1] = 0;
	if (pPdb70Info->CvSignature != RSDS_SIGNATURE )
	{
		_cprintf("Invalid CvSignature");
		goto End;
	}
	bResult = TRUE;
End:
	return (bResult);
}


/*
 * dbghelp is single-threaded, and SymCleanup is process-wide. Downloading on
 * this thread used to hold the only dbghelp lock, so later clicks found no
 * names. Downloads run in a child process. Name lookup keeps one symbol
 * engine for the life of the GUI and never calls SymCleanup.
 */
typedef struct _PdbDownloadJob_T {
	CHAR	SymbolPath[1024];
	CHAR	PdbFileName[MAX_PATH];
	GUID	Signature;
	DWORD	Age;
} PdbDownloadJob_T;

static CRITICAL_SECTION		gQueueLock;
static INIT_ONCE			gPdbOnce = INIT_ONCE_STATIC_INIT;
static HANDLE				gWorkerEvent = NULL;
static HANDLE				gWorkerThread = NULL;
static HANDLE				gSymProcess = NULL;
static PdbDownloadJob_T*	gJobs = NULL;
static int					gJobCapacity = 0;
static int					gJobCount = 0;
static volatile LONG		gDownloadNotice = 0;
static volatile LONG		gQueueReady = 0;
static CHAR					gDlName[MAX_PATH];
static int					gDlPercent = 0;
static BOOL					gDlActive = FALSE;
static BOOL					gDlLinger = FALSE;
static DWORD				gDlLingerStart = 0;

#define HTTP_SAVED       1
#define HTTP_NOT_FOUND   2
#define HTTP_COMPRESSED  3
#define HTTP_FAILED      0
#define PDB_HTTP_CHUNK   (64 * 1024)
#define PDB_LINGER_MS    1200


//------------------------------------------------------------------------------
static BOOL RpcViewReadSymbolPath(CHAR* pPath, DWORD PathSize)
{
	if (GetEnvironmentVariableA("RpcViewSymbolPath", pPath, PathSize) > 0) return TRUE;
	if (GetEnvironmentVariableA("_NT_SYMBOL_PATH", pPath, PathSize) > 0) return TRUE;
	pPath[0] = 0;
	return FALSE;
}


//------------------------------------------------------------------------------
static void SetDownloadProgress(const char* pName, int Percent, BOOL Active, BOOL Linger)
{
	EnterCriticalSection(&gQueueLock);
	if (pName != NULL && pName[0] != 0)
		StringCbCopyA(gDlName, sizeof(gDlName), pName);
	gDlPercent = Percent;
	gDlActive = Active;
	if (Linger)
	{
		gDlLinger = TRUE;
		gDlLingerStart = GetTickCount();
	}
	else
	{
		gDlLinger = FALSE;
	}
	LeaveCriticalSection(&gQueueLock);
}


//------------------------------------------------------------------------------
static BOOL SplitSrvPath(const char* pSymbolPath, char* pCache, size_t CacheBytes, char* pServer, size_t ServerBytes)
{
	const char*	p;
	const char*	pStar;
	size_t		CacheLen;

	pCache[0] = 0;
	pServer[0] = 0;
	if (pSymbolPath == NULL) return FALSE;
	if (_strnicmp(pSymbolPath, "srv*", 4) != 0) return FALSE;
	p = pSymbolPath + 4;
	pStar = strchr(p, '*');
	if (pStar == NULL || pStar == p) return FALSE;
	CacheLen = (size_t)(pStar - p);
	if (CacheLen + 1 > CacheBytes) return FALSE;
	CopyMemory(pCache, p, CacheLen);
	pCache[CacheLen] = 0;
	if (FAILED(StringCbCopyA(pServer, ServerBytes, pStar + 1))) return FALSE;
	return pCache[0] != 0 && pServer[0] != 0;
}


//------------------------------------------------------------------------------
static void BuildGuidAge(const PdbDownloadJob_T* pJob, char* pOut, size_t OutBytes)
{
	StringCbPrintfA(pOut, OutBytes, "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
		pJob->Signature.Data1,
		pJob->Signature.Data2,
		pJob->Signature.Data3,
		pJob->Signature.Data4[0],
		pJob->Signature.Data4[1],
		pJob->Signature.Data4[2],
		pJob->Signature.Data4[3],
		pJob->Signature.Data4[4],
		pJob->Signature.Data4[5],
		pJob->Signature.Data4[6],
		pJob->Signature.Data4[7],
		pJob->Age);
}


//------------------------------------------------------------------------------
static BOOL BuildJobDestPath(const PdbDownloadJob_T* pJob, const char* pCache, char* pDest, size_t DestBytes)
{
	char GuidAge[64];

	BuildGuidAge(pJob, GuidAge, sizeof(GuidAge));
	return SUCCEEDED(StringCbPrintfA(pDest, DestBytes, "%s\\%s\\%s\\%s", pCache, pJob->PdbFileName, GuidAge, pJob->PdbFileName));
}


//------------------------------------------------------------------------------
static BOOL BuildPdbUrl(const char* pServer, const char* pFileName, const char* pGuidAge, WCHAR* pUrl, DWORD Cch)
{
	char	Base[1024];
	char	Full[1600];
	size_t	n;

	if (FAILED(StringCbCopyA(Base, sizeof(Base), pServer))) return FALSE;
	n = strlen(Base);
	while (n > 0 && (Base[n - 1] == '/' || Base[n - 1] == '\\'))
	{
		n--;
		Base[n] = 0;
	}
	if (FAILED(StringCbPrintfA(Full, sizeof(Full), "%s/%s/%s/%s", Base, pFileName, pGuidAge, pFileName))) return FALSE;
	return MultiByteToWideChar(CP_ACP, 0, Full, -1, pUrl, (int)Cch) > 0;
}


//------------------------------------------------------------------------------
static void CreateParentDirsA(const char* pFilePath)
{
	char	Path[1024];
	char*	p;

	if (FAILED(StringCbCopyA(Path, sizeof(Path), pFilePath))) return;
	p = Path;
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
		p += 3;
	for (; *p != 0; p++)
	{
		if (*p == '\\' || *p == '/')
		{
			*p = 0;
			CreateDirectoryA(Path, NULL);
			*p = '\\';
		}
	}
}


//------------------------------------------------------------------------------
static BOOL MakeCompressedName(const char* pName, char* pOut, size_t OutBytes)
{
	size_t n;

	if (FAILED(StringCbCopyA(pOut, OutBytes, pName))) return FALSE;
	n = strlen(pOut);
	if (n == 0) return FALSE;
	pOut[n - 1] = '_';
	return TRUE;
}


//------------------------------------------------------------------------------
static void NoteHttpPercent(const char* pDisplayName, unsigned __int64 Done, unsigned __int64 Expected, int* pLast)
{
	int Pct;

	if (Expected > 0)
	{
		Pct = (int)((Done * 100) / Expected);
		if (Pct > 99) Pct = 99;
	}
	else
	{
		Pct = -1;
	}
	if (Pct != *pLast)
	{
		SetDownloadProgress(pDisplayName, Pct, TRUE, FALSE);
		*pLast = Pct;
	}
}


//------------------------------------------------------------------------------
static int HttpSaveToFile(PCWSTR pUrl, const char* pPartPath, const char* pDisplayName)
{
	URL_COMPONENTSW	Parts;
	WCHAR			Host[256];
	WCHAR			UrlPath[1800];
	HINTERNET		hSession = NULL;
	HINTERNET		hConnect = NULL;
	HINTERNET		hRequest = NULL;
	HANDLE			hFile = INVALID_HANDLE_VALUE;
	BYTE*			pBuf = NULL;
	BYTE			Head[16];
	DWORD			HeadGot = 0;
	DWORD			Status = 0;
	DWORD			StatusBytes = sizeof(Status);
	DWORD			ContentLength = 0;
	DWORD			LengthBytes = sizeof(ContentLength);
	DWORD			Flags;
	DWORD			Got;
	DWORD			Written;
	unsigned __int64 Done = 0;
	unsigned __int64 Expected = 0;
	int				LastPct = -2;
	int				Result = HTTP_FAILED;

	ZeroMemory(&Parts, sizeof(Parts));
	Parts.dwStructSize = sizeof(Parts);
	Parts.dwSchemeLength = (DWORD)-1;
	Parts.dwHostNameLength = (DWORD)-1;
	Parts.dwUrlPathLength = (DWORD)-1;
	if (!WinHttpCrackUrl(pUrl, 0, 0, &Parts)) goto End;
	if (Parts.dwHostNameLength == 0 || Parts.dwHostNameLength >= _countof(Host)) goto End;
	if (Parts.dwUrlPathLength >= _countof(UrlPath)) goto End;
	CopyMemory(Host, Parts.lpszHostName, Parts.dwHostNameLength * sizeof(WCHAR));
	Host[Parts.dwHostNameLength] = 0;
	CopyMemory(UrlPath, Parts.lpszUrlPath, Parts.dwUrlPathLength * sizeof(WCHAR));
	UrlPath[Parts.dwUrlPathLength] = 0;

	hSession = WinHttpOpen(L"Microsoft-Symbol-Server/10.0.22621.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (hSession == NULL) goto End;
	WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 600000);
	hConnect = WinHttpConnect(hSession, Host, Parts.nPort, 0);
	if (hConnect == NULL) goto End;
	Flags = (Parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	hRequest = WinHttpOpenRequest(hConnect, L"GET", UrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Flags);
	if (hRequest == NULL) goto End;
	if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) goto End;
	if (!WinHttpReceiveResponse(hRequest, NULL)) goto End;
	WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &StatusBytes, WINHTTP_NO_HEADER_INDEX);
	if (Status == HTTP_STATUS_NOT_FOUND)
	{
		Result = HTTP_NOT_FOUND;
		goto End;
	}
	if (Status != HTTP_STATUS_OK) goto End;
	if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &ContentLength, &LengthBytes, WINHTTP_NO_HEADER_INDEX))
		Expected = ContentLength;
	if (Expected == 0)
	{
		WCHAR LenText[32];
		DWORD LenTextBytes = sizeof(LenText);
		if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, LenText, &LenTextBytes, WINHTTP_NO_HEADER_INDEX))
			Expected = _wcstoui64(LenText, NULL, 10);
	}

	hFile = CreateFileA(pPartPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) goto End;
	pBuf = (BYTE*)malloc(PDB_HTTP_CHUNK);
	if (pBuf == NULL) goto End;
	NoteHttpPercent(pDisplayName, 0, Expected, &LastPct);

	for (;;)
	{
		DWORD Copy;

		if (!WinHttpReadData(hRequest, pBuf, PDB_HTTP_CHUNK, &Got)) goto End;
		if (Got == 0) break;
		if (!WriteFile(hFile, pBuf, Got, &Written, NULL) || Written != Got) goto End;
		if (HeadGot < sizeof(Head))
		{
			Copy = Got;
			if (Copy > sizeof(Head) - HeadGot) Copy = (DWORD)(sizeof(Head) - HeadGot);
			CopyMemory(Head + HeadGot, pBuf, Copy);
			HeadGot += Copy;
		}
		Done += Got;
		if (HeadGot >= 4 && memcmp(Head, "MSCF", 4) == 0)
		{
			Result = HTTP_COMPRESSED;
			goto End;
		}
		if (HeadGot >= 15 && memcmp(Head, "Microsoft C/C++", 15) != 0) goto End;
		NoteHttpPercent(pDisplayName, Done, Expected, &LastPct);
	}

	if (HeadGot >= 15 && memcmp(Head, "Microsoft C/C++", 15) == 0 && (Expected == 0 || Done == Expected))
		Result = HTTP_SAVED;
End:
	if (pBuf != NULL) free(pBuf);
	if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
	if (hRequest != NULL) WinHttpCloseHandle(hRequest);
	if (hConnect != NULL) WinHttpCloseHandle(hConnect);
	if (hSession != NULL) WinHttpCloseHandle(hSession);
	if (Result != HTTP_SAVED) DeleteFileA(pPartPath);
	return Result;
}


//------------------------------------------------------------------------------
static BOOL RunSymFetchChild(const PdbDownloadJob_T* pJob, PCWSTR pExePath, PCWSTR pWorkDir)
{
	WCHAR				Command[4096];
	STARTUPINFOW		Startup;
	PROCESS_INFORMATION	ProcessInfo;
	BOOL				Started;

	StringCbPrintfW(Command, sizeof(Command),
		L"\"%s\" /symfetch \"%hs\" \"%hs\" %08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X %u",
		pExePath,
		pJob->SymbolPath,
		pJob->PdbFileName,
		pJob->Signature.Data1,
		pJob->Signature.Data2,
		pJob->Signature.Data3,
		pJob->Signature.Data4[0],
		pJob->Signature.Data4[1],
		pJob->Signature.Data4[2],
		pJob->Signature.Data4[3],
		pJob->Signature.Data4[4],
		pJob->Signature.Data4[5],
		pJob->Signature.Data4[6],
		pJob->Signature.Data4[7],
		pJob->Age);
	ZeroMemory(&Startup, sizeof(Startup));
	Startup.cb = sizeof(Startup);
	Startup.dwFlags = STARTF_USESHOWWINDOW;
	Startup.wShowWindow = SW_HIDE;
	ZeroMemory(&ProcessInfo, sizeof(ProcessInfo));
	Started = CreateProcessW(pExePath, Command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, pWorkDir, &Startup, &ProcessInfo);
	if (!Started) return FALSE;
	WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
	CloseHandle(ProcessInfo.hThread);
	CloseHandle(ProcessInfo.hProcess);
	return TRUE;
}


//------------------------------------------------------------------------------
static BOOL TryHttpPdb(const char* pServer, const char* pFileName, const char* pGuidAge, const char* pPartPath, const char* pDisplayName)
{
	WCHAR Url[2048];

	if (!BuildPdbUrl(pServer, pFileName, pGuidAge, Url, _countof(Url))) return FALSE;
	return HttpSaveToFile(Url, pPartPath, pDisplayName) == HTTP_SAVED;
}


//------------------------------------------------------------------------------
static BOOL DownloadOneJob(const PdbDownloadJob_T* pJob, PCWSTR pExePath, PCWSTR pWorkDir)
{
	char	Cache[1024];
	char	Server[1024];
	char	Dest[1024];
	char	Part[1100];
	char	GuidAge[64];
	char	Compressed[MAX_PATH];
	BOOL	HttpOk = FALSE;

	if (!SplitSrvPath(pJob->SymbolPath, Cache, sizeof(Cache), Server, sizeof(Server)))
	{
		SetDownloadProgress(pJob->PdbFileName, -1, TRUE, FALSE);
		RunSymFetchChild(pJob, pExePath, pWorkDir);
		return FALSE;
	}
	if (!BuildJobDestPath(pJob, Cache, Dest, sizeof(Dest))) return FALSE;
	if (GetFileAttributesA(Dest) != INVALID_FILE_ATTRIBUTES)
	{
		SetDownloadProgress(pJob->PdbFileName, 100, TRUE, FALSE);
		return TRUE;
	}
	CreateParentDirsA(Dest);
	if (FAILED(StringCbPrintfA(Part, sizeof(Part), "%s.part", Dest))) return FALSE;
	BuildGuidAge(pJob, GuidAge, sizeof(GuidAge));
	SetDownloadProgress(pJob->PdbFileName, 0, TRUE, FALSE);

	HttpOk = TryHttpPdb(Server, pJob->PdbFileName, GuidAge, Part, pJob->PdbFileName);
	if (!HttpOk && MakeCompressedName(pJob->PdbFileName, Compressed, sizeof(Compressed)))
		HttpOk = TryHttpPdb(Server, Compressed, GuidAge, Part, pJob->PdbFileName);
	if (HttpOk)
	{
		if (MoveFileExA(Part, Dest, MOVEFILE_REPLACE_EXISTING))
		{
			SetDownloadProgress(pJob->PdbFileName, 100, TRUE, FALSE);
			return TRUE;
		}
		DeleteFileA(Part);
	}

	/* Cab-compressed or unreachable files still go through symsrv, which can unpack them. */
	SetDownloadProgress(pJob->PdbFileName, -1, TRUE, FALSE);
	RunSymFetchChild(pJob, pExePath, pWorkDir);
	if (GetFileAttributesA(Dest) != INVALID_FILE_ATTRIBUTES)
	{
		SetDownloadProgress(pJob->PdbFileName, 100, TRUE, FALSE);
		return TRUE;
	}
	return FALSE;
}


//------------------------------------------------------------------------------
static DWORD WINAPI PdbDownloadThread(LPVOID Parameter)
{
	WCHAR	ExePath[MAX_PATH];
	WCHAR	WorkDir[MAX_PATH];
	WCHAR*	pSlash;

	UNREFERENCED_PARAMETER(Parameter);
	if (GetModuleFileNameW(NULL, ExePath, _countof(ExePath)) == 0) return 0;
	StringCbCopyW(WorkDir, sizeof(WorkDir), ExePath);
	pSlash = wcsrchr(WorkDir, L'\\');
	if (pSlash != NULL) *pSlash = 0;

	for (;;)
	{
		PdbDownloadJob_T	Job;
		BOOL				Ok;
		int					More;

		WaitForSingleObject(gWorkerEvent, INFINITE);
		for (;;)
		{
			EnterCriticalSection(&gQueueLock);
			if (gJobCount == 0)
			{
				LeaveCriticalSection(&gQueueLock);
				break;
			}
			Job = gJobs[0];
			MoveMemory(&gJobs[0], &gJobs[1], (gJobCount - 1) * sizeof(gJobs[0]));
			gJobCount--;
			StringCbCopyA(gDlName, sizeof(gDlName), Job.PdbFileName);
			gDlPercent = 0;
			gDlActive = TRUE;
			gDlLinger = FALSE;
			LeaveCriticalSection(&gQueueLock);

			Ok = DownloadOneJob(&Job, ExePath, WorkDir);

			EnterCriticalSection(&gQueueLock);
			More = gJobCount;
			LeaveCriticalSection(&gQueueLock);
			if (More == 0)
			{
				if (Ok)
					SetDownloadProgress(Job.PdbFileName, 100, FALSE, TRUE);
				else
					SetDownloadProgress(Job.PdbFileName, 0, FALSE, FALSE);
			}
		}
	}
}


//------------------------------------------------------------------------------
static BOOL CALLBACK PdbWorkerInitOnce(PINIT_ONCE InitOnce, PVOID Parameter, PVOID* Context)
{
	UNREFERENCED_PARAMETER(InitOnce);
	UNREFERENCED_PARAMETER(Parameter);
	UNREFERENCED_PARAMETER(Context);

	InitializeCriticalSection(&gQueueLock);
	InterlockedExchange(&gQueueReady, 1);
	gWorkerEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (gWorkerEvent != NULL)
	{
		gWorkerThread = CreateThread(NULL, 0, PdbDownloadThread, NULL, 0, NULL);
		if (gWorkerThread != NULL) SetThreadPriority(gWorkerThread, THREAD_PRIORITY_BELOW_NORMAL);
	}
	return TRUE;
}


//------------------------------------------------------------------------------
static void EnsurePdbWorker(void)
{
	InitOnceExecuteOnce(&gPdbOnce, PdbWorkerInitOnce, NULL, NULL);
}


//------------------------------------------------------------------------------
static BOOL BuildCachedPdbPath(const CV_INFO_PDB70* pInfo, CHAR* pPdbPath, UINT PdbPathSize)
{
	CHAR		SymbolPath[1024];
	CHAR		CacheRoot[1024];
	CHAR*		pStar;
	const CHAR*	pFileName;
	int			iResult;

	pPdbPath[0] = 0;
	if (pInfo->PdbFileName[0] == 0) return FALSE;
	pFileName = (const CHAR*)pInfo->PdbFileName;
	if (strchr(pFileName, '\\') != NULL || strchr(pFileName, '/') != NULL)
	{
		StringCbCopyA(pPdbPath, PdbPathSize, pFileName);
		return GetFileAttributesA(pPdbPath) != INVALID_FILE_ATTRIBUTES;
	}

	if (!RpcViewReadSymbolPath(SymbolPath, sizeof(SymbolPath))) return FALSE;
	iResult = sscanf(SymbolPath, "srv*%1023s", CacheRoot);
	if (iResult != 1) return FALSE;
	pStar = strchr(CacheRoot, '*');
	if (pStar != NULL) *pStar = 0;
	if (CacheRoot[0] == 0) return FALSE;

	StringCbPrintfA(pPdbPath, PdbPathSize, "%s\\%s\\%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X\\%s",
		CacheRoot,
		pFileName,
		pInfo->Signature.Data1,
		pInfo->Signature.Data2,
		pInfo->Signature.Data3,
		pInfo->Signature.Data4[0],
		pInfo->Signature.Data4[1],
		pInfo->Signature.Data4[2],
		pInfo->Signature.Data4[3],
		pInfo->Signature.Data4[4],
		pInfo->Signature.Data4[5],
		pInfo->Signature.Data4[6],
		pInfo->Signature.Data4[7],
		pInfo->Age,
		pFileName);
	return GetFileAttributesA(pPdbPath) != INVALID_FILE_ATTRIBUTES;
}


//------------------------------------------------------------------------------
static BOOL GrowJobQueue(void)
{
	int					NewCapacity;
	PdbDownloadJob_T*	pNew;

	NewCapacity = (gJobCapacity == 0) ? 64 : gJobCapacity * 2;
	if (NewCapacity < gJobCapacity) return FALSE;
	pNew = (PdbDownloadJob_T*)realloc(gJobs, (size_t)NewCapacity * sizeof(PdbDownloadJob_T));
	if (pNew == NULL) return FALSE;
	gJobs = pNew;
	gJobCapacity = NewCapacity;
	return TRUE;
}


//------------------------------------------------------------------------------
static BOOL QueuePdbDownload(const CV_INFO_PDB70* pInfo)
{
	CHAR		SymbolPath[1024];
	CHAR		FileName[MAX_PATH];
	const CHAR*	pName;
	const CHAR*	pSlash;
	int			i;
	BOOL		Queued = FALSE;

	if (!RpcViewReadSymbolPath(SymbolPath, sizeof(SymbolPath))) return FALSE;
	if (SymbolPath[0] == 0) return FALSE;

	pName = (const CHAR*)pInfo->PdbFileName;
	pSlash = strrchr(pName, '\\');
	if (pSlash == NULL) pSlash = strrchr(pName, '/');
	if (pSlash != NULL) pName = pSlash + 1;
	StringCbCopyA(FileName, sizeof(FileName), pName);
	if (FileName[0] == 0) return FALSE;

	EnsurePdbWorker();
	if (gWorkerEvent == NULL) return FALSE;

	EnterCriticalSection(&gQueueLock);
	for (i = 0; i < gJobCount; i++)
	{
		if (gJobs[i].Age == pInfo->Age &&
			memcmp(&gJobs[i].Signature, &pInfo->Signature, sizeof(GUID)) == 0 &&
			_stricmp(gJobs[i].PdbFileName, FileName) == 0)
		{
			LeaveCriticalSection(&gQueueLock);
			InterlockedExchange(&gDownloadNotice, 1);
			return TRUE;
		}
	}
	if (gJobCount == gJobCapacity && !GrowJobQueue())
	{
		LeaveCriticalSection(&gQueueLock);
		return FALSE;
	}
	StringCbCopyA(gJobs[gJobCount].SymbolPath, sizeof(gJobs[gJobCount].SymbolPath), SymbolPath);
	StringCbCopyA(gJobs[gJobCount].PdbFileName, sizeof(gJobs[gJobCount].PdbFileName), FileName);
	gJobs[gJobCount].Signature = pInfo->Signature;
	gJobs[gJobCount].Age = pInfo->Age;
	gJobCount++;
	Queued = TRUE;
	InterlockedExchange(&gDownloadNotice, 1);
	SetEvent(gWorkerEvent);
	LeaveCriticalSection(&gQueueLock);
	return Queued;
}


//------------------------------------------------------------------------------
int WINAPI PdbQueueModuleIfMissing(HANDLE hProcess, VOID* pModuleBase)
{
	CV_INFO_PDB70	Info;
	CHAR			Path[1024];

	if (hProcess == NULL || pModuleBase == NULL) return -1;
	if (!GetModulePdbInfo(hProcess, pModuleBase, &Info)) return -1;
	if (BuildCachedPdbPath(&Info, Path, sizeof(Path))) return 0;
	if (!QueuePdbDownload(&Info)) return -1;
	return 1;
}


//------------------------------------------------------------------------------
static BOOL GetSymbolCacheRootW(WCHAR* pRoot, DWORD Cch)
{
	CHAR	SymbolPath[1024];
	CHAR	CacheRoot[1024];
	CHAR*	pStar;

	pRoot[0] = 0;
	if (!RpcViewReadSymbolPath(SymbolPath, sizeof(SymbolPath))) return FALSE;
	if (sscanf(SymbolPath, "srv*%1023s", CacheRoot) == 1)
	{
		pStar = strchr(CacheRoot, '*');
		if (pStar != NULL) *pStar = 0;
	}
	else
	{
		StringCbCopyA(CacheRoot, sizeof(CacheRoot), SymbolPath);
	}
	if (CacheRoot[0] == 0) return FALSE;
	return MultiByteToWideChar(CP_ACP, 0, CacheRoot, -1, pRoot, Cch) > 0;
}


static BOOL EnsureSymbolEngine(void)
{
	WCHAR CacheRoot[1024];

	if (gSymProcess != NULL) return TRUE;
	if (!GetSymbolCacheRootW(CacheRoot, _countof(CacheRoot))) return FALSE;
	gSymProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (gSymProcess == NULL) return FALSE;
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_AUTO_PUBLICS | SYMOPT_NO_PROMPTS | SYMOPT_INCLUDE_32BIT_MODULES);
	if (!SymInitializeW(gSymProcess, CacheRoot, FALSE))
	{
		CloseHandle(gSymProcess);
		gSymProcess = NULL;
		return FALSE;
	}
	return TRUE;
}


//------------------------------------------------------------------------------
__checkReturn void* WINAPI PdbInit(__in HANDLE hProcess, __in VOID* pModuleBase, __in UINT ModuleSize, __in_opt PCWSTR pModulePath)
{
	CHAR			PdbPath[1024];
	PdbCtxt_T*		pPdbCtxt = NULL;
	CV_INFO_PDB70	Pdb70Info;
	DWORD64			Loaded;

	EnsurePdbWorker();
	if (!GetModulePdbInfo(hProcess, pModuleBase, &Pdb70Info)) goto End;
	if (!BuildCachedPdbPath(&Pdb70Info, PdbPath, sizeof(PdbPath)))
	{
		QueuePdbDownload(&Pdb70Info);
		goto End;
	}
	if (pModulePath == NULL || pModulePath[0] == 0) goto End;
	if (!EnsureSymbolEngine()) goto End;

	/*
	 * The image must be the DLL. dbghelp does not resolve addresses when the
	 * image argument is the PDB itself. The base is the remote module base.
	 */
	Loaded = SymLoadModuleExW(gSymProcess, NULL, pModulePath, NULL, (DWORD64)pModuleBase, 0, NULL, 0);
	if (Loaded == 0)
	{
		SymUnloadModule64(gSymProcess, (DWORD64)pModuleBase);
		Loaded = SymLoadModuleExW(gSymProcess, NULL, pModulePath, NULL, (DWORD64)pModuleBase, 0, NULL, 0);
		if (Loaded == 0) goto End;
	}

	pPdbCtxt = (PdbCtxt_T*)malloc(sizeof(PdbCtxt_T));
	if (pPdbCtxt == NULL)
	{
		SymUnloadModule64(gSymProcess, (DWORD64)pModuleBase);
		goto End;
	}
	ZeroMemory(pPdbCtxt, sizeof(PdbCtxt_T));
	pPdbCtxt->hProcess = gSymProcess;
	pPdbCtxt->ModuleSize = ModuleSize;
	pPdbCtxt->pModuleBase = pModuleBase;
End:
	return (pPdbCtxt);
}


//------------------------------------------------------------------------------
__checkReturn BOOL WINAPI PdbGetSymbolName(__in void* hCtxt, __in VOID* pSymbol, __out WCHAR* pName, __in UINT NameLength)
{
	BOOL				bResult		= FALSE;
	IMAGEHLP_SYMBOL64*	pSymbolInfo	= NULL;
	DWORD64				dwDisp		= 0;
	PdbCtxt_T*			pPdbCtxt	= (PdbCtxt_T*)hCtxt;

	if (pPdbCtxt==NULL) goto End;

	pSymbolInfo=(IMAGEHLP_SYMBOL64*)malloc( PDB_MAX_SYMBOL_SIZE );
	if (pSymbolInfo==NULL) goto End;
	ZeroMemory(pSymbolInfo, PDB_MAX_SYMBOL_SIZE);

	pSymbolInfo->MaxNameLength	= NameLength;
	pSymbolInfo->SizeOfStruct	= sizeof(IMAGEHLP_SYMBOL64);

	bResult = SymGetSymFromAddr64( pPdbCtxt->hProcess, (DWORD64)pSymbol, &dwDisp, pSymbolInfo);
	if (!bResult)	goto End;
	if (dwDisp!=0)	goto End;
	StringCbPrintfW(pName,NameLength,L"%S",pSymbolInfo->Name);
End:
	if (pSymbolInfo!=NULL) free(pSymbolInfo);
	return (bResult);
}


//------------------------------------------------------------------------------
void WINAPI PdbUninit(__in void* hCtxt)
{
	PdbCtxt_T*			pPdbCtxt	= (PdbCtxt_T*)hCtxt;

	if (pPdbCtxt==NULL) goto End;
	SymUnloadModule64(pPdbCtxt->hProcess,(DWORD64)pPdbCtxt->pModuleBase);
	free(pPdbCtxt);
End:
	return;
}


//------------------------------------------------------------------------------
static int HexDigit(char Ch)
{
	if (Ch >= '0' && Ch <= '9') return Ch - '0';
	if (Ch >= 'a' && Ch <= 'f') return Ch - 'a' + 10;
	if (Ch >= 'A' && Ch <= 'F') return Ch - 'A' + 10;
	return -1;
}


//------------------------------------------------------------------------------
static BOOL ParseGuidText(const char* pText, GUID* pGuid)
{
	BYTE	Bytes[16];
	int		i;
	int		Hi;
	int		Lo;

	if (pText == NULL) return FALSE;
	for (i = 0; i < 16; i++)
	{
		if (*pText == '-') pText++;
		Hi = HexDigit(*pText++);
		Lo = HexDigit(*pText++);
		if (Hi < 0 || Lo < 0) return FALSE;
		Bytes[i] = (BYTE)((Hi << 4) | Lo);
	}
	pGuid->Data1 = ((DWORD)Bytes[0] << 24) | ((DWORD)Bytes[1] << 16) | ((DWORD)Bytes[2] << 8) | Bytes[3];
	pGuid->Data2 = (USHORT)((Bytes[4] << 8) | Bytes[5]);
	pGuid->Data3 = (USHORT)((Bytes[6] << 8) | Bytes[7]);
	CopyMemory(pGuid->Data4, Bytes + 8, 8);
	return TRUE;
}


//------------------------------------------------------------------------------
int WINAPI PdbRunSymbolFetch(const char* pSymbolPath, const char* pPdbName, const char* pGuid, const char* pAge)
{
	GUID	Signature;
	DWORD	Age;
	WCHAR	SymbolPath[1024];
	WCHAR	PdbName[MAX_PATH];
	WCHAR	Found[MAX_PATH];
	HANDLE	hProc = NULL;
	int		Result = 1;

	if (pSymbolPath == NULL || pPdbName == NULL || pGuid == NULL || pAge == NULL) goto End;
	if (!ParseGuidText(pGuid, &Signature)) goto End;
	Age = strtoul(pAge, NULL, 10);
	if (MultiByteToWideChar(CP_ACP, 0, pSymbolPath, -1, SymbolPath, _countof(SymbolPath)) == 0) goto End;
	if (MultiByteToWideChar(CP_ACP, 0, pPdbName, -1, PdbName, _countof(PdbName)) == 0) goto End;
	hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (hProc == NULL) goto End;
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_AUTO_PUBLICS | SYMOPT_NO_PROMPTS);
	if (!SymInitializeW(hProc, SymbolPath, FALSE)) goto End;
	SymFindFileInPathW(hProc, SymbolPath, PdbName, &Signature, Age, 0, SSRVOPT_GUIDPTR, Found, NULL, NULL);
	SymCleanup(hProc);
	Result = 0;
End:
	if (hProc != NULL) CloseHandle(hProc);
	return Result;
}


//------------------------------------------------------------------------------
BOOL WINAPI PdbConsumeDownloadNotice(void)
{
	return InterlockedExchange(&gDownloadNotice, 0) != 0;
}


//------------------------------------------------------------------------------
BOOL WINAPI PdbGetDownloadStatus(char* pName, DWORD NameBytes, int* pPercent, BOOL* pActive)
{
	BOOL	Show = FALSE;
	DWORD	Now;

	if (pName == NULL || pPercent == NULL || pActive == NULL || NameBytes == 0) return FALSE;
	pName[0] = 0;
	*pPercent = 0;
	*pActive = FALSE;
	if (InterlockedCompareExchange(&gQueueReady, 0, 0) == 0) return TRUE;

	EnterCriticalSection(&gQueueLock);
	if (gDlActive)
	{
		StringCbCopyA(pName, NameBytes, gDlName);
		*pPercent = gDlPercent;
		Show = TRUE;
	}
	else if (gJobCount > 0)
	{
		StringCbCopyA(pName, NameBytes, gJobs[0].PdbFileName);
		*pPercent = 0;
		Show = TRUE;
	}
	else if (gDlLinger)
	{
		Now = GetTickCount();
		if ((Now - gDlLingerStart) < PDB_LINGER_MS)
		{
			StringCbCopyA(pName, NameBytes, gDlName);
			*pPercent = 100;
			Show = TRUE;
		}
		else
		{
			gDlLinger = FALSE;
		}
	}
	LeaveCriticalSection(&gQueueLock);
	*pActive = Show;
	return TRUE;
}