/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2015 Cuckoo Sandbox Developers, Optiv, Inc. (brad.spengler@optiv.com)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
//#define DEBUG_COMMENTS

#include <stdio.h>
#include "ntapi.h"
#include "hooking.h"
#include "log.h"
#include "pipe.h"
#include "misc.h"
#include "hook_sleep.h"
#include "unhook.h"
#include "lookup.h"
#include "CAPE\CAPE.h"
#include "CAPE\Debugger.h"

extern _RtlNtStatusToDosError pRtlNtStatusToDosError;
extern void DebugOutput(_In_ LPCTSTR lpOutputString, ...);
extern void GetThreadContextHandler(DWORD Pid, LPCONTEXT Context);
extern void SetThreadContextHandler(DWORD Pid, const CONTEXT *Context);
extern void ResumeThreadHandler(DWORD Pid);
extern void NtContinueHandler(PCONTEXT ThreadContext);
extern void ProcessMessage(DWORD ProcessId, DWORD ThreadId);
extern BOOL BreakpointsSet, BreakOnNtContinue;
extern PVOID BreakOnNtContinueCallback;
extern int StepOverRegister;

static lookup_t g_ignored_threads;

DWORD LastInjected;

void ignored_threads_init(void)
{
	lookup_init(&g_ignored_threads);
}

BOOLEAN is_ignored_thread(DWORD tid)
{
	void *ret;
	lasterror_t lasterror;

	get_lasterrors(&lasterror);
	ret = lookup_get(&g_ignored_threads, (unsigned int)tid, NULL);
	set_lasterrors(&lasterror);

	if (ret)
		return TRUE;

	return FALSE;
}

void remove_ignored_thread(DWORD tid)
{
	lasterror_t lasterror;

	get_lasterrors(&lasterror);
	lookup_del(&g_ignored_threads, tid);
	set_lasterrors(&lasterror);
}

void add_ignored_thread(DWORD tid)
{
	lasterror_t lasterror;

	get_lasterrors(&lasterror);
	pipe("INFO:Adding ignored thread %d", tid);
	lookup_add(&g_ignored_threads, tid, 0);
	set_lasterrors(&lasterror);
}

