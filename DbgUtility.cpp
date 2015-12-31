#include "DbgUtility.h"

#include <TlHelp32.h>
#include <wchar.h>
#include <Psapi.h>
#include <tchar.h>

// macro
#define BUFSIZE 512

// functions
DbgUtility::DbgUtility(std::string file_name) : m_INT3(0xcc)
{
    ZeroMemory(&processInformation, sizeof(processInformation));
    ZeroMemory(&m_startInfo, sizeof(m_startInfo));

    m_filePath = file_name;
    context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
    m_trapFlag = false;
    m_stop = false;
    m_resume = true;
}

DbgUtility::~DbgUtility()
{
    if (NULL != processInformation.hProcess)
    {
        CloseHandle(processInformation.hProcess);
        CloseHandle(processInformation.hThread);
    }
}

HANDLE DbgUtility::GetWindowsHandle()
{
    return processInformation.hProcess;
}

bool DbgUtility::start()
{
    struct stat statBuf;
    if (stat(m_filePath.c_str(), &statBuf) == 0)
    {
        // 프로세스 생성
        if (!CreateProcessA(
            m_filePath.c_str(),
            NULL,
            NULL,
            NULL,
            FALSE,
            DEBUG_PROCESS,
            NULL,
            NULL,
            &m_startInfo,
            &processInformation))
        {
            DbgUtility::dbgPrint("CreateProcess() Failed!");
            return false; // 프로세스의 생성을 실패했을 때.
        }
        
        peParser.setFilePath(m_filePath.c_str());
        peParser.parse();
        DWORD dwContinueDebugStatus = DBG_CONTINUE;
        while (dwContinueDebugStatus && !m_stop)
        {
            if (!m_resume) continue;

            DEBUG_EVENT debugEvent;
            WaitForDebugEvent(&debugEvent, INFINITE);
            switch (debugEvent.dwDebugEventCode)
            {
            case CREATE_PROCESS_DEBUG_EVENT:
                m_oep = (LPVOID)(debugEvent.u.CreateProcessInfo.lpStartAddress);
                m_module = (HMODULE)(debugEvent.u.CreateProcessInfo.lpBaseOfImage);
                CloseHandle(debugEvent.u.CreateProcessInfo.hFile);
                onCreateProcess(debugEvent);
                break;
            case EXCEPTION_DEBUG_EVENT:
                GetThreadContext(processInformation.hThread, &context);
                switch (debugEvent.u.Exception.ExceptionRecord.ExceptionCode)
                {
                case EXCEPTION_BREAKPOINT:
                    if (debugEvent.u.Exception.dwFirstChance)
                    {
                        if (debugEvent.u.Exception.ExceptionRecord.ExceptionAddress == m_oep)
                        {
                            LPVOID IP = (LPVOID)(--context.Eip);
                            WriteProcessMemory(processInformation.hProcess, IP, &m_breakPoint, 1, NULL);
                            FlushInstructionCache(processInformation.hProcess, IP, 1);
                        }
                        else {
                            ReadProcessMemory(processInformation.hProcess, m_oep, &m_breakPoint, 1, NULL);
                            WriteProcessMemory(processInformation.hProcess, m_oep, &m_INT3, 1, NULL);
                            FlushInstructionCache(processInformation.hProcess, m_oep, 1);

                            break;
                        }
                    }
                case EXCEPTION_SINGLE_STEP:
                    if (m_trapFlag) context.EFlags |= 0x100; // set trap flag bit
                    onExcuteOnceInstruction(debugEvent);

                    break;
                }
                context.Dr6 = 0;
                SetThreadContext(processInformation.hThread, &context);
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                dwContinueDebugStatus = 0;
                onExitProcess(debugEvent);
                return true;
            case LOAD_DLL_DEBUG_EVENT:
                CloseHandle(debugEvent.u.LoadDll.hFile);
                break;
            }
            ContinueDebugEvent(debugEvent.dwProcessId, debugEvent.dwThreadId, dwContinueDebugStatus);
        }
    }
    return false; // 파일이 없을 경우.
}

void DbgUtility::setFilePath(const char *file_name)
{
    m_filePath = file_name;
}

void DbgUtility::setFilePath(std::string& file_name)
{
    m_filePath = file_name;
}

std::string DbgUtility::GetTargetFileName()
{
    return m_filePath;
}

bool DbgUtility::checkRemoteDbgPresent(HANDLE hProc)
{
    BOOL bDbgState = false;
    CheckRemoteDebuggerPresent(hProc, &bDbgState);

    return (bDbgState) ? true : false;
}

