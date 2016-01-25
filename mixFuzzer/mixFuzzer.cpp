// mixFuzzer.cpp : 定义控制台应用程序的入口点。
//
#include <Windows.h>
#include <cstdio>
#include <conio.h>
#include <Shobjidl.h>
#include <tlhelp32.h>
#include <Psapi.h>
#include <io.h>
#include "tstream.h"

#include "common.h"
#include "httpServThread.h"
#include "htmlGenThread.h"

#define SOFT_NAME TEXT("mixFuzzer")
#define SOFT_VER TEXT("v0.4")
#define SOFT_LOGO TEXT(\
	"================================================================================\n"\
	"|                         Wellcome to " SOFT_NAME " " SOFT_VER "                          |\n"\
	"================================================================================\n\n")

const TCHAR* sAUMID = TEXT("Microsoft.MicrosoftEdge_8wekyb3d8bbwe!MicrosoftEdge");
const TCHAR* sMicrosoftEdgeExecutable = TEXT("MicrosoftEdge.exe");
const TCHAR* sBrowserBrokerExecutable = TEXT("browser_broker.exe");
const TCHAR* sRuntimeBrokerExecutable = TEXT("RuntimeBroker.exe");
const TCHAR* sMicrosoftEdgeCPExecutable = TEXT("MicrosoftEdgeCP.exe");

using namespace std;
using namespace gcommon;

GLogger2 glogger;
tstring GetCurrentDirPath();
int GetDebugInfo(HANDLE hPipe, char* buff, int size, int timeout = 2000);
tstring GetCrashPos(HANDLE hinPipeW, HANDLE houtPipeR);
bool CheckCCInt3(char* buff);
bool CheckC3Ret(char* buff);
DWORD GetProcessId(LPCTSTR pszProcessName);
bool TerminateAllProcess(LPCTSTR pszProcessName);
uint32_t GetFilecountInDir(tstring dir, tstring fileext);