HOOKDEF(NTSTATUS, WINAPI, NtQueueApcThread,
	__in HANDLE ThreadHandle,
	__in PIO_APC_ROUTINE ApcRoutine,
	__in_opt PVOID ApcRoutineContext,
	__in_opt PIO_STATUS_BLOCK ApcStatusBlock,
	__in_opt ULONG ApcReserved
) {
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);
	NTSTATUS ret;

	if (pid != GetCurrentProcessId())
		ProcessMessage(pid, tid);

	ret = Old_NtQueueApcThread(ThreadHandle, ApcRoutine, ApcRoutineContext, ApcStatusBlock, ApcReserved);

	LOQ_ntstatus("threading", "iip", "ProcessId", pid, "ThreadId", tid, "ThreadHandle", ThreadHandle);

	if (NT_SUCCESS(ret))
		disable_sleep_skip();

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtQueueApcThreadEx,
	__in HANDLE ThreadHandle,
	__in_opt HANDLE UserApcReserveHandle,
	__in PIO_APC_ROUTINE ApcRoutine,
	__in_opt PVOID ApcRoutineContext,
	__in_opt PIO_STATUS_BLOCK ApcStatusBlock,
	__in_opt PVOID ApcReserved
) {
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);
	NTSTATUS ret;

	if (pid != GetCurrentProcessId())
		ProcessMessage(pid, tid);

	ret = Old_NtQueueApcThreadEx(ThreadHandle, UserApcReserveHandle, ApcRoutine, ApcRoutineContext, ApcStatusBlock, ApcReserved);

	LOQ_ntstatus("threading", "iip", "ProcessId", pid, "ThreadId", tid, "ThreadHandle", ThreadHandle);

	if (NT_SUCCESS(ret))
		disable_sleep_skip();

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtCreateThread,
	__out	 PHANDLE ThreadHandle,
	__in	  ACCESS_MASK DesiredAccess,
	__in_opt  POBJECT_ATTRIBUTES ObjectAttributes,
	__in	  HANDLE ProcessHandle,
	__out	 PCLIENT_ID ClientId,
	__in	  PCONTEXT ThreadContext,
	__in	  PINITIAL_TEB InitialTeb,
	__in	  BOOLEAN CreateSuspended
	) {
	DWORD pid = pid_from_process_handle(ProcessHandle);
	NTSTATUS ret = Old_NtCreateThread(ThreadHandle, DesiredAccess,
		ObjectAttributes, ProcessHandle, ClientId, ThreadContext,
		InitialTeb, TRUE);

	if (NT_SUCCESS(ret)) {
		DWORD tid = tid_from_thread_handle(*ThreadHandle);
		//if (called_by_hook() && pid == GetCurrentProcessId())
		//	add_ignored_thread(tid);

		if (g_config.debugger && !called_by_hook() && BreakpointsSet) {
			DebugOutput("NtCreateThread: Initialising breakpoints for thread %d.\n", tid);
			InitNewThreadBreakpoints(tid);
		}

		if (pid != GetCurrentProcessId())
			ProcessMessage(pid, tid);

		if (CreateSuspended == FALSE) {
			lasterror_t lasterror;
			get_lasterrors(&lasterror);
			ResumeThread(*ThreadHandle);
			set_lasterrors(&lasterror);
		}

		LOQ_ntstatus("threading", "PpOiii", "ThreadHandle", ThreadHandle, "ProcessHandle", ProcessHandle,
			"ObjectAttributes", ObjectAttributes, "CreateSuspended", CreateSuspended, "ThreadId", tid,
			"ProcessId", pid);

		disable_sleep_skip();
	}
	else
		LOQ_ntstatus("threading", "PpOi", "ThreadHandle", ThreadHandle, "ProcessHandle", ProcessHandle,
			"ObjectAttributes", ObjectAttributes, "CreateSuspended", CreateSuspended);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtCreateThreadEx,
	OUT	 PHANDLE hThread,
	IN	  ACCESS_MASK DesiredAccess,
	IN	  PVOID ObjectAttributes,
	IN	  HANDLE ProcessHandle,
	IN	  LPTHREAD_START_ROUTINE lpStartAddress,
	IN	  PVOID lpParameter,
	IN	  DWORD CreateFlags,
	IN	  LONG StackZeroBits,
	IN	  LONG SizeOfStackCommit,
	IN	  LONG SizeOfStackReserve,
	OUT	 PVOID lpBytesBuffer
) {
	DWORD pid = pid_from_process_handle(ProcessHandle);

	NTSTATUS ret = Old_NtCreateThreadEx(hThread, DesiredAccess,
		ObjectAttributes, ProcessHandle, lpStartAddress, lpParameter,
		CreateFlags | 1, StackZeroBits, SizeOfStackCommit, SizeOfStackReserve,
		lpBytesBuffer);

	if (NT_SUCCESS(ret)) {
		DWORD tid = tid_from_thread_handle(*hThread);
		//if (called_by_hook() && pid == GetCurrentProcessId())
		//	add_ignored_thread(tid);

		if (pid != GetCurrentProcessId())
			if (g_config.debugger && !called_by_hook()) {
				DebugOutput("NtCreateThreadEx: Initialising breakpoints for thread %d.\n", tid);
				InitNewThreadBreakpoints(tid);
			}

			ProcessMessage(pid, tid);

			if (!(CreateFlags & 1)) {
			lasterror_t lasterror;
			get_lasterrors(&lasterror);
			ResumeThread(*hThread);
			set_lasterrors(&lasterror);
		}

		LOQ_ntstatus("threading", "Ppphii", "ThreadHandle", hThread, "ProcessHandle", ProcessHandle,
			"StartAddress", lpStartAddress, "CreateFlags", CreateFlags, "ThreadId", tid,
			"ProcessId", pid);

		disable_sleep_skip();
	}
	else
		LOQ_ntstatus("threading", "Ppph", "ThreadHandle", hThread, "ProcessHandle", ProcessHandle,
			"StartAddress", lpStartAddress, "CreateFlags", CreateFlags);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtOpenThread,
	__out  PHANDLE ThreadHandle,
	__in   ACCESS_MASK DesiredAccess,
	__in   POBJECT_ATTRIBUTES ObjectAttributes,
	__in   PCLIENT_ID ClientId
) {
	NTSTATUS ret = Old_NtOpenThread(ThreadHandle, DesiredAccess,
		ObjectAttributes, ClientId);
	DWORD pid = 0;
	DWORD tid = 0;

	if (NT_SUCCESS(ret) && ThreadHandle) {
		pid = pid_from_thread_handle(*ThreadHandle);
		tid = tid_from_thread_handle(*ThreadHandle);
	}

	if (ClientId) {
		LOQ_ntstatus("threading", "Phiii", "ThreadHandle", ThreadHandle, "DesiredAccess", DesiredAccess,
			"ProcessId", pid, "ThreadId", tid, "ProcessId", pid);
	} else {
		LOQ_ntstatus("threading", "PhOi", "ThreadHandle", ThreadHandle, "DesiredAccess", DesiredAccess,
			"ObjectAttributes", ObjectAttributes, "ProcessId", pid);
	}

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtGetContextThread,
	__in	 HANDLE ThreadHandle,
	__inout  LPCONTEXT Context
) {
	ENSURE_HANDLE(ThreadHandle);
	ENSURE_STRUCT(Context, CONTEXT);
	DWORD tid = tid_from_thread_handle(ThreadHandle);

	NTSTATUS ret = Old_NtGetContextThread(ThreadHandle, Context);
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	if (Context && Context->ContextFlags & CONTEXT_CONTROL)
#ifdef _WIN64
		LOQ_ntstatus("threading", "pppi", "ThreadHandle", ThreadHandle, "HollowedInstructionPointer",
			Context->Rcx, "CurrentInstructionPointer", Context->Rip, "ProcessId", pid);
#else
		LOQ_ntstatus("threading", "pppi", "ThreadHandle", ThreadHandle, "HollowedInstructionPointer",
			Context->Eax, "CurrentInstructionPointer", Context->Eip, "ProcessId", pid);
#endif
	else
		LOQ_ntstatus("threading", "pi", "ThreadHandle", ThreadHandle, "ProcessId", pid);

	GetThreadContextHandler(pid, Context);

	if (g_config.debugger) {
		Context->Dr0 = 0;
		Context->Dr1 = 0;
		Context->Dr2 = 0;
		Context->Dr3 = 0;
		Context->Dr6 = 0;
		Context->Dr7 = 0;
	}

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtSetContextThread,
	__in  HANDLE ThreadHandle,
	__in  CONTEXT *Context
) {
	NTSTATUS ret;
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);

	if (pid == GetCurrentProcessId() && g_config.debugger && Context && Context->ContextFlags & CONTEXT_CONTROL) {
		CONTEXT CurrentContext;
		CurrentContext.ContextFlags = CONTEXT_CONTROL;
		ret = Old_NtGetContextThread(ThreadHandle, &CurrentContext);
		if (NT_SUCCESS(ret)) {
			PTHREADBREAKPOINTS ThreadBreakpoints = GetThreadBreakpoints(tid);
			if (ThreadBreakpoints)
			{
				DebugOutput("NtSetContextThread hook: protecting breakpoints for thread %d.\n", tid);
				ContextSetThreadBreakpointsEx(Context, ThreadBreakpoints, TRUE);
			}
		}
		else {
			SetLastError(pRtlNtStatusToDosError(ret));
			ErrorOutput("NtSetContextThread: Failed to protect debugger breakpoints");
		}
	}

	ret = Old_NtSetContextThread(ThreadHandle, Context);

	if (Context && Context->ContextFlags & CONTEXT_CONTROL)
