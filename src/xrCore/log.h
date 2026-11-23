#pragma once
#include "DateTime.hpp"
#include <atomic>
#define VPUSH(a)	((a).x), ((a).y), ((a).z)

void 	XRCORE_API		Msg	(const char* format, ...);

// Old shit
void 	XRCORE_API		Log			(const char* msg);
void 	XRCORE_API		Log			(const char* msg, const Fvector& dop);
void 	XRCORE_API		Log			(const char* msg, const Fmatrix& dop);
void 	XRCORE_API		Log			(const char* msg, const char* dop);
void 	XRCORE_API		Log			(const char* msg, u32 dop);
void 	XRCORE_API		Log			(const char* msg, int dop);
void 	XRCORE_API		Log			(const char* msg, float dop);
void 	XRCORE_API		LogWinErr(const char* msg, long err_code);

xr_string FormatString(LPCSTR fmt, ...);

class XRCORE_API xrLogger
{
public:
	using LogCallback = void(*)	(const char* string);

	void Msg(LPCSTR Msg, va_list argList);
	void SimpleMessage(LPCSTR Message, u32 MessageSize = 0);

	static void OpenLogFile();
	static const string_path& GetLogPath();
	static void EnableFastDebugLog();
	static void InitLog();
	static void FlushLog();
	static void CloseLog();
	static void SetImmediateMode(bool enable);

	static void AddLogCallback(LogCallback logCb);
	static void RemoveLogCallback(LogCallback logCb);

	void PauseLogging();
	void UnpauseLogging();

	xrLogger();
	~xrLogger();

	void LogThreadEntry();

private:
	void InternalCloseLog();
	volatile bool bIsAlive;
	ThreadID hLogThread;

	void InternalOpenLogFile();

	void InternalPrintAllRecords();
	// Anomaly Coop (TZ R38): a line after the close goes straight to the end of the file (coop::log_close::late_text)
	void InternalWriteLate(LPCSTR text);

	void InternalFlushLog();

	string_path logFileName;
	volatile IWriter* logFile;

	struct LogRecord
	{
		LogRecord() {}
		LogRecord(LPCSTR Msg, u32 sizeMsg);
		xr_string Message;
		Time time;
	};
	// Anomaly Coop (TZ R38): a record is taken from the queue in one hold of logDataGuard (coop::log_close::take_front), and printed
	// under logPrintGuard: one printer at a time, and none while the file goes (InternalCloseLog)
	bool InternalTakeRecord(LogRecord& record);
	void InternalPrintRecord(const LogRecord& record);

	xrCriticalSection logDataGuard;
	xrCriticalSection logCallbackGuard;
	xrCriticalSection logFlushGuard;
	// Anomaly Coop (TZ R38): held by whoever prints records or writes a late line, and by InternalCloseLog until the file is taken
	// away. Order: logPrintGuard, then logDataGuard / logFlushGuard / logCallbackGuard; never the other way.
	xrCriticalSection logPrintGuard;
	bool bFileOpened = false; // Anomaly Coop (TZ R38): the file was created: a late line has a file to go to
	bool bLateWritten = false; // Anomaly Coop (TZ R38): the late header is in the file
	bool bFastDebugLog;

	std::atomic_bool bFlushRequested;
	bool bImmediateMode;

	//LogCallback onLogMsg;
	xr_list<LogCallback> logCallbackList;
	xr_fixedqueue<xr_string, 512> tempLogData;
public:
	static xr_queue <LogRecord>* logData;
};