int _tmain(int argc, TCHAR** argv)
{
	const uint16_t LISTEN_PORT = 12228;
	const uint32_t BUFF_SIZE = 1024 * 100;
	const uint32_t FUZZ_TIMEOUT = 3000;
	const uint32_t READ_DBGINFO_TIMEOUT = 1000;
	const uint32_t MAX_POC_COUNT = 5;

	char* htmlBuff = new char[BUFF_SIZE+1]; // http packet buff
	char* htmlTempl = new char[BUFF_SIZE+1]; // html template buff

	tstring configFile = TEXT("config.ini");
	tstring symPath = TEXT("srv*");
	tstring outPath = TEXT("crash");
	tstring htmlPath;
	tstring logPath;

	PRINT_TARGET print_target = PRINT_TARGET::BOTH;
	int debug_level = 0;
	tstring log_file = TEXT("mixfuzz.log");
	tstring mode = TEXT("autofuzz");

	// 初始化glogger	
	glogger.setDebugLevel(debug_level);
	glogger.setHeader(TEXT("main"));
	glogger.enableColor();
	glogger.setLogFile(log_file);
	glogger.setTarget(print_target);

	// 初始化console显示
	tstring title = SOFT_NAME;
	title.append(TEXT(" "));
	title.append(SOFT_VER);
	SetConsoleTitle(title.c_str());
	glogger.screen(SOFT_LOGO);

	// 创建debug Pipe
	HANDLE inputPipeR, inputPipeW;
	HANDLE outputPipeR, outputPipeW;
	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;
	if (!CreatePipe(&inputPipeR, &inputPipeW, &saAttr, 0))
	{
		glogger.error(TEXT("failed to create pipe"));
		exit(_getch());
	}
	if (!CreatePipe(&outputPipeR, &outputPipeW, &saAttr, 0))
	{
		glogger.error(TEXT("failed to create pipe"));
		exit(_getch());
	}

	// 获取当前文件夹路径		
	tstring currentDir = GetCurrentDirPath();
	if (currentDir.empty())
	{
		glogger.warning(TEXT("can not get current dir, use default dir"));
		currentDir = TEXT(".\\");
	}
	SetCurrentDirectory(currentDir.c_str());

	// 读取config文件
	debug_level = _ttoi(GetConfigPara(currentDir+configFile, TEXT("DEBUG_LEVEL"), TEXT("0")).c_str());
	symPath = GetConfigPara(currentDir+configFile, TEXT("SYMBOL_PATH"), symPath);
	outPath = GetConfigPara(currentDir+configFile, TEXT("OUT_PATH"), outPath);
	mode = GetConfigPara(currentDir + configFile, TEXT("MODE"), mode);
	glogger.info(TEXT("symbol path: ") + symPath);
	glogger.info(TEXT(" ouput path: ") + outPath);

	// 创建crash目录
	CreateDirectory(outPath.c_str(), NULL);
	if (outPath.back() != '\\')
	{
		outPath.append(TEXT("\\"));
	}

	// 读取模板文件
	FILE* ftempl;
	if (fopen_s(&ftempl, "template.html", "r") != 0)
	{
		glogger.error(TEXT("failed to open template.html"));
		exit(_getch());
	}
	size_t tmplsize = fread_s(htmlTempl, BUFF_SIZE, 1, BUFF_SIZE - 1, ftempl);
	fclose(ftempl);
	if (tmplsize == 0)
	{
		glogger.error(TEXT("failed to read template.html"));
		exit(_getch());
	}	
	htmlTempl[tmplsize] = 0;

	// semaphore
	HANDLE semaphorep = CreateSemaphore(NULL, 1, 1, TEXT("mixfuzzer_sem_htmlbuff_p"));
	HANDLE semaphorec = CreateSemaphore(NULL, 0, 1, TEXT("mixfuzzer_sem_htmlbuff_c"));

	// 启动http服务线程
	HTTPSERV_THREAD_PARA httpServPara;
	httpServPara.htmlBuff = htmlBuff;
	httpServPara.semHtmlbuff_c = semaphorec;
	httpServPara.semHtmlbuff_p = semaphorep;
	httpServPara.port = LISTEN_PORT;
	HttpServThread httpServThread(&httpServPara);
	if (!httpServThread.Run())
	{
		glogger.error(TEXT("failed to create [HttpServ] thread"));
		exit(_getch());
	}

	// 启动html生成线程
	HTMLGEN_THREA_PARA htmlGenPara;
	htmlGenPara.buffSize = BUFF_SIZE;
	htmlGenPara.htmlBuff = htmlBuff;
	htmlGenPara.htmlTempl = htmlTempl;
	htmlGenPara.semHtmlbuff_c = semaphorec;
	htmlGenPara.semHtmlbuff_p = semaphorep;
	htmlGenPara.port = LISTEN_PORT;
	if (mode == TEXT("webserver"))
		htmlGenPara.autoFuzz = false;
	HtmlGenThread htmlGenThread(&htmlGenPara);
	if (!htmlGenThread.Run())
	{
		glogger.error(TEXT("failed to create [HtmlGen] thread"));
		exit(_getch());
	}

	if (mode == TEXT("webserver"))
	{
		glogger.info(TEXT("webserver mode, listening at port: %d"), LISTEN_PORT);
		while (true)
		{
			Sleep(100);
		}
	}

	// fuzz循环
	DWORD nwrite,nread;
	uint32_t buffsize = 1024;
	char* rbuff = new char[buffsize+1];
	char* pbuff = new char[2*buffsize+1];	
	while (true)
	{
		glogger.screen(TEXT("\n\n"));
		glogger.info(TEXT("Start Fuzzing ..."));

		nread = nwrite = 0;

		// kill Edge所有线程
		glogger.info(TEXT("Kill all edge-related processes"));
		if (!TerminateAllProcess(TEXT("cdb.exe")))
		{
			glogger.error(TEXT("Cannot kill cdb, restart fuzz."));
			continue;
		}
		if (!TerminateAllProcess(sMicrosoftEdgeExecutable))
		{
			glogger.error(TEXT("Cannot kill Edge, restart fuzz."));
			continue;
		}

		// 启动浏览器
		glogger.info(TEXT("Start Edge"));
		STARTUPINFO si_edge = { sizeof(STARTUPINFO) };
		PROCESS_INFORMATION pi_edge;
		si_edge.dwFlags = STARTF_USESHOWWINDOW;
		si_edge.wShowWindow = TRUE; //TRUE表示显示创建的进程的窗口
		TCHAR cmdline[1024];
		_stprintf_s(cmdline, TEXT("explorer Microsoft-Edge:http://localhost:%d"), LISTEN_PORT);
		BOOL bRet = CreateProcess(NULL, cmdline,
			NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si_edge, &pi_edge);
		if (!bRet)
		{
			glogger.error(TEXT("Cannot start Edge."));
			exit(_getch());
		}
		Sleep(1000);

		// 获取PID
		DWORD dwMicrosoftEdgeCP_PID = GetProcessId(sMicrosoftEdgeCPExecutable);
		//DWORD dwMicrosoftEdge_PID = GetProcessId(sMicrosoftEdgeExecutable);
		//DWORD dwRuntimeBroker_PID = GetProcessId(sRuntimeBrokerExecutable);
		//DWORD dwBrowserBroker_PID = GetProcessId(sBrowserBrokerExecutable);
		if (dwMicrosoftEdgeCP_PID == 0)
		{
			glogger.error(TEXT("Cannot start Edge, restart fuzz."));
			continue;
		}

		// attach调试器	
		tstring sCommandLine = TEXT("cdb.exe");
		sCommandLine.append(TEXT(" -o -p "));
		sCommandLine += to_tstring(dwMicrosoftEdgeCP_PID);
		
		glogger.info(TEXT("Attach ") + sCommandLine);
		STARTUPINFO si_cdb = { sizeof(STARTUPINFO) };
		si_cdb.dwFlags |= STARTF_USESTDHANDLES;
		si_cdb.hStdInput = inputPipeR;
		si_cdb.hStdOutput = outputPipeW;
		si_cdb.hStdError = outputPipeW;
		//si_cdb.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		//si_cdb.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		//si_cdb.hStdError = GetStdHandle(STD_OUTPUT_HANDLE);
		PROCESS_INFORMATION pi_cdb = {};
		if (!CreateProcess(NULL, (LPWSTR)sCommandLine.c_str(),
			NULL, NULL, TRUE, 0, NULL, NULL, &si_cdb, &pi_cdb))
		{
			glogger.error(TEXT("Cannot attach debugger, restart fuzz."));
			exit(_getch());
		}

		// 设置symbol path
		sCommandLine = TEXT(".sympath ");
		sCommandLine.append(symPath);
		sCommandLine.append(TEXT("\n"));
		WriteFile(inputPipeW, WStringToString(sCommandLine).c_str(), sCommandLine.size(), &nwrite, NULL);

		// 设置atanh函数断点，用于日志输出		
		//sCommandLine = TEXT("bp chakra!Js::Math::Atanh \".printf \\\"%mu\\\", poi(poi(@rsp+0x28)+0x10);.echo;g\"\n");
		//WriteFile(inputPipeW, WStringToString(sCommandLine).c_str(), sCommandLine.size(), &nwrite, NULL);
		WriteFile(inputPipeW, "g\n", 2, &nwrite, NULL);

		// 监听cdg循环
		glogger.info(TEXT("Fuzzing ..."));
		pbuff[0] = 0;
		uint32_t idletime = 0;
		while (true)
		{
			nread = GetDebugInfo(outputPipeR, rbuff, buffsize, READ_DBGINFO_TIMEOUT);
			if (nread == buffsize)
			{
				idletime = 0;
				memcpy(pbuff, rbuff, nread);
				pbuff[nread] = 0;
				continue;
			}
			else if (nread > 0)
			{			
				idletime = 0;
				memcpy(pbuff + strlen(pbuff), rbuff, nread+1);
			}

			size_t pbufflen = strlen(pbuff);
			if (pbufflen < 2)
			{
				pbuff[0] = 0;
				idletime += READ_DBGINFO_TIMEOUT;
				if (idletime >= FUZZ_TIMEOUT)
				{
					glogger.warning(TEXT("Edge seems dead, restart fuzz ..."));
					break;
				}
				continue;
			}

			if (pbuff[pbufflen-2] == '>' && pbuff[pbufflen - 1] == ' ')
			{
				// No runnable debuggees
				if (strstr(pbuff, "No runnable debuggees") != NULL)
				{
					glogger.warning(TEXT("No runnable debuggees"));
					break;
				}

				// 进程异常
				if (CheckC3Ret(pbuff))
				{	
					glogger.warning(TEXT("break @ \"ret\", continue"));
					//break;
				}

				// 软件中断，g
				if (CheckCCInt3(pbuff))
				{
					glogger.warning(TEXT("break @ \"int 3\", continue"));
					WriteFile(inputPipeW, "g\n", 2, &nwrite, NULL);
					pbuff[0] = 0;
					continue;
				}

				// 判定为crash 
				glogger.error(TEXT("!! find crash !!"));
				char* poc = htmlGenThread.GetPrevHtml();
				
				// 生成文件名
				TCHAR filename[11];
				time_t ct = time(NULL);
				_itot(ct, filename, 10);

				// 获取崩溃位置作为目录名
				tstring crashpos = GetCrashPos(inputPipeW, outputPipeR);
				tstring module = crashpos.substr(0, crashpos.find_first_of('_'));
				htmlPath.assign(outPath);
				htmlPath.append(crashpos);
				htmlPath.append(TEXT("\\"));
				CreateDirectory(htmlPath.c_str(), NULL);
				glogger.info(TEXT("crash = ") + crashpos);
				if (GetFilecountInDir(htmlPath, TEXT("html")) >= MAX_POC_COUNT)
				{
					glogger.warning(TEXT("this crash already logged, restart fuzz ..."));
					break;
				}
				glogger.info(TEXT("create html and log ..."));

				// 补全文件名
				htmlPath.append(filename);
				htmlPath.append(TEXT(".html"));
				logPath.assign(htmlPath);
				logPath.append(TEXT(".log"));
				
				//char* jmp_start;
				//while ((jmp_start = strstr(poc, "http://localhost:")) != NULL)
				//{
				//	char* jmp_end = strstr(jmp_start, "'");
				//	if (jmp_end == NULL)
				//		jmp_end = jmp_start+17;
				//	for (char* i = jmp_start; i < jmp_end; i++)
				//	{
				//		*i = ' ';
				//	}
				//}
				
				// 写入html文件
				FILE* htmlFile;
				_tfopen_s(&htmlFile, htmlPath.c_str(), TEXT("w"));
				fwrite(poc, 1, strlen(poc), htmlFile);
				fclose(htmlFile);

				// 写入log文件
				FILE* logFile;
				_tfopen_s(&logFile, logPath.c_str(), TEXT("w"));
				fwrite("*** mixFuzzer ***\n", 1, 18, logFile);
				WriteFile(inputPipeW, "r\n", 2, &nwrite, NULL);
				if (GetDebugInfo(outputPipeR, pbuff, 2 * buffsize)> 0)
					fwrite(pbuff, 1, strlen(pbuff), logFile);
				
				// *** stack tracing ***
				fwrite("\n\n*** stack tracing ***\n", 1, 24, logFile);
				WriteFile(inputPipeW, "kb\n", 3, &nwrite, NULL);
				while (GetDebugInfo(outputPipeR, pbuff, buffsize) > 0)
				{
					fwrite(pbuff, 1, strlen(pbuff), logFile);
				}				
				
				// *** module info ***
				fwrite("\n\n*** module info ***\n", 1, 22, logFile);
				sCommandLine = TEXT("lmDvm ");
				sCommandLine.append(module);
				sCommandLine.append(TEXT("\n"));
				WriteFile(inputPipeW, WStringToString(sCommandLine).c_str(), sCommandLine.size(), &nwrite, NULL);
				if(GetDebugInfo(outputPipeR, pbuff, 2*buffsize)> 0)
					fwrite(pbuff, 1, strlen(pbuff), logFile);
				
				fclose(logFile);
				break;
			}
			
			pbuff[0] = 0;
		}
	}
		
	delete[] rbuff;
	delete[] pbuff;
	exit(_getch());
}