#ifdef _WIN64
		LOQ_ntstatus("threading", "ppp", "ThreadHandle", ThreadHandle, "HollowedInstructionPointer", Context->Rcx, "CurrentInstructionPointer", Context->Rip);
#else
		LOQ_ntstatus("threading", "ppp", "ThreadHandle", ThreadHandle, "HollowedInstructionPointer", Context->Eax, "CurrentInstructionPointer", Context->Eip);
#endif
	else
		LOQ_ntstatus("threading", "p", "ThreadHandle", ThreadHandle);
	//if (g_config.injection)
	SetThreadContextHandler(pid, Context);
	if (pid != GetCurrentProcessId())
		ProcessMessage(pid, tid);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtSuspendThread,
	__in		HANDLE ThreadHandle,
	__out_opt   ULONG *PreviousSuspendCount
) {
	NTSTATUS ret;
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);
	ENSURE_ULONG(PreviousSuspendCount);

	if (pid == GetCurrentProcessId() && tid && (tid == g_unhook_detect_thread_id || tid == g_unhook_watcher_thread_id ||
		tid == g_watchdog_thread_id || tid == g_terminate_event_thread_id || tid == g_log_thread_id ||
		tid == g_logwatcher_thread_id || tid == g_procname_watcher_thread_id)) {
		ret = 0;
		*PreviousSuspendCount = 0;
		LOQ_ntstatus("threading", "pLsi", "ThreadHandle", ThreadHandle,
			"SuspendCount", PreviousSuspendCount, "Alert", "Attempted to suspend capemon thread",
			"ProcessId", pid);
	}
	else {
		if (pid != GetCurrentProcessId())
			ProcessMessage(pid, tid);
		ret = Old_NtSuspendThread(ThreadHandle, PreviousSuspendCount);
		LOQ_ntstatus("threading", "pLii", "ThreadHandle", ThreadHandle, "SuspendCount", PreviousSuspendCount, "ThreadId", tid,
		"ProcessId", pid);
	}
	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtResumeThread,
	__in		HANDLE ThreadHandle,
	__out_opt   ULONG *SuspendCount
) {
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);
	NTSTATUS ret;
	ENSURE_ULONG(SuspendCount);
	if (g_config.injection)
		ResumeThreadHandler(pid);
	if (pid != GetCurrentProcessId())
		pipe("RESUME:%d,%d", pid, tid);

	ret = Old_NtResumeThread(ThreadHandle, SuspendCount);
	LOQ_ntstatus("threading", "pIi", "ThreadHandle", ThreadHandle, "SuspendCount", SuspendCount, "ProcessId", pid);
	return ret;
}

