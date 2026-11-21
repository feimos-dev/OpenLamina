#!/usr/bin/env python3
"""Runs a command at medium integrity, for OpenXR tests: the OpenXR loader ignores XR_RUNTIME_JSON in a process whose
integrity level is above medium (an elevated process).

  unelevated.py [--env NAME=VALUE]... [--cwd DIR] [--timeout SECONDS] -- COMMAND [ARGS...]
  unelevated.py --self-test

Not elevated (or not on Windows): runs COMMAND directly. At high integrity on Windows: starts COMMAND with a copy of
this process's token lowered to medium integrity (DuplicateTokenEx, SetTokenInformation(TokenIntegrityLevel),
CreateProcessAsUserW, as Microsoft's sample for low-integrity processes does), waits for it and exits with its exit
code. Its output goes to this process's output.
"""

import argparse
import itertools
import os
import subprocess
import sys
import time


def _win():
    import ctypes
    from ctypes import wintypes
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    advapi32.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
    advapi32.GetSidSubAuthorityCount.argtypes = [ctypes.c_void_p]
    advapi32.GetSidSubAuthority.restype = ctypes.POINTER(wintypes.DWORD)
    advapi32.GetSidSubAuthority.argtypes = [ctypes.c_void_p, wintypes.DWORD]
    advapi32.GetLengthSid.argtypes = [ctypes.c_void_p]
    advapi32.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    return ctypes, wintypes, advapi32, kernel32


def integrity_above_medium():
    if os.name != "nt":
        return False
    ctypes, wintypes, advapi32, kernel32 = _win()
    token = wintypes.HANDLE()
    if not advapi32.OpenProcessToken(kernel32.GetCurrentProcess(), 0x0008, ctypes.byref(token)):  # TOKEN_QUERY
        return False
    buf = (ctypes.c_byte * 64)()
    size = wintypes.DWORD()
    ok = advapi32.GetTokenInformation(token, 25, buf, 64, ctypes.byref(size))  # TokenIntegrityLevel
    kernel32.CloseHandle(token)
    if not ok:
        return False
    sid = ctypes.cast(buf, ctypes.POINTER(ctypes.c_void_p))[0]
    count = advapi32.GetSidSubAuthorityCount(sid)[0]
    level = advapi32.GetSidSubAuthority(sid, count - 1)[0]
    return level > 0x2000  # SECURITY_MANDATORY_MEDIUM_RID


# Every test child runs in a job with a memory limit, so a test that runs away (a loop that allocates without end)
# fails on its own instead of taking the PC's memory. Per process and for the child with everything it starts.
PROCESS_MEMORY_LIMIT = 2 * 1024 ** 3
JOB_MEMORY_LIMIT = 3 * 1024 ** 3


def warn(text):
    print(f"unelevated.py: {text}", file=sys.stderr, flush=True)


def memory_limited_job():
    """A job object whose processes may commit PROCESS_MEMORY_LIMIT each and JOB_MEMORY_LIMIT together (Windows; None
    elsewhere or if it can't be made). Processes a member starts are members too, and closing the job ends every
    process still in it."""
    if os.name != "nt":
        return None
    ctypes, wintypes, advapi32, kernel32 = _win()

    class BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

    class EXTENDED(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BASIC), ("IoInfo", ctypes.c_uint64 * 6),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        warn("no job object: the child runs without a memory limit")
        return None
    info = EXTENDED()
    # JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    info.BasicLimitInformation.LimitFlags = 0x100 | 0x200 | 0x2000
    info.ProcessMemoryLimit = PROCESS_MEMORY_LIMIT
    info.JobMemoryLimit = JOB_MEMORY_LIMIT
    kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    if not kernel32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):  # ExtendedLimitInformation
        kernel32.CloseHandle(job)
        warn("no job limits: the child runs without a memory limit")
        return None
    return job


def assign_to_job(job, process_handle):
    if job is None:
        return False
    ctypes, wintypes, advapi32, kernel32 = _win()
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    if kernel32.AssignProcessToJobObject(job, process_handle):
        return True
    warn(f"AssignProcessToJobObject failed ({ctypes.get_last_error()}): the child runs without a memory limit")
    return False


def end_job(job):
    """Ends every process in the job; False if it couldn't"""
    if job is None:
        return False
    ctypes, wintypes, advapi32, kernel32 = _win()
    kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    return bool(kernel32.TerminateJobObject(job, 124))