DWORD DbgUtility::getProcessIdFromeName(std::string& process_name)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    PROCESSENTRY32 pe32 = { 0, };
    pe32.dwSize = sizeof(pe32);

    if (hSnapshot == INVALID_HANDLE_VALUE)
        return NULL;

    if (!Process32First(hSnapshot, &pe32))
    {
        CloseHandle(hSnapshot);
        return NULL;
    }

    do
    {
        if (!process_name.compare(pe32.szExeFile)) return pe32.th32ProcessID;
    } while (Process32Next(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return NULL;
}

HANDLE DbgUtility::getWindowsHandleFromeName(std::string& process_name)
{
    const DWORD dwDestProcId = DbgUtility::getProcessIdFromeName(process_name);
    HANDLE  hTempWnd = FindWindow(NULL, NULL);
    DWORD   dwTempProcId = 0;

    while (hTempWnd != NULL)
    {
        GetWindowThreadProcessId(static_cast<HWND>(hTempWnd), &dwTempProcId);
        if (dwDestProcId == dwTempProcId)
        {
            return hTempWnd;
        }
        hTempWnd = GetWindow(static_cast<HWND>(hTempWnd), GW_HWNDNEXT);
    }

    return NULL;
}

void DbgUtility::dbgPrint(const char *format, ...)
{
    va_list arg_list;
    char tmp[1024] = { 0, };
    va_start(arg_list, format);
    vsprintf_s(tmp, sizeof(tmp), format, arg_list);
    va_end(arg_list);

    OutputDebugString(tmp);
}

void DbgUtility::dbgPrint(const wchar_t *format, ...)
{
    va_list arg_list;
    wchar_t tmp[1024] = { 0, };
    va_start(arg_list, format);
    vswprintf_s(tmp, sizeof(tmp) / sizeof(wchar_t), format, arg_list);

    OutputDebugStringW(tmp);
}

//Ref* https://msdn.microsoft.com/ko-kr/library/windows/desktop/aa366789(v=vs.85).aspx
std::string DbgUtility::getFileNameFromeHandle(HANDLE hFile)
{
    bool bSuccess = false;
    TCHAR pszFilename[MAX_PATH + 1];
    HANDLE hFileMap;

    // Get the file size.
    DWORD dwFileSizeHi = 0;
    DWORD dwFileSizeLo = GetFileSize(hFile, &dwFileSizeHi);

    if (dwFileSizeLo == 0 && dwFileSizeHi == 0)
    {
        return "(unknown)";
    }

    // Create a file mapping object.
    hFileMap = CreateFileMapping(hFile,
        NULL,
        PAGE_READONLY,
        0,
        1,
        NULL);

    if (hFileMap)
    {
        // Create a file mapping to get the file name.
        void* pMem = MapViewOfFile(hFileMap, FILE_MAP_READ, 0, 0, 1);

        if (pMem)
        {
            if (GetMappedFileName(GetCurrentProcess(),
                pMem,
                pszFilename,
                MAX_PATH))
            {

                // Translate path with device name to drive letters.
                TCHAR szTemp[BUFSIZE];
                szTemp[0] = '\0';

                if (GetLogicalDriveStrings(BUFSIZE - 1, szTemp))
                {
                    TCHAR szName[MAX_PATH];
                    TCHAR szDrive[3] = TEXT(" :");
                    BOOL bFound = FALSE;
                    TCHAR* p = szTemp;

                    do
                    {
                        // Copy the drive letter to the template string
                        *szDrive = *p;

                        // Look up each device name
                        if (QueryDosDevice(szDrive, szName, MAX_PATH))
                        {
                            size_t uNameLen = _tcslen(szName);

                            if (uNameLen < MAX_PATH)
                            {
                                bFound = _tcsnicmp(pszFilename, szName, uNameLen) == 0
                                    && *(pszFilename + uNameLen) == _T('\\');

                                if (bFound)
                                {
                                    // Reconstruct pszFilename using szTempFile
                                    // Replace device path with DOS path
                                    TCHAR szTempFile[MAX_PATH];
                                    _stprintf_s(szTempFile,
                                        TEXT("%s%s"),
                                        szDrive,
                                        pszFilename + uNameLen);
                                    _tcsncpy_s(pszFilename, szTempFile, _tcslen(szTempFile));
                                }
                            }
                        }

                        // Go to the next NULL character.
                        while (*p++);
                    } while (!bFound && *p); // end of string
                }
            }
            bSuccess = TRUE;
            UnmapViewOfFile(pMem);
        }

        CloseHandle(hFileMap);
    }

    return(pszFilename);
}

CONTEXT DbgUtility::getCurrentDebuggeeContext()
{
    return context;
}

void DbgUtility::setTrapFlag(bool _enable)
{
    m_trapFlag = _enable;
}

void DbgUtility::stop()
{
    m_stop = true;
}

void DbgUtility::resume()
{
    m_resume = false;
}

PROCESS_INFORMATION DbgUtility::getDebuggeeProcInfo()
{
    return processInformation;
}

void DbgUtility::onExcuteOnceInstruction(DEBUG_EVENT& _event){}
void DbgUtility::onCreateProcess(DEBUG_EVENT& _event){}
void DbgUtility::onExitProcess(DEBUG_EVENT& _event){}