tstring GetCrashPos(HANDLE hinPipeW, HANDLE houtPipeR)
{
	DWORD nwrite,nread;
	char rbuff[1024+1];
	WriteFile(hinPipeW, "u eip L1\n", 9, &nwrite, NULL);
	nread = GetDebugInfo(houtPipeR, rbuff, 1024);
	if (nread == 0)
		return tstring(TEXT("unknown"));
	
	size_t i = 0, start = 0;
	for (i = 0; i < strlen(rbuff); i++)
	{
		if (rbuff[i] == '!')
		{
			while (i > 0 && rbuff[--i] != '\n');
			start = i;
			break;
		}
	}

	if (i!=start)
	{
		return tstring(TEXT("nosymbol"));
	}

	for (i = start; i < strlen(rbuff); i++)
	{
		if (rbuff[i] == ':')
		{
			rbuff[i] = '_';
		}

		if (rbuff[i] == '!')
		{
			rbuff[i] = '_';
		}

		if (rbuff[i] == '\n')
		{
			rbuff[i-1] = 0;
		}
	}

	return StringToWString(string(rbuff+start));
}

bool CheckCCInt3(char* buff)
{
	// cc	int 3
	char* pcc = strstr(buff, " cc ");
	if (pcc == NULL)
		return false;

	char* pint = strstr(pcc, " int ");
	if (pint == NULL)
		return false;
	for (size_t i = 0; i < pint-pcc-4; i++)
	{
		if (pcc[i + 4] != ' ')
			return false;
	}

	char* p3 = strstr(pint, " 3\n");
	if (p3 == NULL)
		return false;
	for (size_t i = 0; i < p3 - pint - 5; i++)
	{
		if (pint[i + 5] != ' ')
			return false;
	}

	return true;
}