extern DWORD tmphookinfo_threadid;

HOOKDEF(NTSTATUS, WINAPI, NtTerminateThread,
	__in  HANDLE ThreadHandle,
	__in  NTSTATUS ExitStatus
) {
	// Thread will terminate. Default logging will not work. Be aware: return value not valid
	DWORD pid = pid_from_thread_handle(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);
	NTSTATUS ret = 0;

	if (tmphookinfo_threadid && tid == tmphookinfo_threadid) {
		tmphookinfo_threadid = 0;
	}

	//remove_ignored_thread(tid);

	if (pid == GetCurrentProcessId() && tid && (tid == g_unhook_detect_thread_id || tid == g_unhook_watcher_thread_id ||
		tid == g_watchdog_thread_id || tid == g_terminate_event_thread_id || tid == g_log_thread_id ||
		tid == g_logwatcher_thread_id || tid == g_procname_watcher_thread_id)) {
		ret = 0;
		LOQ_ntstatus("threading", "phsi", "ThreadHandle", ThreadHandle, "ExitStatus", ExitStatus, "Alert", "Attempted to kill capemon thread",
		"ProcessId", pid);
		return ret;
	}

	LOQ_ntstatus("threading", "phii", "ThreadHandle", ThreadHandle, "ExitStatus", ExitStatus, "ThreadId", tid, "ProcessId", pid);

	ret = Old_NtTerminateThread(ThreadHandle, ExitStatus);

	disable_tail_call_optimization();

	return ret;
}