def close_job(job):
    """Closes the job, which ends every process still in it (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)"""
    if job is not None:
        ctypes, wintypes, advapi32, kernel32 = _win()
        kernel32.CloseHandle(job)


def run_limited(command, env, cwd, timeout, quiet=False):
    """subprocess.run in a memory-limited job (not elevated, or not on Windows): its exit code, 124 on a timeout. With
    quiet, its output is discarded as it comes (none of it is held here). What it started and left running is ended
    when it ends or times out: on Windows everything in its job, elsewhere its process group."""
    out = subprocess.DEVNULL if quiet else None
    if os.name != "nt":
        import signal
        p = subprocess.Popen(command, env=env, cwd=cwd, stdout=out, stderr=out, start_new_session=True)
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124
        finally:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass  # nothing left in its group
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                warn(f"process {p.pid} didn't end after SIGKILL")

    ctypes, wintypes, advapi32, kernel32 = _win()
    job = memory_limited_job()
    # CREATE_SUSPENDED: in the job before it runs, so nothing it starts escapes the job
    p = subprocess.Popen(command, env=env, cwd=cwd, stdout=out, stderr=out, creationflags=0x4)
    try:
        assign_to_job(job, int(p._handle))
        ntdll = ctypes.WinDLL("ntdll")
        ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
        if ntdll.NtResumeProcess(int(p._handle)) != 0:
            warn("NtResumeProcess failed")
            p.kill()
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if not end_job(job):
                p.kill()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                warn(f"process {p.pid} didn't end after its timeout")
            return 124
    finally:
        close_job(job)


def run_medium(command, env, cwd, timeout):
    import msvcrt
    ctypes, wintypes, advapi32, kernel32 = _win()

    class SID_AND_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wintypes.DWORD)]

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
                    ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                    ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD), ("dwXCountChars", wintypes.DWORD),
                    ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD),
                    ("dwFlags", wintypes.DWORD), ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                    ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                    ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE), ("dwProcessId", wintypes.DWORD),
                    ("dwThreadId", wintypes.DWORD)]

    def check(ok, what):
        if not ok:
            raise OSError(ctypes.get_last_error(), what)

    token, dup = wintypes.HANDLE(), wintypes.HANDLE()
    # TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT
    check(advapi32.OpenProcessToken(kernel32.GetCurrentProcess(), 0x0001 | 0x0002 | 0x0008 | 0x0080,
                                    ctypes.byref(token)), "OpenProcessToken")
    # MAXIMUM_ALLOWED, SecurityImpersonation, TokenPrimary
    check(advapi32.DuplicateTokenEx(token, 0x02000000, None, 2, 1, ctypes.byref(dup)), "DuplicateTokenEx")
    sid = ctypes.c_void_p()
    check(advapi32.ConvertStringSidToSidW("S-1-16-8192", ctypes.byref(sid)), "ConvertStringSidToSidW")
    label = SID_AND_ATTRIBUTES(sid, 0x20)  # SE_GROUP_INTEGRITY
    check(advapi32.SetTokenInformation(dup, 25, ctypes.byref(label),
                                       ctypes.sizeof(label) + advapi32.GetLengthSid(sid)), "SetTokenInformation")

    handles = []
    for f in (sys.stdin, sys.stdout, sys.stderr):
        try:
            h = msvcrt.get_osfhandle(f.fileno())
            os.set_handle_inheritable(h, True)
            handles.append(h)
        except (OSError, ValueError, AttributeError):
            handles.append(None)
    sys.stdout.flush()
    sys.stderr.flush()

    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.dwFlags = 0x100  # STARTF_USESTDHANDLES
    si.hStdInput, si.hStdOutput, si.hStdError = handles
    pi = PROCESS_INFORMATION()
    block = ctypes.create_unicode_buffer("".join(f"{k}={v}\0" for k, v in env.items()) + "\0")
    cmdline = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
    # bInheritHandles, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED: in the memory-limited job before it runs
    check(advapi32.CreateProcessAsUserW(dup, None, cmdline, None, None, True, 0x400 | 0x4, block, cwd,
                                        ctypes.byref(si), ctypes.byref(pi)), "CreateProcessAsUserW")
    job = memory_limited_job()
    assign_to_job(job, pi.hProcess)
    kernel32.ResumeThread(pi.hThread)
    wait = kernel32.WaitForSingleObject(pi.hProcess, int(timeout * 1000))
    if wait != 0:  # WAIT_TIMEOUT
        if not end_job(job):
            kernel32.TerminateProcess(pi.hProcess, 124)
        kernel32.WaitForSingleObject(pi.hProcess, 5000)
        print(f"unelevated.py: timed out after {timeout} s", file=sys.stderr)
        result = 124
    else:
        code = wintypes.DWORD()
        kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
        result = code.value
    for h in (pi.hProcess, pi.hThread, dup, token):
        kernel32.CloseHandle(h)
    close_job(job)
    kernel32.LocalFree(sid)
    return result