bool CheckC3Ret(char* buff)
{
	// c3	ret
	char* pc3 = strstr(buff, " c3 ");
	if (pc3 == NULL)
		return false;

	char* pret = strstr(pc3, " ret\n");
	if (pret == NULL)
		return false;
	for (size_t i = 0; i < pret - pc3 - 4; i++)
	{
		if (pc3[i + 4] != ' ')
			return false;
	}

	return true;
}

int GetDebugInfo(HANDLE hPipe, char* buff, int size, int timeout)
{
	int count = timeout/100;
	DWORD nread = 0;
	while (count--)
	{
		Sleep(100);
		if (!PeekNamedPipe(hPipe, buff, size, &nread, 0, 0))
		{
			continue;
		}

		if (nread == size)
		{
			break;
		}		
	}

	if (nread == 0)
	{
		return 0;
	}

	nread = 0;
	ReadFile(hPipe, buff, size, &nread, NULL);
	if (nread>0)
	{
		buff[nread] = 0;
	}
	return nread;
}

tstring GetCurrentDirPath()
{
	tstring strCurrentDir;
	TCHAR* pCurrentDir = new TCHAR[MAX_PATH + 1];
	memset(pCurrentDir, 0, MAX_PATH + 1);
	DWORD nRet = GetModuleFileName(NULL, pCurrentDir, MAX_PATH);
	if (nRet == 0)
	{
		delete[] pCurrentDir;
		return TEXT(".\\");
	}

	(_tcsrchr(pCurrentDir, '\\'))[1] = 0;
	strCurrentDir = pCurrentDir;
	delete[] pCurrentDir;

	return strCurrentDir;
}

