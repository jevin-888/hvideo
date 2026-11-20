"""Socket-level regression tests using the production TCP sender (Linux g++).

The executable and extracted source live only in an automatically removed
temporary directory. No hardware commands or network scans are performed.
"""
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
BODY = SOURCE[SOURCE.index('std::string tcpSocketError('):
              SOURCE.index('\nJson::Value callBluetoothAction')]
HEADER = r'''
#include <algorithm>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <cstdio>
#define LOG_INFO(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n',stderr); } while(0)
#define LOG_WARN(...) LOG_INFO(__VA_ARGS__)
'''
MAIN = r'''
int main(int argc, char **argv) {
  std::string error, reply;
  std::vector<std::string> payloads;
  for (int i=2;i<argc;++i) payloads.emplace_back(argv[i]);
  bool ok=sendTcpDirect("127.0.0.1",std::stoi(argv[1]),payloads,error,&reply);
  std::cout<<(ok?"OK":"FAIL")<<"\n"<<error<<"\n"<<reply<<"\n";
}
'''
POWER = b'%1POWR 0\r'
INPUT = b'%1INPT 31\r'


def receive(conn):
    data = b''
    while not data.endswith(b'\r'):
        part = conn.recv(1024)
        if not part:
            break
        data += part
    return data


def raw(conn):
    assert receive(conn) == POWER
    time.sleep(.4)  # Legacy peer has no application acknowledgement.


def direct(conn):
    assert receive(conn) == POWER  # No greeting was sent before the command.
    conn.sendall(b'%1POWR=OK\r')


def standard(conn):
    conn.sendall(b'PJLINK 0\r')
    assert receive(conn) == POWER
    conn.sendall(b'%1POWR=OK\r')


def strict(conn):
    # The fallback must wait for the greeting before sending any command.
    conn.settimeout(.08)
    try:
        unexpected = conn.recv(1024)
    except socket.timeout:
        unexpected = None
    assert unexpected is None, f'fallback sent before greeting: {unexpected!r}'
    conn.settimeout(5)
    standard(conn)


def drop(conn):
    conn.shutdown(socket.SHUT_RDWR)


def auth(conn):
    assert receive(conn) == POWER
    conn.sendall(b'PJLINK 1 12345678\r')


def auth_rejected(conn):
    assert receive(conn) == POWER
    conn.sendall(b'PJLINK ERRA\r')


def reject(code):
    def handle(conn):
        assert receive(conn) == POWER
        conn.sendall(b'PJLINK 0\r%1POWR=' + code.encode() + b'\r')
    return handle


def fragmented(conn):
    assert receive(conn) == POWER
    for part in [b'PJ', b'LINK 0\r', b'\n%1PO', b'WR=OK\r\n']:
        conn.sendall(part)
        time.sleep(.02)


def combined(conn):
    assert receive(conn) == POWER
    conn.sendall(b'PJLINK 0\r%1POWR=OK\r')
    conn.shutdown(socket.SHUT_WR)  # Read buffered data even alongside HUP.


def mismatched(conn):
    assert receive(conn) == POWER
    conn.sendall(b'%1INPT=OK\r')


def partial_batch(conn):
    assert receive(conn) == POWER
    conn.sendall(b'%1POWR=OK\r')
    assert receive(conn) == INPUT
    conn.shutdown(socket.SHUT_RDWR)


def resume_batch(conn):
    conn.sendall(b'PJLINK 0\r')
    assert receive(conn) == INPUT  # The confirmed power command is not replayed.
    conn.sendall(b'%1INPT=OK\r')


def no_command_reply(conn):
    assert receive(conn) == POWER
    conn.sendall(b'PJLINK 0\r')
    assert conn.recv(1) == b''  # Sender reaches its bounded attempt deadline.


