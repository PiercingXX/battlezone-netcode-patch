#!/usr/bin/env python3
import ctypes
import errno
import os
import signal
import subprocess
import sys
import argparse
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


def parse_maps(pid: int):
    out = []
    with open(f"/proc/{pid}/maps", "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 5:
                continue
            rng, perms = parts[0], parts[1]
            start_s, end_s = rng.split("-")
            path = parts[5] if len(parts) >= 6 else ""
            out.append((int(start_s, 16), int(end_s, 16), perms, path))
    return out


def executable_maps(pid: int) -> List[Tuple[int, int]]:
    out: List[Tuple[int, int]] = []
    for start, end, perms, _path in parse_maps(pid):
        if "x" in perms and "r" in perms:
            out.append((start, end))
    return out


def main_module_exec_ranges(pid: int) -> List[Tuple[int, int]]:
    ranges: List[Tuple[int, int]] = []
    target = "/battlezone98redux.exe"
    for start, end, perms, path in parse_maps(pid):
        p = path.lower()
        if target in p and "x" in perms and "r" in perms:
            ranges.append((start, end))

    if ranges:
        return ranges

    # Fallback for unusual map labeling.
    return executable_maps(pid)


def find_signature_addrs_in_ranges(pid: int, sig: bytes, ranges: List[Tuple[int, int]]) -> List[int]:
    hits: List[int] = []
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


def find_push_imm_candidates_in_ranges(pid: int, values: List[bytes], ranges: List[Tuple[int, int]]) -> List[int]:
    # Match x86 opcode: 68 <imm32>
    hits: List[int] = []
    patterns = [bytes([0x68]) + v for v in values]
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
            for pat in patterns:
                i = 0
                while True:
                    j = buf.find(pat, i)
                    if j < 0:
                        break
                    # Address of immediate bytes (skip opcode byte)
                    hits.append(start + j + 1)
                    i = j + 1
    # Deduplicate while preserving order
    seen = set()
    uniq = []
    for h in hits:
        if h not in seen:
            seen.add(h)
            uniq.append(h)
    return uniq


def choose_addr_pair(send_addrs: List[int], recv_addrs: List[int], preferred_delta: int = 0x1F4):
    pairs = []
    recv_set = set(recv_addrs)
    for s in send_addrs:
        # Strong preference path.
        r = s + preferred_delta
        if r in recv_set:
            pairs.append((s, r, 0))

    if not pairs:
        # Broader but still constrained pairing.
        for s in send_addrs:
            for r in recv_addrs:
                d = r - s
                if 0 < d <= 0x2000:
                    pairs.append((s, r, abs(d - preferred_delta)))

    if not pairs:
        return None

    pairs.sort(key=lambda x: x[2])
    best_score = pairs[0][2]
    best = [p for p in pairs if p[2] == best_score]
    if len(best) == 1:
        return best[0][0], best[0][1], best_score

    return "ambiguous", best, best_score


def main() -> int:
    parser = argparse.ArgumentParser(description="Runtime patch Battlezone net buffers on Linux/Proton")
    parser.add_argument("--pid", type=int, default=0, help="Patch this specific Battlezone PID")
    args = parser.parse_args()

    # Runtime addresses observed in one build; only fast path.
    send_addr_known = 0x02D1016A
    recv_addr_known = 0x02D1035E

    send_sig_old = bytes.fromhex("68008000006a0053ff95")
    send_sig_new = bytes.fromhex("68000008006a0053ff95")
    recv_sig_old = bytes.fromhex("6800001000ff95d8feffff")
    recv_sig_new = bytes.fromhex("6800002000ff95d8feffff")
    send_new = bytes.fromhex("00000800")
    recv_new = bytes.fromhex("00002000")

    try:
        pid = args.pid if args.pid > 0 else find_pid()
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
            send_addr = send_addr_known
            recv_addr = recv_addr_known

            # Fast path on known build.
            send_before = read4_mem(mem, send_addr).hex()
            recv_before = read4_mem(mem, recv_addr).hex()
            if send_before in {"00800000", "00000800"} and recv_before in {"00001000", "00002000"}:
                print(f"Send imm addr: 0x{send_addr:08x} (known)")
                print(f"Recv imm addr: 0x{recv_addr:08x} (known)")
            else:
                # Build-tolerant resolution path.
                ranges = main_module_exec_ranges(pid)

                send_hits = find_signature_addrs_in_ranges(pid, send_sig_old, ranges)
                if not send_hits:
                    send_hits = find_signature_addrs_in_ranges(pid, send_sig_new, ranges)

                recv_hits = find_signature_addrs_in_ranges(pid, recv_sig_old, ranges)
                if not recv_hits:
                    recv_hits = find_signature_addrs_in_ranges(pid, recv_sig_new, ranges)

                # Fallback to looser push-imm scanning if full signatures drift across builds.
                if not send_hits:
                    send_hits = [a - 1 for a in find_push_imm_candidates_in_ranges(pid, [bytes.fromhex("00800000"), bytes.fromhex("00000800")], ranges)]
                if not recv_hits:
                    recv_hits = [a - 1 for a in find_push_imm_candidates_in_ranges(pid, [bytes.fromhex("00001000"), bytes.fromhex("00002000")], ranges)]

                # Convert signature hit (start of pattern) to immediate address.
                send_imm_addrs = [a + 1 for a in send_hits]
                recv_imm_addrs = [a + 1 for a in recv_hits]

                choice = choose_addr_pair(send_imm_addrs, recv_imm_addrs)
                if choice is None:
                    print("Could not resolve runtime patch addresses for this build.", file=sys.stderr)
                    return 3
                if isinstance(choice[0], str) and choice[0] == "ambiguous":
                    print("Ambiguous runtime patch addresses; refusing to guess.", file=sys.stderr)
                    for s, r, score in choice[1][:8]:
                        print(f"- send 0x{s:08x}, recv 0x{r:08x}, score {score}", file=sys.stderr)
                    return 3

                send_addr, recv_addr, score = choice
                print(f"Send imm addr: 0x{send_addr:08x} (resolved, score {score})")
                print(f"Recv imm addr: 0x{recv_addr:08x} (resolved, score {score})")

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
