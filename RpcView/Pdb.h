#ifndef _PDB_H_
#define _PDB_H_

#include <windows.h>

EXTERN_C __checkReturn	void* WINAPI PdbInit(__in HANDLE hProcess, __in VOID* pModuleBase, __in UINT ModuleSize, __in_opt PCWSTR pModulePath);
EXTERN_C __checkReturn	BOOL WINAPI PdbGetSymbolName(__in void* pPdbCtxt, __in VOID* pSymbol, __out WCHAR* pName, __in UINT NameLength);
EXTERN_C				void WINAPI PdbUninit(__in void* pPdbCtxt);
/* TRUE once after a PDB download was queued. The UI thread must not wait on symsrv. */
EXTERN_C				BOOL WINAPI PdbConsumeDownloadNotice(void);
/*
 * NameBytes is the size of pName in bytes. *pPercent is 0-100, or -1 when the
 * size is not known yet. *pActive is TRUE while a download should be shown.
 */
EXTERN_C				BOOL WINAPI PdbGetDownloadStatus(char* pName, DWORD NameBytes, int* pPercent, BOOL* pActive);
/* 1 if a download was queued, 0 if that PDB is already on disk, -1 if the module has no PDB. */
EXTERN_C				int WINAPI PdbQueueModuleIfMissing(HANDLE hProcess, VOID* pModuleBase);
/* Child-process entry. Downloads one PDB and exits. Not used by the GUI thread. */
EXTERN_C				int WINAPI PdbRunSymbolFetch(const char* pSymbolPath, const char* pPdbName, const char* pGuid, const char* pAge);

#endif // _PDB_H_