def run_case(executable, name, handlers, ok=True, message='', payloads=None, port=0):
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', port))
    server.listen(8)
    server.settimeout(.1)
    port = server.getsockname()[1]
    errors, count = [], []
    stop = threading.Event()

    def serve():
        while not stop.is_set():
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            index = len(count)
            count.append(index)
            with conn:
                conn.settimeout(5)
                try:
                    assert index < len(handlers), f'extra connection {index+1}'
                    handlers[index](conn)
                except Exception as exc:
                    errors.append(repr(exc))

    worker = threading.Thread(target=serve)
    worker.start()
    try:
        args = [str(executable), str(port)] + [
            p.decode() for p in (payloads or [POWER])]
        result = subprocess.run(args, text=True, capture_output=True, timeout=32)
    finally:
        stop.set()
        worker.join(6)
        server.close()
    assert not worker.is_alive(), name
    assert not errors, (name, errors, result.stdout, result.stderr)
    assert len(count) == len(handlers), (name, len(count), result.stdout, result.stderr)
    assert result.returncode == 0, (name, result.stderr)
    assert result.stdout.startswith('OK\n' if ok else 'FAIL\n'), (name, result.stdout)
    assert message in result.stdout, (name, result.stdout)
    print(f'PASS {name}: {len(count)} connection(s)', flush=True)
    return result


def main():
    with tempfile.TemporaryDirectory(prefix='hplayer-tcp-pjlink-') as tmp:
        tmp = Path(tmp)
        source, executable = tmp / 'test.cpp', tmp / 'test'
        source.write_text(HEADER + BODY + MAIN, encoding='utf-8')
        subprocess.run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                        '-Werror', str(source), '-o', str(executable)], check=True)
        run_case(executable, 'direct_before_greeting', [direct])
        run_case(executable, 'legacy_no_reply_on_4352', [raw], port=4352)
        run_case(executable, 'standard_peer_accepts_direct', [standard])
        run_case(executable, 'fresh_connection_strict_fallback', [drop, strict])
        run_case(executable, 'recovery_on_fourth_round', [drop] * 6 + [direct])
        run_case(executable, 'fallback_on_fourth_round', [drop] * 7 + [strict])
        run_case(executable, 'eight_attempt_limit', [drop] * 8, ok=False,
                 message='已额外重试 3 轮，共 4 轮')
        run_case(executable, 'auth_is_terminal', [auth], ok=False, message='密码认证')
        run_case(executable, 'standard_auth_rejection', [auth_rejected], ok=False,
                 message='设备拒绝 PJLink 认证')
        for code, message in [('ERR1', '不支持'), ('ERR2', '参数不正确'),
                              ('ERR3', '预热或冷却'), ('ERR4', '内部故障'),
                              ('ERRA', '认证失败')]:
            run_case(executable, code, [reject(code)], ok=False, message=message)
        run_case(executable, 'fragmented_crlf', [fragmented])
        run_case(executable, 'coalesced_greeting_reply_and_close', [combined])
        run_case(executable, 'wrong_command_reply_fallback', [mismatched, strict])
        run_case(executable, 'resume_unconfirmed_payload_only',
                 [partial_batch, resume_batch], payloads=[POWER, INPUT])
        run_case(executable, 'greeting_without_reply_fallback', [no_command_reply, strict])

        def generic(conn):
            assert receive(conn) == b'RAW\r'
        run_case(executable, 'generic_tcp_unchanged', [generic], payloads=[b'RAW\r'])

        # Reserve a non-listening port to deterministically reproduce refusal.
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            port = reserved.getsockname()[1]
            result = subprocess.run([str(executable), str(port), POWER.decode()],
                                    text=True, capture_output=True, timeout=8)
        assert result.stdout.startswith('FAIL\n'), result.stdout
        assert '目标端口拒绝连接' in result.stdout, result.stdout
        assert '已额外重试 3 轮，共 4 轮' in result.stdout, result.stdout
        assert result.stderr.count('mode=direct') == 4, result.stderr
        assert result.stderr.count('mode=PJLink') == 4, result.stderr
        print('PASS connection_refused_semantics_and_retry_count', flush=True)
        result = subprocess.run([str(executable), '0', POWER.decode()],
                                text=True, capture_output=True, timeout=3)
        assert '端口无效' in result.stdout and '共 4 轮' not in result.stdout
        print('PASS invalid_config_is_terminal', flush=True)
    print('All 22 cases passed; temporary compilation directory removed.')


if __name__ == '__main__':
    main()