HOOKDEF(HANDLE, WINAPI, CreateThread,
	__in   LPSECURITY_ATTRIBUTES lpThreadAttributes,
	__in   SIZE_T dwStackSize,
	__in   LPTHREAD_START_ROUTINE lpStartAddress,
	__in   LPVOID lpParameter,
	__in   DWORD dwCreationFlags,
	__out_opt  LPDWORD lpThreadId
) {
	HANDLE ret;
	ENSURE_DWORD(lpThreadId);

	ret = Old_CreateThread(lpThreadAttributes, dwStackSize,
		lpStartAddress, lpParameter, dwCreationFlags | CREATE_SUSPENDED, lpThreadId);

	if (ret != NULL) {
		if (g_config.debugger && !called_by_hook()) {
			DebugOutput("CreateThread: Initialising breakpoints for thread %d.\n", *lpThreadId);
			InitNewThreadBreakpoints(*lpThreadId);
		}

		if (!(dwCreationFlags & CREATE_SUSPENDED)) {
			lasterror_t lasterror;
			get_lasterrors(&lasterror);
			ResumeThread(ret);
			set_lasterrors(&lasterror);
		}

		LOQ_nonnull("threading", "pphI", "StartRoutine", lpStartAddress, "Parameter", lpParameter,
			"CreationFlags", dwCreationFlags, "ThreadId", lpThreadId);

		disable_sleep_skip();
	}
	else
		LOQ_nonnull("threading", "pphI", "StartRoutine", lpStartAddress, "Parameter", lpParameter,
			"CreationFlags", dwCreationFlags);

	return ret;
}