def run(command, env_pairs, cwd, timeout):
    env = dict(os.environ)
    env.update(env_pairs)
    if not integrity_above_medium():
        code = run_limited(command, env, cwd, timeout)
        if code == 124:
            print(f"unelevated.py: timed out after {timeout} s", file=sys.stderr)
        return code
    return run_medium(command, env, cwd, timeout)


def process_alive(pid):
    if os.name != "nt":
        try:
            with open(f"/proc/{pid}/stat", encoding="utf-8") as f:
                return f.read().rsplit(")", 1)[1].split()[0] != "Z"
        except OSError:
            return False
    ctypes, wintypes, advapi32, kernel32 = _win()
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    h =kernel32.OpenProcess(0x00100000 | 0x1000, False, pid)  # SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    alive = kernel32.WaitForSingleObject(h, 0) != 0
    kernel32.CloseHandle(h)
    return alive


def peak_memory():
    """This process's peak memory use, in bytes"""
    if os.name != "nt":
        import resource
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024
    ctypes, wintypes, advapi32, kernel32 = _win()

    class COUNTERS(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

    c = COUNTERS()
    c.cb = ctypes.sizeof(c)
    psapi = ctypes.WinDLL("psapi")
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    psapi.GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(c), c.cb)
    return max(c.PeakWorkingSetSize, c.PeakPagefileUsage)


def self_test():
    """A child's output isn't held in this process, and nothing a child started outlives it (after its end or its
    timeout). Every child here ends on its own within a minute."""
    import tempfile
    failed = []
    env = dict(os.environ)
    py = sys.executable

    # 256 MiB printed as fast as it goes
    before = peak_memory()
    code = run_limited([py, "-c", "import sys\nfor _ in range(256): sys.stdout.write('x' * 1048576)"], env, None, 60,
                       quiet=True)
    grew = peak_memory() - before
    if code != 0 or grew > 64 * 1024 ** 2:
        failed.append(f"output: exit {code}, this process grew by {grew // 1024 ** 2} MiB")

    # A grandchild that would sleep for a minute, left behind by a child that ends or times out (2 s); in a job of
    # this process's, and (elevated) at medium integrity
    runners = [("", lambda cmd, timeout: run_limited(cmd, env, None, timeout))]
    if integrity_above_medium():
        runners.append(("medium integrity, ", lambda cmd, timeout: run_medium(cmd, env, None, timeout)))
    with tempfile.TemporaryDirectory() as folder:
        cases = itertools.product(runners, (("ended", 0, 30), ("timed out", 60, 2)))
        for i, ((how, runner), (what, child_sleeps, timeout)) in enumerate(cases):
            name = how + what
            pid_file = os.path.join(folder, f"{i}.pid")
            script = ("import subprocess, sys, time\n"
                      "g = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                      f"open({pid_file!r}, 'w').write(str(g.pid))\n"
                      f"time.sleep({child_sleeps})\n")
            code = runner([py, "-c", script], timeout)
            try:
                with open(pid_file, encoding="utf-8") as f:
                    pid = int(f.read())
            except (OSError, ValueError):
                failed.append(f"{name}: the child wrote no grandchild pid (exit {code})")
                continue
            for _ in range(50):
                if not process_alive(pid):
                    break
                time.sleep(0.1)
            else:
                failed.append(f"{name}: grandchild {pid} still running")
            if (code == 124) != (timeout == 2):
                failed.append(f"{name}: exit {code}")
    if failed:
        print("self-test FAILED: " + "; ".join(failed))
        return 1
    print("self-test passed")
    return 0


def main():
    argv = sys.argv[1:]
    if argv == ["--self-test"]:
        return self_test()
    if "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    split = argv.index("--")
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--cwd")
    ap.add_argument("--timeout", type=float, default=120)
    args = ap.parse_args(argv[:split])
    env = dict(e.split("=", 1) for e in args.env)
    return run(argv[split + 1:], env, args.cwd, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