DWORD GetProcessId(LPCTSTR pszProcessName)
{
	BOOL bFound = FALSE;
	DWORD aProcesses[1024], cbNeeded, cProcesses;
	unsigned int i;

	// Enumerate all processes
	if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded))
		return FALSE;

	// Calculate how many process identifiers were returned.
	cProcesses = cbNeeded / sizeof(DWORD);

	TCHAR szEXEName[MAX_PATH] = { 0 };
	// Loop through all process to find the one that matches
	// the one we are looking for
	for (i = 0; i < cProcesses; i++)
	{
		// Get a handle to the process
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
			PROCESS_VM_READ, FALSE, aProcesses[i]);

		// Get the process name
		if (NULL != hProcess)
		{
			HMODULE hMod;
			DWORD cbNeeded;

			if (EnumProcessModules(hProcess, &hMod,
				sizeof(hMod), &cbNeeded))
			{
				//Get the name of the exe file
				GetModuleBaseName(hProcess, hMod, szEXEName,
					sizeof(szEXEName) / sizeof(TCHAR));

				if (_tcsicmp(szEXEName, pszProcessName) == 0)
				{
					bFound = TRUE;
					CloseHandle(hProcess);
					break;
				}
			}
			CloseHandle(hProcess);
		}
	}

	return bFound ? aProcesses[i] : 0;
}

bool TerminateAllProcess(LPCTSTR pszProcessName)
{
	bool ret = false;
	do {
		ret = false;
		DWORD pid = GetProcessId(pszProcessName);
		if (pid != 0)
		{
			HANDLE hProcess = OpenProcess(
				PROCESS_TERMINATE | 
				PROCESS_QUERY_LIMITED_INFORMATION | 
				SYNCHRONIZE, FALSE, pid);
			if (hProcess != NULL)
			{
				TerminateProcess(hProcess, 0);
				ret = true;
			}
		}
	} while (ret);
	Sleep(1000);

	DWORD pid = GetProcessId(pszProcessName);
	if (pid == 0)
		return true;
	else
		return false;
}

uint32_t GetFilecountInDir(tstring dir, tstring fileext)
{
	_tfinddata_t FileInfo;
	tstring strfind = dir + TEXT("\\*.") + fileext;
	intptr_t hh = _tfindfirst(strfind.c_str(), &FileInfo);
	int count = 0;

	if (hh == -1L)
	{
		return count;
	}

	do {
		//判断是否有子目录
		if (FileInfo.attrib & _A_SUBDIR)
		{
			continue;
		}
		else
		{
			count++;
		}
	} while (_tfindnext(hh, &FileInfo) == 0);

	_findclose(hh);
	return count;
}