HOOKDEF(HANDLE, WINAPI, CreateRemoteThread,
	__in   HANDLE hProcess,
	__in   LPSECURITY_ATTRIBUTES lpThreadAttributes,
	__in   SIZE_T dwStackSize,
	__in   LPTHREAD_START_ROUTINE lpStartAddress,
	__in   LPVOID lpParameter,
	__in   DWORD dwCreationFlags,
	__out_opt  LPDWORD lpThreadId
) {
	DWORD pid;
	HANDLE ret;
	ENSURE_DWORD(lpThreadId);

	pid = pid_from_process_handle(hProcess);
	ret = Old_CreateRemoteThread(hProcess, lpThreadAttributes,
		dwStackSize, lpStartAddress, lpParameter, dwCreationFlags | CREATE_SUSPENDED,
		lpThreadId);

	if (ret != NULL) {
		if (pid != GetCurrentProcessId())
			ProcessMessage(pid, *lpThreadId);
		else if (g_config.debugger && !called_by_hook()) {
			DebugOutput("CreateRemoteThread: Initialising breakpoints for (local) thread %d.\n", *lpThreadId);
			InitNewThreadBreakpoints(*lpThreadId);
		}

		if (!(dwCreationFlags & CREATE_SUSPENDED)) {
			lasterror_t lasterror;
			get_lasterrors(&lasterror);
			ResumeThread(ret);
			set_lasterrors(&lasterror);
		}

		disable_sleep_skip();
	}

	LOQ_nonnull("threading", "ppphI", "ProcessHandle", hProcess, "StartRoutine", lpStartAddress,
		"Parameter", lpParameter, "CreationFlags", dwCreationFlags,
		"ThreadId", lpThreadId, "ProcessId", pid);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, RtlCreateUserThread,
	IN HANDLE ProcessHandle,
	IN PSECURITY_DESCRIPTOR SecurityDescriptor OPTIONAL,
	IN BOOLEAN CreateSuspended,
	IN ULONG StackZeroBits,
	IN OUT PULONG StackReserved,
	IN OUT PULONG StackCommit,
	IN PVOID StartAddress,
	IN PVOID StartParameter OPTIONAL,
	OUT PHANDLE ThreadHandle,
	OUT PCLIENT_ID ClientId
) {
	DWORD pid;
	NTSTATUS ret;
	ENSURE_HANDLE(ThreadHandle);
	ENSURE_CLIENT_ID(ClientId);

	pid = pid_from_process_handle(ProcessHandle);

	ret = Old_RtlCreateUserThread(ProcessHandle, SecurityDescriptor,
		TRUE, StackZeroBits, StackReserved, StackCommit,
		StartAddress, StartParameter, ThreadHandle, ClientId);

	if (NT_SUCCESS(ret) && ClientId && ThreadHandle) {
		DWORD tid = tid_from_thread_handle(ThreadHandle);
		if (pid != GetCurrentProcessId())
			ProcessMessage(pid, tid);
		else if (g_config.debugger && !called_by_hook()) {
			DebugOutput("RtlCreateUserThread: Initialising breakpoints for (local) thread %d.\n", tid);
			InitNewThreadBreakpoints(tid);
		}
		if (CreateSuspended == FALSE && is_valid_address_range((ULONG_PTR)ThreadHandle, 4)) {
			lasterror_t lasterror;
			get_lasterrors(&lasterror);
			ResumeThread(*ThreadHandle);
			set_lasterrors(&lasterror);
		}
		disable_sleep_skip();
	}

	LOQ_ntstatus("threading", "pippPi", "ProcessHandle", ProcessHandle,
		"CreateSuspended", CreateSuspended, "StartAddress", StartAddress,
		"StartParameter", StartParameter, "ThreadHandle", ThreadHandle,
		"ThreadId", ClientId->UniqueThread);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtSetInformationThread,
	IN HANDLE ThreadHandle,
	IN THREADINFOCLASS ThreadInformationClass,
	IN PVOID ThreadInformation,
	IN ULONG ThreadInformationLength
) {
	NTSTATUS ret;
	ENSURE_HANDLE(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);

	ret = Old_NtSetInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);

	if (ThreadInformationClass == ThreadHideFromDebugger)
		LOQ_ntstatus("threading", "pii", "ThreadHandle", ThreadHandle,
			"ThreadInformationClass", ThreadInformationClass,
			"ThreadId", tid);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtQueryInformationThread,
	IN HANDLE ThreadHandle,
	IN THREADINFOCLASS ThreadInformationClass,
	OUT PVOID ThreadInformation,
	IN ULONG ThreadInformationLength,
	OUT PULONG ReturnLength OPTIONAL
) {
	NTSTATUS ret;
	ENSURE_HANDLE(ThreadHandle);
	DWORD tid = tid_from_thread_handle(ThreadHandle);

	ret = Old_NtQueryInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength, ReturnLength);

	LOQ_ntstatus("threading", "pibi", "ThreadHandle", ThreadHandle,
		"ThreadInformationClass", ThreadInformationClass,
		"ThreadInformation", ThreadInformationLength, ThreadInformation,
		"ThreadId", tid);

	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtYieldExecution,
	VOID
) {
	NTSTATUS ret = 0;
	LOQ_void("threading", "");
	ret = Old_NtYieldExecution();
	return ret;
}

