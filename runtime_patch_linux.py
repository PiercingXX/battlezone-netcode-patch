#!/usr/bin/env python3
import ctypes
import errno
import os
import signal
import subprocess
import sys
from typing import List, Tuple

PTRACE_ATTACH = 16
PTRACE_DETACH = 17

LIBC = ctypes.CDLL(None, use_errno=True)


def ptrace(req: int, pid: int, addr: int, data: int) -> int:
    res = LIBC.ptrace(req, pid, ctypes.c_void_p(addr), ctypes.c_void_p(data))
    if res == -1:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))
    return res


def wait_stopped(pid: int) -> None:
    _, status = os.waitpid(pid, 0)
    if not os.WIFSTOPPED(status):
        raise RuntimeError(f"Process {pid} did not stop after attach")


def read4_mem(mem, addr: int) -> bytes:
    mem.seek(addr)
    return mem.read(4)


def write4_mem(mem, addr: int, data4: bytes) -> None:
    mem.seek(addr)
    mem.write(data4)
    mem.flush()


def find_pid() -> int:
    out = subprocess.check_output(["pgrep", "-f", r"Battlezone98Redux\.exe"], text=True).strip().splitlines()
    if not out:
        raise RuntimeError("No running Battlezone98Redux.exe process found")
    return int(out[0])


def executable_maps(pid: int) -> List[Tuple[int, int]]:
    out: List[Tuple[int, int]] = []
    with open(f"/proc/{pid}/maps", "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            rng, perms = parts[0], parts[1]
            if "x" not in perms or "r" not in perms:
                continue
            start_s, end_s = rng.split("-")
            out.append((int(start_s, 16), int(end_s, 16)))
    return out


def find_signature_addrs(pid: int, sig: bytes) -> List[int]:
    hits: List[int] = []
    ranges = executable_maps(pid)
    with open(f"/proc/{pid}/mem", "rb", buffering=0) as mem:
        for start, end in ranges:
            size = end - start
            if size <= 0:
                continue
            try:
                mem.seek(start)
                buf = mem.read(size)
            except OSError:
                continue
            i = 0
            while True:
                j = buf.find(sig, i)
                if j < 0:
                    break
                hits.append(start + j)
                i = j + 1
    return hits


def main() -> int:
    # Runtime addresses observed in Proton/Wine for this build.
    send_addr_known = 0x02D1016A
    recv_addr_known = 0x02D1035E

    send_sig_old = bytes.fromhex("68008000006a0053ff95")
    send_sig_new = bytes.fromhex("68000008006a0053ff95")
    recv_sig_old = bytes.fromhex("6800001000ff95d8feffff")
    recv_sig_new = bytes.fromhex("6800002000ff95d8feffff")
    send_new = bytes.fromhex("00000800")
    recv_new = bytes.fromhex("00002000")

    try:
        pid = find_pid()
    except Exception as e:
        print(str(e), file=sys.stderr)
        return 2

    print(f"Found process PID: {pid}")

    attached = False
    try:
        ptrace(PTRACE_ATTACH, pid, 0, 0)
        attached = True
        wait_stopped(pid)

        with open(f"/proc/{pid}/mem", "r+b", buffering=0) as mem:
            send_before = read4_mem(mem, send_addr_known).hex()
            recv_before = read4_mem(mem, recv_addr_known).hex()

            # If known addresses do not contain expected old/new immediates, fall back to signature scan.
            if send_before not in {"00800000", "00000800"} or recv_before not in {"00001000", "00002000"}:
                send_hits = find_signature_addrs(pid, send_sig_old)
                if not send_hits:
                    send_hits = find_signature_addrs(pid, send_sig_new)

                recv_hits = find_signature_addrs(pid, recv_sig_old)
                if not recv_hits:
                    recv_hits = find_signature_addrs(pid, recv_sig_new)

                if not send_hits or not recv_hits:
                    print("Could not locate runtime signatures in process memory.", file=sys.stderr)
                    return 3

                send_addr = send_hits[0] + 1
                recv_addr = recv_hits[0] + 1
                print(f"Send imm addr: 0x{send_addr:08x} (matches: {len(send_hits)})")
                print(f"Recv imm addr: 0x{recv_addr:08x} (matches: {len(recv_hits)})")
            else:
                send_addr = send_addr_known
                recv_addr = recv_addr_known
                print(f"Send imm addr: 0x{send_addr:08x}")
                print(f"Recv imm addr: 0x{recv_addr:08x}")

            write4_mem(mem, send_addr, send_new)
            write4_mem(mem, recv_addr, recv_new)
            send_now = read4_mem(mem, send_addr).hex()
            recv_now = read4_mem(mem, recv_addr).hex()

        print(f"send now: {send_now}")
        print(f"recv now: {recv_now}")

        if send_now == "00000800" and recv_now == "00002000":
            print("Runtime patch applied in memory.")
            return 0

        print("Runtime patch could not be verified.", file=sys.stderr)
        return 3
    except OSError as e:
        if e.errno == errno.EPERM:
            print("Runtime patch failed: ptrace attach is blocked by policy or tracer conflict.", file=sys.stderr)
            print("Run this once, then rerun patch:", file=sys.stderr)
            print("  sudo sysctl -w kernel.yama.ptrace_scope=0", file=sys.stderr)
            print("Also ensure no stale tracer is attached (TracerPid must be 0).", file=sys.stderr)
            return 4
        print(f"Runtime patch failed: {e}", file=sys.stderr)
        return 5
    finally:
        if attached:
            try:
                ptrace(PTRACE_DETACH, pid, 0, 0)
            except Exception:
                pass
            try:
                os.kill(pid, signal.SIGCONT)
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