HOOKDEF(NTSTATUS, WINAPI, NtContinue,
	IN PCONTEXT ThreadContext,
	IN BOOLEAN  RaiseAlert
)
{
	NTSTATUS ret = 0;
	DWORD ThreadId = GetCurrentThreadId();
	if (g_config.debugger) {
		PTHREADBREAKPOINTS Bps = GetThreadBreakpoints(ThreadId);
		if (Bps) {
#ifdef DEBUG_COMMENTS
			DebugOutput("NtContinue hook: restoring breakpoints for thread %d: Dr0 0x%x, Dr1 0x%x, Dr2 0x%x, Dr3 0x%x\n", ThreadId, ThreadContext->Dr0, ThreadContext->Dr1, ThreadContext->Dr2, ThreadContext->Dr3);
#endif
			if (ThreadContext->Dr0 && (PVOID)ThreadContext->Dr0 == Bps->BreakpointInfo[0].Address)
				ContextSetThreadBreakpointEx(ThreadContext, Bps->BreakpointInfo[0].Register, Bps->BreakpointInfo[0].Size, Bps->BreakpointInfo[0].Address, Bps->BreakpointInfo[0].Type, Bps->BreakpointInfo[0].HitCount, Bps->BreakpointInfo[0].Callback, TRUE);
			if (ThreadContext->Dr1 && (PVOID)ThreadContext->Dr1 == Bps->BreakpointInfo[1].Address)
				ContextSetThreadBreakpointEx(ThreadContext, Bps->BreakpointInfo[1].Register, Bps->BreakpointInfo[1].Size, Bps->BreakpointInfo[1].Address, Bps->BreakpointInfo[1].Type, Bps->BreakpointInfo[1].HitCount, Bps->BreakpointInfo[1].Callback, TRUE);
			if (ThreadContext->Dr2 && (PVOID)ThreadContext->Dr2 == Bps->BreakpointInfo[2].Address)
				ContextSetThreadBreakpointEx(ThreadContext, Bps->BreakpointInfo[2].Register, Bps->BreakpointInfo[2].Size, Bps->BreakpointInfo[2].Address, Bps->BreakpointInfo[2].Type, Bps->BreakpointInfo[2].HitCount, Bps->BreakpointInfo[2].Callback, TRUE);
			if (ThreadContext->Dr3 && (PVOID)ThreadContext->Dr3 == Bps->BreakpointInfo[3].Address)
				ContextSetThreadBreakpointEx(ThreadContext, Bps->BreakpointInfo[3].Register, Bps->BreakpointInfo[3].Size, Bps->BreakpointInfo[3].Address, Bps->BreakpointInfo[3].Type, Bps->BreakpointInfo[3].HitCount, Bps->BreakpointInfo[3].Callback, TRUE);
		}
#ifdef DEBUG_COMMENTS
		else
			DebugOutput("NtContinue hook: Unable to restore breakpoints for thread %d.\n", ThreadId);
#endif

#ifndef _WIN64
		if (BreakOnNtContinue) {
			BreakOnNtContinue = FALSE;
			for (unsigned int Register = 0; Register < NUMBER_OF_DEBUG_REGISTERS; Register++) {
				if (!Bps->BreakpointInfo[Register].Address) {
					ContextSetThreadBreakpointEx(ThreadContext, Register, 0, (PBYTE)ThreadContext->Eip, BP_EXEC, 0, BreakOnNtContinueCallback, TRUE);
					break;
				}
			}
			BreakOnNtContinueCallback = NULL;
		}
		else if (BreakOnNtContinueCallback) {
			//BreakOnNtContinue = TRUE;
			PEXCEPTION_REGISTRATION_RECORD SEH = (PEXCEPTION_REGISTRATION_RECORD)__readfsdword(0);
			for (unsigned int Register = 0; Register < NUMBER_OF_DEBUG_REGISTERS; Register++) {
				if (!Bps->BreakpointInfo[Register].Address) {
					ContextSetThreadBreakpointEx(ThreadContext, Register, 0, (PBYTE)SEH->Handler, BP_EXEC, 0, BreakOnNtContinueCallback, TRUE);
					StepOverRegister = Register;
					break;
				}
			}
			BreakOnNtContinueCallback = NULL;
		}
#endif
	}
	ret = Old_NtContinue(ThreadContext, RaiseAlert);
	return ret;
}

HOOKDEF(BOOL, WINAPI, SwitchToThread,
	void
) {
	BOOL ret = Old_SwitchToThread();
	//LOQ_zero("threading", "i", "ReturnValue", ret);
	//return ret;
	return TRUE;
}
